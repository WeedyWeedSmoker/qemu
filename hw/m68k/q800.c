/*
 * QEMU Motorla 680x0 Macintosh hardware System Emulator
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu-common.h"
#include "qemu/datadir.h"
#include "sysemu/sysemu.h"
#include "cpu.h"
#include "hw/boards.h"
#include "hw/or-irq.h"
#include "elf.h"
#include "hw/loader.h"
#include "ui/console.h"
#include "hw/char/escc.h"
#include "hw/sysbus.h"
#include "hw/scsi/esp.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "standard-headers/asm-m68k/bootinfo-mac.h"
#include "bootinfo.h"
#include "hw/m68k/q800.h"
#include "hw/misc/mac_via.h"
#include "hw/misc/djmemc.h"
#include "hw/misc/iosb.h"
#include "hw/input/adb.h"
#include "hw/audio/asc.h"
#include "hw/nubus/mac-nubus-bridge.h"
#include "hw/display/macfb.h"
#include "hw/block/swim.h"
#include "net/net.h"
#include "qapi/error.h"
#include "sysemu/qtest.h"
#include "sysemu/runstate.h"
#include "sysemu/reset.h"
#include "migration/vmstate.h"

#define MACROM_ADDR     0x40800000
#define MACROM_SIZE     0x00100000

#define MACROM_FILENAME "MacROM.bin"

#define IO_BASE               0x50000000
#define IO_SLICE              0x00040000
#define IO_SLICE_MASK         (IO_SLICE - 1)
#define IO_SIZE               0x04000000

#define VIA_BASE              (IO_BASE + 0x00000)
#define SONIC_PROM_BASE       (IO_BASE + 0x08000)
#define SONIC_BASE            (IO_BASE + 0x0a000)
#define SCC_BASE              (IO_BASE + 0x0c020)
#define DJMEMC_BASE           (IO_BASE + 0x0e000)
#define ESP_BASE              (IO_BASE + 0x10000)
#define ESP_PDMA              (IO_BASE + 0x10100)
#define ASC_BASE              (IO_BASE + 0x14000)
#define IOSB_BASE             (IO_BASE + 0x18000)
#define SWIM_BASE             (IO_BASE + 0x1E000)

#define NUBUS_SUPER_SLOT_BASE 0x60000000
#define NUBUS_SLOT_BASE       0xf0000000

/*
 * the video base, whereas it a Nubus address,
 * is needed by the kernel to have early display and
 * thus provided by the bootloader
 */
#define VIDEO_BASE            0xf9000000

#define MAC_CLOCK  3686418

/* Size of whole RAM area */
#define RAM_SIZE              0x40000000


static MemTxResult macio_alias_read(void *opaque, hwaddr addr, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    MemoryRegion *mr = opaque;
    MemoryRegionSection mrs;

    addr &= IO_SLICE_MASK;
    mrs = memory_region_find(mr, addr, size);

    if (mrs.mr) {
        return memory_region_dispatch_read(mrs.mr, mrs.offset_within_region,
                                           data, size_memop(size) | MO_BE,
                                           attrs);
    } else {
        return MEMTX_DECODE_ERROR;
    }
}

static MemTxResult macio_alias_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    MemoryRegion *mr = opaque;
    MemoryRegionSection mrs;

    addr &= IO_SLICE_MASK;
    mrs = memory_region_find(mr, addr, size);

    if (mrs.mr) {
        return memory_region_dispatch_write(mrs.mr, mrs.offset_within_region,
                                            value, size_memop(size) | MO_BE,
                                            attrs);
    } else {
        return MEMTX_DECODE_ERROR;
    }
}

static const MemoryRegionOps macio_alias_ops = {
    .read_with_attrs = macio_alias_read,
    .write_with_attrs = macio_alias_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};


static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint8_t fake_mac_rom[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

    /* offset: 0xa - mac_reset */

    /* via2[vDirB] |= VIA2B_vPower */
    0x20, 0x7C, 0x50, 0xF0, 0x24, 0x00, /* moveal VIA2_BASE+vDirB,%a0 */
    0x10, 0x10,                         /* moveb %a0@,%d0 */
    0x00, 0x00, 0x00, 0x04,             /* orib #4,%d0 */
    0x10, 0x80,                         /* moveb %d0,%a0@ */

    /* via2[vBufB] &= ~VIA2B_vPower */
    0x20, 0x7C, 0x50, 0xF0, 0x20, 0x00, /* moveal VIA2_BASE+vBufB,%a0 */
    0x10, 0x10,                         /* moveb %a0@,%d0 */
    0x02, 0x00, 0xFF, 0xFB,             /* andib #-5,%d0 */
    0x10, 0x80,                         /* moveb %d0,%a0@ */

    /* while (true) ; */
    0x60, 0xFE                          /* bras [self] */
};

