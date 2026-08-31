/*
 * Apple S5L8900 I2C controller
 *
 * The register and completion contract follows the S5L8900 OpeniBoot
 * producer.  In particular, register 0x20 reports and acknowledges completed
 * send/receive and bus-condition operations; it is not present in later
 * Samsung controllers with an otherwise similar five-register prefix.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/i2c/s5l8900_i2c.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define IICCON                  0x00
#define IICSTAT                 0x04
#define IICADD                  0x08
#define IICDS                   0x0c
#define IICLC                   0x10
#define IICREG14                0x14
#define IICREG18                0x18
#define IICREG1C                0x1c
#define IICREG20                0x20

#define IICCON_ACK_GENERATE     BIT(7)
#define IICCON_IRQ_ENABLE       BIT(5)
#define IICCON_IRQ_PENDING      BIT(4)

#define IICREG14_OPERATION_IRQ_ENABLE BIT(0)

#define IICSTAT_MODE_SHIFT      6
#define IICSTAT_MODE_MASK       3
#define IICSTAT_MASTER_RECEIVE  2
#define IICSTAT_MASTER_TRANSMIT 3
#define IICSTAT_START           BIT(5)
#define IICSTAT_OUTPUT_ENABLE   BIT(4)
#define IICSTAT_LAST_BIT        BIT(0)

#define IIC_OPERATION_TRANSFER  BIT(8)
#define IIC_OPERATION_CONDITION BIT(13)
#define IIC_OPERATION_MASK      (IIC_OPERATION_TRANSFER | \
                                 IIC_OPERATION_CONDITION)

static unsigned s5l8900_i2c_mode(uint32_t status)
{
    return (status >> IICSTAT_MODE_SHIFT) & IICSTAT_MODE_MASK;
}

static void s5l8900_i2c_update_irq(S5L8900I2CState *s)
{
    bool control_irq = (s->control & IICCON_IRQ_ENABLE) &&
                       (s->control & IICCON_IRQ_PENDING);
    bool operation_irq = (s->unknown[0] &
                          IICREG14_OPERATION_IRQ_ENABLE) &&
                         (s->operation_status & IIC_OPERATION_MASK);

    qemu_set_irq(s->irq, control_irq || operation_irq);
}

static void s5l8900_i2c_complete(S5L8900I2CState *s, uint32_t operation)
{
    s->operation_status |= operation;
    s->control |= IICCON_IRQ_PENDING;
    s5l8900_i2c_update_irq(s);
}

static void s5l8900_i2c_end_transfer(S5L8900I2CState *s)
{
    if (s->active) {
        i2c_end_transfer(s->bus);
    }
    s->active = false;
    s->data_ready = false;
}

static void s5l8900_i2c_continue(S5L8900I2CState *s)
{
    int result;

    if (!s->active) {
        return;
    }

    s->status &= ~IICSTAT_LAST_BIT;
    if (s5l8900_i2c_mode(s->status) == IICSTAT_MASTER_RECEIVE) {
        s->data = i2c_recv(s->bus);
        if (!(s->control & IICCON_ACK_GENERATE)) {
            i2c_nack(s->bus);
        }
    } else {
        if (!s->data_ready) {
            return;
        }
        result = i2c_send(s->bus, s->data);
        if (result) {
            s->status |= IICSTAT_LAST_BIT;
        }
        s->data_ready = false;
    }
    s5l8900_i2c_complete(s, IIC_OPERATION_TRANSFER);
}

static void s5l8900_i2c_write_status(S5L8900I2CState *s, uint32_t value)
{
    uint32_t old_status = s->status;
    unsigned old_mode = s5l8900_i2c_mode(old_status);
    unsigned mode = s5l8900_i2c_mode(value);
    bool old_started = old_status & IICSTAT_START;
    bool start = value & IICSTAT_START;
    bool master = mode == IICSTAT_MASTER_RECEIVE ||
                  mode == IICSTAT_MASTER_TRANSMIT;
    bool receive = mode == IICSTAT_MASTER_RECEIVE;
    bool condition_changed = old_started &&
                             (!start || mode != old_mode);
    int result;

    if (s->active && condition_changed) {
        s5l8900_i2c_end_transfer(s);
    }

    s->status = value & ~IICSTAT_LAST_BIT;
    if (condition_changed) {
        s5l8900_i2c_complete(s, IIC_OPERATION_CONDITION);
    }
    if (!start) {
        return;
    }
    if (!(value & IICSTAT_OUTPUT_ENABLE) || !master || s->active) {
        return;
    }

    result = i2c_start_transfer(s->bus, extract32(s->data, 1, 7), receive);
    if (result) {
        s->status |= IICSTAT_LAST_BIT;
    } else {
        s->active = true;
    }
    s->data_ready = false;
    s5l8900_i2c_complete(s, IIC_OPERATION_TRANSFER);
}

static uint64_t s5l8900_i2c_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    S5L8900I2CState *s = opaque;

    switch (offset) {
    case IICCON:
        return s->control;
    case IICSTAT:
        return s->status;
    case IICADD:
        return s->address;
    case IICDS:
        return s->data;
    case IICLC:
        return s->line_control;
    case IICREG14:
    case IICREG18:
    case IICREG1C:
        return s->unknown[(offset - IICREG14) / 4];
    case IICREG20:
        return s->operation_status;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.i2c: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void s5l8900_i2c_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    S5L8900I2CState *s = opaque;

    switch (offset) {
    case IICCON: {
        bool acknowledge = value & IICCON_IRQ_PENDING;

        s->control = value & ~IICCON_IRQ_PENDING;
        s5l8900_i2c_update_irq(s);
        if (acknowledge) {
            s5l8900_i2c_continue(s);
        }
        break;
    }
    case IICSTAT:
        s5l8900_i2c_write_status(s, value);
        break;
    case IICADD:
        if (!(s->status & IICSTAT_OUTPUT_ENABLE)) {
            s->address = value;
        }
        break;
    case IICDS:
        s->data = value;
        s->data_ready = true;
        break;
    case IICLC:
        s->line_control = value;
        break;
    case IICREG14:
        s->unknown[0] = value;
        s5l8900_i2c_update_irq(s);
        break;
    case IICREG18:
    case IICREG1C:
        s->unknown[(offset - IICREG14) / 4] = value;
        break;
    case IICREG20:
        s->operation_status &= ~(value & IIC_OPERATION_MASK);
        s5l8900_i2c_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "s5l8900.i2c: unimplemented write 0x%08" PRIx64
                      " at 0x%03" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8900_i2c_ops = {
    .read = s5l8900_i2c_read,
    .write = s5l8900_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_i2c_reset(DeviceState *dev)
{
    S5L8900I2CState *s = S5L8900_I2C(dev);

    if (s->bus && i2c_bus_busy(s->bus)) {
        i2c_end_transfer(s->bus);
    }
    s->control = 0;
    s->status = 0;
    s->address = 0;
    s->data = 0xff;
    s->line_control = 0;
    memset(s->unknown, 0, sizeof(s->unknown));
    s->operation_status = 0;
    s->active = false;
    s->data_ready = false;
    s5l8900_i2c_update_irq(s);
}

static int s5l8900_i2c_post_load(void *opaque, int version_id)
{
    S5L8900I2CState *s = opaque;

    if (s->operation_status & ~IIC_OPERATION_MASK) {
        return -EINVAL;
    }
    s5l8900_i2c_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_i2c = {
    .name = TYPE_S5L8900_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8900_i2c_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, S5L8900I2CState),
        VMSTATE_UINT32(status, S5L8900I2CState),
        VMSTATE_UINT32(address, S5L8900I2CState),
        VMSTATE_UINT32(data, S5L8900I2CState),
        VMSTATE_UINT32(line_control, S5L8900I2CState),
        VMSTATE_UINT32_ARRAY(unknown, S5L8900I2CState, 3),
        VMSTATE_UINT32(operation_status, S5L8900I2CState),
        VMSTATE_BOOL(active, S5L8900I2CState),
        VMSTATE_BOOL(data_ready, S5L8900I2CState),
        VMSTATE_END_OF_LIST()
    },
};

I2CBus *s5l8900_i2c_get_bus(DeviceState *dev)
{
    return S5L8900_I2C(dev)->bus;
}

static void s5l8900_i2c_init(Object *obj)
{
    S5L8900I2CState *s = S5L8900_I2C(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_i2c_ops, s,
                          TYPE_S5L8900_I2C, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static void s5l8900_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Apple S5L8900 I2C controller";
    dc->vmsd = &vmstate_s5l8900_i2c;
    device_class_set_legacy_reset(dc, s5l8900_i2c_reset);
}

static const TypeInfo s5l8900_i2c_info = {
    .name = TYPE_S5L8900_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900I2CState),
    .instance_init = s5l8900_i2c_init,
    .class_init = s5l8900_i2c_class_init,
};

static void s5l8900_i2c_register_types(void)
{
    type_register_static(&s5l8900_i2c_info);
}

type_init(s5l8900_i2c_register_types)
