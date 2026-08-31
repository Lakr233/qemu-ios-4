/*
 * Apple S5L8900 timer controller
 *
 * The register layout and the 12 MHz timebase are consumed by OpeniBoot's
 * S5L8900 platform code.  Timers 4 through 6 publish their three-bit status
 * groups in IRQSTAT; the event timer uses the low flag in timer 4's group.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/timer/s5l8900_timer.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "trace.h"

#define S5L8900_TIMER_MMIO_SIZE       0x10004
#define S5L8900_RTC_FREQUENCY         12000000
#define S5L8900_TIMER_SOURCE_FREQUENCY 24000000

#define TIMER_CONFIG                  0x00
#define TIMER_STATE                   0x04
#define TIMER_COUNT_BUFFER            0x08
#define TIMER_COUNT_BUFFER2           0x0c
#define TIMER_PRESCALER               0x10
#define TIMER_CURRENT_COUNT           0x14

#define TIMER_TICKS_HIGH              0x80
#define TIMER_TICKS_LOW               0x84
#define TIMER_RTC_CONTROL_BASE        0x88
#define TIMER_IRQ_LATCH               0xf8
#define TIMER_IRQ_STATUS              0x10000

#define TIMER_STATE_START             BIT(0)
#define TIMER_STATE_MANUAL_UPDATE     BIT(1)
#define TIMER_STATE_VALID_MASK        (TIMER_STATE_START | \
                                       TIMER_STATE_MANUAL_UPDATE)
#define TIMER_CONFIG_DIVIDER_SHIFT    8
#define TIMER_CONFIG_DIVIDER_MASK     0x7
#define TIMER_CONFIG_ONE_SHOT         BIT(4)
#define TIMER_INT0_ENABLE             BIT(12)

static const hwaddr timer_base[S5L8900_TIMER_COUNT] = {
    0x00, 0x20, 0x40, 0x60, 0xa0, 0xc0, 0xe0,
};

static void s5l8900_timer_update_irq(S5L8900TimerState *s)
{
    trace_s5l8900_timer_irq(s->irq_status, s->irq_status != 0);
    qemu_set_irq(s->irq, s->irq_status != 0);
}

static bool s5l8900_timer_decode(hwaddr offset, unsigned *index,
                                 hwaddr *reg)
{
    for (unsigned i = 0; i < S5L8900_TIMER_COUNT; i++) {
        if (offset >= timer_base[i] &&
            offset <= timer_base[i] + TIMER_CURRENT_COUNT) {
            *index = i;
            *reg = offset - timer_base[i];
            return true;
        }
    }
    return false;
}

static uint64_t s5l8900_rtc_ticks(void)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                    S5L8900_RTC_FREQUENCY, NANOSECONDS_PER_SECOND);
}

static uint64_t s5l8900_timer_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    S5L8900TimerState *s = S5L8900_TIMER(opaque);
    unsigned index;
    hwaddr reg;

    switch (offset) {
    case TIMER_TICKS_HIGH:
        return s5l8900_rtc_ticks() >> 32;
    case TIMER_TICKS_LOW:
        return (uint32_t)s5l8900_rtc_ticks();
    case TIMER_RTC_CONTROL_BASE ... TIMER_RTC_CONTROL_BASE + 0x10:
        return s->rtc_control[(offset - TIMER_RTC_CONTROL_BASE) / 4];
    case TIMER_IRQ_LATCH:
        return s->irq_status;
    case TIMER_IRQ_STATUS:
        return s->irq_status;
    default:
        break;
    }

    if (!s5l8900_timer_decode(offset, &index, &reg)) {
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900-timer: unimplemented read at 0x%" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }

    switch (reg) {
    case TIMER_CONFIG:
        return s->config[index];
    case TIMER_STATE:
        return s->state[index];
    case TIMER_COUNT_BUFFER:
        return s->count_buffer[index];
    case TIMER_COUNT_BUFFER2:
        return s->count_buffer2[index];
    case TIMER_PRESCALER:
        return s->prescaler[index];
    case TIMER_CURRENT_COUNT:
        return ptimer_get_count(s->timer[index]);
    default:
        g_assert_not_reached();
    }
}

static uint32_t s5l8900_timer_frequency(uint32_t config)
{
    switch ((config >> TIMER_CONFIG_DIVIDER_SHIFT) &
            TIMER_CONFIG_DIVIDER_MASK) {
    case 4:
        return S5L8900_TIMER_SOURCE_FREQUENCY;
    case 0:
        return S5L8900_TIMER_SOURCE_FREQUENCY / 2;
    case 1:
        return S5L8900_TIMER_SOURCE_FREQUENCY / 4;
    case 2:
        return S5L8900_TIMER_SOURCE_FREQUENCY / 16;
    case 3:
        return S5L8900_TIMER_SOURCE_FREQUENCY / 64;
    default:
        /* The alternate clock-source encodings are not yet proven. */
        return S5L8900_RTC_FREQUENCY;
    }
}