static uint64_t machine_id_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0xa55a2bad; /* Quadra 800 ID */
}

static void machine_id_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
}

static const MemoryRegionOps machine_id_ops = {
    .read = machine_id_read,
    .write = machine_id_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0x0;
}

static void ramio_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    return;
}

static const MemoryRegionOps ramio_ops = {
    .read = ramio_read,
    .write = ramio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void q800_init(MachineState *machine)
{
    Q800MachineState *m = Q800_MACHINE(machine);
    int linux_boot;
    int32_t kernel_size;
    uint64_t elf_entry;
    char *filename;
    int bios_size;
    ram_addr_t initrd_base;
    int32_t initrd_size;
    MacFbMode *macfb_mode;
    ram_addr_t ram_size = machine->ram_size;
    const char *kernel_filename = machine->kernel_filename;
    const char *initrd_filename = machine->initrd_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *bios_name = machine->firmware ?: MACROM_FILENAME;
    hwaddr parameters_base;
    CPUState *cs;
    DeviceState *dev;
    DeviceState *via_dev;
    DeviceState *escc_orgate;
    SysBusESPState *sysbus_esp;
    ESPState *esp;
    SysBusDevice *sysbus;
    BusState *adb_bus;
    NubusBus *nubus;
    DriveInfo *dinfo;

    linux_boot = (kernel_filename != NULL);

    if (ram_size > 1 * GiB) {
        error_report("Too much memory for this machine: %" PRId64 " MiB, "
                     "maximum 1024 MiB", ram_size / MiB);
        exit(1);
    }

    /* init CPUs */
    m->cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(main_cpu_reset, m->cpu);

    /* RAM */
    memory_region_init_io(&m->ramio, NULL, &ramio_ops, &m->ramio,
                          "ram", RAM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x0, &m->ramio);

    memory_region_add_subregion(&m->ramio, 0, machine->ram);

    /*
     * Create container for all IO devices
     */
    memory_region_init(&m->macio, NULL, "mac-io", IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE, &m->macio);

    /*
     * Memory from IO_BASE to IO_BASE + IO_SLICE is repeated
     * from IO_BASE + IO_SLICE to IO_BASE + IO_SIZE
     */
    memory_region_init_io(&m->macio_alias, NULL, &macio_alias_ops, &m->macio,
                          "mac-io.alias", IO_SIZE - IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE + IO_SLICE,
                                &m->macio_alias);

    memory_region_init_io(&m->machine_id, NULL, &machine_id_ops, NULL,
                          "Machine ID", 4);
    memory_region_add_subregion(get_system_memory(), 0x5ffffffc,
                                &m->machine_id);

    /* djMEMC memory and interrupt controller */

    m->djmemc = qdev_new(TYPE_DJMEMC);
    object_property_set_link(OBJECT(m->djmemc), "cpu", OBJECT(m->cpu),
                             &error_abort);
    sysbus = SYS_BUS_DEVICE(m->djmemc);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, DJMEMC_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));

    /* IOSB subsystem */

    dev = qdev_new(TYPE_IOSB);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, IOSB_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));

    /* VIA */

    via_dev = qdev_new(TYPE_MAC_VIA);
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive(via_dev, "drive", blk_by_legacy_dinfo(dinfo));
    }
    sysbus = SYS_BUS_DEVICE(via_dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, VIA_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    qdev_connect_gpio_out_named(DEVICE(sysbus), "irq", 0,
                                qdev_get_gpio_in(m->djmemc, 0));
    qdev_connect_gpio_out_named(DEVICE(sysbus), "irq", 1,
                                qdev_get_gpio_in(m->djmemc, 1));


    adb_bus = qdev_get_child_bus(via_dev, "adb.0");
    dev = qdev_new(TYPE_ADB_KEYBOARD);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    dev = qdev_new(TYPE_ADB_MOUSE);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);

    /* MACSONIC */

    if (nb_nics > 1) {
        error_report("q800 can only have one ethernet interface");
        exit(1);
    }

    qemu_check_nic_model(&nd_table[0], "dp83932");

    /*
     * MacSonic driver needs an Apple MAC address
     * Valid prefix are:
     * 00:05:02 Apple
     * 00:80:19 Dayna Communications, Inc.
     * 00:A0:40 Apple
     * 08:00:07 Apple
     * (Q800 use the last one)
     */
    nd_table[0].macaddr.a[0] = 0x08;
    nd_table[0].macaddr.a[1] = 0x00;
    nd_table[0].macaddr.a[2] = 0x07;

    dev = qdev_new("dp8393x");
    qdev_set_nic_properties(dev, &nd_table[0]);
    qdev_prop_set_uint8(dev, "it_shift", 2);
    qdev_prop_set_bit(dev, "big_endian", true);
    object_property_set_link(OBJECT(dev), "dma_mr",
                             OBJECT(get_system_memory()), &error_abort);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SONIC_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_add_subregion(&m->macio, SONIC_PROM_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 1));
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(m->djmemc, 2));

    /* SCC */

    dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", MAC_CLOCK);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_bit(dev, "bit_swap", true);
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    qdev_prop_set_uint32(dev, "chnBtype", 0);
    qdev_prop_set_uint32(dev, "chnAtype", 0);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);

    /* Logically OR both its IRQs together */
    escc_orgate = DEVICE(object_new(TYPE_OR_IRQ));
    object_property_set_int(OBJECT(escc_orgate), "num-lines", 2, &error_fatal);
    qdev_realize_and_unref(escc_orgate, NULL, &error_fatal);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(escc_orgate, 0));
    sysbus_connect_irq(sysbus, 1, qdev_get_gpio_in(escc_orgate, 1));
    qdev_connect_gpio_out(DEVICE(escc_orgate), 0, qdev_get_gpio_in(m->djmemc, 3));
    memory_region_add_subregion(&m->macio, SCC_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));


    /* SCSI */

    dev = qdev_new(TYPE_SYSBUS_ESP);
    sysbus_esp = SYSBUS_ESP(dev);
    esp = &sysbus_esp->esp;
    esp->dma_memory_read = NULL;
    esp->dma_memory_write = NULL;
    esp->dma_opaque = NULL;
    sysbus_esp->it_shift = 4;
    esp->dma_enabled = 1;

    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in_named(via_dev,
                                                         "via2-irq",
                                                         VIA2_IRQ_SCSI_BIT));
    sysbus_connect_irq(sysbus, 1,
                       qdev_get_gpio_in_named(via_dev, "via2-irq",
                                              VIA2_IRQ_SCSI_DATA_BIT));
    memory_region_add_subregion(&m->macio, ESP_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_add_subregion(&m->macio, ESP_PDMA - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 1));

    scsi_bus_legacy_handle_cmdline(&esp->bus);

    /* Apple Sound Chip */

    dev = qdev_new(TYPE_ASC);
    qdev_prop_set_uint8(dev, "asctype", ASC_TYPE_ASC);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, ASC_BASE - IO_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in_named(via_dev, "via2-irq",
                                                         VIA2_IRQ_ASC_BIT));

    /* SWIM floppy controller */

    dev = qdev_new(TYPE_SWIM);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(&m->macio, SWIM_BASE - IO_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));

    /* NuBus */

    dev = qdev_new(TYPE_MAC_NUBUS_BRIDGE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, NUBUS_SUPER_SLOT_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, NUBUS_SLOT_BASE);

    nubus = MAC_NUBUS_BRIDGE(dev)->bus;

    /* framebuffer in nubus slot #9 */

    dev = qdev_new(TYPE_NUBUS_MACFB);
    qdev_prop_set_uint32(dev, "width", graphic_width);
    qdev_prop_set_uint32(dev, "height", graphic_height);
    qdev_prop_set_uint8(dev, "depth", graphic_depth);
    if (graphic_width == 1152 && graphic_height == 870) {
        qdev_prop_set_uint8(dev, "display", MACFB_DISPLAY_APPLE_21_COLOR);
    } else {
        qdev_prop_set_uint8(dev, "display", MACFB_DISPLAY_VGA);
    }
    qdev_realize_and_unref(dev, BUS(nubus), &error_fatal);
    qdev_connect_gpio_out(dev, 0,
                          qdev_get_gpio_in_named(via_dev, "via2-slot-irq",
                                                 VIA2_SLOT_IRQ_INTVIDEO));
    macfb_mode = (NUBUS_MACFB(dev)->macfb).mode;

    cs = CPU(m->cpu);
    if (linux_boot) {
        uint64_t high;
        kernel_size = load_elf(kernel_filename, NULL, NULL, NULL,
                               &elf_entry, NULL, &high, NULL, 1,
                               EM_68K, 0, 0);
        if (kernel_size < 0) {
            error_report("could not load kernel '%s'", kernel_filename);
            exit(1);
        }
        stl_phys(cs->as, 4, elf_entry); /* reset initial PC */
        parameters_base = (high + 1) & ~1;

        BOOTINFO1(cs->as, parameters_base, BI_MACHTYPE, MACH_MAC);
        BOOTINFO1(cs->as, parameters_base, BI_FPUTYPE, FPU_68040);
        BOOTINFO1(cs->as, parameters_base, BI_MMUTYPE, MMU_68040);
        BOOTINFO1(cs->as, parameters_base, BI_CPUTYPE, CPU_68040);
        BOOTINFO1(cs->as, parameters_base, BI_MAC_CPUID, CPUB_68040);
        BOOTINFO1(cs->as, parameters_base, BI_MAC_MODEL, MAC_MODEL_Q800);
        BOOTINFO1(cs->as, parameters_base,
                  BI_MAC_MEMSIZE, ram_size >> 20); /* in MB */
        BOOTINFO2(cs->as, parameters_base, BI_MEMCHUNK, 0, ram_size);
        BOOTINFO1(cs->as, parameters_base,
                  BI_MAC_VADDR, VIDEO_BASE + macfb_mode->offset);
        BOOTINFO1(cs->as, parameters_base, BI_MAC_VDEPTH, graphic_depth);
        BOOTINFO1(cs->as, parameters_base, BI_MAC_VDIM,
                  (graphic_height << 16) | graphic_width);
        BOOTINFO1(cs->as, parameters_base, BI_MAC_VROW, macfb_mode->stride);
        BOOTINFO1(cs->as, parameters_base, BI_MAC_SCCBASE, SCC_BASE);

        memory_region_init_ram_ptr(&m->rom, NULL, "m68k_fake_mac.rom",
                                   sizeof(fake_mac_rom), fake_mac_rom);
        memory_region_set_readonly(&m->rom, true);
        memory_region_add_subregion(get_system_memory(), MACROM_ADDR, &m->rom);

        if (kernel_cmdline) {
            BOOTINFOSTR(cs->as, parameters_base, BI_COMMAND_LINE,
                        kernel_cmdline);
        }

        /* load initrd */
        if (initrd_filename) {
            initrd_size = get_image_size(initrd_filename);
            if (initrd_size < 0) {
                error_report("could not load initial ram disk '%s'",
                             initrd_filename);
                exit(1);
            }

            initrd_base = (ram_size - initrd_size) & TARGET_PAGE_MASK;
            load_image_targphys(initrd_filename, initrd_base,
                                ram_size - initrd_base);
            BOOTINFO2(cs->as, parameters_base, BI_RAMDISK, initrd_base,
                      initrd_size);
        } else {
            initrd_base = 0;
            initrd_size = 0;
        }
        BOOTINFO0(cs->as, parameters_base, BI_LAST);
    } else {
        uint8_t *ptr;
        /* allocate and load BIOS */
        memory_region_init_rom(&m->rom, NULL, "m68k_mac.rom", MACROM_SIZE,
                               &error_abort);

        /* WIP: load Quadra800.elf file for symbols with executing with -d in_asm */
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, "Quadra800ROM.elf");
        if (filename) {
            load_elf(filename, NULL, NULL, NULL, &elf_entry, NULL, NULL, NULL,
                     1, EM_68K, 0, 0);
            fprintf(stderr, "Loaded Quadra800ROM.elf symbols!\n");
        }
        g_free(filename);

        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
        memory_region_add_subregion(get_system_memory(), MACROM_ADDR, &m->rom);

        /* Load MacROM binary */
        if (filename) {
            bios_size = load_image_targphys(filename, MACROM_ADDR, MACROM_SIZE);
            g_free(filename);
        } else {
            bios_size = -1;
        }

        /* Remove qtest_enabled() check once firmware files are in the tree */
        if (!qtest_enabled()) {
            if (bios_size < 0 || bios_size > MACROM_SIZE) {
                error_report("could not load MacROM '%s'", bios_name);
                exit(1);
            }

            ptr = rom_ptr(MACROM_ADDR, MACROM_SIZE);
            stl_phys(cs->as, 0, ldl_p(ptr));    /* reset initial SP */
            stl_phys(cs->as, 4,
                     MACROM_ADDR + ldl_p(ptr + 4)); /* reset initial PC */
        }
    }
}

static void q800_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Macintosh Quadra 800";
    mc->init = q800_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_id = "m68k_mac.ram";
}

static const TypeInfo q800_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("q800"),
    .parent     = TYPE_MACHINE,
    .instance_size = sizeof(Q800MachineState),
    .class_init = q800_machine_class_init,
};

static void q800_machine_register_types(void)
{
    type_register_static(&q800_machine_typeinfo);
}

type_init(q800_machine_register_types)
