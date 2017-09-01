/*
 * QEMU model of SUN GEM ethernet controller
 *
 * As found in Apple ASICs among others
 *
 * Copyright 2016 Ben Herrenschmidt
 */
#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "qemu/log.h"
#include "net/net.h"
#include "net/checksum.h"
#include "hw/net/mii.h"
#include "trace.h"
/* For crc32 */
#include <zlib.h>

#include "sungem.h"

#define TYPE_SUNGEM "sungem"

#define SUNGEM(obj) OBJECT_CHECK(SunGEMState, (obj), TYPE_SUNGEM)

#define MAX_PACKET_SIZE 9016

typedef struct {
    PCIDevice pdev;

    MemoryRegion sungem;
    MemoryRegion greg;
    MemoryRegion txdma;
    MemoryRegion rxdma;
    MemoryRegion mmio;
    NICState *nic;
    NICConf conf;
    uint32_t nregsblocks;
    uint32_t **regs;
    uint32_t phy_addr;

    /* Cache some useful things */
    uint32_t rx_mask;
    uint32_t tx_mask;

    /* Current tx packet */
    uint8_t tx_data[MAX_PACKET_SIZE];
    uint32_t tx_size;
    uint64_t tx_first_ctl;
} SunGEMState;

#define SUNGEM_MMIO_SIZE        0x200000

/* Global registers */
#define SUNGEM_MMIO_GREG_SIZE   0x2000
#define GREG_SEBSTATE     0x0000UL    /* SEB State Register */
#define GREG_STAT         0x000CUL    /* Status Register */
#define GREG_IMASK        0x0010UL    /* Interrupt Mask Register */
#define GREG_IACK         0x0014UL    /* Interrupt ACK Register */
#define GREG_STAT2        0x001CUL    /* Alias of GREG_STAT */
#define GREG_PCIESTAT     0x1000UL    /* PCI Error Status Register */
#define GREG_PCIEMASK     0x1004UL    /* PCI Error Mask Register */
#define GREG_SWRST        0x1010UL    /* Software Reset Register */

/* TX DMA Registers */
#define SUNGEM_MMIO_TXDMA_SIZE   0x1000
#define TXDMA_KICK        0x0000UL    /* TX Kick Register */
#define TXDMA_CFG         0x0004UL    /* TX Configuration Register */
#define TXDMA_DBLOW       0x0008UL    /* TX Desc. Base Low */
#define TXDMA_DBHI        0x000CUL    /* TX Desc. Base High */
#define TXDMA_PCNT        0x0024UL    /* TX FIFO Packet Counter	*/
#define TXDMA_SMACHINE    0x0028UL    /* TX State Machine Register */
#define TXDMA_DPLOW       0x0030UL    /* TX Data Pointer Low */
#define TXDMA_DPHI        0x0034UL    /* TX Data Pointer High */
#define TXDMA_TXDONE      0x0100UL    /* TX Completion Register */
#define TXDMA_FTAG        0x0108UL    /* TX FIFO Tag */
#define TXDMA_FSZ         0x0118UL    /* TX FIFO Size */

/* Receive DMA Registers */
#define SUNGEM_MMIO_RXDMA_SIZE   0x2000
#define RXDMA_CFG         0x0000UL    /* RX Configuration Register */
#define RXDMA_DBLOW       0x0004UL    /* RX Descriptor Base Low */
#define RXDMA_DBHI        0x0008UL    /* RX Descriptor Base High */
#define RXDMA_PCNT        0x0018UL    /* RX FIFO Packet Counter */
#define RXDMA_SMACHINE    0x001CUL    /* RX State Machine Register */
#define RXDMA_PTHRESH     0x0020UL    /* Pause Thresholds */
#define RXDMA_DPLOW       0x0024UL    /* RX Data Pointer Low */
#define RXDMA_DPHI        0x0028UL    /* RX Data Pointer High */
#define RXDMA_KICK        0x0100UL    /* RX Kick Register */
#define RXDMA_DONE        0x0104UL    /* RX Completion Register */
#define RXDMA_BLANK       0x0108UL    /* RX Blanking Register */
#define RXDMA_FTAG        0x0110UL    /* RX FIFO Tag */
#define RXDMA_FSZ         0x0120UL    /* RX FIFO Size */


static const struct RegBlock {
    uint32_t base;      /* Base offset */
    uint32_t count;     /* Number of registers */
} RegBlocks[] = {
    { 0x0000, 0x0006 },      /* 0x0000..0x001c : GREG bank 0 */
    { 0x1000, 0x0005 },      /* 0x1000..0x1010 : GREG bank 1 */
    { 0x2000, 0x000e },      /* 0x2000..0x2034 : TX DMA bank 0 */
    { 0x2100, 0x0007 },      /* 0x2100..0x2118 : TX DMA bank 1 */
    { 0x3000, 0x0005 },      /* 0x3000..0x3010 : WakeOnLan */
    { 0x4000, 0x000b },      /* 0x4000..0c4028 : RX DMA bank 0 */
    { 0x4100, 0x0009 },      /* 0x4100..0x4120 : RX DMA bank 1 */
    { 0x6000, 0x004e },      /* 0x6000..0x6134 : MAC */
    { 0x6200, 0x0008 },      /* 0x6200..0x621c : MIF */
    { 0x9000, 0x0007 },      /* 0x9000..0x9018 : PCS */
    { 0x9050, 0x0004 },      /* 0x9050..0x905c : PCS */
};