static void s5l8900_timer_set_clock(S5L8900TimerState *s, unsigned index)
{
    ptimer_set_freq(s->timer[index],
                    s5l8900_timer_frequency(s->config[index]));
}

static void s5l8900_timer_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    S5L8900TimerState *s = S5L8900_TIMER(opaque);
    unsigned index;
    hwaddr reg;

    switch (offset) {
    case TIMER_RTC_CONTROL_BASE ... TIMER_RTC_CONTROL_BASE + 0x10:
        s->rtc_control[(offset - TIMER_RTC_CONTROL_BASE) / 4] =
            (uint32_t)value;
        return;
    case TIMER_IRQ_LATCH:
        s->irq_status &= ~(uint32_t)value;
        s5l8900_timer_update_irq(s);
        return;
    case TIMER_IRQ_STATUS:
        return;
    default:
        break;
    }

    if (!s5l8900_timer_decode(offset, &index, &reg)) {
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900-timer: unimplemented write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", value, offset);
        return;
    }

    switch (reg) {
    case TIMER_CONFIG:
        s->config[index] = (uint32_t)value;
        ptimer_transaction_begin(s->timer[index]);
        s5l8900_timer_set_clock(s, index);
        ptimer_transaction_commit(s->timer[index]);
        break;
    case TIMER_STATE:
        trace_s5l8900_timer_state(index, value,
                                  s->count_buffer[index],
                                  s->config[index]);
        if (value & ~TIMER_STATE_VALID_MASK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s5l8900-timer: invalid state 0x%" PRIx64
                          " for timer %u\n", value, index);
            break;
        }
        ptimer_transaction_begin(s->timer[index]);
        if (value & TIMER_STATE_MANUAL_UPDATE) {
            ptimer_set_limit(s->timer[index], s->count_buffer[index], 1);
        }
        if (value & TIMER_STATE_START) {
            if (s->count_buffer[index]) {
                ptimer_run(s->timer[index],
                           !!(s->config[index] & TIMER_CONFIG_ONE_SHOT));
            } else {
                ptimer_stop(s->timer[index]);
            }
        } else {
            ptimer_stop(s->timer[index]);
        }
        ptimer_transaction_commit(s->timer[index]);
        s->state[index] = (uint32_t)value;
        break;
    case TIMER_COUNT_BUFFER:
        s->count_buffer[index] = (uint32_t)value;
        break;
    case TIMER_COUNT_BUFFER2:
        s->count_buffer2[index] = (uint32_t)value;
        break;
    case TIMER_PRESCALER:
        /* Stored until the clock-source/divider interaction is proven. */
        s->prescaler[index] = (uint32_t)value;
        break;
    case TIMER_CURRENT_COUNT:
        break;
    default:
        g_assert_not_reached();
    }
}

