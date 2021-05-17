/*
 *  Copyright (c) 2012-2018 Laurent Vivier <laurent@vivier.eu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef HW_AUDIO_ASC_H
#define HW_AUDIO_ASC_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "audio/audio.h"

enum {
    ASC_TYPE_ASC    = 0,  /* original discrete Apple Sound Chip */
    ASC_TYPE_EASC   = 1,  /* discrete Enhanced Apple Sound Chip */
    ASC_TYPE_V8     = 2,  /* ASC included in the V8 ASIC (LC/LCII) */
    ASC_TYPE_EAGLE  = 3,  /* ASC included in the Eagle ASIC (Classic II) */
    ASC_TYPE_SPICE  = 4,  /* ASC included in the Spice ASIC (Color Classic) */
    ASC_TYPE_SONORA = 5,  /* ASC included in the Sonora ASIC (LCIII) */
    ASC_TYPE_VASP   = 6,  /* ASC included in the VASP ASIC  (IIvx/IIvi) */
    ASC_TYPE_ARDBEG = 7,  /* ASC included in the Ardbeg ASIC (LC520) */
};

struct ASCState {
    SysBusDevice parent_obj;

    MemoryRegion asc;
    MemoryRegion mem_fifo;
    MemoryRegion mem_regs;
    MemoryRegion mem_extregs;

    QEMUSoundCard card;
    SWVoiceOut *voice;
    int8_t *mixbuf;
    int left, pos, samples, shift;

    qemu_irq irq;

    uint8_t type;
    int a_wptr, a_rptr, a_cnt;
    int b_wptr, b_rptr, b_cnt;

    int xa_acnt;
    uint8_t xa_aval;
    uint8_t xa_aflags;
    int16_t xa_alast[2];

    int xa_bcnt;
    uint8_t xa_bval;
    uint8_t xa_bflags;
    int16_t xa_blast[2];

    uint8_t *fifo;

    uint8_t regs[64];
    uint8_t extregs[64];
};

#define TYPE_ASC "apple-sound-chip"
OBJECT_DECLARE_SIMPLE_TYPE(ASCState, ASC)

#endif
