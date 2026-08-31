/*
 * ARM PrimeCell PL192 interrupt controller as integrated in S5L8900.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/intc/s5l8900_vic.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "system/tcg.h"

#define S5L8900_VIC_NUM_IRQS 32
#define S5L8900_VIC_NUM_PRIORITIES 16

#define VIC_IRQ_STATUS        0x000
#define VIC_FIQ_STATUS        0x004
#define VIC_RAW_INTR          0x008
#define VIC_INT_SELECT        0x00c
#define VIC_INT_ENABLE        0x010
#define VIC_INT_ENABLE_CLEAR  0x014
#define VIC_SOFT_INT          0x018
#define VIC_SOFT_INT_CLEAR    0x01c
#define VIC_PROTECTION        0x020
#define VIC_SW_PRIORITY_MASK  0x024
#define VIC_DAISY_PRIORITY    0x028
#define VIC_VECTOR_ADDRESS_0  0x100
#define VIC_VECTOR_PRIORITY_0 0x200
#define VIC_ADDRESS           0xf00
#define VIC_PERIPH_ID_0       0xfe0

#define VIC_DAISY_IRQ S5L8900_VIC_NUM_IRQS

struct S5L8900VICState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QemuMutex lock;
    QemuMutex *chain_lock;
    bool chain_lock_required;
    uint32_t level;
    uint32_t soft_level;
    uint32_t interrupt_enable;
    uint32_t fiq_select;
    uint32_t vector_address[S5L8900_VIC_NUM_IRQS];
    uint8_t vector_priority[S5L8900_VIC_NUM_IRQS];
    uint16_t software_priority_mask;
    uint8_t daisy_priority;
    uint8_t protection;
    uint8_t current_priority;
    uint8_t priority_stack[S5L8900_VIC_NUM_PRIORITIES];
    uint8_t stack_depth;
    uint32_t last_address;
    S5L8900VICState *upstream;
    S5L8900VICState *daisy_source;
    uint32_t daisy_address;
    bool daisy_irq;
    bool daisy_fiq;
    qemu_irq irq;
    qemu_irq fiq;
};

static void s5l8900_vic_lock(S5L8900VICState *s)
{
    if (s->chain_lock_required) {
        qemu_mutex_lock(s->chain_lock);
    }
}

static void s5l8900_vic_unlock(S5L8900VICState *s)
{
    if (s->chain_lock_required) {
        qemu_mutex_unlock(s->chain_lock);
    }
}

static uint32_t s5l8900_vic_raw_status(S5L8900VICState *s)
{
    return s->level | s->soft_level;
}

static uint32_t s5l8900_vic_irq_status(S5L8900VICState *s)
{
    return s5l8900_vic_raw_status(s) & s->interrupt_enable & ~s->fiq_select;
}

static uint32_t s5l8900_vic_fiq_status(S5L8900VICState *s)
{
    return s5l8900_vic_raw_status(s) & s->interrupt_enable & s->fiq_select;
}

static int s5l8900_vic_pending_local_irq(S5L8900VICState *s)
{
    uint32_t status = s5l8900_vic_irq_status(s);
    int selected = -1;

    for (int irq = 0; irq < S5L8900_VIC_NUM_IRQS; irq++) {
        unsigned priority = s->vector_priority[irq];

        if (!(status & BIT(irq)) ||
            !(s->software_priority_mask & BIT(priority)) ||
            priority >= s->current_priority) {
            continue;
        }
        if (selected < 0 ||
            priority < s->vector_priority[selected]) {
            selected = irq;
        }
    }
    return selected;
}

static int s5l8900_vic_pending_irq(S5L8900VICState *s)
{
    int selected = s5l8900_vic_pending_local_irq(s);

    if (s->daisy_irq && s->daisy_source &&
        (s->software_priority_mask & BIT(s->daisy_priority)) &&
        s->daisy_priority < s->current_priority &&
        (selected < 0 ||
         s->daisy_priority < s->vector_priority[selected])) {
        return VIC_DAISY_IRQ;
    }
    return selected;
}

static uint32_t s5l8900_vic_pending_address(S5L8900VICState *s,
                                             int selected)
{
    return selected == VIC_DAISY_IRQ ? s->daisy_address :
                                       s->vector_address[selected];
}

static void s5l8900_vic_update(S5L8900VICState *s)
{
    int selected = s5l8900_vic_pending_irq(s);
    bool fiq = s5l8900_vic_fiq_status(s) != 0 || s->daisy_fiq;

    if (s->upstream) {
        s->upstream->daisy_irq = selected >= 0;
        s->upstream->daisy_fiq = fiq;
        if (selected >= 0) {
            s->upstream->daisy_address =
                s5l8900_vic_pending_address(s, selected);
        }
        s5l8900_vic_update(s->upstream);
    } else {
        qemu_set_irq(s->irq, selected >= 0);
        qemu_set_irq(s->fiq, fiq);
    }
}

static void s5l8900_vic_set_irq(void *opaque, int irq, int level)
{
    S5L8900VICState *s = opaque;

    s5l8900_vic_lock(s);
    if (level) {
        s->level |= BIT(irq);
    } else {
        s->level &= ~BIT(irq);
    }
    s5l8900_vic_update(s);
    s5l8900_vic_unlock(s);
}

static uint32_t s5l8900_vic_acknowledge(S5L8900VICState *s)
{
    int irq = s5l8900_vic_pending_irq(s);
    uint32_t address;

    if (irq < 0) {
        return s->last_address;
    }
    if (s->stack_depth < ARRAY_SIZE(s->priority_stack)) {
        s->priority_stack[s->stack_depth++] = s->current_priority;
    }
    s->current_priority = irq == VIC_DAISY_IRQ ? s->daisy_priority :
                                                 s->vector_priority[irq];
    address = irq == VIC_DAISY_IRQ ?
              s5l8900_vic_acknowledge(s->daisy_source) :
              s->vector_address[irq];
    s->last_address = address;
    s5l8900_vic_update(s);
    return address;
}

static void s5l8900_vic_complete(S5L8900VICState *s)
{
    if (s->stack_depth) {
        s->current_priority = s->priority_stack[--s->stack_depth];
    } else {
        s->current_priority = S5L8900_VIC_NUM_PRIORITIES;
    }
    s5l8900_vic_update(s);
}

static uint64_t s5l8900_vic_read_locked(S5L8900VICState *s, hwaddr offset)
{
    static const uint8_t id[] = {
        0x92, 0x11, 0x04, 0x00, 0x0d, 0xf0, 0x05, 0xb1,
    };
    if (offset >= VIC_PERIPH_ID_0 && offset < 0x1000) {
        return id[(offset - VIC_PERIPH_ID_0) >> 2];
    }
    if (offset >= VIC_VECTOR_ADDRESS_0 && offset < 0x180) {
        return s->vector_address[(offset - VIC_VECTOR_ADDRESS_0) >> 2];
    }
    if (offset >= VIC_VECTOR_PRIORITY_0 && offset < 0x280) {
        return s->vector_priority[(offset - VIC_VECTOR_PRIORITY_0) >> 2];
    }

    switch (offset) {
    case VIC_IRQ_STATUS:
        return s5l8900_vic_irq_status(s);
    case VIC_FIQ_STATUS:
        return s5l8900_vic_fiq_status(s);
    case VIC_RAW_INTR:
        return s5l8900_vic_raw_status(s);
    case VIC_INT_SELECT:
        return s->fiq_select;
    case VIC_INT_ENABLE:
    case VIC_INT_ENABLE_CLEAR:
        return s->interrupt_enable;
    case VIC_SOFT_INT:
    case VIC_SOFT_INT_CLEAR:
        return s->soft_level;
    case VIC_PROTECTION:
        return s->protection;
    case VIC_SW_PRIORITY_MASK:
        return s->software_priority_mask;
    case VIC_DAISY_PRIORITY:
        return s->daisy_priority;
    case VIC_ADDRESS:
        return s5l8900_vic_acknowledge(s);
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900-vic: invalid read at 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static uint64_t s5l8900_vic_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8900VICState *s = opaque;
    uint64_t value;

    s5l8900_vic_lock(s);
    value = s5l8900_vic_read_locked(s, offset);
    s5l8900_vic_unlock(s);
    return value;
}

static void s5l8900_vic_write_locked(S5L8900VICState *s, hwaddr offset,
                                     uint64_t value)
{
    if (offset >= VIC_VECTOR_ADDRESS_0 && offset < 0x180) {
        s->vector_address[(offset - VIC_VECTOR_ADDRESS_0) >> 2] = value;
        s5l8900_vic_update(s);
        return;
    }
    if (offset >= VIC_VECTOR_PRIORITY_0 && offset < 0x280) {
        s->vector_priority[(offset - VIC_VECTOR_PRIORITY_0) >> 2] = value & 0xf;
        s5l8900_vic_update(s);
        return;
    }

    switch (offset) {
    case VIC_INT_SELECT:
        s->fiq_select = value;
        break;
    case VIC_INT_ENABLE:
        s->interrupt_enable |= value;
        break;
    case VIC_INT_ENABLE_CLEAR:
        s->interrupt_enable &= ~value;
        break;
    case VIC_SOFT_INT:
        s->soft_level |= value;
        break;
    case VIC_SOFT_INT_CLEAR:
        s->soft_level &= ~value;
        break;
    case VIC_PROTECTION:
        s->protection = value & 1;
        break;
    case VIC_SW_PRIORITY_MASK:
        s->software_priority_mask = value;
        break;
    case VIC_DAISY_PRIORITY:
        s->daisy_priority = value & 0xf;
        break;
    case VIC_ADDRESS:
        s5l8900_vic_complete(s);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900-vic: invalid write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", value, offset);
        return;
    }
    s5l8900_vic_update(s);
}

static void s5l8900_vic_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    S5L8900VICState *s = opaque;

    s5l8900_vic_lock(s);
    s5l8900_vic_write_locked(s, offset, value);
    s5l8900_vic_unlock(s);
}

static const MemoryRegionOps s5l8900_vic_ops = {
    .read = s5l8900_vic_read,
    .write = s5l8900_vic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_vic_reset(DeviceState *dev)
{
    S5L8900VICState *s = S5L8900_VIC(dev);

    s5l8900_vic_lock(s);
    s->level = 0;
    s->soft_level = 0;
    s->interrupt_enable = 0;
    s->fiq_select = 0;
    memset(s->vector_address, 0, sizeof(s->vector_address));
    memset(s->vector_priority, 0xf, sizeof(s->vector_priority));
    s->software_priority_mask = 0xffff;
    s->daisy_priority = 0xf;
    s->protection = 0;
    s->current_priority = S5L8900_VIC_NUM_PRIORITIES;
    memset(s->priority_stack, 0, sizeof(s->priority_stack));
    s->stack_depth = 0;
    s->last_address = 0;
    s->daisy_address = 0;
    s->daisy_irq = false;
    s->daisy_fiq = false;
    s5l8900_vic_update(s);
    s5l8900_vic_unlock(s);
}

static int s5l8900_vic_pre_load(void *opaque)
{
    S5L8900VICState *s = opaque;

    s->daisy_priority = 0xf;
    s->daisy_address = 0;
    s->daisy_irq = false;
    s->daisy_fiq = false;
    return 0;
}

static void s5l8900_vic_init(Object *obj)
{
    S5L8900VICState *s = S5L8900_VIC(obj);
    DeviceState *dev = DEVICE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    qemu_mutex_init(&s->lock);
    s->chain_lock = &s->lock;
    memory_region_init_io(&s->iomem, obj, &s5l8900_vic_ops, s,
                          TYPE_S5L8900_VIC, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
    qdev_init_gpio_in(dev, s5l8900_vic_set_irq, S5L8900_VIC_NUM_IRQS);
}

static void s5l8900_vic_finalize(Object *obj)
{
    S5L8900VICState *s = S5L8900_VIC(obj);

    qemu_mutex_destroy(&s->lock);
}

static void s5l8900_vic_realize(DeviceState *dev, Error **errp)
{
    S5L8900VICState *s = S5L8900_VIC(dev);

    s->chain_lock_required = tcg_enabled() && qemu_tcg_mttcg_enabled();
    if (!s->upstream) {
        return;
    }
    if (s->upstream == s) {
        error_setg(errp, TYPE_S5L8900_VIC " cannot daisy-chain to itself");
        return;
    }
    if (s->upstream->daisy_source &&
        s->upstream->daisy_source != s) {
        error_setg(errp, TYPE_S5L8900_VIC
                   " upstream already has a daisy-chain source");
        return;
    }
    s->upstream->daisy_source = s;
    s->chain_lock = s->upstream->chain_lock;
    s->chain_lock_required = s->upstream->chain_lock_required;
}

static int s5l8900_vic_post_load(void *opaque, int version_id)
{
    S5L8900VICState *s = opaque;

    s5l8900_vic_update(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_vic = {
    .name = TYPE_S5L8900_VIC,
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_load = s5l8900_vic_pre_load,
    .post_load = s5l8900_vic_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(level, S5L8900VICState),
        VMSTATE_UINT32(soft_level, S5L8900VICState),
        VMSTATE_UINT32(interrupt_enable, S5L8900VICState),
        VMSTATE_UINT32(fiq_select, S5L8900VICState),
        VMSTATE_UINT32_ARRAY(vector_address, S5L8900VICState,
                             S5L8900_VIC_NUM_IRQS),
        VMSTATE_UINT8_ARRAY(vector_priority, S5L8900VICState,
                            S5L8900_VIC_NUM_IRQS),
        VMSTATE_UINT16(software_priority_mask, S5L8900VICState),
        VMSTATE_UINT8_V(daisy_priority, S5L8900VICState, 2),
        VMSTATE_UINT8(protection, S5L8900VICState),
        VMSTATE_UINT8(current_priority, S5L8900VICState),
        VMSTATE_UINT8_ARRAY(priority_stack, S5L8900VICState,
                            S5L8900_VIC_NUM_PRIORITIES),
        VMSTATE_UINT8(stack_depth, S5L8900VICState),
        VMSTATE_UINT32(last_address, S5L8900VICState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s5l8900_vic_properties[] = {
    DEFINE_PROP_LINK("upstream", S5L8900VICState, upstream,
                     TYPE_S5L8900_VIC, S5L8900VICState *),
};

static void s5l8900_vic_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = s5l8900_vic_realize;
    device_class_set_legacy_reset(dc, s5l8900_vic_reset);
    device_class_set_props(dc, s5l8900_vic_properties);
    dc->vmsd = &vmstate_s5l8900_vic;
}

static const TypeInfo s5l8900_vic_info = {
    .name = TYPE_S5L8900_VIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900VICState),
    .instance_init = s5l8900_vic_init,
    .instance_finalize = s5l8900_vic_finalize,
    .class_init = s5l8900_vic_class_init,
};

static void s5l8900_vic_register_types(void)
{
    type_register_static(&s5l8900_vic_info);
}

type_init(s5l8900_vic_register_types)
