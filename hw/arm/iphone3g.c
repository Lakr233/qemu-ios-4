/*
 * Apple iPhone 3G (iPhone1,2 / N82AP) machine
 *
 * Copyright (c) 2026 QEMU iPhone contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/arm/machines-qom.h"
#include "hw/audio/s5l8900_i2s.h"
#include "hw/audio/wm8991.h"
#include "hw/block/s5l8900_nand.h"
#include "hw/char/s5l8900_uart.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/s5l8900_lcd.h"
#include "hw/display/s5l8900_mbx.h"
#include "hw/display/s5l8900_tvout.h"
#include "hw/dma/pl080.h"
#include "hw/gpio/s5l8900_gpio.h"
#include "hw/i2c/s5l8900_i2c.h"
#include "hw/input/s5l8900_buttons.h"
#include "hw/intc/s5l8900_edgeic.h"
#include "hw/intc/s5l8900_vic.h"
#include "hw/misc/pcf50635.h"
#include "hw/misc/s5l8900_adm.h"
#include "hw/misc/s5l8900_aes.h"
#include "hw/misc/s5l8900_chipid.h"
#include "hw/misc/s5l8900_clock.h"
#include "hw/misc/s5l8900_sha1.h"
#include "hw/misc/s5l8900_pke.h"
#include "hw/sd/s5l8900_sdio.h"
#include "hw/sensor/isl29003.h"
#include "hw/ssi/s5l8900_merlot.h"
#include "hw/ssi/s5l8900_spi.h"
#include "hw/ssi/s5l8900_zephyr2.h"
#include "hw/timer/s5l8900_timer.h"
#include "hw/usb/s5l8900_usb.h"
#include "hw/watchdog/s5l8900_watchdog.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "system/system.h"
#include "target/arm/cpu.h"

#define TYPE_IPHONE3G_MACHINE MACHINE_TYPE_NAME("iphone3g")
OBJECT_DECLARE_SIMPLE_TYPE(IPhone3GMachineState, IPHONE3G_MACHINE)

#define IPHONE3G_RAM_SIZE       (128 * MiB)
#define S5L8900_BOOTROM_BASE    0x50000000
#define S5L8900_BOOTROM_SIZE    (64 * KiB)
#define S5L8900_RAM_ALIAS_BASE  0x08000000
#define S5L8900_IBOOT_RAM_BASE  0x18000000
#define S5L8900_IBOOT_RAM_SIZE  (4 * MiB)
#define S5L8900_SRAM_BASE       0x22000000
#define S5L8900_SRAM_SIZE       (256 * KiB)
#define S5L8900_SYSIC_BASE      0x39a00000
#define S5L8900_VIC0_BASE       0x38e00000
#define S5L8900_VIC1_BASE       0x38e01000
#define S5L8900_EDGEIC_BASE     0x38e02000
#define S5L8900_EDGEIC_LOW_IRQ  35
#define S5L8900_EDGEIC_HIGH_IRQ 41
#define S5L8900_LCD_BASE        0x38900000
#define S5L8900_LCD_IRQ         13
#define S5L8900_MBX_BASE        0x3b000000
#define S5L8900_MBX_IRQ         12
#define S5L8900_TVOUT_BANK0_BASE 0x39300000
#define S5L8900_TVOUT_BANK1_BASE 0x39200000
#define S5L8900_TVOUT_BANK2_BASE 0x39100000
#define S5L8900_TVOUT_IRQ        38
#define S5L8900_DMAC0_BASE      0x38200000
#define S5L8900_DMAC1_BASE      0x39900000
#define S5L8900_DMAC0_IRQ       16
#define S5L8900_DMAC1_IRQ       17
#define S5L8900_SPI1_TX_DMA_REQ 12
#define S5L8900_NAND_BASE       0x38a00000
#define S5L8900_NAND_ECC_BASE   0x38f00000
#define S5L8900_NAND_IRQ        20
#define S5L8900_NAND_ECC_IRQ    43
#define S5L8900_NAND_DMA_REQ    2
#define S5L8900_SDIO_BASE       0x38d00000
#define S5L8900_SDIO_IRQ        42
#define S5L8900_AES_BASE        0x38c00000
#define S5L8900_AES_IRQ         39
#define S5L8900_SHA1_BASE       0x38000000
#define S5L8900_SHA1_IRQ        40
#define S5L8900_PKE_BASE        0x3d000000
#define S5L8900_ADM_BASE        0x38800000
#define S5L8900_ADM_IRQ         37
#define S5L8900_CLOCK0_BASE     0x38100000
#define S5L8900_CLOCK1_BASE     0x3c500000
#define S5L8900_CHIPID_BASE     0x3e500000
#define S5L8900_GPIO_BASE       0x3e400000
#define S5L8900_SPI0_BASE       0x3c300000
#define S5L8900_SPI1_BASE       0x3ce00000
#define S5L8900_SPI2_BASE       0x3d200000
#define S5L8900_SPI0_IRQ        9
#define S5L8900_NOR_CS_PIN      (4 * S5L8900_GPIO_PINS_PER_PAD)
#define S5L8900_PANEL_CS_PIN    (7 * S5L8900_GPIO_PINS_PER_PAD + 5)
#define S5L8900_SERIALIZER_CS_PIN (7 * S5L8900_GPIO_PINS_PER_PAD + 6)
#define S5L8900_TOUCH_RESET_PIN (6 * S5L8900_GPIO_PINS_PER_PAD + 6)
#define S5L8900_TOUCH_POWER_PIN (7 * S5L8900_GPIO_PINS_PER_PAD + 1)
#define S5L8900_TOUCH_ATN_PIN   155
#define S5L8900_TOUCH_CS_PIN    (24 * S5L8900_GPIO_PINS_PER_PAD)
#define S5L8900_ALS_IRQ_PIN     73
#define S5L8900_PMU_IRQ_PIN     85
#define S5L8900_BUTTON_HOLD_PIN (22 * S5L8900_GPIO_PINS_PER_PAD + 5)
#define S5L8900_BUTTON_MENU_PIN (22 * S5L8900_GPIO_PINS_PER_PAD)
#define S5L8900_BUTTON_VOLUP_PIN (22 * S5L8900_GPIO_PINS_PER_PAD + 1)
#define S5L8900_BUTTON_VOLDOWN_PIN (22 * S5L8900_GPIO_PINS_PER_PAD + 2)
#define S5L8900_BUTTON_RINGER_PIN (22 * S5L8900_GPIO_PINS_PER_PAD + 3)
#define S5L8900_UART_BASE        0x3cc00000
#define S5L8900_UART_STRIDE      0x00004000
#define S5L8900_UART0_IRQ        24
#define S5L8900_I2C0_BASE        0x3c600000
#define S5L8900_I2C1_BASE        0x3c900000
#define S5L8900_I2C0_IRQ         21
#define S5L8900_I2S0_BASE        0x3ca00000
#define S5L8900_I2S1_BASE        0x3cd00000
#define S5L8900_TIMER_BASE      0x3e200000
#define S5L8900_TIMER_IRQ       7
#define S5L8900_USB_BASE        0x38400000
#define S5L8900_USB_PHY_BASE    0x3c400000
#define S5L8900_USB_IRQ         19
#define S5L8900_WATCHDOG_BASE   0x3e300000
#define S5L8900_WATCHDOG_IRQ    51

struct IPhone3GMachineState {
    MachineState parent_obj;

    uint8_t security_epoch;
    ARMCPU *cpu;
    DeviceState *vic[2];
    DeviceState *edgeic;
    DeviceState *lcd;
    DeviceState *mbx;
    DeviceState *tvout;
    DeviceState *dmac[2];
    DeviceState *nand;
    DeviceState *sdio;
    DeviceState *aes;
    DeviceState *sha1;
    DeviceState *pke;
    DeviceState *adm;
    DeviceState *clock;
    DeviceState *chipid;
    DeviceState *gpio;
    DeviceState *spi[3];
    DeviceState *nor;
    DeviceState *panel;
    DeviceState *serializer;
    DeviceState *touch;
    DeviceState *buttons;
    DeviceState *uart[5];
    DeviceState *i2c[2];
    DeviceState *i2s[2];
    DeviceState *timer;
    DeviceState *usb;
    DeviceState *watchdog;
    MemoryRegion ram_alias;
    MemoryRegion iboot_ram;
    MemoryRegion sram;
    MemoryRegion bootrom;
    MemoryRegion bootrom_reset_alias;
};

static void iphone3g_get_security_epoch(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    IPhone3GMachineState *s = IPHONE3G_MACHINE(obj);
    uint8_t epoch = s->security_epoch;

    visit_type_uint8(v, name, &epoch, errp);
}

static void iphone3g_set_security_epoch(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    IPhone3GMachineState *s = IPHONE3G_MACHINE(obj);
    uint8_t epoch;

    if (!visit_type_uint8(v, name, &epoch, errp)) {
        return;
    }
    if (epoch != 4 && epoch != 5) {
        error_setg(errp, "iphone3g security-epoch must be 4 or 5");
        return;
    }
    s->security_epoch = epoch;
}

static void iphone3g_load_bootrom(IPhone3GMachineState *s,
                                  MachineState *machine)
{
    g_autofree char *filename = NULL;
    ssize_t size;

    if (qtest_enabled()) {
        return;
    }
    if (!machine->firmware) {
        error_report("iphone3g: -bios must name a 64 KiB S5L8900 boot ROM");
        exit(EXIT_FAILURE);
    }

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!filename) {
        error_report("iphone3g: cannot find boot ROM '%s'",
                     machine->firmware);
        exit(EXIT_FAILURE);
    }

    size = load_image_mr(filename, &s->bootrom);
    if (size < 0 || size > S5L8900_BOOTROM_SIZE) {
        error_report("iphone3g: boot ROM '%s' must not exceed %" PRIu64
                     " bytes", machine->firmware,
                     (uint64_t)S5L8900_BOOTROM_SIZE);
        exit(EXIT_FAILURE);
    }
}

static void iphone3g_init(MachineState *machine)
{
    IPhone3GMachineState *s = IPHONE3G_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    DriveInfo *nand_drive = drive_get(IF_MTD, 0, 0);
    DriveInfo *nor_drive = drive_get(IF_PFLASH, 0, 0);
    I2CSlave *als;
    I2CSlave *pmu;

    if (machine->ram_size != mc->default_ram_size) {
        g_autofree char *size = size_to_str(mc->default_ram_size);

        error_report("iphone3g: RAM size is fixed at %s", size);
        exit(EXIT_FAILURE);
    }

    s->cpu = ARM_CPU(cpu_create(machine->cpu_type));

    s->vic[0] = qdev_new(TYPE_S5L8900_VIC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->vic[0]), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->vic[0]), 0, S5L8900_VIC0_BASE);

    s->vic[1] = qdev_new(TYPE_S5L8900_VIC);
    object_property_set_link(OBJECT(s->vic[1]), "upstream",
                             OBJECT(s->vic[0]), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->vic[1]), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->vic[1]), 0, S5L8900_VIC1_BASE);

    sysbus_connect_irq(SYS_BUS_DEVICE(s->vic[0]), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->vic[0]), 1,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));

    s->edgeic = qdev_new(TYPE_S5L8900_EDGEIC);
    object_property_add_child(OBJECT(machine), "edgeic", OBJECT(s->edgeic));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->edgeic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->edgeic), 0, S5L8900_EDGEIC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->edgeic), 0,
                       qdev_get_gpio_in(s->vic[1],
                           S5L8900_EDGEIC_LOW_IRQ - 32));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->edgeic), 1,
                       qdev_get_gpio_in(s->vic[1],
                           S5L8900_EDGEIC_HIGH_IRQ - 32));

    for (unsigned i = 0; i < ARRAY_SIZE(s->spi); i++) {
        static const hwaddr base[] = {
            S5L8900_SPI0_BASE, S5L8900_SPI1_BASE, S5L8900_SPI2_BASE,
        };

        s->spi[i] = qdev_new(TYPE_S5L8900_SPI);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->spi[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->spi[i]), 0, base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(s->spi[i]), 0,
                           qdev_get_gpio_in(s->vic[0],
                                            S5L8900_SPI0_IRQ + i));
    }

    s->nor = qdev_new("sst25vf080b");
    if (nor_drive) {
        qdev_prop_set_drive(s->nor, "drive", blk_by_legacy_dinfo(nor_drive));
    }
    qdev_realize_and_unref(s->nor, BUS(s5l8900_spi_get_bus(s->spi[0])),
                           &error_fatal);

    s->panel = qdev_new(TYPE_S5L8900_MERLOT_PANEL);
    qdev_prop_set_uint8(s->panel, "cs", 1);
    qdev_realize_and_unref(s->panel, BUS(s5l8900_spi_get_bus(s->spi[0])),
                           &error_fatal);
    s->serializer = qdev_new(TYPE_S5L8900_LM2512);
    qdev_prop_set_uint8(s->serializer, "cs", 2);
    qdev_realize_and_unref(s->serializer,
                           BUS(s5l8900_spi_get_bus(s->spi[0])),
                           &error_fatal);

    s->touch = qdev_new(TYPE_S5L8900_ZEPHYR2);
    qdev_realize_and_unref(s->touch,
                           BUS(s5l8900_spi_get_bus(s->spi[1])),
                           &error_fatal);

    for (unsigned i = 0; i < ARRAY_SIZE(s->uart); i++) {
        Chardev *chr = i == 0 ? serial_hd(0) :
                       i == ARRAY_SIZE(s->uart) - 1 ? serial_hd(1) : NULL;

        s->uart[i] = qdev_new(TYPE_S5L8900_UART);
        if (chr) {
            qdev_prop_set_chr(s->uart[i], "chardev", chr);
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->uart[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->uart[i]), 0,
                        S5L8900_UART_BASE + i * S5L8900_UART_STRIDE);
        sysbus_connect_irq(SYS_BUS_DEVICE(s->uart[i]), 0,
                           qdev_get_gpio_in(s->vic[0],
                                            S5L8900_UART0_IRQ + i));
    }

    for (unsigned i = 0; i < ARRAY_SIZE(s->i2c); i++) {
        hwaddr base = i ? S5L8900_I2C1_BASE : S5L8900_I2C0_BASE;

        s->i2c[i] = qdev_new(TYPE_S5L8900_I2C);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->i2c[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->i2c[i]), 0, base);
        sysbus_connect_irq(SYS_BUS_DEVICE(s->i2c[i]), 0,
                           qdev_get_gpio_in(s->vic[0],
                                            S5L8900_I2C0_IRQ + i));
    }

    pmu = i2c_slave_create_simple(s5l8900_i2c_get_bus(s->i2c[0]),
                                  TYPE_PCF50635, 0x73);
    als = i2c_slave_create_simple(s5l8900_i2c_get_bus(s->i2c[0]),
                                  TYPE_ISL29003, 0x44);
    i2c_slave_create_simple(s5l8900_i2c_get_bus(s->i2c[0]),
                            TYPE_WM8991, 0x1b);

    for (unsigned i = 0; i < ARRAY_SIZE(s->dmac); i++) {
        hwaddr base = i ? S5L8900_DMAC1_BASE : S5L8900_DMAC0_BASE;
        unsigned irq = i ? S5L8900_DMAC1_IRQ : S5L8900_DMAC0_IRQ;

        s->dmac[i] = qdev_new(TYPE_PL080);
        object_property_set_link(OBJECT(s->dmac[i]), "downstream",
                                 OBJECT(sysmem), &error_fatal);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->dmac[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->dmac[i]), 0, base);
        sysbus_connect_irq(SYS_BUS_DEVICE(s->dmac[i]), 0,
                           qdev_get_gpio_in(s->vic[0], irq));
    }

    for (unsigned i = 0; i < ARRAY_SIZE(s->i2s); i++) {
        hwaddr base = i ? S5L8900_I2S1_BASE : S5L8900_I2S0_BASE;

        s->i2s[i] = qdev_new(TYPE_S5L8900_I2S);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->i2s[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->i2s[i]), 0, base);
    }
    for (unsigned dmac = 0; dmac < ARRAY_SIZE(s->dmac); dmac++) {
        qdev_connect_gpio_out_named(s->i2s[0], "tx-dreq", dmac,
            qdev_get_gpio_in_named(s->dmac[dmac], "single-request", 0));
        qdev_connect_gpio_out_named(s->i2s[0], "rx-dreq", dmac,
            qdev_get_gpio_in_named(s->dmac[dmac], "single-request", 1));
    }
    qdev_connect_gpio_out_named(s->i2s[1], "tx-dreq", 1,
        qdev_get_gpio_in_named(s->dmac[1], "single-request", 2));
    qdev_connect_gpio_out_named(s->i2s[1], "rx-dreq", 1,
        qdev_get_gpio_in_named(s->dmac[1], "single-request", 3));
    qdev_connect_gpio_out_named(s->spi[1], "tx-dreq", 0,
        qdev_get_gpio_in_named(s->dmac[1], "single-request",
                               S5L8900_SPI1_TX_DMA_REQ));

    s->nand = qdev_new(TYPE_S5L8900_NAND);
    object_property_set_link(OBJECT(s->nand), "dma-memory", OBJECT(sysmem),
                             &error_fatal);
    if (nand_drive) {
        qdev_prop_set_drive(s->nand, "drive",
                            blk_by_legacy_dinfo(nand_drive));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->nand), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->nand), 0, S5L8900_NAND_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->nand), 1, S5L8900_NAND_ECC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->nand), 0,
                       qdev_get_gpio_in(s->vic[0], S5L8900_NAND_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->nand), 1,
                       qdev_get_gpio_in(s->vic[1],
                           S5L8900_NAND_ECC_IRQ - 32));
    qdev_connect_gpio_out_named(s->nand, "dreq", 0,
        qdev_get_gpio_in_named(s->dmac[0], "single-request",
                               S5L8900_NAND_DMA_REQ));

    s->sdio = qdev_new(TYPE_S5L8900_SDIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sdio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->sdio), 0, S5L8900_SDIO_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->sdio), 0,
                       qdev_get_gpio_in(s->vic[1], S5L8900_SDIO_IRQ - 32));

    s->aes = qdev_new(TYPE_S5L8900_AES);
    object_property_set_link(OBJECT(s->aes), "dma-memory", OBJECT(sysmem),
                             &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->aes), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->aes), 0, S5L8900_AES_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->aes), 0,
                       qdev_get_gpio_in(s->vic[1], S5L8900_AES_IRQ - 32));

    s->sha1 = qdev_new(TYPE_S5L8900_SHA1);
    object_property_set_link(OBJECT(s->sha1), "dma-memory", OBJECT(sysmem),
                             &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sha1), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->sha1), 0, S5L8900_SHA1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->sha1), 0,
                       qdev_get_gpio_in(s->vic[1], S5L8900_SHA1_IRQ - 32));

    s->pke = qdev_new(TYPE_S5L8900_PKE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->pke), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->pke), 0, S5L8900_PKE_BASE);

    s->adm = qdev_new(TYPE_S5L8900_ADM);
    object_property_set_link(OBJECT(s->adm), "dma-memory", OBJECT(sysmem),
                             &error_fatal);
    object_property_set_link(OBJECT(s->adm), "nand", OBJECT(s->nand),
                             &error_fatal);
    object_property_set_link(OBJECT(s->adm), "dmac", OBJECT(s->dmac[0]),
                             &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->adm), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->adm), 0, S5L8900_ADM_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->adm), 0,
                       qdev_get_gpio_in(s->vic[1], S5L8900_ADM_IRQ - 32));

    s->clock = qdev_new(TYPE_S5L8900_CLOCK);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->clock), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->clock), 0, S5L8900_CLOCK0_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->clock), 1, S5L8900_CLOCK1_BASE);

    s->chipid = qdev_new(TYPE_S5L8900_CHIPID);
    qdev_prop_set_uint8(s->chipid, "security-epoch", s->security_epoch);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->chipid), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->chipid), 0, S5L8900_CHIPID_BASE);

    s->lcd = qdev_new(TYPE_S5L8900_LCD);
    object_property_set_link(OBJECT(s->lcd), "framebuffer-memory",
                             OBJECT(sysmem), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->lcd), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->lcd), 0, S5L8900_LCD_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->lcd), 0,
                       qdev_get_gpio_in(s->vic[0], S5L8900_LCD_IRQ));

    s->mbx = qdev_new(TYPE_S5L8900_MBX);
    object_property_set_link(OBJECT(s->mbx), "system-memory",
                             OBJECT(sysmem), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->mbx), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->mbx), 0, S5L8900_MBX_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->mbx), 0,
                       qdev_get_gpio_in(s->vic[0], S5L8900_MBX_IRQ));

    s->tvout = qdev_new(TYPE_S5L8900_TVOUT);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->tvout), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->tvout), 0,
                    S5L8900_TVOUT_BANK0_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->tvout), 1,
                    S5L8900_TVOUT_BANK1_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->tvout), 2,
                    S5L8900_TVOUT_BANK2_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->tvout), 0,
                       qdev_get_gpio_in(s->vic[1], S5L8900_TVOUT_IRQ - 32));

    s->timer = qdev_new(TYPE_S5L8900_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->timer), 0, S5L8900_TIMER_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->timer), 0,
                       qdev_get_gpio_in(s->vic[0], S5L8900_TIMER_IRQ));

    s->usb = qdev_new(TYPE_S5L8900_USB);
    qdev_connect_gpio_out_named(s->usb, "cable-present", 0,
                                qdev_get_gpio_in_named(DEVICE(pmu),
                                                       "usb-power-present",
                                                       0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->usb), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->usb), 0, S5L8900_USB_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->usb), 1, S5L8900_USB_PHY_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->usb), 0,
                       qdev_get_gpio_in(s->vic[0], S5L8900_USB_IRQ));

    s->watchdog = qdev_new(TYPE_S5L8900_WATCHDOG);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->watchdog), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->watchdog), 0,
                    S5L8900_WATCHDOG_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->watchdog), 0,
                       qdev_get_gpio_in(s->vic[1],
                                        S5L8900_WATCHDOG_IRQ - 32));

    memory_region_add_subregion(sysmem, 0, machine->ram);
    memory_region_init_alias(&s->ram_alias, OBJECT(machine),
                             "iphone3g.ram-alias", machine->ram, 0,
                             IPHONE3G_RAM_SIZE);
    memory_region_add_subregion(sysmem, S5L8900_RAM_ALIAS_BASE,
                                &s->ram_alias);
    memory_region_init_ram(&s->iboot_ram, NULL, "iphone3g.iboot-ram",
                           S5L8900_IBOOT_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, S5L8900_IBOOT_RAM_BASE,
                                &s->iboot_ram);
    memory_region_init_ram(&s->sram, NULL, "iphone3g.sram",
                           S5L8900_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, S5L8900_SRAM_BASE, &s->sram);

    memory_region_init_rom(&s->bootrom, NULL,
                           "iphone3g.bootrom", S5L8900_BOOTROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, S5L8900_BOOTROM_BASE, &s->bootrom);
    memory_region_init_alias(&s->bootrom_reset_alias, OBJECT(machine),
                             "iphone3g.bootrom-reset-alias", &s->bootrom, 0,
                             S5L8900_BOOTROM_SIZE);
    memory_region_add_subregion_overlap(sysmem, 0,
                                        &s->bootrom_reset_alias, 1);

    s->gpio = qdev_new(TYPE_S5L8900_GPIO);
    qdev_prop_set_uint8(s->gpio, "security-epoch", s->security_epoch);
    object_property_add_child(OBJECT(machine), "syscon", OBJECT(s->gpio));
    object_property_set_link(OBJECT(s->gpio), "vrom-alias",
                             OBJECT(&s->bootrom_reset_alias), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gpio), 0, S5L8900_SYSIC_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gpio), 1, S5L8900_GPIO_BASE);
    for (unsigned group = 0; group < S5L8900_GPIO_INTERRUPT_GROUPS;
         group++) {
        static const unsigned irq[] = { 33, 32, 31, 3, 2, 1, 0 };
        unsigned number = irq[group];

        sysbus_connect_irq(SYS_BUS_DEVICE(s->gpio), group,
                           qdev_get_gpio_in(s->vic[number / 32],
                                            number % 32));
    }
    qdev_connect_gpio_out_named(s->gpio, "pin-output",
                                S5L8900_NOR_CS_PIN,
                                qdev_get_gpio_in_named(s->nor, SSI_GPIO_CS,
                                                       0));
    qdev_connect_gpio_out_named(s->gpio, "pin-output",
                                S5L8900_PANEL_CS_PIN,
                                qdev_get_gpio_in_named(s->panel, SSI_GPIO_CS,
                                                       0));
    qdev_connect_gpio_out_named(s->gpio, "pin-output",
                                S5L8900_SERIALIZER_CS_PIN,
                                qdev_get_gpio_in_named(s->serializer,
                                                       SSI_GPIO_CS, 0));
    qdev_connect_gpio_out_named(s->gpio, "pin-output",
                                S5L8900_TOUCH_CS_PIN,
                                qdev_get_gpio_in_named(s->touch, SSI_GPIO_CS,
                                                       0));
    qdev_connect_gpio_out_named(s->gpio, "pin-output",
                                S5L8900_TOUCH_POWER_PIN,
                                qdev_get_gpio_in_named(s->touch, "power", 0));
    qdev_connect_gpio_out_named(s->gpio, "pin-output",
                                S5L8900_TOUCH_RESET_PIN,
                                qdev_get_gpio_in_named(s->touch, "reset", 0));
    qdev_connect_gpio_out_named(s->touch, "atn", 0,
                                qdev_get_gpio_in_named(s->gpio, "pin",
                                                       S5L8900_TOUCH_ATN_PIN));
    qdev_connect_gpio_out_named(DEVICE(als), "irq", 0,
                                qdev_get_gpio_in_named(s->gpio, "pin",
                                                       S5L8900_ALS_IRQ_PIN));
    qdev_connect_gpio_out_named(DEVICE(pmu), "irq", 0,
                                qdev_get_gpio_in_named(s->gpio, "pin",
                                                       S5L8900_PMU_IRQ_PIN));

    s->buttons = qdev_new(TYPE_S5L8900_BUTTONS);
    object_property_add_child(OBJECT(machine), "buttons", OBJECT(s->buttons));
    qdev_connect_gpio_out_named(s->buttons, "pin-level", S5L8900_BUTTON_HOLD,
                                qdev_get_gpio_in_named(s->gpio, "external-pin",
                                      S5L8900_BUTTON_HOLD_PIN));
    qdev_connect_gpio_out_named(s->buttons, "pin-level", S5L8900_BUTTON_MENU,
                                qdev_get_gpio_in_named(s->gpio, "external-pin",
                                      S5L8900_BUTTON_MENU_PIN));
    qdev_connect_gpio_out_named(s->buttons, "pin-level",
                                S5L8900_BUTTON_VOLUME_UP,
                                qdev_get_gpio_in_named(s->gpio, "external-pin",
                                      S5L8900_BUTTON_VOLUP_PIN));
    qdev_connect_gpio_out_named(s->buttons, "pin-level",
                                S5L8900_BUTTON_VOLUME_DOWN,
                                qdev_get_gpio_in_named(s->gpio, "external-pin",
                                      S5L8900_BUTTON_VOLDOWN_PIN));
    qdev_connect_gpio_out_named(s->buttons, "pin-level",
                                S5L8900_BUTTON_RINGER,
                                qdev_get_gpio_in_named(s->gpio, "external-pin",
                                      S5L8900_BUTTON_RINGER_PIN));
    qdev_connect_gpio_out_named(s->buttons, "interrupt-level",
                                S5L8900_BUTTON_HOLD,
                                qdev_get_gpio_in_named(s->gpio, "interrupt", 45));
    qdev_connect_gpio_out_named(s->buttons, "interrupt-level",
                                S5L8900_BUTTON_MENU,
                                qdev_get_gpio_in_named(s->gpio, "interrupt", 40));
    qdev_connect_gpio_out_named(s->buttons, "interrupt-level",
                                S5L8900_BUTTON_VOLUME_UP,
                                qdev_get_gpio_in_named(s->gpio, "interrupt", 41));
    qdev_connect_gpio_out_named(s->buttons, "interrupt-level",
                                S5L8900_BUTTON_VOLUME_DOWN,
                                qdev_get_gpio_in_named(s->gpio, "interrupt", 42));
    qdev_connect_gpio_out_named(s->buttons, "interrupt-level",
                                S5L8900_BUTTON_RINGER,
                                qdev_get_gpio_in_named(s->gpio, "interrupt", 43));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->buttons), &error_fatal);

    iphone3g_load_bootrom(s, machine);
}

static void iphone3g_machine_instance_init(Object *obj)
{
    IPhone3GMachineState *s = IPHONE3G_MACHINE(obj);

    s->security_epoch = 5;
}

static void iphone3g_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("arm1176"),
        NULL,
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Apple iPhone 3G (iPhone1,2 / N82AP)";
    mc->init = iphone3g_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = IPHONE3G_RAM_SIZE;
    mc->default_ram_id = "iphone3g.ram";
    mc->max_cpus = 1;
    mc->no_floppy = true;
    mc->no_cdrom = true;

    object_class_property_add(oc, "security-epoch", "uint8",
                              iphone3g_get_security_epoch,
                              iphone3g_set_security_epoch, NULL, NULL);
    object_class_property_set_description(
        oc, "security-epoch",
        "Set the S5L8900 security epoch exposed by CHIPID and SYSIC");
}

static const TypeInfo iphone3g_machine_type = {
    .name = TYPE_IPHONE3G_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(IPhone3GMachineState),
    .instance_init = iphone3g_machine_instance_init,
    .class_init = iphone3g_machine_class_init,
    .interfaces = arm_machine_interfaces,
};

static void iphone3g_machine_register_types(void)
{
    type_register_static(&iphone3g_machine_type);
}

type_init(iphone3g_machine_register_types)
