/*
 * Apple S5L8900 system controller and GPIO
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/gpio/s5l8900_gpio.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define SYSIC_POWER_CONFIG0       0x000
#define SYSIC_POWER_SETSTATE      0x008
#define SYSIC_POWER_ONCTRL        0x00c
#define SYSIC_POWER_OFFCTRL       0x010
#define SYSIC_POWER_STATE         0x014
#define SYSIC_POWER_CONFIG1       0x020
#define SYSIC_POWER_ID            0x044
#define SYSIC_POWER_CONFIG2       0x06c
#define SYSIC_MEMORY_CONFIG       0x070
#define SYSIC_MEMORY_STATUS       0x07c
#define SYSIC_GPIO_INTLEVEL       0x080
#define SYSIC_GPIO_INTSTAT        0x0a0
#define SYSIC_GPIO_INTEN          0x0c0
#define SYSIC_GPIO_INTTYPE        0x0e0

#define SYSIC_POWER_ID_BOARD_4    0x00040000
#define SYSIC_POWER_VROM          BIT(12)
#define SYSIC_MEMORY_READY        BIT(0)

#define GPIO_PAD_STRIDE           0x020
#define GPIO_CON                  0x000
#define GPIO_DAT                  0x004
#define GPIO_PUD1                 0x008
#define GPIO_PUD2                 0x00c
#define GPIO_CONSLP1              0x010
#define GPIO_CONSLP2              0x014
#define GPIO_PUDSLP1              0x018
#define GPIO_PUDSLP2              0x01c
#define GPIO_FSEL                 0x320

#define GPIO_FUNCTION_OUTPUT_LOW  0xe
#define GPIO_FUNCTION_OUTPUT_HIGH 0xf

static uint32_t s5l8900_gpio_group_mask(unsigned group)
{
    unsigned remaining = S5L8900_GPIO_PIN_COUNT - group * 32;

    return remaining >= 32 ? UINT32_MAX : MAKE_64BIT_MASK(0, remaining);
}

static uint32_t s5l8900_gpio_status(S5L8900GPIOState *s, unsigned group)
{
    uint32_t mask = s5l8900_gpio_group_mask(group);
    uint32_t level_active =
        ~(s->interrupt_level[group] ^ s->int_level[group]);

    return ((s->edge_pending[group] & ~s->int_type[group]) |
            (level_active & s->int_type[group])) &
           s->int_enable[group] & mask;
}

static void s5l8900_gpio_update_interrupts(S5L8900GPIOState *s)
{
    for (unsigned group = 0; group < S5L8900_GPIO_INTERRUPT_GROUPS;
         group++) {
        qemu_set_irq(s->irq[group],
                     s5l8900_gpio_status(s, group) != 0);
    }
}

static void s5l8900_gpio_update_vrom(S5L8900GPIOState *s)
{
    memory_region_set_enabled(s->vrom_alias,
                              !(s->power_state & SYSIC_POWER_VROM));
}

static unsigned s5l8900_gpio_function(S5L8900GPIOState *s, unsigned pin)
{
    unsigned pad = pin / S5L8900_GPIO_PINS_PER_PAD;
    unsigned index = pin % S5L8900_GPIO_PINS_PER_PAD;

    return extract32(s->con[pad], index * 4, 4);
}

static void s5l8900_gpio_update_pad_outputs(S5L8900GPIOState *s,
                                            unsigned pad)
{
    for (unsigned index = 0; index < S5L8900_GPIO_PINS_PER_PAD; index++) {
        unsigned pin = pad * S5L8900_GPIO_PINS_PER_PAD + index;
        unsigned function = s5l8900_gpio_function(s, pin);
        bool output = function == GPIO_FUNCTION_OUTPUT_LOW ||
                      function == GPIO_FUNCTION_OUTPUT_HIGH;

        qemu_set_irq(s->pin_out[pin],
                     output && (s->output_latch[pad] & BIT(index)));
    }
}

static uint32_t s5l8900_gpio_data(S5L8900GPIOState *s, unsigned pad)
{
    uint32_t value = 0;

    for (unsigned index = 0; index < S5L8900_GPIO_PINS_PER_PAD; index++) {
        unsigned pin = pad * S5L8900_GPIO_PINS_PER_PAD + index;
        unsigned group = pin / 32;
        unsigned bit = pin % 32;
        unsigned function = s5l8900_gpio_function(s, pin);

        if (function == GPIO_FUNCTION_OUTPUT_LOW ||
            function == GPIO_FUNCTION_OUTPUT_HIGH) {
            value |= s->output_latch[pad] & BIT(index);
        } else if (s->pin_level[group] & BIT(bit)) {
            value |= BIT(index);
        }
    }
    return value;
}

static void s5l8900_gpio_set_pin_level(void *opaque, int pin, int level)
{
    S5L8900GPIOState *s = opaque;
    unsigned group = pin / 32;
    uint32_t bit = BIT(pin % 32);
    bool old_level = (s->pin_level[group] & bit) != 0;
    bool new_level = level != 0;

    if (old_level == new_level) {
        return;
    }
    if (new_level) {
        s->pin_level[group] |= bit;
    } else {
        s->pin_level[group] &= ~bit;
    }
}

static void s5l8900_gpio_set_interrupt_level(void *opaque, int line,
                                              int level)
{
    S5L8900GPIOState *s = opaque;
    unsigned group = line / 32;
    uint32_t bit = BIT(line % 32);
    bool old_level = (s->interrupt_level[group] & bit) != 0;
    bool new_level = level != 0;
    bool high_trigger = (s->int_level[group] & bit) != 0;

    if (old_level == new_level) {
        return;
    }
    if (new_level) {
        s->interrupt_level[group] |= bit;
    } else {
        s->interrupt_level[group] &= ~bit;
    }
    if (!(s->int_type[group] & bit) && new_level == high_trigger) {
        s->edge_pending[group] |= bit;
    }
    s5l8900_gpio_update_interrupts(s);
}

static void s5l8900_gpio_set_input(void *opaque, int pin, int level)
{
    s5l8900_gpio_set_pin_level(opaque, pin, level);
    s5l8900_gpio_set_interrupt_level(opaque, pin, level);
}

static bool s5l8900_gpio_group_offset(hwaddr offset, hwaddr base,
                                      unsigned *group)
{
    if (offset < base || offset >= base +
        S5L8900_GPIO_INTERRUPT_GROUPS * sizeof(uint32_t)) {
        return false;
    }
    *group = (offset - base) / sizeof(uint32_t);
    return true;
}

static uint64_t s5l8900_sysic_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    S5L8900GPIOState *s = opaque;
    unsigned group;

    if (s5l8900_gpio_group_offset(offset, SYSIC_GPIO_INTLEVEL, &group)) {
        return s->int_level[group];
    }
    if (s5l8900_gpio_group_offset(offset, SYSIC_GPIO_INTSTAT, &group)) {
        return s5l8900_gpio_status(s, group);
    }
    if (s5l8900_gpio_group_offset(offset, SYSIC_GPIO_INTEN, &group)) {
        return s->int_enable[group];
    }
    if (s5l8900_gpio_group_offset(offset, SYSIC_GPIO_INTTYPE, &group)) {
        return s->int_type[group];
    }

    switch (offset) {
    case SYSIC_POWER_CONFIG0:
        return s->power_config[0];
    case SYSIC_POWER_SETSTATE:
    case SYSIC_POWER_STATE:
        return s->power_state;
    case SYSIC_POWER_CONFIG1:
        return s->power_config[1];
    case SYSIC_POWER_ID:
        return (s->security_epoch << 24) | SYSIC_POWER_ID_BOARD_4;
    case SYSIC_POWER_CONFIG2:
        return s->power_config[2];
    case SYSIC_MEMORY_CONFIG:
        return s->memory_config;
    case SYSIC_MEMORY_STATUS:
        return s->power_config[2] & SYSIC_MEMORY_READY;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.sysic: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void s5l8900_sysic_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    S5L8900GPIOState *s = opaque;
    unsigned group;

    if (s5l8900_gpio_group_offset(offset, SYSIC_GPIO_INTLEVEL, &group)) {
        s->int_level[group] = value & s5l8900_gpio_group_mask(group);
        s5l8900_gpio_update_interrupts(s);
        return;
    }
    if (s5l8900_gpio_group_offset(offset, SYSIC_GPIO_INTSTAT, &group)) {
        s->edge_pending[group] &= ~(uint32_t)value;
        s5l8900_gpio_update_interrupts(s);
        return;
    }
    if (s5l8900_gpio_group_offset(offset, SYSIC_GPIO_INTEN, &group)) {
        s->int_enable[group] = value & s5l8900_gpio_group_mask(group);
        s5l8900_gpio_update_interrupts(s);
        return;
    }
    if (s5l8900_gpio_group_offset(offset, SYSIC_GPIO_INTTYPE, &group)) {
        uint32_t new_type = value & s5l8900_gpio_group_mask(group);

        s->edge_pending[group] &= ~(s->int_type[group] ^ new_type);
        s->int_type[group] = new_type;
        s5l8900_gpio_update_interrupts(s);
        return;
    }

    switch (offset) {
    case SYSIC_POWER_CONFIG0:
        s->power_config[0] = value;
        break;
    case SYSIC_POWER_ONCTRL:
        s->power_state &= ~(uint32_t)value;
        s5l8900_gpio_update_vrom(s);
        break;
    case SYSIC_POWER_OFFCTRL:
        s->power_state |= (uint32_t)value;
        s5l8900_gpio_update_vrom(s);
        break;
    case SYSIC_POWER_CONFIG1:
        s->power_config[1] = value;
        break;
    case SYSIC_POWER_CONFIG2:
        s->power_config[2] = value;
        break;
    case SYSIC_MEMORY_CONFIG:
        s->memory_config = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.sysic: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static uint64_t s5l8900_gpio_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    S5L8900GPIOState *s = opaque;
    unsigned pad;
    hwaddr reg;

    if (offset >= GPIO_FSEL) {
        return 0;
    }
    pad = offset / GPIO_PAD_STRIDE;
    reg = offset % GPIO_PAD_STRIDE;
    switch (reg) {
    case GPIO_CON:
        return s->con[pad];
    case GPIO_DAT:
        return s5l8900_gpio_data(s, pad);
    case GPIO_PUD1:
        return s->pud1[pad];
    case GPIO_PUD2:
        return s->pud2[pad];
    case GPIO_CONSLP1:
        return s->conslp1[pad];
    case GPIO_CONSLP2:
        return s->conslp2[pad];
    case GPIO_PUDSLP1:
        return s->pudslp1[pad];
    case GPIO_PUDSLP2:
        return s->pudslp2[pad];
    default:
        return 0;
    }
}

static void s5l8900_gpio_write_fsel(S5L8900GPIOState *s, uint32_t value)
{
    unsigned pad = extract32(value, 16, 5);
    unsigned index = extract32(value, 8, 3);
    unsigned function = extract32(value, 0, 4);
    uint32_t bit;

    if (pad >= S5L8900_GPIO_PAD_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8900.gpio: invalid FSEL pad %u\n", pad);
        return;
    }
    bit = BIT(index);
    s->con[pad] = deposit32(s->con[pad], index * 4, 4, function);
    if (function == GPIO_FUNCTION_OUTPUT_LOW) {
        s->output_latch[pad] &= ~bit;
    } else if (function == GPIO_FUNCTION_OUTPUT_HIGH) {
        s->output_latch[pad] |= bit;
    }
    s5l8900_gpio_update_pad_outputs(s, pad);
}

static void s5l8900_gpio_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    S5L8900GPIOState *s = opaque;
    unsigned pad;
    hwaddr reg;

    if (offset == GPIO_FSEL) {
        s5l8900_gpio_write_fsel(s, value);
        return;
    }
    if (offset > GPIO_FSEL) {
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.gpio: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        return;
    }

    pad = offset / GPIO_PAD_STRIDE;
    reg = offset % GPIO_PAD_STRIDE;
    switch (reg) {
    case GPIO_CON:
        s->con[pad] = value;
        for (unsigned index = 0; index < S5L8900_GPIO_PINS_PER_PAD;
             index++) {
            unsigned function = extract32(value, index * 4, 4);

            if (function == GPIO_FUNCTION_OUTPUT_LOW) {
                s->output_latch[pad] &= ~BIT(index);
            } else if (function == GPIO_FUNCTION_OUTPUT_HIGH) {
                s->output_latch[pad] |= BIT(index);
            }
        }
        s5l8900_gpio_update_pad_outputs(s, pad);
        break;
    case GPIO_DAT:
        s->output_latch[pad] = value & 0xff;
        s5l8900_gpio_update_pad_outputs(s, pad);
        break;
    case GPIO_PUD1:
        s->pud1[pad] = value & 0xff;
        break;
    case GPIO_PUD2:
        s->pud2[pad] = value & 0xff;
        break;
    case GPIO_CONSLP1:
        s->conslp1[pad] = value;
        break;
    case GPIO_CONSLP2:
        s->conslp2[pad] = value;
        break;
    case GPIO_PUDSLP1:
        s->pudslp1[pad] = value & 0xff;
        break;
    case GPIO_PUDSLP2:
        s->pudslp2[pad] = value & 0xff;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps s5l8900_sysic_ops = {
    .read = s5l8900_sysic_read,
    .write = s5l8900_sysic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static const MemoryRegionOps s5l8900_gpio_ops = {
    .read = s5l8900_gpio_read,
    .write = s5l8900_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_gpio_reset(DeviceState *dev)
{
    S5L8900GPIOState *s = S5L8900_GPIO(dev);

    s->power_config[0] = 0x01123009;
    s->power_config[1] = 0x00000020;
    s->power_config[2] = 0;
    s->memory_config = 0;
    s->power_state = 0;
    memset(s->int_level, 0, sizeof(s->int_level));
    memset(s->int_enable, 0, sizeof(s->int_enable));
    memset(s->int_type, 0, sizeof(s->int_type));
    memset(s->edge_pending, 0, sizeof(s->edge_pending));
    memset(s->pin_level, 0, sizeof(s->pin_level));
    memset(s->interrupt_level, 0, sizeof(s->interrupt_level));
    memset(s->con, 0, sizeof(s->con));
    memset(s->output_latch, 0, sizeof(s->output_latch));
    memset(s->pud1, 0, sizeof(s->pud1));
    memset(s->pud2, 0, sizeof(s->pud2));
    memset(s->conslp1, 0, sizeof(s->conslp1));
    memset(s->conslp2, 0, sizeof(s->conslp2));
    memset(s->pudslp1, 0, sizeof(s->pudslp1));
    memset(s->pudslp2, 0, sizeof(s->pudslp2));
    for (unsigned pad = 0; pad < S5L8900_GPIO_PAD_COUNT; pad++) {
        s5l8900_gpio_update_pad_outputs(s, pad);
    }
    s5l8900_gpio_update_interrupts(s);
    s5l8900_gpio_update_vrom(s);
}

static int s5l8900_gpio_post_load(void *opaque, int version_id)
{
    S5L8900GPIOState *s = opaque;
    unsigned last_group = S5L8900_GPIO_INTERRUPT_GROUPS - 1;
    uint32_t last_mask = s5l8900_gpio_group_mask(last_group);

    s->int_level[last_group] &= last_mask;
    s->int_enable[last_group] &= last_mask;
    s->int_type[last_group] &= last_mask;
    s->edge_pending[last_group] &= last_mask;
    s->pin_level[last_group] &= last_mask;
    if (version_id < 3) {
        memcpy(s->interrupt_level, s->pin_level,
               sizeof(s->interrupt_level));
    }
    s->interrupt_level[last_group] &= last_mask;
    for (unsigned pad = 0; pad < S5L8900_GPIO_PAD_COUNT; pad++) {
        s->output_latch[pad] &= 0xff;
        s->pud1[pad] &= 0xff;
        s->pud2[pad] &= 0xff;
        s->pudslp1[pad] &= 0xff;
        s->pudslp2[pad] &= 0xff;
        s5l8900_gpio_update_pad_outputs(s, pad);
    }
    s5l8900_gpio_update_interrupts(s);
    s5l8900_gpio_update_vrom(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_gpio = {
    .name = TYPE_S5L8900_GPIO,
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = s5l8900_gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(power_config, S5L8900GPIOState, 3),
        VMSTATE_UINT32_V(memory_config, S5L8900GPIOState, 2),
        VMSTATE_UINT32(power_state, S5L8900GPIOState),
        VMSTATE_UINT32_ARRAY(int_level, S5L8900GPIOState,
                             S5L8900_GPIO_INTERRUPT_GROUPS),
        VMSTATE_UINT32_ARRAY(int_enable, S5L8900GPIOState,
                             S5L8900_GPIO_INTERRUPT_GROUPS),
        VMSTATE_UINT32_ARRAY(int_type, S5L8900GPIOState,
                             S5L8900_GPIO_INTERRUPT_GROUPS),
        VMSTATE_UINT32_ARRAY(edge_pending, S5L8900GPIOState,
                             S5L8900_GPIO_INTERRUPT_GROUPS),
        VMSTATE_UINT32_ARRAY(pin_level, S5L8900GPIOState,
                             S5L8900_GPIO_INTERRUPT_GROUPS),
        VMSTATE_UINT32_ARRAY_V(interrupt_level, S5L8900GPIOState,
                               S5L8900_GPIO_INTERRUPT_GROUPS, 3),
        VMSTATE_UINT32_ARRAY(con, S5L8900GPIOState,
                             S5L8900_GPIO_PAD_COUNT),
        VMSTATE_UINT32_ARRAY(output_latch, S5L8900GPIOState,
                             S5L8900_GPIO_PAD_COUNT),
        VMSTATE_UINT32_ARRAY(pud1, S5L8900GPIOState,
                             S5L8900_GPIO_PAD_COUNT),
        VMSTATE_UINT32_ARRAY(pud2, S5L8900GPIOState,
                             S5L8900_GPIO_PAD_COUNT),
        VMSTATE_UINT32_ARRAY(conslp1, S5L8900GPIOState,
                             S5L8900_GPIO_PAD_COUNT),
        VMSTATE_UINT32_ARRAY(conslp2, S5L8900GPIOState,
                             S5L8900_GPIO_PAD_COUNT),
        VMSTATE_UINT32_ARRAY(pudslp1, S5L8900GPIOState,
                             S5L8900_GPIO_PAD_COUNT),
        VMSTATE_UINT32_ARRAY(pudslp2, S5L8900GPIOState,
                             S5L8900_GPIO_PAD_COUNT),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_gpio_realize(DeviceState *dev, Error **errp)
{
    S5L8900GPIOState *s = S5L8900_GPIO(dev);

    if (!s->vrom_alias) {
        error_setg(errp, TYPE_S5L8900_GPIO " 'vrom-alias' link not set");
    }
}

static const Property s5l8900_gpio_properties[] = {
    DEFINE_PROP_LINK("vrom-alias", S5L8900GPIOState, vrom_alias,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT8("security-epoch", S5L8900GPIOState,
                      security_epoch, 5),
};

static void s5l8900_gpio_init(Object *obj)
{
    S5L8900GPIOState *s = S5L8900_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->sysic_iomem, obj, &s5l8900_sysic_ops, s,
                          "s5l8900.sysic", 0x1000);
    memory_region_init_io(&s->gpio_iomem, obj, &s5l8900_gpio_ops, s,
                          "s5l8900.gpio", 0x1000);
    sysbus_init_mmio(sbd, &s->sysic_iomem);
    sysbus_init_mmio(sbd, &s->gpio_iomem);
    for (unsigned group = 0; group < S5L8900_GPIO_INTERRUPT_GROUPS;
         group++) {
        sysbus_init_irq(sbd, &s->irq[group]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), s5l8900_gpio_set_input, "pin",
                            S5L8900_GPIO_PIN_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), s5l8900_gpio_set_pin_level,
                            "external-pin", S5L8900_GPIO_PIN_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), s5l8900_gpio_set_interrupt_level,
                            "interrupt", S5L8900_GPIO_PIN_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), s->pin_out, "pin-output",
                             S5L8900_GPIO_PIN_COUNT);
}

static void s5l8900_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 system controller and GPIO";
    dc->realize = s5l8900_gpio_realize;
    dc->vmsd = &vmstate_s5l8900_gpio;
    device_class_set_props(dc, s5l8900_gpio_properties);
    device_class_set_legacy_reset(dc, s5l8900_gpio_reset);
}

static const TypeInfo s5l8900_gpio_info = {
    .name = TYPE_S5L8900_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900GPIOState),
    .instance_init = s5l8900_gpio_init,
    .class_init = s5l8900_gpio_class_init,
};

static void s5l8900_gpio_register_types(void)
{
    type_register_static(&s5l8900_gpio_info);
}

type_init(s5l8900_gpio_register_types)