static const MemoryRegionOps s5l8900_timer_ops = {
    .read = s5l8900_timer_read,
    .write = s5l8900_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_timer_tick(void *opaque)
{
    S5L8900TimerContext *context = opaque;
    S5L8900TimerState *s = context->parent;
    unsigned index = context->index;

    trace_s5l8900_timer_tick(index, s->state[index],
                             s->config[index], s->irq_status);
    if (!(s->state[index] & TIMER_STATE_START) || index < 4 ||
        !(s->config[index] & TIMER_INT0_ENABLE)) {
        return;
    }

    s->irq_status |= BIT((S5L8900_TIMER_COUNT - index - 1) * 8);
    s5l8900_timer_update_irq(s);
}

static void s5l8900_timer_reset(DeviceState *dev)
{
    S5L8900TimerState *s = S5L8900_TIMER(dev);

    memset(s->config, 0, sizeof(s->config));
    memset(s->state, 0, sizeof(s->state));
    memset(s->count_buffer, 0, sizeof(s->count_buffer));
    memset(s->count_buffer2, 0, sizeof(s->count_buffer2));
    memset(s->prescaler, 0, sizeof(s->prescaler));
    memset(s->rtc_control, 0, sizeof(s->rtc_control));
    s->irq_status = 0;
    s5l8900_timer_update_irq(s);

    for (unsigned i = 0; i < S5L8900_TIMER_COUNT; i++) {
        ptimer_transaction_begin(s->timer[i]);
        ptimer_stop(s->timer[i]);
        s5l8900_timer_set_clock(s, i);
        ptimer_set_limit(s->timer[i], 0, 1);
        ptimer_transaction_commit(s->timer[i]);
    }
}

static int s5l8900_timer_post_load(void *opaque, int version_id)
{
    S5L8900TimerState *s = S5L8900_TIMER(opaque);

    s5l8900_timer_update_irq(s);
    return 0;
}

static const VMStateDescription s5l8900_timer_vmstate = {
    .name = TYPE_S5L8900_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_timer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(config, S5L8900TimerState,
                             S5L8900_TIMER_COUNT),
        VMSTATE_UINT32_ARRAY(state, S5L8900TimerState,
                             S5L8900_TIMER_COUNT),
        VMSTATE_UINT32_ARRAY(count_buffer, S5L8900TimerState,
                             S5L8900_TIMER_COUNT),
        VMSTATE_UINT32_ARRAY(count_buffer2, S5L8900TimerState,
                             S5L8900_TIMER_COUNT),
        VMSTATE_UINT32_ARRAY(prescaler, S5L8900TimerState,
                             S5L8900_TIMER_COUNT),
        VMSTATE_UINT32_ARRAY(rtc_control, S5L8900TimerState, 5),
        VMSTATE_UINT32(irq_status, S5L8900TimerState),
        VMSTATE_PTIMER_ARRAY(timer, S5L8900TimerState,
                             S5L8900_TIMER_COUNT),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_timer_init(Object *obj)
{
    S5L8900TimerState *s = S5L8900_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_timer_ops, s,
                          TYPE_S5L8900_TIMER, S5L8900_TIMER_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    for (unsigned i = 0; i < S5L8900_TIMER_COUNT; i++) {
        s->context[i].parent = s;
        s->context[i].index = i;
        s->timer[i] = ptimer_init(s5l8900_timer_tick, &s->context[i],
                                  PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD |
                                  PTIMER_POLICY_TRIGGER_ONLY_ON_DECREMENT |
                                  PTIMER_POLICY_NO_IMMEDIATE_RELOAD |
                                  PTIMER_POLICY_NO_COUNTER_ROUND_DOWN);
    }
}

static void s5l8900_timer_finalize(Object *obj)
{
    S5L8900TimerState *s = S5L8900_TIMER(obj);

    for (unsigned i = 0; i < S5L8900_TIMER_COUNT; i++) {
        ptimer_free(s->timer[i]);
    }
}

static void s5l8900_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8900_timer_reset);
    dc->desc = "Apple S5L8900 timer controller";
    dc->vmsd = &s5l8900_timer_vmstate;
}

static const TypeInfo s5l8900_timer_info = {
    .name = TYPE_S5L8900_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900TimerState),
    .instance_init = s5l8900_timer_init,
    .instance_finalize = s5l8900_timer_finalize,
    .class_init = s5l8900_timer_class_init,
};

static void s5l8900_timer_register_types(void)
{
    type_register_static(&s5l8900_timer_info);
}

type_init(s5l8900_timer_register_types)
