/*
 * QEMU Macintosh Nubus
 *
 * Copyright (c) 2013-2018 Laurent Vivier <laurent@vivier.eu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "hw/nubus/nubus.h"
#include "qapi/error.h"


static void nubus_device_realize(DeviceState *dev, Error **errp)
{
    NubusBus *nubus = NUBUS_BUS(qdev_get_parent_bus(dev));
    NubusDevice *nd = NUBUS_DEVICE(dev);
    char *name;
    hwaddr slot_offset;
    uint16_t s;

    if (nd->slot == -1) {
        /* No slot specified, find first available free slot */
        s = ctz32(nubus->slot_available_mask);
        if (s) {
            nd->slot = s;
        } else {
            error_setg(errp, "Cannot register nubus card, no free slot "
                             "available");
            return;
        }
    } else {
        /* Slot specified, make sure the slot is available */
        if (nd->slot < NUBUS_FIRST_SLOT || nd->slot > NUBUS_LAST_SLOT) {
            error_setg(errp, "Cannot register nubus card, slot must be "
                             "between %d and %d", NUBUS_FIRST_SLOT,
                             NUBUS_LAST_SLOT);
            return;
        }

        if (!(nubus->slot_available_mask & (1UL << nd->slot))) {
            error_setg(errp, "Cannot register nubus card, slot %d is "
                             "unavailable or already occupied", nd->slot);
            return;
        }
    }

    nubus->slot_available_mask &= ~(1UL << nd->slot);

    /* Super */
    slot_offset = (nd->slot - 6) * NUBUS_SUPER_SLOT_SIZE;

    name = g_strdup_printf("nubus-super-slot-%x", nd->slot);
    memory_region_init(&nd->super_slot_mem, OBJECT(dev), name,
                        NUBUS_SUPER_SLOT_SIZE);
    memory_region_add_subregion(&nubus->super_slot_io, slot_offset,
                                &nd->super_slot_mem);
    g_free(name);

    /* Normal */
    slot_offset = nd->slot * NUBUS_SLOT_SIZE;

    name = g_strdup_printf("nubus-slot-%x", nd->slot);
    memory_region_init(&nd->slot_mem, OBJECT(dev), name, NUBUS_SLOT_SIZE);
    memory_region_add_subregion(&nubus->slot_io, slot_offset,
                                &nd->slot_mem);
    g_free(name);
}

static Property nubus_device_properties[] = {
    DEFINE_PROP_INT32("slot", NubusDevice, slot, -1),
    DEFINE_PROP_END_OF_LIST()
};

static void nubus_device_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = nubus_device_realize;
    dc->bus_type = TYPE_NUBUS_BUS;
    device_class_set_props(dc, nubus_device_properties);
}

static const TypeInfo nubus_device_type_info = {
    .name = TYPE_NUBUS_DEVICE,
    .parent = TYPE_DEVICE,
    .abstract = true,
    .instance_size = sizeof(NubusDevice),
    .class_init = nubus_device_class_init,
};

static void nubus_register_types(void)
{
    type_register_static(&nubus_device_type_info);
}

type_init(nubus_register_types)