/* Fast access, hopefully optimized out by the compiler */
static uint32_t *sungem_get_reg(SunGEMState *s, uint32_t reg)
{
        uint32_t idx = (reg & 0xff) >> 2;
        if (reg <= 0x1c) {
                return &s->regs[0][idx];
        } else if (reg >= 0x1000 && reg <= 0x1010) {
                return &s->regs[1][idx];
        } else if (reg >= 0x2000 && reg <= 0x2034) {
                return &s->regs[2][idx];
        } else if (reg >= 0x2100 && reg <= 0x2118) {
                return &s->regs[3][idx];
        } else if (reg >= 0x3000 && reg <= 0x3010) {
                return &s->regs[4][idx];
        } else if (reg >= 0x4000 && reg <= 0x4028) {
                return &s->regs[5][idx];
        } else if (reg >= 0x4100 && reg <= 0x4120) {
                return &s->regs[6][idx];
        } else if (reg >= 0x6000 && reg <= 0x6134) {
                return &s->regs[7][idx];
        } else if (reg >= 0x6200 && reg <= 0x621c) {
                return &s->regs[8][idx];
        } else if (reg >= 0x9000 && reg <= 0x9018) {
                return &s->regs[9][idx];
        } else if (reg >= 0x9050 && reg <= 0x905c) {
                return &s->regs[10][idx];
        }
        return NULL;
}

#define SET_REG(s, reg, val)                                    \
    do {                                                        \
        uint32_t *regp = sungem_get_reg(s, reg);                \
        assert(regp);                                           \
        *regp = val;                                            \
    } while (0)

#define GET_REG(s, reg)                                         \
    ({                                                      \
        uint32_t *regp = sungem_get_reg(s, reg);                \
        assert(regp);                                           \
        *regp;                                                  \
    })

static void sungem_eval_irq(SunGEMState *s)
{
    uint32_t stat, mask;

    mask = GET_REG(s, GREG_IMASK);
    stat = GET_REG(s, GREG_STAT) & ~GREG_STAT_TXNR;
    if (stat & ~mask) {
        pci_set_irq(PCI_DEVICE(s), 1);
    } else {
        pci_set_irq(PCI_DEVICE(s), 0);
    }
}

static void sungem_update_status(SunGEMState *s, uint32_t bits, bool val)
{
    uint32_t stat;

    stat = GET_REG(s, GREG_STAT);
    if (val) {
        stat |= bits;
    } else {
        stat &= ~bits;
    }
    SET_REG(s, GREG_STAT, stat);
    sungem_eval_irq(s);
}

static void sungem_eval_cascade_irq(SunGEMState *s)
{
    uint32_t stat, mask;

    mask = GET_REG(s, MAC_TXSTAT);
    stat = GET_REG(s, MAC_TXMASK);
    if (stat & ~mask) {
        sungem_update_status(s, GREG_STAT_TXMAC, true);
    } else {
        sungem_update_status(s, GREG_STAT_TXMAC, false);
    }

    mask = GET_REG(s, MAC_RXSTAT);
    stat = GET_REG(s, MAC_RXMASK);
    if (stat & ~mask) {
        sungem_update_status(s, GREG_STAT_RXMAC, true);
    } else {
        sungem_update_status(s, GREG_STAT_RXMAC, false);
    }

    mask = GET_REG(s, MAC_CSTAT);
    stat = GET_REG(s, MAC_MCMASK) & ~MAC_CSTAT_PTR;
    if (stat & ~mask) {
        sungem_update_status(s, GREG_STAT_MAC, true);
    } else {
        sungem_update_status(s, GREG_STAT_MAC, false);
    }
}

static void sungem_do_tx_csum(SunGEMState *s)
{
    uint16_t start, off;
    uint32_t csum;

    start = (s->tx_first_ctl & TXDCTRL_CSTART) >> 15;
    off = (s->tx_first_ctl & TXDCTRL_COFF) >> 21;

    trace_sungem_tx_checksum(start, off);

    if (start > (s->tx_size - 2) || off > (s->tx_size - 2)) {
        trace_sungem_tx_checksum_oob();
        return;
    }

    csum = net_raw_checksum(s->tx_data + start, s->tx_size - start);
    stw_be_p(s->tx_data + off, csum);
}

static void sungem_send_packet(SunGEMState *s, const uint8_t *buf,
                               int size)
{
    NetClientState *nc = qemu_get_queue(s->nic);

    if (GET_REG(s, MAC_XIFCFG) & MAC_XIFCFG_LBCK) {
        nc->info->receive(nc, buf, size);
    } else {
        qemu_send_packet(nc, buf, size);
    }
}

