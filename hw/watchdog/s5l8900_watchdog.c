/*
 * Apple S5L8900 watchdog
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/watchdog/s5l8900_watchdog.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/watchdog.h"

#define WDT_CONTROL          0x00
#define WDT_COUNT            0x04

#define WDT_DISABLE_KEY      0xa5
#define WDT_CLEAR_KEY        0xa00
#define WDT_CLOCK_SHIFT      12
#define WDT_CLOCK_LENGTH     3
#define WDT_INTERRUPT_ENABLE BIT(15)
#define WDT_PRESCALE_SHIFT   16
#define WDT_PRESCALE_LENGTH  4
#define WDT_ENABLE           BIT(20)
#define WDT_COUNT_MAX        2048

/* Derived from the N82 reset clock tuple consumed by OpeniBoot. */
#define WDT_PERIPHERAL_HZ    51500000ULL

static uint64_t s5l8900_watchdog_period_ns(uint32_t control)
{
    uint64_t prescale = extract32(control, WDT_PRESCALE_SHIFT,
                                  WDT_PRESCALE_LENGTH) + 1;
    uint64_t divider = 1ULL <<
        (extract32(control, WDT_CLOCK_SHIFT, WDT_CLOCK_LENGTH) + 7);
    uint64_t cycles = WDT_COUNT_MAX * prescale * divider;

    return muldiv64(cycles, NANOSECONDS_PER_SECOND,
                    4 * WDT_PERIPHERAL_HZ);
}

static void s5l8900_watchdog_update_irq(S5L8900WatchdogState *s)
{
    qemu_set_irq(s->irq, s->irq_pending);
}

static void s5l8900_watchdog_expired(void *opaque)
{
    S5L8900WatchdogState *s = opaque;

    if (s->control & WDT_INTERRUPT_ENABLE) {
        s->irq_pending = true;
        s5l8900_watchdog_update_irq(s);
    } else {
        watchdog_perform_action();
    }
}

static void s5l8900_watchdog_reload(S5L8900WatchdogState *s)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              s5l8900_watchdog_period_ns(s->control));
}

static uint64_t s5l8900_watchdog_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    S5L8900WatchdogState *s = opaque;

    switch (offset) {
    case WDT_CONTROL:
        return s->control;
    case WDT_COUNT:
        if (timer_pending(s->timer)) {
            int64_t remaining = timer_expire_time_ns(s->timer) -
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            uint64_t period = s5l8900_watchdog_period_ns(s->control);

            return MIN(muldiv64(MAX(remaining, 0), WDT_COUNT_MAX, period),
                       WDT_COUNT_MAX - 1);
        }
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid read at 0x%" HWADDR_PRIx "\n",
                      TYPE_S5L8900_WATCHDOG, offset);
        return 0;
    }
}

static void s5l8900_watchdog_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    S5L8900WatchdogState *s = opaque;

    switch (offset) {
    case WDT_CONTROL:
        s->control = value;
        if ((value & 0xff) == WDT_DISABLE_KEY || !(value & WDT_ENABLE)) {
            timer_del(s->timer);
            s->irq_pending = false;
        } else if ((value & 0xf00) == WDT_CLEAR_KEY ||
                   !timer_pending(s->timer)) {
            s->irq_pending = false;
            s5l8900_watchdog_reload(s);
        }
        s5l8900_watchdog_update_irq(s);
        break;
    case WDT_COUNT:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: count register is read-only\n",
                      TYPE_S5L8900_WATCHDOG);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid write at 0x%" HWADDR_PRIx "\n",
                      TYPE_S5L8900_WATCHDOG, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_watchdog_ops = {
    .read = s5l8900_watchdog_read,
    .write = s5l8900_watchdog_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static int s5l8900_watchdog_post_load(void *opaque, int version_id)
{
    s5l8900_watchdog_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_watchdog = {
    .name = TYPE_S5L8900_WATCHDOG,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_watchdog_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, S5L8900WatchdogState),
        VMSTATE_UINT32(control, S5L8900WatchdogState),
        VMSTATE_BOOL(irq_pending, S5L8900WatchdogState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_watchdog_reset(DeviceState *dev)
{
    S5L8900WatchdogState *s = S5L8900_WATCHDOG(dev);

    timer_del(s->timer);
    s->control = 0;
    s->irq_pending = false;
    s5l8900_watchdog_update_irq(s);
}

static void s5l8900_watchdog_init(Object *obj)
{
    S5L8900WatchdogState *s = S5L8900_WATCHDOG(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_watchdog_ops, s,
                          TYPE_S5L8900_WATCHDOG, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, s5l8900_watchdog_expired, s);
}

static void s5l8900_watchdog_finalize(Object *obj)
{
    S5L8900WatchdogState *s = S5L8900_WATCHDOG(obj);

    timer_free(s->timer);
}

static void s5l8900_watchdog_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8900_watchdog_reset);
    dc->vmsd = &vmstate_s5l8900_watchdog;
}

static const TypeInfo s5l8900_watchdog_info = {
    .name = TYPE_S5L8900_WATCHDOG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900WatchdogState),
    .instance_init = s5l8900_watchdog_init,
    .instance_finalize = s5l8900_watchdog_finalize,
    .class_init = s5l8900_watchdog_class_init,
};

static void s5l8900_watchdog_register_types(void)
{
    type_register_static(&s5l8900_watchdog_info);
}

type_init(s5l8900_watchdog_register_types)