static void sungem_process_tx_desc(SunGEMState *s, struct gem_txd *desc)
{
    PCIDevice *d = PCI_DEVICE(s);
    uint32_t len;

    /* If it's a start of frame, discard anything we had in the
     * buffer and start again. This should be an error condition
     * if we had something ... for now we ignore it
     */
    if (desc->control_word & TXDCTRL_SOF) {
        if (s->tx_first_ctl) {
            trace_sungem_tx_unfinished();
        }
        s->tx_size = 0;
        s->tx_first_ctl = desc->control_word;
    }

    /* Grab data size */
    len = desc->control_word & TXDCTRL_BUFSZ;

    /* Clamp it to our max size */
    if ((s->tx_size + len) > MAX_PACKET_SIZE) {
        trace_sungem_tx_overflow();
        len = MAX_PACKET_SIZE - s->tx_size;
    }

    /* Read the data */
    pci_dma_read(d, desc->buffer, &s->tx_data[s->tx_size], len);
    s->tx_size += len;

    /* If end of frame, send packet */
    if (desc->control_word & TXDCTRL_EOF) {
        trace_sungem_tx_finished(s->tx_size);

        /* Handle csum */
        if (s->tx_first_ctl & TXDCTRL_CENAB) {
            sungem_do_tx_csum(s);
        }

        /* Send it */
        sungem_send_packet(s, s->tx_data, s->tx_size);

        /* No more pending packet */
        s->tx_size = 0;
        s->tx_first_ctl = 0;
    }
}

static void sungem_tx_kick(SunGEMState *s)
{
    PCIDevice *d = PCI_DEVICE(s);
    uint32_t comp, kick;
    uint32_t txdma_cfg, txmac_cfg, ints;
    uint64_t dbase;

    trace_sungem_tx_kick();

    /* Check that both TX MAC and TX DMA are enabled. We don't
     * handle DMA-less direct FIFO operations (we don't emulate
     * the FIFO at all).
     *
     * A write to TXDMA_KICK while DMA isn't enabled can happen
     * when the driver is resetting the pointer.
     */
    txdma_cfg = GET_REG(s, TXDMA_CFG + 0x2000);
    txmac_cfg = GET_REG(s, MAC_TXCFG);
    if (!(txdma_cfg & TXDMA_CFG_ENABLE) ||
        !(txmac_cfg & MAC_TXCFG_ENAB)) {
        trace_sungem_tx_disabled();
        return;
    }

    /* XXX Test min frame size register ? */
    /* XXX Test max frame size register ? */

    dbase = GET_REG(s, TXDMA_DBHI + 0x2000);
    dbase = (dbase << 32) | GET_REG(s, TXDMA_DBLOW + 0x2000);

    comp = GET_REG(s, TXDMA_TXDONE + 0x2000) & s->tx_mask;
    kick = GET_REG(s, TXDMA_KICK + 0x2000) & s->tx_mask;

    trace_sungem_tx_process(comp, kick, s->tx_mask + 1);

    /* This is rather primitive for now, we just send everything we
     * can in one go, like e1000. Ideally we should do the sending
     * from some kind of background task
     */
    while (comp != kick) {
        struct gem_txd desc;

        /* Read the next descriptor */
        pci_dma_read(d, dbase + comp * sizeof(desc), &desc, sizeof(desc));

        /* Byteswap descriptor */
        desc.control_word = le64_to_cpu(desc.control_word);
        desc.buffer = le64_to_cpu(desc.buffer);
        trace_sungem_tx_desc(comp, desc.control_word, desc.buffer);

        /* Send it for processing */
        sungem_process_tx_desc(s, &desc);

        /* Interrupt */
        ints = GREG_STAT_TXDONE;
        if (desc.control_word & TXDCTRL_INTME) {
            ints |= GREG_STAT_TXINTME;
        }
        sungem_update_status(s, ints, true);

        /* Next ! */
        comp = (comp + 1) & s->tx_mask;
        SET_REG(s, TXDMA_TXDONE + 0x2000, comp);
    }

    /* We sent everything, set status/irq bit */
    sungem_update_status(s, GREG_STAT_TXALL, true);
}

static bool sungem_rx_full(SunGEMState *s, uint32_t kick, uint32_t done)
{
    return kick == ((done + 1) & s->rx_mask);
}

static int sungem_can_receive(NetClientState *nc)
{
    SunGEMState *s = qemu_get_nic_opaque(nc);
    uint32_t kick, done, rxdma_cfg, rxmac_cfg;
    bool full;

    rxmac_cfg = GET_REG(s, MAC_RXCFG);
    rxdma_cfg = GET_REG(s, RXDMA_CFG + 0x4000);

    /* If MAC disabled, can't receive */
    if ((rxmac_cfg & MAC_RXCFG_ENAB) == 0) {
        trace_sungem_rx_mac_disabled();
        return 0;
    }
    if ((rxdma_cfg & RXDMA_CFG_ENABLE) == 0) {
        trace_sungem_rx_txdma_disabled();
        return 0;
    }

    /* Check RX availability */
    kick = GET_REG(s, RXDMA_KICK + 0x4000);
    done = GET_REG(s, RXDMA_DONE + 0x4000);
    full = sungem_rx_full(s, kick, done);

    trace_sungem_rx_check(!full, kick, done);

    return !full;
}

enum {
        rx_no_match,
        rx_match_promisc,
        rx_match_bcast,
        rx_match_allmcast,
        rx_match_mcast,
        rx_match_mac,
        rx_match_altmac,
};

static int sungem_check_rx_mac(SunGEMState *s, const uint8_t *mac, uint32_t crc)
{
    uint32_t rxcfg = GET_REG(s, MAC_RXCFG);
    uint32_t mac0, mac1, mac2;

    /* Promisc enabled ? */
    if (rxcfg & MAC_RXCFG_PROM) {
        return rx_match_promisc;
    }

    /* Format MAC address into dwords */
    mac0 = (mac[4] << 8) | mac[5];
    mac1 = (mac[2] << 8) | mac[3];
    mac2 = (mac[0] << 8) | mac[1];

    trace_sungem_rx_mac_check(mac0, mac1, mac2);

    /* Is this a broadcast frame ? */
    if (mac0 == 0xffff && mac1 == 0xffff && mac2 == 0xffff) {
        return rx_match_bcast;
    }

    /* TODO: Implement address filter registers (or we don't care ?) */

    /* Is this a multicast frame ? */
    if (mac[0] & 1) {
        trace_sungem_rx_mac_multicast();

        /* Promisc group enabled ? */
        if (rxcfg & MAC_RXCFG_PGRP) {
            return rx_match_allmcast;
        }

        /* TODO: Check MAC control frames (or we don't care) ? */

        /* Check hash filter (somebody check that's correct ?) */
        if (rxcfg & MAC_RXCFG_HFE) {
            uint32_t hash, idx;

            crc >>= 24;
            idx = (crc >> 2) & 0x3c;
            hash = GET_REG(s, MAC_HASH0 + idx);
            if (hash & (1 << (15 - (crc & 0xf)))) {
                return rx_match_mcast;
            }
        }
        return rx_no_match;
    }

    /* Main MAC check */
    trace_sungem_rx_mac_compare(GET_REG(s, MAC_ADDR0), GET_REG(s, MAC_ADDR1),
                                GET_REG(s, MAC_ADDR2));
    if (mac0 == GET_REG(s, MAC_ADDR0) &&
        mac1 == GET_REG(s, MAC_ADDR1) &&
        mac2 == GET_REG(s, MAC_ADDR2)) {
        return rx_match_mac;
    }

    /* Alt MAC check */
    if (mac0 == GET_REG(s, MAC_ADDR3) &&
        mac1 == GET_REG(s, MAC_ADDR4) &&
        mac2 == GET_REG(s, MAC_ADDR5)) {
        return rx_match_altmac;
    }

    return rx_no_match;
}

static ssize_t sungem_receive(NetClientState *nc, const uint8_t *buf,
                              size_t size)
{
    SunGEMState *s = qemu_get_nic_opaque(nc);
    PCIDevice *d = PCI_DEVICE(s);
    uint32_t mac_crc, done, kick, max_fsize;
    uint32_t fcs_size, ints, rxdma_cfg, rxmac_cfg, csum, coff;
    uint8_t smallbuf[60];
    struct gem_rxd desc;
    uint64_t dbase, baddr;
    unsigned int rx_cond;

    trace_sungem_rx_packet(size);

    rxmac_cfg = GET_REG(s, MAC_RXCFG);
    rxdma_cfg = GET_REG(s, RXDMA_CFG + 0x4000);
    max_fsize = GET_REG(s, MAC_MAXFSZ) & 0x7fff;

    /* If MAC or DMA disabled, can't receive */
    if (!(rxdma_cfg & RXDMA_CFG_ENABLE) ||
        !(rxmac_cfg & MAC_RXCFG_ENAB)) {
        trace_sungem_rx_disabled();
        return 0;
    }

    /* Size adjustment for FCS */
    if (rxmac_cfg & MAC_RXCFG_SFCS) {
        fcs_size = 0;
    } else {
        fcs_size = 4;
    }

    /* Discard frame smaller than a MAC or larger than max frame size
     * (when accounting for FCS)
     */
    if (size < 6 || (size + 4) > max_fsize) {
        trace_sungem_rx_bad_frame_size(size);
        /* XXX Increment error statistics ? */
        return size;
    }

    /* We don't drop too small frames since we get them in qemu, we pad
     * them instead. We should probably use the min frame size register
     * but I don't want to use a variable size staging buffer and I
     * know both MacOS and Linux use the default 64 anyway. We use 60
     * here to account for the non-existent FCS.
     */
    if (size < 60) {
        memcpy(smallbuf, buf, size);
        memset(&smallbuf[size], 0, 60 - size);
        buf = smallbuf;
        size = 60;
    }

    /* Get MAC crc */
    mac_crc = crc32(~0, buf, 6);

    /* Packet isn't for me ? */
    rx_cond = sungem_check_rx_mac(s, buf, mac_crc);
    if (rx_cond == rx_no_match) {
        /* Just drop it */
        trace_sungem_rx_unmatched();
        return size;
    }

    /* Get ring pointers */
    kick = GET_REG(s, RXDMA_KICK + 0x4000) & s->rx_mask;
    done = GET_REG(s, RXDMA_DONE + 0x4000) & s->rx_mask;

    trace_sungem_rx_process(done, kick, s->rx_mask + 1);

    /* Ring full ? Can't receive */
    if (sungem_rx_full(s, kick, done)) {
        trace_sungem_rx_ringfull();
        return 0;
    }

    /* Note: The real GEM will fetch descriptors in blocks of 4,
     * for now we handle them one at a time, I think the driver will
     * cope
     */

    dbase = GET_REG(s, RXDMA_DBHI + 0x4000);
    dbase = (dbase << 32) | GET_REG(s, RXDMA_DBLOW + 0x4000);

    /* Read the next descriptor */
    pci_dma_read(d, dbase + done * sizeof(desc), &desc, sizeof(desc));

    trace_sungem_rx_desc(le64_to_cpu(desc.status_word),
                         le64_to_cpu(desc.buffer));

    /* Effective buffer address */
    baddr = le64_to_cpu(desc.buffer) & ~7ull;
    baddr |= (rxdma_cfg & RXDMA_CFG_FBOFF) >> 10;

    /* Write buffer out */
    pci_dma_write(d, baddr, buf, size);

    if (fcs_size) {
        /* Should we add an FCS ? Linux doesn't ask us to strip it,
         * however I believe nothing checks it... For now we just
         * do nothing. It's faster this way.
         */
    }

    /* Calculate the checksum */
    coff = (rxdma_cfg & RXDMA_CFG_CSUMOFF) >> 13;
    csum = net_raw_checksum((uint8_t *)buf + coff, size - coff);

    /* Build the updated descriptor */
    desc.status_word = (size + fcs_size) << 16;
    desc.status_word |= ((uint64_t)(mac_crc >> 16)) << 44;
    desc.status_word |= csum;
    if (rx_cond == rx_match_mcast) {
        desc.status_word |= RXDCTRL_HPASS;
    }
    if (rx_cond == rx_match_altmac) {
        desc.status_word |= RXDCTRL_ALTMAC;
    }
    desc.status_word = cpu_to_le64(desc.status_word);

    pci_dma_write(d, dbase + done * sizeof(desc), &desc, sizeof(desc));

    done = (done + 1) & s->rx_mask;
    SET_REG(s, RXDMA_DONE + 0x4000, done);

    /* XXX Unconditionally set RX interrupt for now. The interrupt
     * mitigation timer might well end up adding more overhead than
     * helping here...
     */
    ints = GREG_STAT_RXDONE;
    if (sungem_rx_full(s, kick, done)) {
        ints |= GREG_STAT_RXNOBUF;
    }
    sungem_update_status(s, ints, true);

    return size;
}

static void sungem_set_link_status(NetClientState *nc)
{
    /* We don't do anything for now as I believe none of the OSes
     * drivers use the MIF autopoll feature nor the PHY interrupt
     */
}

static void sungem_update_masks(SunGEMState *s)
{
    uint32_t sz;

    sz = 1 << (((GET_REG(s, RXDMA_CFG + 0x4000) & RXDMA_CFG_RINGSZ) >> 1) + 5);
    s->rx_mask = sz - 1;

    sz = 1 << (((GET_REG(s, TXDMA_CFG + 0x2000) & TXDMA_CFG_RINGSZ) >> 1) + 5);
    s->tx_mask = sz - 1;
}

static void sungem_reset_rx(SunGEMState *s)
{
    trace_sungem_rx_reset();

    /* XXX Do RXCFG */
    /* XXX Check value */
    SET_REG(s, RXDMA_FSZ + 0x4000, 0x140);
    SET_REG(s, RXDMA_DONE + 0x4000, 0);
    SET_REG(s, RXDMA_KICK + 0x4000, 0);
    SET_REG(s, RXDMA_CFG + 0x4000, 0x1000010);
    SET_REG(s, RXDMA_PTHRESH + 0x4000, 0xf8);
    SET_REG(s, RXDMA_BLANK + 0x4000, 0);

    sungem_update_masks(s);
}

static void sungem_reset_tx(SunGEMState *s)
{
    trace_sungem_tx_reset();

    /* XXX Do TXCFG */
    /* XXX Check value */
    SET_REG(s, TXDMA_FSZ + 0x2000, 0x90);
    SET_REG(s, TXDMA_TXDONE + 0x2000, 0);
    SET_REG(s, TXDMA_KICK + 0x2000, 0);
    SET_REG(s, TXDMA_CFG + 0x2000, 0x118010);

    sungem_update_masks(s);

    s->tx_size = 0;
    s->tx_first_ctl = 0;
}

static void sungem_reset_all(SunGEMState *s, bool pci_reset)
{
    trace_sungem_reset(pci_reset);

    sungem_reset_rx(s);
    sungem_reset_tx(s);

    SET_REG(s, GREG_IMASK, 0xFFFFFFF);
    SET_REG(s, GREG_STAT, 0);
    if (pci_reset) {
        uint8_t *ma = s->conf.macaddr.a;

        SET_REG(s, GREG_SWRST, 0);
        SET_REG(s, MAC_ADDR0, (ma[4] << 8) | ma[5]);
        SET_REG(s, MAC_ADDR1, (ma[2] << 8) | ma[3]);
        SET_REG(s, MAC_ADDR2, (ma[0] << 8) | ma[1]);
    } else {
        SET_REG(s, GREG_SWRST, GET_REG(s, GREG_SWRST) & GREG_SWRST_RSTOUT);
    }
    SET_REG(s, MIF_CFG, MIF_CFG_MDI0);
}

static void sungem_mii_write(SunGEMState *s, uint8_t phy_addr,
                             uint8_t reg_addr, uint16_t val)
{
    trace_sungem_mii_write(phy_addr, reg_addr, val);

    /* XXX TODO */
}

static uint16_t __sungem_mii_read(SunGEMState *s, uint8_t phy_addr,
                                uint8_t reg_addr)
{
    if (phy_addr != s->phy_addr) {
        return 0xffff;
    }
    /* Primitive emulation of a BCM5201 to please the driver,
     * ID is 0x00406210. TODO: Do a gigabit PHY like BCM5400
     */
    switch (reg_addr) {
    case MII_BMCR:
        return 0;
    case MII_PHYID1:
        return 0x0040;
    case MII_PHYID2:
        return 0x6210;
    case MII_BMSR:
        if (qemu_get_queue(s->nic)->link_down) {
            return MII_BMSR_100TX_FD  | MII_BMSR_AUTONEG;
        } else {
            return MII_BMSR_100TX_FD | MII_BMSR_AN_COMP |
                    MII_BMSR_AUTONEG | MII_BMSR_LINK_ST;
        }
    case MII_ANLPAR:
    case MII_ANAR:
        return MII_ANLPAR_TXFD;
    case 0x18: /* 5201 AUX status */
        return 3; /* 100FD */
    default:
        return 0;
    };
}
static uint16_t sungem_mii_read(SunGEMState *s, uint8_t phy_addr,
                                uint8_t reg_addr)
{
    uint16_t val;

    val = __sungem_mii_read(s, phy_addr, reg_addr);

    trace_sungem_mii_read(phy_addr, reg_addr, val);

    return val;
}

static uint32_t sungem_mii_op(SunGEMState *s, uint32_t val)
{
    uint8_t phy_addr, reg_addr, op;

    /* Ignore not start of frame */
    if ((val >> 30) != 1) {
        trace_sungem_mii_invalid_sof(val >> 30);
        return 0xffff;
    }
    phy_addr = (val & MIF_FRAME_PHYAD) >> 23;
    reg_addr = (val & MIF_FRAME_REGAD) >> 18;
    op = (val & MIF_FRAME_OP) >> 28;
    switch (op) {
    case 1:
        sungem_mii_write(s, phy_addr, reg_addr, val & MIF_FRAME_DATA);
        return val | MIF_FRAME_TALSB;
    case 2:
        return sungem_mii_read(s, phy_addr, reg_addr) | MIF_FRAME_TALSB;
    default:
        trace_sungem_mii_invalid_op(op);
    }
    return 0xffff | MIF_FRAME_TALSB;
}

static void sungem_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    SunGEMState *s = opaque;
    uint32_t *regp;

    /* Drop writes to PROM image */
    if (addr > PROM_START) {
        return;
    }

    regp = sungem_get_reg(s, addr);
    if (!regp) {
        trace_sungem_mmio_write_unknown(addr);
        return;
    }

    trace_sungem_mmio_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case MAC_TXRST: /* Not technically read-only but will do for now */
    case MAC_RXRST: /* Not technically read-only but will do for now */
    case MAC_TXSTAT:
    case MAC_RXSTAT:
    case MAC_CSTAT:
    case MAC_PATMPS:
    case MAC_SMACHINE:
    case MIF_STATUS:
    case MIF_SMACHINE:
    case PCS_MIISTAT:
    case PCS_ISTAT:
    case PCS_SSTATE:
        return; /* No actual write */

    case MIF_CFG:
        /* Maintain the RO MDI bits to advertize an MDIO PHY on MDI0 */
        val &= ~MIF_CFG_MDI1;
        val |= MIF_CFG_MDI0;
        break;
    case MAC_MINFSZ:
        /* 10-bits implemented */
        val &= 0x3ff;
        break;
    }

    *regp = val;

    /* Post write action */
    switch (addr) {
    case MAC_TXMASK:
    case MAC_RXMASK:
    case MAC_MCMASK:
        sungem_eval_cascade_irq(s);
        break;
    case MIF_FRAME:
        *regp = sungem_mii_op(s, val);
        break;
    case MAC_RXCFG:
        sungem_update_masks(s);
        if ((GET_REG(s, MAC_RXCFG) & MAC_RXCFG_ENAB) != 0 &&
            (GET_REG(s, RXDMA_CFG + 0x4000) & RXDMA_CFG_ENABLE) != 0) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    }
}

static uint64_t sungem_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    SunGEMState *s = opaque;
    uint32_t val, *regp;

    /* No PROM image to read for now... */
    if (addr > PROM_START) {
        return 0xffffffff;
    }

    regp = sungem_get_reg(s, addr);
    if (!regp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown register 0x%04x\n",
                      (unsigned int)addr);
        return 0;
    }
    val = *regp;

    trace_sungem_mmio_read(addr, val);

    switch (addr) {
    case MAC_TXSTAT:
        *regp = 0; /* Side effect, clear all */
        sungem_update_status(s, GREG_STAT_TXMAC, false);
        break;
    case MAC_RXSTAT:
        *regp = 0; /* Side effect, clear all */
        sungem_update_status(s, GREG_STAT_RXMAC, false);
        break;
    case MAC_CSTAT:
        *regp &= MAC_CSTAT_PTR; /* Side effect, interrupt bits */
        sungem_update_status(s, GREG_STAT_MAC, false);
        break;
    }

    return val;
}

static void sungem_mmio_greg_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    SunGEMState *s = opaque;
    uint32_t *regp;

    regp = sungem_get_reg(s, addr);
    if (!regp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Write to unknown GREG register 0x%"HWADDR_PRIx,
                      addr);
        return;
    }

    trace_sungem_mmio_greg_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case GREG_SEBSTATE:
    case GREG_STAT:
    case GREG_STAT2:
    case GREG_PCIESTAT:
        return; /* No actual write */
    case GREG_IACK:
        val &= GREG_STAT_LATCH;
        SET_REG(s, GREG_STAT, GET_REG(s, GREG_STAT) & ~val);
        sungem_eval_irq(s);
        return; /* No actual write */
    case GREG_PCIEMASK:
        val &= 0x7;
        break;
    }
    
    *regp = val;
    
    /* Post write action */
    switch (addr) {
    case GREG_IMASK:
        /* Re-evaluate interrupt */
        sungem_eval_irq(s);
        break;
    case GREG_SWRST:
        switch (val & (GREG_SWRST_TXRST | GREG_SWRST_RXRST)) {
        case GREG_SWRST_RXRST:
            sungem_reset_rx(s);
            break;
        case GREG_SWRST_TXRST:
            sungem_reset_tx(s);
            break;
        case GREG_SWRST_RXRST | GREG_SWRST_TXRST:
            sungem_reset_all(s, false);
        }
        break;
    }
}

static uint64_t sungem_mmio_greg_read(void *opaque, hwaddr addr, unsigned size)
{
    SunGEMState *s = opaque;
    uint32_t val, *regp;

    regp = sungem_get_reg(s, addr);
    if (!regp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown GREG register 0x%"HWADDR_PRIx,
                      addr);
        return 0;
    }
    val = *regp;

    trace_sungem_mmio_greg_read(addr, val);

    switch (addr) {
    case GREG_STAT:
        /* Side effect, clear bottom 7 bits */
        *regp = val & ~GREG_STAT_LATCH;
        sungem_eval_irq(s);

        /* Inject TX completion in returned value */
        val = (val & ~GREG_STAT_TXNR) |
                (GET_REG(s, TXDMA_TXDONE + 0x2000) << GREG_STAT_TXNR_SHIFT);
        break;
    case GREG_STAT2:
        /* Return the status reg without side effect
         * (and inject TX completion in returned value)
         */
        return (GET_REG(s, GREG_STAT) & ~GREG_STAT_TXNR) |
                (GET_REG(s, TXDMA_TXDONE + 0x2000) << GREG_STAT_TXNR_SHIFT);
    }

    return val;
}

static const MemoryRegionOps sungem_mmio_greg_ops = {
    .read = sungem_mmio_greg_read,
    .write = sungem_mmio_greg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void sungem_mmio_txdma_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    SunGEMState *s = opaque;
    uint32_t *regp;

    regp = sungem_get_reg(s, addr + 0x2000);
    if (!regp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Write to unknown TXDMA register 0x%"HWADDR_PRIx,
                      addr);
        return;
    }

    trace_sungem_mmio_txdma_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case TXDMA_TXDONE:
    case TXDMA_PCNT:
    case TXDMA_SMACHINE:
    case TXDMA_DPLOW:
    case TXDMA_DPHI:
    case TXDMA_FSZ:
    case TXDMA_FTAG:
        return; /* No actual write */
    }

    *regp = val;
    
    /* Post write action */
    switch (addr) {
    case TXDMA_KICK:
        sungem_tx_kick(s);
        break;
    case TXDMA_CFG:
        sungem_update_masks(s);
        break;
    }
}

static uint64_t sungem_mmio_txdma_read(void *opaque, hwaddr addr, unsigned size)
{
    SunGEMState *s = opaque;
    uint32_t val, *regp;

    regp = sungem_get_reg(s, addr + 0x2000);
    if (!regp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown TXDMA register 0x%"HWADDR_PRIx,
                      addr);
        return 0;
    }

    val = *regp;

    trace_sungem_mmio_txdma_read(addr, val);

    return val;
}

static const MemoryRegionOps sungem_mmio_txdma_ops = {
    .read = sungem_mmio_txdma_read,
    .write = sungem_mmio_txdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void sungem_mmio_rxdma_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    SunGEMState *s = opaque;
    uint32_t *regp;

    regp = sungem_get_reg(s, addr + 0x4000);
    if (!regp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Write to unknown RXDMA register 0x%"HWADDR_PRIx,
                      addr);
        return;
    }

    trace_sungem_mmio_rxdma_write(addr, val);

    /* Pre-write filter */
    switch (addr) {
    /* Read only registers */
    case RXDMA_DONE:
    case RXDMA_PCNT:
    case RXDMA_SMACHINE:
    case RXDMA_DPLOW:
    case RXDMA_DPHI:
    case RXDMA_FSZ:
    case RXDMA_FTAG:
        return; /* No actual write */
    }

    *regp = val;

    /* Post write action */
    switch (addr) {
    case RXDMA_KICK:
        trace_sungem_mmio_write_rx_kick(val);
        break;
    case RXDMA_CFG:
        sungem_update_masks(s);
        if ((GET_REG(s, MAC_RXCFG) & MAC_RXCFG_ENAB) != 0 &&
            (GET_REG(s, RXDMA_CFG + 0x4000) & RXDMA_CFG_ENABLE) != 0) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    }
}

static uint64_t sungem_mmio_rxdma_read(void *opaque, hwaddr addr, unsigned size)
{
    SunGEMState *s = opaque;
    uint32_t val, *regp;

    regp = sungem_get_reg(s, addr + 0x4000);
    if (!regp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from unknown RXDMA register 0x%"HWADDR_PRIx,
                      addr);
        return 0;
    }

    val = *regp;

    trace_sungem_mmio_rxdma_read(addr, val);

    return val;
}

static const MemoryRegionOps sungem_mmio_rxdma_ops = {
    .read = sungem_mmio_rxdma_read,
    .write = sungem_mmio_rxdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};


static void sungem_init_regs(SunGEMState *s)
{
    uint32_t i;

    s->nregsblocks = ARRAY_SIZE(RegBlocks);
    s->regs = g_malloc0(sizeof(uint32_t *) * s->nregsblocks);
    for (i = 0; i < s->nregsblocks; i++) {
        s->regs[i] = g_malloc0(sizeof(uint32_t) * RegBlocks[i].count);
    }
    sungem_reset_all(s, true);
}

/* PCI interface */

static const MemoryRegionOps sungem_mmio_ops = {
    .read = sungem_mmio_read,
    .write = sungem_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void sungem_uninit(PCIDevice *dev)
{
    SunGEMState *s = SUNGEM(dev);

    qemu_del_nic(s->nic);
}

static NetClientInfo net_sungem_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = sungem_can_receive,
    .receive = sungem_receive,
    .link_status_changed = sungem_set_link_status,
};

static void sungem_realize(PCIDevice *pci_dev, Error **errp)
{
    DeviceState *dev = DEVICE(pci_dev);
    SunGEMState *s = SUNGEM(pci_dev);
    uint8_t *pci_conf;

    pci_conf = pci_dev->config;

    pci_set_word(pci_conf + PCI_STATUS,
                 PCI_STATUS_FAST_BACK |
                 PCI_STATUS_DEVSEL_MEDIUM |
                 PCI_STATUS_66MHZ);

    pci_set_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID, 0x0);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_ID, 0x0);

    pci_conf[PCI_INTERRUPT_PIN] = 1; /* interrupt pin A */
    pci_conf[PCI_MIN_GNT] = 0x40;
    pci_conf[PCI_MAX_LAT] = 0x40;

    sungem_init_regs(s);
    memory_region_init(&s->sungem, OBJECT(s), "sungem", SUNGEM_MMIO_SIZE);

    memory_region_init_io(&s->greg, OBJECT(s), &sungem_mmio_greg_ops, s,
                          "sungem.greg", SUNGEM_MMIO_GREG_SIZE);
    memory_region_add_subregion_overlap(&s->sungem, 0, &s->greg, 1);

    memory_region_init_io(&s->txdma, OBJECT(s), &sungem_mmio_txdma_ops, s,
                          "sungem.txdma", SUNGEM_MMIO_TXDMA_SIZE);
    memory_region_add_subregion_overlap(&s->sungem, 0x2000, &s->txdma, 1);

    memory_region_init_io(&s->rxdma, OBJECT(s), &sungem_mmio_rxdma_ops, s,
                          "sungem.rxdma", SUNGEM_MMIO_RXDMA_SIZE);
    memory_region_add_subregion_overlap(&s->sungem, 0x4000, &s->rxdma, 1);

    memory_region_init_io(&s->mmio, OBJECT(s), &sungem_mmio_ops, s,
                          "sungem.mmio", SUNGEM_MMIO_SIZE - SUNGEM_MMIO_GREG_SIZE);
    memory_region_add_subregion(&s->sungem, 0, &s->mmio);

    pci_register_bar(pci_dev, 0, 0, &s->sungem);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_sungem_info, &s->conf,
                          object_get_typename(OBJECT(dev)),
                          dev->id, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic),
                             s->conf.macaddr.a);
}

static void sungem_reset(DeviceState *dev)
{
    SunGEMState *s = SUNGEM(dev);

    sungem_reset_all(s, true);
}

static void sungem_instance_init(Object *obj)
{
    /* XXX bootindex stuff */
}

static Property sungem_properties[] = {
    DEFINE_NIC_PROPERTIES(SunGEMState, conf),
    /* Phy address should be 0 for most Apple machines except
     * for K2 in which case it's 1. Will be set by a machine
     * override.
     */
    DEFINE_PROP_UINT32("phy_addr", SunGEMState, phy_addr, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void sungem_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = sungem_realize;
    k->exit = sungem_uninit;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_UNI_N_GMAC;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    dc->reset = sungem_reset;
    dc->props = sungem_properties;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo sungem_info = {
    .name          = TYPE_SUNGEM,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SunGEMState),
    .class_init    = sungem_class_init,
    .instance_init = sungem_instance_init,
};

static void sungem_register_types(void)
{
    type_register_static(&sungem_info);
}

type_init(sungem_register_types)
