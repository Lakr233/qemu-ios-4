/*
 * QTest tests for the Apple iPhone 3G machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "libqtest-single.h"

#define RAM_ALIAS_BASE       0x08000000
#define IBOOT_RAM_BASE       0x18000000
#define SRAM_BASE            0x22000000
#define SRAM_SIZE            (256 * 1024)
#define VIC0_BASE            0x38e00000
#define VIC1_BASE            0x38e01000
#define EDGEIC_BASE          0x38e02000
#define DMAC0_BASE           0x38200000
#define DMAC1_BASE           0x39900000
#define NAND_BASE            0x38a00000
#define NAND_ECC_BASE        0x38f00000
#define SDIO_BASE            0x38d00000
#define CLOCK0_BASE          0x38100000
#define CLOCK1_BASE          0x3c500000
#define CHIPID_BASE          0x3e500000
#define GPIO_BASE            0x3e400000
#define LCD_BASE             0x38900000
#define SYSIC_BASE           0x39a00000
#define TIMER_BASE           0x3e200000
#define TIMER_IRQ            7
#define USB_BASE             0x38400000
#define USB_PHY_BASE         0x3c400000
#define WATCHDOG_BASE        0x3e300000
#define VIC_IRQ_STATUS       0x000
#define VIC_INT_ENABLE       0x010
#define VIC_SOFT_INT         0x018
#define VIC_SOFT_INT_CLEAR   0x01c
#define VIC_SW_PRIORITY_MASK 0x024
#define VIC_DAISY_PRIORITY   0x028
#define VIC_VECTOR_ADDRESS   0x100
#define VIC_VECTOR_PRIORITY  0x200
#define VIC_ADDRESS          0xf00
#define VIC_PERIPH_ID        0xfe0
#define EDGEIC_CONFIG0       0x000
#define EDGEIC_CONFIG1       0x004
#define EDGEIC_LOW_STATUS    0x008
#define EDGEIC_HIGH_STATUS   0x00c
#define EDGEIC_LOW_VIC_IRQ   35
#define EDGEIC_HIGH_VIC_IRQ  41
#define LCD_ENABLE           0x000
#define LCD_DISABLE          0x004
#define LCD_CON2             0x008
#define LCD_CONTROL_ENABLE   BIT(0)
#define LCD_INTERRUPT_ENABLE 0x014
#define LCD_INTERRUPT_STATUS 0x018
#define LCD_INTERRUPT_FRAME  BIT(0)
#define LCD_WINDOW1          0x058
#define LCD_WINDOW2          0x070
#define LCD_WINDOW3          0x088
#define LCD_WINDOW_HSPAN     0x000
#define LCD_WINDOW_FORMAT    0x004
#define LCD_WINDOW_ADDRESS   0x008
#define LCD_WINDOW_SIZE      0x00c
#define LCD_VIDCON0          0x200
#define LCD_VIDCON1          0x204
#define LCD_VIDTCON0         0x20c
#define LCD_VIDTCON1         0x210
#define LCD_VIDTCON2         0x214
#define LCD_VIDTCON3         0x218
#define LCD_WIDTH             320
#define LCD_HEIGHT            480
#define LCD_RGB565            3
#define LCD_ARGB8888          7
#define LCD_WINDOW2_ENABLE    BIT(5)
#define LCD_WINDOW1_ENABLE    BIT(6)
#define LCD_WINDOW3_ENABLE    BIT(4)
#define LCD_FRAME_NS          16673333
#define LCD_IRQ               13
#define DMAC_INT_STATUS       0x000
#define DMAC_INT_TC_STATUS    0x004
#define DMAC_INT_TC_CLEAR     0x008
#define DMAC_ENABLED_CHANNELS 0x01c
#define DMAC_CONFIG           0x030
#define DMAC_CHANNEL0         0x100
#define DMAC_CHANNEL_STRIDE   0x020
#define DMAC_CHANNEL_SRC      0x000
#define DMAC_CHANNEL_DEST     0x004
#define DMAC_CHANNEL_LLI      0x008
#define DMAC_CHANNEL_CONTROL  0x00c
#define DMAC_CHANNEL_CONFIG   0x010
#define DMAC_CONTROL_TC_IRQ   BIT(31)
#define DMAC_CONTROL_DEST_INC BIT(27)
#define DMAC_CONTROL_SRC_INC  BIT(26)
#define DMAC_CONTROL_HALF_SRC (1U << 18)
#define DMAC_CONTROL_HALF_DST (1U << 21)
#define DMAC_CONTROL_WORD_SRC (2U << 18)
#define DMAC_CONTROL_WORD_DST (2U << 21)
#define DMAC_CHANNEL_TC_IRQ   BIT(15)
#define DMAC_CHANNEL_ENABLE   BIT(0)
#define DMAC_FLOW_M2P         (1U << 11)
#define DMAC_FLOW_P2M         (2U << 11)
#define DMAC_SOURCE_PERIPH(x) ((x) << 1)
#define DMAC_DEST_PERIPH(x)   ((x) << 6)
#define DMAC_SPI1_TX_REQUEST  12
#define DMAC0_IRQ             16
#define DMAC_NAND_REQUEST     2
#define I2S0_BASE             0x3ca00000
#define I2S1_BASE             0x3cd00000
#define I2S_CLKCON            0x000
#define I2S_TXCON             0x004
#define I2S_TXCOM             0x008
#define I2S_TXDATA            0x010
#define I2S_RXCON             0x030
#define I2S_RXCOM             0x034
#define I2S_RXDATA            0x038
#define I2S_STATUS            0x03c
#define I2S_CLOCK_ENABLE      BIT(0)
#define I2S_DMA_ENABLE        BIT(1)
#define I2S_INTERFACE_ENABLE  BIT(2)
#define NAND_FMCTRL0          0x000
#define NAND_FMCTRL1          0x004
#define NAND_COMMAND          0x008
#define NAND_FMADDR0          0x00c
#define NAND_FMADDR1          0x010
#define NAND_FMANUM           0x02c
#define NAND_FMDNUM           0x030
#define NAND_FMCSTAT          0x048
#define NAND_FIFO             0x080
#define NAND_FMCTRL1_ADDRESS  BIT(0)
#define NAND_FMCTRL1_READ     BIT(1)
#define NAND_FMCTRL1_FLUSH    (BIT(6) | BIT(7))
#define NAND_STATUS_READY     BIT(0)
#define NAND_STATUS_CMD_DONE  BIT(1)
#define NAND_STATUS_ADDR_DONE BIT(2)
#define NAND_STATUS_XFER_DONE BIT(3)
#define NAND_STATUS_BANK_READY(bank) BIT((bank) + 4)
#define NAND_STATUS_BANK_READY_MASK  (0xffU << 4)
#define NAND_CMD_READ0        0x00
#define NAND_CMD_READ_ID      0x90
#define NAND_CMD_READ_CONFIRM 0x30
#define NAND_CMD_RESET        0xff
#define NAND_DEVICE_ID        0xba94d598
#define NAND_BANKS            4
#define NAND_BLOCKS_PER_BANK  4096
#define NAND_PAGES_PER_BLOCK  128
#define NAND_PAGE_DATA_SIZE   4096
#define NAND_PAGE_SPARE_SIZE  216
#define NAND_PAGE_TOTAL_SIZE  (NAND_PAGE_DATA_SIZE + NAND_PAGE_SPARE_SIZE)
#define NAND_BACKING_SIZE     \
    ((uint64_t)NAND_BANKS * NAND_BLOCKS_PER_BANK * NAND_PAGES_PER_BLOCK * \
     NAND_PAGE_TOTAL_SIZE)
#define NAND_ECC_DATA         0x004
#define NAND_ECC_CODE         0x008
#define NAND_ECC_START        0x00c
#define NAND_ECC_STATUS       0x010
#define NAND_ECC_SETUP        0x014
#define NAND_ECC_CLEARINT     0x040
#define SDIO_CTRL             0x000
#define SDIO_DCTRL            0x004
#define SDIO_COMMAND          0x008
#define SDIO_ARGUMENT         0x00c
#define SDIO_STATE            0x010
#define SDIO_STATUS_ACK       0x014
#define SDIO_DATA_STATUS      0x018
#define SDIO_RESPONSE0        0x020
#define SDIO_CLKDIV           0x030
#define SDIO_CSR              0x034
#define SDIO_COMMAND_START    BIT(31)
#define SDIO_COMMAND_READY    BIT(0)
#define SDIO_COMMAND_DONE     BIT(4)
#define WATCHDOG_CONTROL      0x000
#define WATCHDOG_COUNT        0x004
#define WATCHDOG_ENABLE       BIT(20)
#define WATCHDOG_INTERRUPT    BIT(15)
#define WATCHDOG_CLEAR        0xa00
#define WATCHDOG_DISABLE      0xa5
#define WATCHDOG_IRQ          51
#define WATCHDOG_PERIOD_NS    325771184
#define CLOCK0_CONFIG         0x000
#define CLOCK0_ADJ1           0x008
#define CLOCK0_ADJ2           0x404
#define CLOCK1_CONFIG0        0x000
#define CLOCK1_CONFIG1        0x004
#define CLOCK1_CONFIG2        0x008
#define CLOCK1_PLL0CON        0x020
#define CLOCK1_PLLLOCK        0x040
#define CLOCK1_PLLMODE        0x044
#define CLOCK1_CL2_GATES      0x048
#define CHIPID_SPI_CLOCK_TYPE 0x004
#define CHIPID_SECURITY_INFO  0x008
#define TIMER_4              0x0a0
#define TIMER_CONFIG         0x000
#define TIMER_STATE          0x004
#define TIMER_COUNT_BUFFER   0x008
#define TIMER_TICKS_HIGH     0x080
#define TIMER_TICKS_LOW      0x084
#define TIMER_IRQ_LATCH      0x0f8
#define TIMER_IRQ_STATUS     0x10000
#define SYSIC_POWER_ONCTRL   0x0c
#define SYSIC_POWER_OFFCTRL  0x10
#define SYSIC_POWER_STATE    0x14
#define SYSIC_POWER_ID       0x44
#define SYSIC_POWER_CONFIG2  0x6c
#define SYSIC_MEMORY_CONFIG  0x70
#define SYSIC_MEMORY_STATUS  0x7c
#define SYSIC_POWER_VROM     (1U << 12)
#define SYSIC_POWER_EPOCH_5  0x05000000
#define SYSIC_BOARD_ID_4     0x00040000
#define SYSIC_IPHONE1_2_ID   (SYSIC_POWER_EPOCH_5 | SYSIC_BOARD_ID_4)
#define SYSIC_GPIO_INTLEVEL  0x080
#define SYSIC_GPIO_INTSTAT   0x0a0
#define SYSIC_GPIO_INTEN     0x0c0
#define SYSIC_GPIO_INTTYPE   0x0e0
#define GPIO_PAD_STRIDE      0x020
#define GPIO_DAT             0x004
#define GPIO_FSEL            0x320
#define GPIO_FUNCTION_LOW    0xe
#define GPIO_FUNCTION_HIGH   0xf
#define ALS_IRQ_PIN          73
#define PMU_IRQ_PIN          85
#define BUTTON_PAD           22
#define BUTTON_HOLD_PIN      5
#define BUTTON_MENU_PIN      0
#define BUTTON_VOLUP_PIN     1
#define BUTTON_VOLDOWN_PIN   2
#define BUTTON_RINGER_PIN    3
#define BUTTON_HOLD_LINE     45
#define BUTTON_MENU_LINE     40
#define BUTTON_VOLUP_LINE    41
#define BUTTON_VOLDOWN_LINE  42
#define BUTTON_RINGER_LINE   43
#define SPI0_BASE            0x3c300000
#define SPI1_BASE            0x3ce00000
#define SPI0_IRQ             9
#define SPI1_IRQ             10
#define SPI_CONTROL          0x000
#define SPI_SETUP            0x004
#define SPI_STATUS           0x008
#define SPI_TXDATA           0x010
#define SPI_RXDATA           0x020
#define SPI_CLOCK_DIVIDER    0x030
#define SPI_TRANSFER_COUNT   0x034
#define SPI_CONTROL_ENABLE   BIT(0)
#define SPI_CONTROL_RX_RESET BIT(2)
#define SPI_CONTROL_TX_RESET BIT(3)
#define SPI_SETUP_DMA        BIT(6)
#define SPI_SETUP_TX_SERVICE BIT(7)
#define SPI_SETUP_RX_SERVICE BIT(8)
#define SPI_STATUS_RX_SERVICE BIT(0)
#define SPI_STATUS_TX_SERVICE BIT(1)
#define SPI_STATUS_TX_COUNT_SHIFT 4
#define SPI_STATUS_RX_COUNT_SHIFT 8
#define NOR_CS_PAD           4
#define NOR_CS_INDEX         0
#define PANEL_CS_PIN         (7 * 8 + 5)
#define SERIALIZER_CS_PIN    (7 * 8 + 6)
#define NOR_SIZE             (1024 * 1024)
#define NOR_SENTINEL_OFFSET  0xfc000
#define NOR_READ             0x03
#define NOR_READ_JEDEC_ID    0x9f
#define NOR_WRITE_DISABLE    0x04
#define NOR_WRITE_ENABLE     0x06
#define NOR_ERASE_4K         0x20
#define NOR_AAI_WORD_PROGRAM 0xad
#define TOUCH_RESET_PIN      (6 * 8 + 6)
#define TOUCH_POWER_PIN      (7 * 8 + 1)
#define TOUCH_ATN_PIN        155
#define TOUCH_CS_PIN         (24 * 8)
#define TOUCH_FAMILY_ID      0x52
#define TOUCH_MAX_PACKET     660
#define TOUCH_FRAME_SIZE     42
#define TOUCH_FINGER_OFFSET  10
#define TOUCH_FINGER_SIZE    32
#define UART_BASE            0x3cc00000
#define UART_STRIDE          0x00004000
#define UART_PORTS           5
#define UART0_IRQ            24
#define UART_UCON            0x004
#define UART_UFCON           0x008
#define UART_UTRSTAT         0x010
#define UART_UFSTAT          0x018
#define UART_UMSTAT          0x01c
#define UART_UTXH            0x020
#define UART_URXH            0x024
#define UART_UBAUD           0x028
#define UART_UDIVSLOT        0x02c
#define UART_UCON_RX_IRQ     BIT(0)
#define UART_UCON_LOOPBACK   BIT(5)
#define UART_UFCON_ENABLE    BIT(0)
#define UART_UFCON_RX_RESET  BIT(1)
#define UART_UFCON_TX_RESET  BIT(2)
#define UART_UTRSTAT_RX_READY BIT(0)
#define UART_UTRSTAT_TX_EMPTY (BIT(1) | BIT(2))
#define UART_UFSTAT_RX_FULL  BIT(8)
#define UART_UMSTAT_CTS      BIT(0)
#define AES_BASE             0x38c00000
#define AES_CONTROL          0x000
#define AES_GO               0x004
#define AES_STATUS           0x00c
#define AES_IRQ_ENABLE       0x010
#define AES_KEYLEN           0x014
#define AES_TRANSFER_SIZE    0x018
#define AES_OUTPUT_ADDRESS   0x020
#define AES_OUTPUT_CAPACITY  0x024
#define AES_INPUT_ADDRESS    0x028
#define AES_INPUT_CAPACITY   0x02c
#define AES_AUXILIARY_ADDRESS 0x030
#define AES_SIZE3            0x034
#define AES_KEY              0x04c
#define AES_KEY_TYPE         0x06c
#define AES_IV               0x074
#define AES_STATUS_DONE      BIT(0)
#define AES_STATUS_OUTPUT_REQ BIT(1)
#define AES_STATUS_INPUT_REQ BIT(2)
#define AES_KEYLEN_CONSUMED_MODE 6
#define AES_KEYLEN_ENCRYPT   BIT(0)
#define AES_CUSTOM_KEY       0
#define AES_GID_KEY          1
#define AES_IRQ              39
#define AES_COMPLETION_NS    1
#define AES_KBAG_BUNDLE_SIZE (12 + 4 + 64)
#define MBX_BASE             0x3b000000
#define MBX_STATUS           0x012c
#define MBX_INTERRUPT_MASK   0x0130
#define MBX_STATUS_ACK       0x0134
#define MBX_ID               0x0f00
#define MBX_RESET            0x1020
#define MBX_RESET_REQUEST    BIT(0)
#define MBX_RESET_DONE       BIT(16)
#define MBX_COMMAND_MEMORY   0xa00000
#define MBX_IRQ              12
#define MBX_2D_HEADER        0xa0060500
#define MBX_2D_SUBMIT        0xf0000000
#define MBX_2D_END           0x70000000
#define MBX_3D_FIFO          0x800000
#define MBX_3D_SUBMIT        0xf0000000
#define MBX_TA_START         0x0800
#define MBX_TA_CONTEXT_LOAD  0x0814
#define MBX_TA_CONTEXT_STORE 0x0818
#define MBX_TA_CONTEXT_RESET 0x081c
#define MBX_TA_OBJECT_DB     0x083c
#define TVOUT_BANK0_BASE     0x39300000
#define TVOUT_BANK1_BASE     0x39200000
#define TVOUT_BANK2_BASE     0x39100000

static const uint8_t aes_test_kbag_wrapped[32] = {
    0x2f, 0x7a, 0x3e, 0x61, 0xf3, 0xd3, 0x3b, 0x83,
    0xbd, 0xc3, 0x04, 0x9d, 0x05, 0xa6, 0x25, 0x82,
    0xd9, 0x6d, 0xe7, 0x0e, 0x59, 0x29, 0x26, 0x0a,
    0x47, 0x71, 0x8b, 0x73, 0xb7, 0x2f, 0x2a, 0x5e,
};

static const uint8_t aes_test_kbag_clear[32] = {
    0x63, 0xe4, 0xf6, 0x10, 0x7e, 0x33, 0x37, 0x91,
    0x00, 0xb2, 0xf3, 0xe1, 0xcc, 0x0f, 0xdd, 0x94,
    0x89, 0x95, 0x04, 0xf0, 0x5b, 0x25, 0x4e, 0x54,
    0x3d, 0x98, 0xfc, 0xff, 0x61, 0xd1, 0xdc, 0x03,
};
#define SHA1_BASE            0x38000000
#define PKE_BASE             0x3d000000
#define PKE_COMMAND          0x008
#define PKE_OPERATION        0x00c
#define PKE_SEGMENT_CONFIG   0x014
#define PKE_SEGMENT_BASE     0x800
#define SHA1_CONFIG          0x000
#define SHA1_RESET           0x004
#define SHA1_IRQ_ACK         0x008
#define SHA1_IRQ_ENABLE      0x00c
#define SHA1_DIGEST          0x020
#define SHA1_DATA            0x040
#define SHA1_DMA_CONTROL     0x080
#define SHA1_DMA_ADDRESS     0x084
#define SHA1_DMA_LENGTH      0x08c
#define SHA1_START           BIT(1)
#define SHA1_COMPLETION_IRQ  BIT(2)
#define SHA1_CUSTOM_IV       BIT(3)
#define SHA1_IRQ             40
#define ADM_BASE             0x38800000
#define ADM_CONTROL          0x000
#define ADM_COMMAND          0x004
#define ADM_UPLOAD_DATA      0x010
#define ADM_EVENT_DATA       0x030
#define ADM_UPLOAD_ACTION0   0x050
#define ADM_EVENT_ACTION0    0x054
#define ADM_UPLOAD_ACTION2   0x06c
#define ADM_EVENT_ACTION2    0x088
#define ADM_EVENT_ACTION3    0x08c
#define ADM_CONTROL_RUNNING  BIT(0)
#define ADM_CONTROL_READY    BIT(1)
#define ADM_CONTROL_RESET    BIT(2)
#define ADM_IRQ_EVENT        BIT(4)
#define ADM_IRQ_COMMAND      BIT(5)
#define ADM_IRQ_UPLOAD       BIT(6)
#define ADM_IRQ              37
#define ADM_FMC_COMPLETION_NS 1000
#define ADM_FMC_DMA_COMPLETION_NS 1000
#define ADM_FMC_DMA_ARM_GRACE_NS 1000000
#define ADM_FMC_DMA_SEGMENT_NS 1000000
#define ADM_FMC_COMMAND_OFFSET 0x1104
#define ADM_FMC_OPCODE       0x024
#define ADM_FMC_PAGE_COUNT   0x028
#define ADM_FMC_PAD_SIZE     0x02c
#define ADM_FMC_CE_COUNT     0x030
#define ADM_FMC_ERASE_BANK   0x034
#define ADM_FMC_BANKS        0x044
#define ADM_FMC_PAGES        0x244
#define ADM_FMC_DESCRIPTORS  0xa44
#define ADM_FMC_TEST_COMMAND_SIZE (ADM_FMC_DESCRIPTORS + 2 * 8)
#define ADM_FMC_BANK_SLOTS   8
#define ADM_FMC_MAX_PAGES    512
#define ADM_FMC_INIT         0x100
#define ADM_FMC_READ         0x200
#define ADM_FMC_READ_MAX_ECC 0x300
#define ADM_FMC_WRITE        0x400
#define ADM_FMC_WRITE_MAX_ECC 0x500
#define ADM_FMC_ERASE        0x600
#define ADM_FMC_READ_RESULT_EMPTY 0xfe
#define I2C0_BASE            0x3c600000
#define I2C1_BASE            0x3c900000
#define I2C_CONTROL          0x000
#define I2C_STATUS           0x004
#define I2C_ADDRESS          0x008
#define I2C_DATA             0x00c
#define I2C_LINE_CONTROL     0x010
#define I2C_OPERATION_IRQ_CONTROL 0x014
#define I2C_OPERATION_STATUS 0x020
#define I2C_CONTROL_IRQ_ENABLE BIT(5)
#define I2C_CONTROL_CONTINUE BIT(4)
#define I2C_CONTROL_ACK_GENERATE BIT(7)
#define I2C_OPERATION_IRQ_ENABLE BIT(0)
#define I2C_STATUS_MASTER_RX (2 << 6)
#define I2C_STATUS_MASTER_TX (3 << 6)
#define I2C_STATUS_START     BIT(5)
#define I2C_STATUS_OUTPUT    BIT(4)
#define I2C_STATUS_NACK      BIT(0)
#define I2C_OPERATION_TRANSFER BIT(8)
#define I2C_OPERATION_CONDITION BIT(13)
#define I2C0_IRQ             21
#define USB_GAHBCFG          0x008
#define USB_GRSTCTL          0x010
#define USB_GINTSTS          0x014
#define USB_GINTMSK          0x018
#define USB_GHWCFG1          0x044
#define USB_GHWCFG2          0x048
#define USB_GHWCFG3          0x04c
#define USB_GHWCFG4          0x050
#define USB_DCTL             0x804
#define USB_DIEPMSK          0x810
#define USB_DAINTMSK         0x81c
#define USB_IN_EP0_CONTROL   0x900
#define USB_IN_EP0_INTERRUPT 0x908
#define USB_OUT_EP0_CONTROL  0xb00
#define USB_AHB_IDLE         BIT(31)
#define USB_GLOBAL_IRQ       BIT(0)
#define USB_IN_EP_IRQ        BIT(18)
#define USB_EP_ENABLE        BIT(31)
#define USB_EP_DISABLE       BIT(30)
#define USB_EP_SET_NAK       BIT(27)
#define USB_EP_CLEAR_NAK     BIT(26)
#define USB_EP_NAK_STATUS    BIT(17)
#define USB_EP_ACTIVE        BIT(15)
#define USB_IN_NAK_EFFECTIVE BIT(6)
#define USB_EP_DISABLED      BIT(1)
#define USB_SET_GLOBAL_IN_NAK BIT(7)
#define USB_CLEAR_GLOBAL_IN_NAK BIT(8)
#define USB_SET_GLOBAL_OUT_NAK BIT(9)
#define USB_CLEAR_GLOBAL_OUT_NAK BIT(10)
#define USB_GLOBAL_IN_NAK_STATUS BIT(2)
#define USB_GLOBAL_OUT_NAK_STATUS BIT(3)

static void test_sysic_identity(void)
{
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_POWER_ID), ==,
                    SYSIC_IPHONE1_2_ID);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_POWER_STATE), ==, 0);
}

static void test_sysic_memory_ready(void)
{
    qtest_system_reset(global_qtest);
    writel(SYSIC_BASE + SYSIC_MEMORY_CONFIG, 0x32);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_MEMORY_CONFIG), ==, 0x32);

    writel(SYSIC_BASE + SYSIC_POWER_CONFIG2, 3);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_MEMORY_STATUS) & BIT(0), ==,
                    BIT(0));
    writel(SYSIC_BASE + SYSIC_POWER_CONFIG2, 2);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_MEMORY_STATUS) & BIT(0), ==, 0);
}

static void set_gpio_input(unsigned pin, bool level)
{
    qtest_set_irq_in(global_qtest, "/machine/syscon", "pin", pin, level);
}

static void test_gpio_edge_and_level_interrupts(void)
{
    const unsigned edge_pin = 5;
    const unsigned level_pin = 6;
    const uint32_t edge_bit = BIT(edge_pin);
    const uint32_t level_bit = BIT(level_pin);

    qtest_system_reset(global_qtest);
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(1));
    writel(SYSIC_BASE + SYSIC_GPIO_INTLEVEL, edge_bit | level_bit);
    writel(SYSIC_BASE + SYSIC_GPIO_INTTYPE, level_bit);
    writel(SYSIC_BASE + SYSIC_GPIO_INTEN, edge_bit | level_bit);

    set_gpio_input(edge_pin, true);
    g_assert_cmphex(readl(GPIO_BASE + GPIO_DAT) & edge_bit, ==, edge_bit);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT), ==, edge_bit);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, BIT(1));

    writel(SYSIC_BASE + SYSIC_GPIO_INTSTAT, edge_bit);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT), ==, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
    set_gpio_input(edge_pin, false);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT), ==, 0);

    set_gpio_input(level_pin, true);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT), ==, level_bit);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, BIT(1));
    writel(SYSIC_BASE + SYSIC_GPIO_INTSTAT, level_bit);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT), ==, level_bit);
    writel(SYSIC_BASE + SYSIC_GPIO_INTEN, edge_bit);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT), ==, 0);
    writel(SYSIC_BASE + SYSIC_GPIO_INTEN, edge_bit | level_bit);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT), ==, level_bit);
    set_gpio_input(level_pin, false);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT), ==, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_gpio_function_select_output(void)
{
    const unsigned pad = 22;
    const unsigned index = 5;
    const uint32_t bit = BIT(index);
    const uint32_t fsel = (pad << 16) | (index << 8);
    const uint64_t data = GPIO_BASE + pad * GPIO_PAD_STRIDE + GPIO_DAT;

    qtest_system_reset(global_qtest);
    writel(GPIO_BASE + GPIO_FSEL, fsel | GPIO_FUNCTION_HIGH);
    g_assert_cmphex(readl(data) & bit, ==, bit);
    writel(GPIO_BASE + GPIO_FSEL, fsel | GPIO_FUNCTION_LOW);
    g_assert_cmphex(readl(data) & bit, ==, 0);
}

static void send_key(const char *qcode, bool down)
{
    qtest_qmp_assert_success(
        global_qtest,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'key','data':{'down':%i,'key':"
        "{'type':'qcode','data':%s}}}]}}",
        down, qcode);
}

static void test_gpio_n82_buttons(void)
{
    const uint64_t data = GPIO_BASE + BUTTON_PAD * GPIO_PAD_STRIDE +
                          GPIO_DAT;
    const unsigned group = 1;
    const uint32_t hold = BIT(BUTTON_HOLD_LINE % 32);
    const uint32_t menu = BIT(BUTTON_MENU_LINE % 32);
    const uint32_t volup = BIT(BUTTON_VOLUP_LINE % 32);
    const uint32_t voldown = BIT(BUTTON_VOLDOWN_LINE % 32);
    const uint32_t ringer = BIT(BUTTON_RINGER_LINE % 32);
    const uint32_t levels = hold | menu | ringer;
    const uint32_t enabled = hold | menu | volup | voldown | ringer;

    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(data), ==,
                    BIT(BUTTON_VOLUP_PIN) | BIT(BUTTON_VOLDOWN_PIN));

    writel(SYSIC_BASE + SYSIC_GPIO_INTLEVEL + group * 4, levels);
    writel(SYSIC_BASE + SYSIC_GPIO_INTTYPE + group * 4, enabled);
    writel(SYSIC_BASE + SYSIC_GPIO_INTEN + group * 4, enabled);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4),
                    ==, 0);

    send_key("home", true);
    g_assert_cmphex(readl(data), ==,
                    BIT(BUTTON_MENU_PIN) | BIT(BUTTON_VOLUP_PIN) |
                    BIT(BUTTON_VOLDOWN_PIN));
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4),
                    ==, menu);
    send_key("home", false);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4),
                    ==, 0);

    send_key("power", true);
    g_assert_cmphex(readl(data) & BIT(BUTTON_HOLD_PIN), !=, 0);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4),
                    ==, hold);
    send_key("power", false);

    send_key("volumeup", true);
    g_assert_cmphex(readl(data) & BIT(BUTTON_VOLUP_PIN), ==, 0);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4),
                    ==, volup);
    send_key("volumeup", false);

    send_key("audiomute", true);
    send_key("audiomute", false);
    g_assert_cmphex(readl(data) & BIT(BUTTON_RINGER_PIN), !=, 0);
    send_key("audiomute", true);
    send_key("audiomute", false);
    g_assert_cmphex(readl(data) & BIT(BUTTON_RINGER_PIN), ==, 0);
}

static void set_gpio_output(unsigned pin, bool high)
{
    unsigned pad = pin / 8;
    unsigned index = pin % 8;
    uint32_t function = high ? GPIO_FUNCTION_HIGH : GPIO_FUNCTION_LOW;
    uint32_t fsel = (pad << 16) | (index << 8) | function;

    writel(GPIO_BASE + GPIO_FSEL, fsel);
}

static void set_nor_selected(bool selected)
{
    set_gpio_output(NOR_CS_PAD * 8 + NOR_CS_INDEX, !selected);
}

static void spi_reset_fifos(uint32_t base)
{
    writel(base + SPI_CONTROL, SPI_CONTROL_RX_RESET);
    writel(base + SPI_CONTROL, SPI_CONTROL_TX_RESET);
}

static void spi_send(uint32_t base, unsigned irq, const uint8_t *bytes,
                     size_t count)
{
    size_t sent = 0;

    spi_reset_fifos(base);
    while (sent < count) {
        size_t chunk = MIN(count - sent, 8);

        writel(base + SPI_CONTROL, 0);
        for (size_t i = 0; i < chunk; i++) {
            writel(base + SPI_TXDATA, bytes[sent + i]);
        }
        writel(base + SPI_CONTROL, SPI_CONTROL_ENABLE);
        g_assert_cmphex(readl(base + SPI_STATUS), ==,
                        SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE |
                        (chunk << SPI_STATUS_RX_COUNT_SHIFT));
        g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(irq));
        for (size_t i = 0; i < chunk; i++) {
            readl(base + SPI_RXDATA);
        }
        writel(base + SPI_STATUS,
               SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE);
        g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
        sent += chunk;
    }
}

static void spi_receive(uint32_t base, unsigned irq, uint8_t *bytes,
                        size_t count)
{
    size_t received = 0;

    spi_reset_fifos(base);
    writel(base + SPI_SETUP, 1);
    writel(base + SPI_TRANSFER_COUNT, count);
    writel(base + SPI_CONTROL, SPI_CONTROL_ENABLE);
    while (received < count) {
        size_t available = MIN(count - received, 8);
        uint32_t status;

        status = readl(base + SPI_STATUS);
        g_assert_cmphex(status, ==,
                        SPI_STATUS_RX_SERVICE |
                        (available << SPI_STATUS_RX_COUNT_SHIFT));
        g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(irq));
        writel(base + SPI_STATUS, status);
        g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
        for (size_t i = 0; i < available; i++) {
            bytes[received++] = readl(base + SPI_RXDATA);
        }
    }
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void spi_apple_exchange(uint32_t base, unsigned irq, uint32_t setup,
                               const uint8_t *tx, size_t tx_count,
                               uint8_t *rx, size_t rx_count)
{
    size_t transfer_count = MAX(tx_count, rx_count);
    size_t queued = 0;
    size_t received = 0;

    g_assert_cmpuint(transfer_count, >, 0);
    spi_reset_fifos(base);
    writel(base + SPI_SETUP, setup);
    writel(base + SPI_TRANSFER_COUNT, transfer_count);
    while (queued < MIN(transfer_count, 8)) {
        writel(base + SPI_TXDATA,
               queued < tx_count ? tx[queued] : 0xff);
        queued++;
    }
    writel(base + SPI_SETUP,
           setup | SPI_SETUP_TX_SERVICE | SPI_SETUP_RX_SERVICE);

    while (received < transfer_count) {
        uint32_t status = readl(base + SPI_STATUS);
        size_t tx_slots = 8 - ((status >> SPI_STATUS_TX_COUNT_SHIFT) & 0xf);
        size_t available = (status >> SPI_STATUS_RX_COUNT_SHIFT) & 0xf;

        g_assert_cmphex(status & (SPI_STATUS_TX_SERVICE |
                                  SPI_STATUS_RX_SERVICE), ==,
                        SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE);
        g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(irq));
        g_assert_cmpuint(available, >, 0);
        for (size_t i = 0; i < available; i++) {
            uint8_t value = readl(base + SPI_RXDATA);

            if (received < rx_count) {
                rx[received] = value;
            }
            received++;
        }
        while (tx_slots && queued < transfer_count) {
            writel(base + SPI_TXDATA,
                   queued < tx_count ? tx[queued] : 0xff);
            queued++;
            tx_slots--;
        }
        writel(base + SPI_STATUS,
               status & (SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE));
    }

    g_assert_cmphex(readl(base + SPI_TRANSFER_COUNT), ==, 0);
    g_assert_cmphex(readl(base + SPI_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_spi_nor_contract(void)
{
    static const uint8_t expected_id[] = { 0xbf, 0x25, 0x8e };
    static const uint8_t expected_data[] = { 0x67, 0x45, 0x23, 0x01 };
    const uint8_t read_id[] = { NOR_READ_JEDEC_ID };
    const uint8_t read_data[] = {
        NOR_READ,
        (NOR_SENTINEL_OFFSET >> 16) & 0xff,
        (NOR_SENTINEL_OFFSET >> 8) & 0xff,
        NOR_SENTINEL_OFFSET & 0xff,
    };
    uint8_t received[16] = { 0 };

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(9));
    writel(SPI0_BASE + SPI_CLOCK_DIVIDER, 2);
    g_assert_cmphex(readl(SPI0_BASE + SPI_CLOCK_DIVIDER), ==, 2);

    set_nor_selected(false);
    set_gpio_output(PANEL_CS_PIN, true);
    set_gpio_output(SERIALIZER_CS_PIN, true);
    set_nor_selected(true);
    spi_send(SPI0_BASE, SPI0_IRQ, read_id, sizeof(read_id));
    spi_receive(SPI0_BASE, SPI0_IRQ, received, sizeof(expected_id));
    g_assert_cmpmem(received, sizeof(expected_id), expected_id,
                    sizeof(expected_id));

    set_nor_selected(false);
    set_nor_selected(true);
    spi_send(SPI0_BASE, SPI0_IRQ, read_data, sizeof(read_data));
    spi_receive(SPI0_BASE, SPI0_IRQ, received, sizeof(received));
    g_assert_cmpmem(received, sizeof(expected_data), expected_data,
                    sizeof(expected_data));
    for (size_t i = sizeof(expected_data); i < sizeof(received); i++) {
        g_assert_cmphex(received[i], ==, 0xff);
    }
    set_nor_selected(false);
}

static void test_spi_nor_full_duplex(void)
{
    static const uint8_t expected_id[] = { 0xbf, 0x25, 0x8e };
    uint8_t received[sizeof(expected_id)];

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI0_IRQ));
    set_gpio_output(PANEL_CS_PIN, true);
    set_gpio_output(SERIALIZER_CS_PIN, true);
    set_nor_selected(true);

    spi_reset_fifos(SPI0_BASE);
    writel(SPI0_BASE + SPI_SETUP, 1);
    writel(SPI0_BASE + SPI_TXDATA, NOR_READ_JEDEC_ID);
    writel(SPI0_BASE + SPI_TRANSFER_COUNT,
           1 + sizeof(received));
    writel(SPI0_BASE + SPI_CONTROL, SPI_CONTROL_ENABLE);
    g_assert_cmphex(readl(SPI0_BASE + SPI_STATUS), ==,
                    SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE |
                    (1 << SPI_STATUS_RX_COUNT_SHIFT));
    readl(SPI0_BASE + SPI_RXDATA);
    writel(SPI0_BASE + SPI_STATUS,
           SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE);

    g_assert_cmphex(readl(SPI0_BASE + SPI_STATUS), ==,
                    SPI_STATUS_RX_SERVICE |
                    (sizeof(received) << SPI_STATUS_RX_COUNT_SHIFT));
    for (size_t i = 0; i < sizeof(received); i++) {
        received[i] = readl(SPI0_BASE + SPI_RXDATA);
    }
    writel(SPI0_BASE + SPI_STATUS, SPI_STATUS_RX_SERVICE);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    g_assert_cmpmem(received, sizeof(received), expected_id,
                    sizeof(expected_id));
    set_nor_selected(false);
}

static void test_spi_nor_apple_refill_read(void)
{
    enum { DATA_SIZE = 4096, COMMAND_SIZE = 4 };
    static const uint8_t expected[] = { 0x67, 0x45, 0x23, 0x01 };
    uint8_t tx[COMMAND_SIZE + DATA_SIZE];
    uint8_t rx[COMMAND_SIZE + DATA_SIZE];

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI0_IRQ));
    set_gpio_output(PANEL_CS_PIN, true);
    set_gpio_output(SERIALIZER_CS_PIN, true);
    set_nor_selected(true);

    memset(tx, 0xff, sizeof(tx));
    tx[0] = NOR_READ;
    tx[1] = (NOR_SENTINEL_OFFSET >> 16) & 0xff;
    tx[2] = (NOR_SENTINEL_OFFSET >> 8) & 0xff;
    tx[3] = NOR_SENTINEL_OFFSET & 0xff;
    memset(rx, 0, sizeof(rx));
    spi_apple_exchange(SPI0_BASE, SPI0_IRQ, 0x1018,
                       tx, sizeof(tx), rx, sizeof(rx));

    g_assert_cmpmem(rx + COMMAND_SIZE, sizeof(expected),
                    expected, sizeof(expected));
    g_assert_cmphex(rx[COMMAND_SIZE + sizeof(expected)], ==, 0xff);
    set_nor_selected(false);
}

static void test_spi_apple_service_start(void)
{
    uint8_t response[2];

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI0_IRQ));
    set_gpio_output(PANEL_CS_PIN, true);
    set_gpio_output(SERIALIZER_CS_PIN, true);
    set_nor_selected(true);

    spi_reset_fifos(SPI0_BASE);
    writel(SPI0_BASE + SPI_SETUP, 0x1038);
    writel(SPI0_BASE + SPI_TRANSFER_COUNT, 2);
    writel(SPI0_BASE + SPI_TXDATA, 0x05);
    writel(SPI0_BASE + SPI_TXDATA, 0xff);
    g_assert_cmphex(readl(SPI0_BASE + SPI_STATUS), ==, 2 << 4);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);

    writel(SPI0_BASE + SPI_SETUP,
           0x1038 | SPI_SETUP_TX_SERVICE | SPI_SETUP_RX_SERVICE);
    g_assert_cmphex(readl(SPI0_BASE + SPI_STATUS), ==,
                    SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE |
                    (2 << SPI_STATUS_RX_COUNT_SHIFT));
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(SPI0_IRQ));
    for (size_t i = 0; i < ARRAY_SIZE(response); i++) {
        response[i] = readl(SPI0_BASE + SPI_RXDATA);
    }
    g_assert_cmphex(readl(SPI0_BASE + SPI_TRANSFER_COUNT), ==, 0);
    g_assert_cmphex(response[1], ==, 0);
    writel(SPI0_BASE + SPI_STATUS,
           SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE);
    g_assert_cmphex(readl(SPI0_BASE + SPI_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    set_nor_selected(false);
}

static void spi_apple_command(const uint8_t *command, size_t count)
{
    uint32_t status;

    g_assert_cmpuint(count, >, 0);
    g_assert_cmpuint(count, <=, 8);
    set_nor_selected(true);
    spi_reset_fifos(SPI0_BASE);
    writel(SPI0_BASE + SPI_SETUP, 0x1038);
    writel(SPI0_BASE + SPI_TRANSFER_COUNT, count);
    for (size_t i = 0; i < count; i++) {
        writel(SPI0_BASE + SPI_TXDATA, command[i]);
    }
    g_assert_cmphex(readl(SPI0_BASE + SPI_STATUS), ==,
                    count << SPI_STATUS_TX_COUNT_SHIFT);

    writel(SPI0_BASE + SPI_SETUP,
           0x1038 | SPI_SETUP_TX_SERVICE | SPI_SETUP_RX_SERVICE);
    status = readl(SPI0_BASE + SPI_STATUS);
    g_assert_cmphex(status, ==,
                    SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE |
                    (count << SPI_STATUS_RX_COUNT_SHIFT));
    g_assert_cmphex(readl(SPI0_BASE + SPI_TRANSFER_COUNT), ==, 0);
    for (size_t i = 0; i < count; i++) {
        readl(SPI0_BASE + SPI_RXDATA);
    }
    writel(SPI0_BASE + SPI_STATUS,
           SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE);
    status = readl(SPI0_BASE + SPI_STATUS);
    g_assert_cmphex(status, ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    set_nor_selected(false);
}

static void test_spi_nor_apple_aai_word_program(void)
{
    static const uint32_t address = 0x1000;
    static const uint8_t expected[] = { 0x32, 0x47, 0x4d, 0x49, 0xff };
    const uint8_t erase[] = {
        NOR_ERASE_4K,
        (address >> 16) & 0xff,
        (address >> 8) & 0xff,
        address & 0xff,
    };
    const uint8_t first_word[] = {
        NOR_AAI_WORD_PROGRAM,
        (address >> 16) & 0xff,
        (address >> 8) & 0xff,
        address & 0xff,
        expected[0], expected[1],
    };
    const uint8_t second_word[] = {
        NOR_AAI_WORD_PROGRAM, expected[2], expected[3],
    };
    const uint8_t read[] = {
        NOR_READ,
        (address >> 16) & 0xff,
        (address >> 8) & 0xff,
        address & 0xff,
    };
    const uint8_t write_enable[] = { NOR_WRITE_ENABLE };
    const uint8_t write_disable[] = { NOR_WRITE_DISABLE };
    uint8_t actual[sizeof(expected)];

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI0_IRQ));
    set_gpio_output(PANEL_CS_PIN, true);
    set_gpio_output(SERIALIZER_CS_PIN, true);

    spi_apple_command(write_enable, sizeof(write_enable));
    spi_apple_command(erase, sizeof(erase));
    spi_apple_command(write_enable, sizeof(write_enable));
    spi_apple_command(first_word, sizeof(first_word));
    spi_apple_command(second_word, sizeof(second_word));
    spi_apple_command(write_disable, sizeof(write_disable));

    writel(SPI0_BASE + SPI_SETUP, 0);
    set_nor_selected(true);
    spi_send(SPI0_BASE, SPI0_IRQ, read, sizeof(read));
    spi_receive(SPI0_BASE, SPI0_IRQ, actual, sizeof(actual));
    set_nor_selected(false);
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
}

static void test_spi_merlot_panel_id(void)
{
    static const uint8_t commands[] = { 0xda, 0xdb, 0xdc };
    static const uint8_t expected[] = { 0x1a, 0xc2, 0xb3 };
    const uint8_t enter_register_mode = 0xde;
    const uint8_t read_status = 0x95;
    uint8_t response;

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI0_IRQ));
    set_nor_selected(false);
    set_gpio_output(SERIALIZER_CS_PIN, true);

    for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
        set_gpio_output(PANEL_CS_PIN, false);
        spi_send(SPI0_BASE, SPI0_IRQ, &commands[i], 1);
        spi_receive(SPI0_BASE, SPI0_IRQ, &response, 1);
        set_gpio_output(PANEL_CS_PIN, true);
        g_assert_cmphex(response, ==, expected[i]);
    }

    set_gpio_output(PANEL_CS_PIN, false);
    spi_send(SPI0_BASE, SPI0_IRQ, &enter_register_mode, 1);
    set_gpio_output(PANEL_CS_PIN, true);
    set_gpio_output(PANEL_CS_PIN, false);
    spi_send(SPI0_BASE, SPI0_IRQ, &read_status, 1);
    spi_receive(SPI0_BASE, SPI0_IRQ, &response, 1);
    set_gpio_output(PANEL_CS_PIN, true);
    g_assert_cmphex(response, ==, 1);
}

static void spi_apple_tx(uint32_t base, uint8_t byte)
{
    uint32_t status;

    spi_reset_fifos(base);
    writel(base + SPI_TRANSFER_COUNT, 0);
    writel(base + SPI_TXDATA, byte);
    writel(base + SPI_CONTROL, SPI_CONTROL_ENABLE);
    status = readl(base + SPI_STATUS);
    g_assert_cmphex(status, ==,
                    SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE |
                    (1 << SPI_STATUS_RX_COUNT_SHIFT));
    writel(base + SPI_STATUS, status);
}

static uint8_t spi_apple_rx(uint32_t base)
{
    uint32_t status;
    uint8_t response;

    spi_reset_fifos(base);
    writel(base + SPI_SETUP, readl(base + SPI_SETUP) | 1);
    writel(base + SPI_TRANSFER_COUNT, 1);
    writel(base + SPI_CONTROL, SPI_CONTROL_ENABLE);
    status = readl(base + SPI_STATUS);
    g_assert_cmphex(status, ==,
                    SPI_STATUS_RX_SERVICE |
                    (1 << SPI_STATUS_RX_COUNT_SHIFT));
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(SPI0_IRQ));
    writel(base + SPI_STATUS, status);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    g_assert_cmphex(readl(base + SPI_STATUS), ==,
                    1 << SPI_STATUS_RX_COUNT_SHIFT);
    response = readl(base + SPI_RXDATA);
    return response;
}

static void test_spi_merlot_apple_driver_phasing(void)
{
    uint8_t response;

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI0_IRQ));
    set_nor_selected(false);
    set_gpio_output(SERIALIZER_CS_PIN, true);
    writel(SPI0_BASE + SPI_SETUP, 0x11b8);

    set_gpio_output(PANEL_CS_PIN, false);
    spi_apple_tx(SPI0_BASE, 0xde);
    set_gpio_output(PANEL_CS_PIN, true);

    set_gpio_output(PANEL_CS_PIN, false);
    spi_apple_tx(SPI0_BASE, 0x95);
    response = spi_apple_rx(SPI0_BASE);
    set_gpio_output(PANEL_CS_PIN, true);

    g_assert_cmphex(response, ==, 1);
}

static void touch_set_selected(bool selected)
{
    set_gpio_output(TOUCH_CS_PIN, !selected);
}

static void touch_checksum(uint8_t *packet, size_t size)
{
    uint16_t checksum = 0;

    for (size_t i = 0; i < size - 2; i++) {
        checksum += packet[i];
    }
    packet[size - 2] = checksum;
    packet[size - 1] = checksum >> 8;
}

static void touch_exchange(const uint8_t *request, size_t request_size,
                           uint8_t *response, size_t response_size)
{
    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       request, request_size, response, response_size);
    touch_set_selected(false);
}

static void touch_start_firmware(void)
{
    static const uint8_t upload[] = {
        0x18, 0xe1,
        0x30, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t ack_request[] = { 0x1a, 0xa1 };
    static const uint8_t execute[] = {
        0x1d, 0x53, 0x18, 0x00, 0x10, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x29,
    };
    uint8_t ack[2] = { 0 };

    touch_set_selected(false);
    set_gpio_output(TOUCH_POWER_PIN, true);
    set_gpio_output(TOUCH_RESET_PIN, true);

    touch_set_selected(true);
    spi_send(SPI1_BASE, SPI1_IRQ, upload, sizeof(upload));
    touch_set_selected(false);
    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       ack_request, sizeof(ack_request), ack, sizeof(ack));
    touch_set_selected(false);
    writel(SPI1_BASE + SPI_SETUP, 0);
    g_assert_cmpmem(ack, sizeof(ack), "\x4b\xc1", 2);

    touch_set_selected(true);
    spi_send(SPI1_BASE, SPI1_IRQ, execute, sizeof(execute));
    touch_set_selected(false);
}

static void test_spi_touch_hbpp_full_duplex(void)
{
    static const uint8_t probe[16] = {
        0x1a, 0xa1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1,
        0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1,
    };
    static const uint8_t version_read[8] = {
        0x1c, 0x73, 0x8f, 0xfc, 0x10, 0x00, 0x00, 0x00,
    };
    static const uint8_t ack_request[8] = {
        0x1a, 0xa1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1,
    };
    static const uint8_t expected_read_ack[8] = {
        0x4b, 0xc1, 0x00, 0x28, 0x5a, 0x03, 0x00, 0x00,
    };
    static const uint8_t data_first[8] = {
        0x18, 0xe1, 0x30, 0x01, 0x00, 0x01, 0x00, 0x00,
    };
    static const uint8_t data_rest[12] = { 0 };
    static const uint8_t expected_data_first[8] = {
        0x18, 0xe1, 0, 0, 0, 0, 0, 0,
    };
    static const uint8_t expected_data_ack[2] = { 0x4b, 0xc1 };
    uint8_t response[sizeof(probe)] = { 0 };

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI1_IRQ));
    touch_set_selected(false);
    set_gpio_output(TOUCH_POWER_PIN, true);

    /* The dummy transfer while reset is asserted must not advance framing. */
    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       probe, sizeof(probe), response, sizeof(response));
    touch_set_selected(false);
    for (size_t i = 0; i < sizeof(response); i++) {
        g_assert_cmphex(response[i], ==, 0);
    }

    set_gpio_output(TOUCH_RESET_PIN, true);
    memset(response, 0, sizeof(response));
    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       probe, sizeof(probe), response, sizeof(response));
    touch_set_selected(false);
    g_assert_cmpmem(response, sizeof(response), probe, sizeof(probe));

    writel(SPI1_BASE + SPI_SETUP, 0);
    touch_set_selected(true);
    spi_send(SPI1_BASE, SPI1_IRQ, version_read, sizeof(version_read));
    touch_set_selected(false);
    memset(response, 0, sizeof(response));
    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       ack_request, sizeof(ack_request),
                       response, sizeof(ack_request));
    touch_set_selected(false);
    g_assert_cmpmem(response, sizeof(expected_read_ack), expected_read_ack,
                    sizeof(expected_read_ack));

    writel(SPI1_BASE + SPI_SETUP, 0);
    memset(response, 0, sizeof(response));
    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       data_first, sizeof(data_first),
                       response, sizeof(data_first));
    touch_set_selected(false);
    g_assert_cmpmem(response, sizeof(expected_data_first), expected_data_first,
                    sizeof(expected_data_first));

    writel(SPI1_BASE + SPI_SETUP, 0);
    touch_set_selected(true);
    spi_send(SPI1_BASE, SPI1_IRQ, data_rest, sizeof(data_rest));
    touch_set_selected(false);
    memset(response, 0, sizeof(response));
    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       ack_request, sizeof(expected_data_ack),
                       response, sizeof(expected_data_ack));
    touch_set_selected(false);
    g_assert_cmpmem(response, sizeof(expected_data_ack), expected_data_ack,
                    sizeof(expected_data_ack));
}

static void test_spi_touch_dma_bootload(void)
{
    static const uint8_t header[8] = {
        0x18, 0xe1, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t tail[8] = { 0 };
    static const uint8_t ack_request[2] = { 0x1a, 0xa1 };
    static const uint8_t expected_ack[2] = { 0x4b, 0xc1 };
    const uint32_t source = RAM_ALIAS_BASE + 0x00263000;
    const uint32_t channel = DMAC1_BASE + DMAC_CHANNEL0;
    const uint32_t control = 2 | DMAC_CONTROL_TC_IRQ |
                             DMAC_CONTROL_SRC_INC |
                             DMAC_CONTROL_WORD_SRC;
    uint8_t response[sizeof(header)] = { 0 };

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI1_IRQ));
    touch_set_selected(false);
    set_gpio_output(TOUCH_POWER_PIN, true);
    set_gpio_output(TOUCH_RESET_PIN, true);

    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       header, sizeof(header), response, sizeof(response));
    touch_set_selected(false);

    qtest_memwrite(global_qtest, source, tail, sizeof(tail));
    writel(DMAC1_BASE + DMAC_CONFIG, 1);
    writel(channel + DMAC_CHANNEL_SRC, source);
    writel(channel + DMAC_CHANNEL_DEST, SPI1_BASE + SPI_TXDATA);
    writel(channel + DMAC_CHANNEL_LLI, 0);
    writel(channel + DMAC_CHANNEL_CONTROL, control);
    writel(channel + DMAC_CHANNEL_CONFIG,
           DMAC_CHANNEL_TC_IRQ | DMAC_FLOW_M2P |
           DMAC_DEST_PERIPH(DMAC_SPI1_TX_REQUEST) | DMAC_CHANNEL_ENABLE);

    touch_set_selected(true);
    writel(SPI1_BASE + SPI_TRANSFER_COUNT, 0);
    writel(SPI1_BASE + SPI_SETUP, SPI_SETUP_DMA);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_SRC), ==,
                    source + sizeof(tail));
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_CONTROL) & 0xfff, ==, 0);
    g_assert_cmphex(readl(DMAC1_BASE + DMAC_INT_TC_STATUS), ==, BIT(0));
    writel(SPI1_BASE + SPI_SETUP, 0);
    touch_set_selected(false);

    memset(response, 0, sizeof(response));
    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       ack_request, sizeof(ack_request),
                       response, sizeof(expected_ack));
    touch_set_selected(false);
    g_assert_cmpmem(response, sizeof(expected_ack), expected_ack,
                    sizeof(expected_ack));
}

static void touch_read_report(uint8_t id, const uint8_t *expected,
                              size_t expected_size)
{
    uint8_t request[16] = { 0 };
    uint8_t response[16] = { 0 };

    request[0] = 0xe3;
    request[1] = id;
    touch_checksum(request, sizeof(request));
    touch_exchange(request, sizeof(request), response, sizeof(response));
    g_assert_cmphex(response[0], ==, 0xe3);
    g_assert_cmphex(response[1], ==, 0);
    g_assert_cmphex(response[2], ==, 0);
    g_assert_cmphex(response[3] | response[4] << 8, ==, expected_size);

    if (expected_size <= 11) {
        memset(request, 0, sizeof(request));
        request[0] = 0xe6;
        request[1] = id;
        request[2] = 1;
        request[3] = expected_size;
        touch_checksum(request, sizeof(request));
        touch_exchange(request, sizeof(request), response, sizeof(response));
        g_assert_cmphex(response[0], ==, 0xe6);
        g_assert_cmphex(response[1], ==, 0);
        g_assert_cmpmem(response + 3, expected_size, expected, expected_size);
    } else {
        g_autofree uint8_t *long_request = g_malloc0(expected_size + 5);
        g_autofree uint8_t *long_response = g_malloc0(expected_size + 5);

        long_request[0] = 0xe7;
        long_request[1] = id;
        long_request[2] = 1;
        stw_le_p(long_request + 3, expected_size);
        touch_checksum(long_request, expected_size + 5);
        touch_exchange(long_request, expected_size + 5, long_response,
                       expected_size + 5);
        g_assert_cmphex(long_response[0], ==, 0xe7);
        g_assert_cmphex(long_response[1], ==, 0);
        g_assert_cmpmem(long_response + 3, expected_size, expected,
                        expected_size);
    }
}

static void touch_read_frame(uint8_t expected_event, int32_t expected_x,
                             int32_t expected_y)
{
    uint8_t length_request[16] = { 0 };
    uint8_t length_response[16] = { 0 };
    uint8_t data_request[TOUCH_FRAME_SIZE + 7] = { 0 };
    uint8_t data_response[TOUCH_FRAME_SIZE + 7] = { 0 };
    const uint8_t *frame = data_response + 5;
    const uint8_t *finger = frame + TOUCH_FINGER_OFFSET;
    uint16_t checksum = 0;

    length_request[0] = 0xeb;
    length_request[1] = 1;
    touch_checksum(length_request, sizeof(length_request));
    touch_exchange(length_request, sizeof(length_request), length_response,
                   sizeof(length_response));
    g_assert_cmphex(lduw_le_p(length_response + 1), ==,
                    TOUCH_FRAME_SIZE + 2);

    data_request[0] = 0xeb;
    data_request[1] = 1;
    data_request[2] = 1;
    for (size_t i = 0; i < 14; i++) {
        checksum += data_request[i];
    }
    stw_le_p(data_request + sizeof(data_request) - 2, checksum);
    touch_exchange(data_request, sizeof(data_request), data_response,
                   sizeof(data_response));

    g_assert_cmphex(data_response[0], ==, 0xeb);
    g_assert_cmphex((data_response[0] + data_response[1] +
                     data_response[2] + data_response[3] +
                     data_response[4]) & 0xff, ==, 0);
    g_assert_cmphex(lduw_le_p(data_response + 2), ==,
                    TOUCH_FRAME_SIZE + 2);
    g_assert_cmphex(frame[0], ==, 0xcc);
    g_assert_cmphex(frame[3], ==, 1);
    g_assert_cmphex(finger[0], ==, 1);
    g_assert_cmphex(finger[1], ==, expected_event);
    g_assert_cmphex(finger[2], ==, 1);
    g_assert_cmphex(finger[3], ==, 1);
    g_assert_cmpint((int32_t)ldl_le_p(finger + 4) / 256, ==, expected_x);
    g_assert_cmpint((int32_t)ldl_le_p(finger + 8) / 256, ==, expected_y);
    g_assert_cmphex(lduw_le_p(finger + 20), ==,
                    expected_event == 5 ? 0 : 160);
    g_assert_cmphex(lduw_le_p(finger + 28), ==, 360);
    g_assert_cmphex(lduw_le_p(finger + 30), ==, 300);

    checksum = 0;
    for (size_t i = 0; i < TOUCH_FRAME_SIZE; i++) {
        checksum += frame[i];
    }
    g_assert_cmphex(lduw_le_p(data_response + 5 + TOUCH_FRAME_SIZE), ==,
                    checksum);
}

static void test_spi_touch_control(void)
{
    static const uint8_t hbpp_probe[16] = {
        0x1a, 0xa1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1,
        0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1,
    };
    static const uint8_t family[] = { TOUCH_FAMILY_ID };
    static const uint8_t sensor_info[] = { 1, 15, 10, 1, 0 };
    static const uint8_t region_desc[] = {
        0, 0, 0, 0, 0, 0, 0, 1, 0, 15, 1, 0, 10, 0,
    };
    static const uint8_t region_param[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t dimensions[] = {
        0xc0, 0x12, 0, 0, 0x20, 0x1c, 0, 0,
    };
    uint8_t request[16] = { 0 };
    uint8_t response[16] = { 0 };

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI1_IRQ));
    touch_start_firmware();

    request[0] = 0xe2;
    touch_checksum(request, sizeof(request));
    touch_exchange(request, sizeof(request), response, sizeof(response));
    g_assert_cmphex(response[0], ==, 0xe2);
    for (size_t i = 1; i < 14; i++) {
        g_assert_cmphex(response[i], ==, 0);
    }

    touch_read_report(0xd1, family, sizeof(family));
    touch_read_report(0xd3, sensor_info, sizeof(sensor_info));
    touch_read_report(0xd0, region_desc, sizeof(region_desc));
    touch_read_report(0xa1, region_param, sizeof(region_param));
    touch_read_report(0xd9, dimensions, sizeof(dimensions));

    /* ResetWhenExitingUILock pulses RESET_N without removing LDO power. */
    set_gpio_output(TOUCH_RESET_PIN, false);
    set_gpio_output(TOUCH_RESET_PIN, true);
    memset(response, 0, sizeof(response));
    touch_exchange(hbpp_probe, sizeof(hbpp_probe),
                   response, sizeof(response));
    g_assert_cmpmem(response, sizeof(response),
                    hbpp_probe, sizeof(hbpp_probe));
    memset(response, 0, sizeof(response));
    request[0] = 0xe2;
    touch_checksum(request, sizeof(request));
    touch_exchange(request, sizeof(request), response, sizeof(response));
    g_assert_cmphex(response[0], ==, 0xe2);
}

static void test_spi_touch_apple_refill(void)
{
    uint8_t request[16] = { 0 };
    uint8_t response[32] = { 0 };

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI1_IRQ));
    touch_start_firmware();

    request[0] = 0xe2;
    touch_checksum(request, sizeof(request));
    touch_set_selected(true);
    spi_apple_exchange(SPI1_BASE, SPI1_IRQ, 0x101e,
                       request, sizeof(request),
                       response, sizeof(response));
    touch_set_selected(false);

    g_assert_cmphex(response[0], ==, 0xe2);
    for (size_t i = sizeof(request); i < sizeof(response); i++) {
        g_assert_cmphex(response[i], ==, 0);
    }
}

static void test_spi_touch_frames(void)
{
    uint8_t request[16] = { 0 };
    uint8_t response[16] = { 0 };

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(SPI1_IRQ));
    touch_start_firmware();

    qtest_qmp_assert_success(
        global_qtest,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':16384}},"
        "{'type':'abs','data':{'axis':'y','value':8192}},"
        "{'type':'btn','data':{'button':'left','down':true}}]}}"
    );
    touch_read_frame(3, 2283, 5446);

    qtest_qmp_assert_success(
        global_qtest,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'abs','data':{'axis':'x','value':32767}},"
        "{'type':'abs','data':{'axis':'y','value':0}}]}}"
    );
    touch_read_frame(4, 4648, 7268);

    qtest_qmp_assert_success(
        global_qtest,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'btn','data':{'button':'left','down':false}}]}}"
    );
    touch_read_frame(5, 4648, 7268);

    request[0] = 0xea;
    request[1] = 2;
    touch_checksum(request, sizeof(request));
    touch_exchange(request, sizeof(request), response, sizeof(response));
    g_assert_cmphex(lduw_le_p(response + 1), ==, 0);
}

static void test_spi_touch_atn_routing(void)
{
    static const uint8_t upload[] = {
        0x18, 0xe1,
        0x30, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    unsigned group = TOUCH_ATN_PIN / 32;
    uint32_t bit = BIT(TOUCH_ATN_PIN % 32);

    qtest_system_reset(global_qtest);
    writel(SYSIC_BASE + SYSIC_GPIO_INTLEVEL + group * 4, bit);
    writel(SYSIC_BASE + SYSIC_GPIO_INTTYPE + group * 4, 0);
    writel(SYSIC_BASE + SYSIC_GPIO_INTEN + group * 4, bit);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(2) | BIT(SPI1_IRQ));

    touch_set_selected(false);
    set_gpio_output(TOUCH_POWER_PIN, true);
    set_gpio_output(TOUCH_RESET_PIN, true);
    touch_set_selected(true);
    spi_send(SPI1_BASE, SPI1_IRQ, upload, sizeof(upload) - 1);

    /* The last DATA byte raises ATN while the SPI completion is also live. */
    spi_reset_fifos(SPI1_BASE);
    writel(SPI1_BASE + SPI_CONTROL, 0);
    writel(SPI1_BASE + SPI_TXDATA, upload[sizeof(upload) - 1]);
    writel(SPI1_BASE + SPI_CONTROL, SPI_CONTROL_ENABLE);
    g_assert_cmphex(readl(SPI1_BASE + SPI_STATUS), ==,
                    SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE |
                    (1 << SPI_STATUS_RX_COUNT_SHIFT));
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==,
                    BIT(SPI1_IRQ) | BIT(2));
    readl(SPI1_BASE + SPI_RXDATA);
    writel(SPI1_BASE + SPI_STATUS,
           SPI_STATUS_TX_SERVICE | SPI_STATUS_RX_SERVICE);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(2));
    touch_set_selected(false);

    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4), ==,
                    bit);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(2));
    writel(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4, bit);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_uart_registers_and_loopback(void)
{
    qtest_system_reset(global_qtest);

    for (unsigned port = 0; port < UART_PORTS; port++) {
        uint64_t base = UART_BASE + port * UART_STRIDE;
        uint32_t irq = BIT(UART0_IRQ + port);

        g_assert_cmphex(readl(base + UART_UTRSTAT), ==,
                        UART_UTRSTAT_TX_EMPTY);
        g_assert_cmphex(readl(base + UART_UMSTAT), ==, UART_UMSTAT_CTS);
        writel(VIC0_BASE + VIC_INT_ENABLE, irq);
        writel(base + UART_UBAUD, 0x1234 + port);
        writel(base + UART_UDIVSLOT, 0x8080 + port);
        writel(base + UART_UFCON,
               UART_UFCON_ENABLE | UART_UFCON_RX_RESET |
               UART_UFCON_TX_RESET);
        g_assert_cmphex(readl(base + UART_UFCON), ==, UART_UFCON_ENABLE);
        g_assert_cmphex(readl(base + UART_UBAUD), ==, 0x1234 + port);
        g_assert_cmphex(readl(base + UART_UDIVSLOT), ==, 0x8080 + port);

        writel(base + UART_UCON, UART_UCON_RX_IRQ | UART_UCON_LOOPBACK);
        writel(base + UART_UTXH, 'A' + port);
        g_assert_cmphex(readl(base + UART_UTRSTAT), ==,
                        UART_UTRSTAT_TX_EMPTY | UART_UTRSTAT_RX_READY);
        g_assert_cmphex(readl(base + UART_UFSTAT), ==, 1);
        g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, irq);
        g_assert_cmphex(readl(base + UART_URXH), ==, 'A' + port);
        g_assert_cmphex(readl(base + UART_UFSTAT), ==, 0);
        g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    }
}

static void test_uart_fifo_full_and_reset(void)
{
    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(UART0_IRQ));
    writel(UART_BASE + UART_UFCON, UART_UFCON_ENABLE);
    writel(UART_BASE + UART_UCON, UART_UCON_RX_IRQ | UART_UCON_LOOPBACK);

    for (unsigned byte = 0; byte < 16; byte++) {
        writel(UART_BASE + UART_UTXH, byte);
    }
    g_assert_cmphex(readl(UART_BASE + UART_UFSTAT), ==,
                    UART_UFSTAT_RX_FULL);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(UART0_IRQ));

    writel(UART_BASE + UART_UFCON,
           UART_UFCON_ENABLE | UART_UFCON_RX_RESET);
    g_assert_cmphex(readl(UART_BASE + UART_UFCON), ==, UART_UFCON_ENABLE);
    g_assert_cmphex(readl(UART_BASE + UART_UFSTAT), ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void aes_write_be_words(uint64_t address, const uint8_t *bytes,
                               size_t length)
{
    g_assert_cmpuint(length % 4, ==, 0);
    for (size_t offset = 0; offset < length; offset += 4) {
        writel(address + offset, ldl_be_p(bytes + offset));
    }
}

static void test_aes_key_iv_subword_access(void)
{
    qtest_system_reset(global_qtest);

    writel(AES_BASE + AES_KEY, 0x11223344);
    qtest_writeb(global_qtest, AES_BASE + AES_KEY + 1, 0xaa);
    g_assert_cmphex(readl(AES_BASE + AES_KEY), ==, 0x1122aa44);

    writel(AES_BASE + AES_IV, 0x11223344);
    qtest_writew(global_qtest, AES_BASE + AES_IV + 2, 0xaabb);
    g_assert_cmphex(readl(AES_BASE + AES_IV), ==, 0xaabb3344);

    for (unsigned offset = 0; offset < 16; offset++) {
        qtest_writeb(global_qtest, AES_BASE + AES_IV + offset, 0);
    }
    for (unsigned offset = 0; offset < 16; offset += 4) {
        g_assert_cmphex(readl(AES_BASE + AES_IV + offset), ==, 0);
    }
}

static void aes_run_with_capacities(uint32_t input, uint32_t output,
                                    size_t length, size_t input_capacity,
                                    size_t output_capacity,
                                    unsigned key_type, bool encrypt)
{
    writel(AES_BASE + AES_CONTROL, 1);
    writel(AES_BASE + AES_IRQ_ENABLE, 0x7);
    writel(AES_BASE + AES_KEY_TYPE, key_type);
    writel(AES_BASE + AES_KEYLEN,
           AES_KEYLEN_CONSUMED_MODE | (encrypt ? AES_KEYLEN_ENCRYPT : 0));
    writel(AES_BASE + AES_TRANSFER_SIZE, length);
    /*
     * Keep these raw offsets producer-anchored: 8C148's SHSH helper places
     * its source in gather 0x28 and its distinct destination in scatter 0x20.
     * Using the model's symbolic role names here would let a shared reversal
     * make both the implementation and its regression agree incorrectly.
     */
    writel(AES_BASE + 0x028, input);
    writel(AES_BASE + 0x02c, input_capacity);
    writel(AES_BASE + 0x020, output);
    writel(AES_BASE + 0x024, output_capacity);
    writel(AES_BASE + AES_AUXILIARY_ADDRESS, input);
    writel(AES_BASE + AES_SIZE3, length);
    writel(AES_BASE + AES_GO, 1);
    qtest_clock_step(global_qtest, AES_COMPLETION_NS);
}

static void aes_run(uint32_t input, uint32_t output, size_t length,
                    unsigned key_type, bool encrypt)
{
    aes_run_with_capacities(input, output, length, length, length,
                            key_type, encrypt);
}

static void test_aes_custom_key_dma(void)
{
    static const uint8_t key[] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
    };
    static const uint8_t iv[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const uint8_t plaintext[] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
        0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
        0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
        0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
        0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
        0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
        0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10,
    };
    static const uint8_t ciphertext[] = {
        0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
        0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
        0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72, 0x19, 0xee,
        0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2,
        0x73, 0xbe, 0xd6, 0xb8, 0xe3, 0xc1, 0x74, 0x3b,
        0x71, 0x16, 0xe6, 0x9e, 0x22, 0x22, 0x95, 0x16,
        0x3f, 0xf1, 0xca, 0xa1, 0x68, 0x1f, 0xac, 0x09,
        0x12, 0x0e, 0xca, 0x30, 0x75, 0x86, 0xe1, 0xa7,
    };
    const uint32_t input = RAM_ALIAS_BASE + 0x00210000;
    const uint32_t encrypted = RAM_ALIAS_BASE + 0x00211000;
    const uint32_t decrypted = RAM_ALIAS_BASE + 0x00212000;
    uint8_t result[sizeof(plaintext)] = { 0 };
    uint8_t source[sizeof(plaintext)] = { 0 };

    qtest_system_reset(global_qtest);
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(AES_IRQ - 32));
    qtest_memwrite(global_qtest, input, plaintext, sizeof(plaintext));
    aes_write_be_words(AES_BASE + AES_KEY + 16, key, sizeof(key));
    aes_write_be_words(AES_BASE + AES_IV, iv, sizeof(iv));
    aes_run_with_capacities(input, encrypted, sizeof(plaintext),
                            sizeof(plaintext), 2 * sizeof(plaintext),
                            AES_CUSTOM_KEY, true);

    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==, AES_STATUS_DONE);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(AES_IRQ - 32));
    qtest_memread(global_qtest, encrypted, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), ciphertext, sizeof(ciphertext));
    qtest_memread(global_qtest, input, source, sizeof(source));
    g_assert_cmpmem(source, sizeof(source), plaintext, sizeof(plaintext));
    writel(AES_BASE + AES_STATUS, AES_STATUS_DONE);
    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);

    aes_write_be_words(AES_BASE + AES_IV, iv, sizeof(iv));
    aes_run(encrypted, decrypted, sizeof(ciphertext), AES_CUSTOM_KEY, false);
    qtest_memread(global_qtest, decrypted, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), plaintext, sizeof(plaintext));
    writel(AES_BASE + AES_STATUS, AES_STATUS_DONE);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_aes_polled_completion(void)
{
    static const uint8_t key[] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
    };
    static const uint8_t iv[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const uint8_t plaintext[] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
    };
    static const uint8_t ciphertext[] = {
        0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
        0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
    };
    const uint32_t input = RAM_ALIAS_BASE + 0x0021d000;
    const uint32_t output = RAM_ALIAS_BASE + 0x0021e000;
    uint8_t observed[sizeof(plaintext)];

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, input, plaintext, sizeof(plaintext));
    qtest_memset(global_qtest, output, 0xa5, sizeof(observed));
    aes_write_be_words(AES_BASE + AES_KEY + 16, key, sizeof(key));
    aes_write_be_words(AES_BASE + AES_IV, iv, sizeof(iv));
    writel(AES_BASE + AES_CONTROL, 1);
    writel(AES_BASE + AES_KEY_TYPE, AES_CUSTOM_KEY);
    writel(AES_BASE + AES_KEYLEN,
           AES_KEYLEN_CONSUMED_MODE | AES_KEYLEN_ENCRYPT);
    writel(AES_BASE + AES_TRANSFER_SIZE, sizeof(plaintext));
    writel(AES_BASE + AES_INPUT_ADDRESS, input);
    writel(AES_BASE + AES_INPUT_CAPACITY, sizeof(plaintext));
    writel(AES_BASE + AES_OUTPUT_ADDRESS, output);
    writel(AES_BASE + AES_OUTPUT_CAPACITY, sizeof(plaintext));
    writel(AES_BASE + AES_AUXILIARY_ADDRESS, input);
    writel(AES_BASE + AES_SIZE3, sizeof(plaintext));
    writel(AES_BASE + AES_GO, 1);

    /* GO only arms the request; the first status poll may complete it. */
    qtest_memread(global_qtest, output, observed, sizeof(observed));
    for (unsigned i = 0; i < sizeof(observed); i++) {
        g_assert_cmphex(observed[i], ==, 0xa5);
    }
    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==, AES_STATUS_DONE);
    qtest_memread(global_qtest, output, observed, sizeof(observed));
    g_assert_cmpmem(observed, sizeof(observed),
                    ciphertext, sizeof(ciphertext));
}

static void test_aes_segmented_dma(void)
{
    static const uint8_t key[] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
    };
    static const uint8_t iv[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const uint8_t plaintext[] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
        0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
        0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
        0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
        0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
        0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
        0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10,
    };
    static const uint8_t ciphertext[] = {
        0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
        0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
        0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72, 0x19, 0xee,
        0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2,
        0x73, 0xbe, 0xd6, 0xb8, 0xe3, 0xc1, 0x74, 0x3b,
        0x71, 0x16, 0xe6, 0x9e, 0x22, 0x22, 0x95, 0x16,
        0x3f, 0xf1, 0xca, 0xa1, 0x68, 0x1f, 0xac, 0x09,
        0x12, 0x0e, 0xca, 0x30, 0x75, 0x86, 0xe1, 0xa7,
    };
    const uint32_t input0 = RAM_ALIAS_BASE + 0x00219000;
    const uint32_t input1 = RAM_ALIAS_BASE + 0x0021a000;
    const uint32_t output0 = RAM_ALIAS_BASE + 0x0021b000;
    const uint32_t output1 = RAM_ALIAS_BASE + 0x0021c000;
    uint8_t actual[sizeof(ciphertext)] = { 0 };
    uint8_t source[sizeof(plaintext)] = { 0 };

    qtest_system_reset(global_qtest);
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(AES_IRQ - 32));
    qtest_memwrite(global_qtest, input0, plaintext, 32);
    qtest_memwrite(global_qtest, input1, plaintext + 32, 32);
    qtest_memset(global_qtest, output0, 0xa5, 48);
    qtest_memset(global_qtest, output1, 0xa5, 16);
    aes_write_be_words(AES_BASE + AES_KEY + 16, key, sizeof(key));
    aes_write_be_words(AES_BASE + AES_IV, iv, sizeof(iv));

    aes_run_with_capacities(input0, output0, sizeof(plaintext), 32, 48,
                            AES_CUSTOM_KEY, true);
    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==,
                    AES_STATUS_INPUT_REQ);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(AES_IRQ - 32));
    qtest_memread(global_qtest, output0, actual, 48);
    g_assert_cmpmem(actual, 32, ciphertext, 32);
    for (unsigned i = 32; i < 48; i++) {
        g_assert_cmphex(actual[i], ==, 0xa5);
    }

    /* Mirror the Apple driver's feeder-before-W1C ordering. */
    writel(AES_BASE + AES_INPUT_ADDRESS, input1);
    writel(AES_BASE + AES_INPUT_CAPACITY, 32);
    writel(AES_BASE + AES_STATUS, AES_STATUS_INPUT_REQ);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
    writel(AES_BASE + AES_GO, 3);
    qtest_clock_step(global_qtest, AES_COMPLETION_NS);
    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==,
                    AES_STATUS_OUTPUT_REQ);
    qtest_memread(global_qtest, output0, actual, 48);
    g_assert_cmpmem(actual, 48, ciphertext, 48);

    writel(AES_BASE + AES_OUTPUT_ADDRESS, output1);
    writel(AES_BASE + AES_AUXILIARY_ADDRESS, output1);
    writel(AES_BASE + AES_OUTPUT_CAPACITY, 16);
    writel(AES_BASE + AES_STATUS, AES_STATUS_OUTPUT_REQ);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
    writel(AES_BASE + AES_GO, 3);
    qtest_clock_step(global_qtest, AES_COMPLETION_NS);
    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==, AES_STATUS_DONE);
    qtest_memread(global_qtest, output1, actual + 48, 16);
    g_assert_cmpmem(actual, sizeof(actual), ciphertext, sizeof(ciphertext));

    qtest_memread(global_qtest, input0, source, 32);
    qtest_memread(global_qtest, input1, source + 32, 32);
    g_assert_cmpmem(source, sizeof(source), plaintext, sizeof(plaintext));
}

static void test_aes_gid_secret(void)
{
    static const uint8_t plaintext[] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
    };
    static const uint8_t expected[] = {
        0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60,
        0xa8, 0x9e, 0xca, 0xf3, 0x24, 0x66, 0xef, 0x97,
    };
    const uint32_t input = RAM_ALIAS_BASE + 0x00213000;
    const uint32_t output = RAM_ALIAS_BASE + 0x00214000;
    uint8_t result[sizeof(expected)] = { 0 };

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, input, plaintext, sizeof(plaintext));
    aes_run(input, output, sizeof(plaintext), AES_GID_KEY, true);
    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==, AES_STATUS_DONE);
    qtest_memread(global_qtest, output, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), expected, sizeof(expected));
}

static void test_aes_ios2_ramdisk_dma_span(void)
{
    static const uint8_t zero_key_decrypt[] = {
        0x14, 0x0f, 0x0f, 0x10, 0x11, 0xb5, 0x22, 0x3d,
        0x79, 0x58, 0x77, 0x17, 0xff, 0xd9, 0xec, 0x3a,
    };
    const uint32_t address = 0x0c000020;
    const uint32_t length = 0x01810000;
    uint8_t first[sizeof(zero_key_decrypt)];
    uint8_t last[sizeof(zero_key_decrypt)];

    qtest_system_reset(global_qtest);
    qtest_memset(global_qtest, address, 0, length);
    aes_run(address, address, length, AES_CUSTOM_KEY, false);

    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==, AES_STATUS_DONE);
    qtest_memread(global_qtest, address, first, sizeof(first));
    qtest_memread(global_qtest, address + length - sizeof(last),
                  last, sizeof(last));
    g_assert_cmpmem(first, sizeof(first),
                    zero_key_decrypt, sizeof(zero_key_decrypt));
    g_assert_cmpmem(last, sizeof(last),
                    zero_key_decrypt, sizeof(zero_key_decrypt));
}

static void test_aes_ios2_devicetree_partial_block(void)
{
    static const uint8_t zero_key_decrypt[] = {
        0x14, 0x0f, 0x0f, 0x10, 0x11, 0xb5, 0x22, 0x3d,
        0x79, 0x58, 0x77, 0x17, 0xff, 0xd9, 0xec, 0x3a,
    };
    static const uint8_t tail[] = {
        0xa5, 0x5a, 0xc3, 0x3c, 0x69, 0x96,
        0xf0, 0x0f, 0x87, 0x78, 0x1e, 0xe1,
    };
    const uint32_t address = 0x0bf00020;
    const uint32_t length = 0x00009b5c;
    const uint32_t aligned_length = length & ~0xfU;
    uint8_t first[sizeof(zero_key_decrypt)];
    uint8_t last[sizeof(zero_key_decrypt)];
    uint8_t actual_tail[sizeof(tail)];

    qtest_system_reset(global_qtest);
    qtest_memset(global_qtest, address, 0, length);
    qtest_memwrite(global_qtest, address + aligned_length,
                   tail, sizeof(tail));
    aes_run(address, address, length, AES_CUSTOM_KEY, false);

    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==, AES_STATUS_DONE);
    qtest_memread(global_qtest, address, first, sizeof(first));
    qtest_memread(global_qtest,
                  address + aligned_length - sizeof(last),
                  last, sizeof(last));
    qtest_memread(global_qtest, address + aligned_length,
                  actual_tail, sizeof(actual_tail));
    g_assert_cmpmem(first, sizeof(first),
                    zero_key_decrypt, sizeof(zero_key_decrypt));
    g_assert_cmpmem(last, sizeof(last),
                    zero_key_decrypt, sizeof(zero_key_decrypt));
    g_assert_cmpmem(actual_tail, sizeof(actual_tail), tail, sizeof(tail));
}

static void test_mbx_wrapper_contract(void)
{
    qtest_system_reset(global_qtest);

    g_assert_cmphex(readl(MBX_BASE + MBX_STATUS), ==, 0);
    g_assert_cmphex(readl(MBX_BASE + MBX_ID), ==, 0x01020000);
    g_assert_cmphex(readl(MBX_BASE + MBX_RESET), ==, 0);

    writel(MBX_BASE + MBX_RESET, MBX_RESET_REQUEST);
    g_assert_cmphex(readl(MBX_BASE + MBX_RESET), ==,
                    MBX_RESET_REQUEST | MBX_RESET_DONE);
    writel(MBX_BASE + MBX_RESET, MBX_RESET_DONE);
    g_assert_cmphex(readl(MBX_BASE + MBX_RESET), ==, 0);
    writel(MBX_BASE + MBX_RESET, 0x12345001);
    g_assert_cmphex(readl(MBX_BASE + MBX_RESET), ==,
                    0x12355001);

    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(MBX_IRQ));
    writel(MBX_BASE + MBX_INTERRUPT_MASK, BIT(10));
    writel(MBX_BASE + MBX_STATUS, BIT(10) | BIT(2));
    g_assert_cmphex(readl(MBX_BASE + MBX_STATUS), ==,
                    BIT(10) | BIT(2));
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS) & BIT(MBX_IRQ), ==,
                    BIT(MBX_IRQ));
    writel(MBX_BASE + MBX_STATUS_ACK, BIT(10));
    g_assert_cmphex(readl(MBX_BASE + MBX_STATUS), ==, BIT(2));
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS) & BIT(MBX_IRQ), ==, 0);
    writel(MBX_BASE + 0x80, 0x89abcdef);
    g_assert_cmphex(readl(MBX_BASE + 0x80), ==, 0x89abcdef);
    writel(MBX_BASE + MBX_COMMAND_MEMORY, 0x76543210);
    g_assert_cmphex(readl(MBX_BASE + MBX_COMMAND_MEMORY), ==, 0x76543210);
}

static void test_mbx_2d_copy(void)
{
    const uint32_t target_gpu = 0x00400000;
    const uint32_t source_gpu = 0x00800000;
    const uint32_t target_root = RAM_ALIAS_BASE + 0x1000;
    const uint32_t source_root = RAM_ALIAS_BASE + 0x2000;
    const uint32_t target_physical = RAM_ALIAS_BASE + 0x10000;
    const uint32_t source_physical = RAM_ALIAS_BASE + 0x20000;
    const uint32_t source_pixels[] = {
        0xff0000ff, 0xff00ff00,
        0xffff0000, 0xffffffff,
    };
    const uint32_t command[] = {
        MBX_2D_HEADER,
        target_gpu,
        0x94060008,
        source_gpu,
        0x30000000,
        0x60800200,
        0x8000cccc,
        0xffffffff,
        0x00000000,
        0x00020002,
        MBX_2D_END, MBX_2D_END, MBX_2D_END,
        MBX_2D_END, MBX_2D_END, MBX_2D_END,
    };

    qtest_system_reset(global_qtest);
    writel(MBX_BASE + 0x1004, target_root);
    writel(MBX_BASE + 0x1008, source_root);
    writel(target_root, target_physical);
    writel(source_root, source_physical);
    qtest_memwrite(global_qtest, source_physical,
                   source_pixels, sizeof(source_pixels));
    qtest_memwrite(global_qtest, MBX_BASE + MBX_COMMAND_MEMORY,
                   command, sizeof(command));
    writel(MBX_BASE + MBX_COMMAND_MEMORY, MBX_2D_SUBMIT);

    g_assert_cmphex(readl(MBX_BASE + MBX_STATUS), ==, BIT(10));
    g_assert_cmphex(readl(target_physical), ==, source_pixels[0]);
    g_assert_cmphex(readl(target_physical + 4), ==, source_pixels[1]);
    g_assert_cmphex(readl(target_physical + 0x500), ==, source_pixels[2]);
    g_assert_cmphex(readl(target_physical + 0x504), ==, source_pixels[3]);
}

static void test_mbx_ta_capture_and_handshakes(void)
{
    const uint32_t object_gpu = 0x00400000;
    const uint32_t object_root = RAM_ALIAS_BASE + 0x3000;
    const uint32_t object_physical = RAM_ALIAS_BASE + 0x30000;
    const uint32_t words[] = { 0x12345678, MBX_3D_SUBMIT };

    qtest_system_reset(global_qtest);
    writel(MBX_BASE + 0x1004, object_root);
    writel(object_root, object_physical);

    writel(MBX_BASE + MBX_TA_CONTEXT_RESET, 1);
    g_assert_cmphex(readl(MBX_BASE + MBX_STATUS), ==, BIT(8));
    writel(MBX_BASE + MBX_STATUS_ACK, BIT(8));
    writel(MBX_BASE + MBX_TA_CONTEXT_STORE, 1);
    g_assert_cmphex(readl(MBX_BASE + MBX_STATUS), ==, BIT(8));
    writel(MBX_BASE + MBX_STATUS_ACK, BIT(8));
    writel(MBX_BASE + MBX_TA_CONTEXT_LOAD, 1);
    g_assert_cmphex(readl(MBX_BASE + MBX_STATUS), ==, BIT(8));
    writel(MBX_BASE + MBX_STATUS_ACK, BIT(8));

    writel(MBX_BASE + MBX_TA_OBJECT_DB, object_gpu);
    writel(MBX_BASE + MBX_STATUS_ACK, BIT(6));
    writel(MBX_BASE + MBX_TA_START, 1);
    writel(MBX_BASE + MBX_3D_FIFO, words[0]);
    writel(MBX_BASE + MBX_3D_FIFO, words[1]);

    g_assert_cmphex(readl(MBX_BASE + MBX_TA_START), ==, 0);
    g_assert_cmphex(readl(MBX_BASE + MBX_STATUS), ==, BIT(4));
    g_assert_cmphex(readl(object_physical), ==, words[0]);
    g_assert_cmphex(readl(object_physical + 4), ==, words[1]);
}

static void test_tvout_ordered_apertures(void)
{
    static const uint32_t base[] = {
        TVOUT_BANK0_BASE,
        TVOUT_BANK1_BASE,
        TVOUT_BANK2_BASE,
    };

    qtest_system_reset(global_qtest);
    for (size_t i = 0; i < ARRAY_SIZE(base); i++) {
        uint32_t value = 0x10203040 + i;

        writel(base[i] + 0x6c, value);
        writel(base[i] + 0xffc, ~value);
        g_assert_cmphex(readl(base[i] + 0x6c), ==, value);
        g_assert_cmphex(readl(base[i] + 0xffc), ==, ~value);
    }

    qtest_system_reset(global_qtest);
    for (size_t i = 0; i < ARRAY_SIZE(base); i++) {
        g_assert_cmphex(readl(base[i] + 0x6c), ==, 0);
        g_assert_cmphex(readl(base[i] + 0xffc), ==, 0);
    }
}

static void test_aes_gid_kbag_oracle(void)
{
    const uint32_t input = RAM_ALIAS_BASE + 0x00217000;
    const uint32_t output = RAM_ALIAS_BASE + 0x00218000;
    uint8_t result[sizeof(aes_test_kbag_clear)] = { 0 };

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, input, aes_test_kbag_wrapped,
                   sizeof(aes_test_kbag_wrapped));
    aes_run(input, output, sizeof(aes_test_kbag_wrapped), AES_GID_KEY, false);
    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==, AES_STATUS_DONE);
    qtest_memread(global_qtest, output, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), aes_test_kbag_clear,
                    sizeof(aes_test_kbag_clear));

    qtest_memwrite(global_qtest, input, aes_test_kbag_clear,
                   sizeof(aes_test_kbag_clear));
    aes_run(input, output, sizeof(aes_test_kbag_clear), AES_GID_KEY, true);
    g_assert_cmphex(readl(AES_BASE + AES_STATUS), ==, AES_STATUS_DONE);
    qtest_memread(global_qtest, output, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), aes_test_kbag_wrapped,
                    sizeof(aes_test_kbag_wrapped));
}

static void test_sha1_dma_continuation(void)
{
    static const uint32_t expected[] = {
        0x2500907f, 0xd718497a, 0xea552607, 0xcd408546, 0x0c2ed4cb,
    };
    const uint32_t first_address = RAM_ALIAS_BASE + 0x00215000;
    const uint32_t final_address = RAM_ALIAS_BASE + 0x00216000;
    uint8_t first[64];
    uint8_t final[64] = { 0 };

    memset(first, 'a', sizeof(first));
    memset(final, 'a', 36);
    final[36] = 0x80;
    stq_be_p(final + 56, 100 * 8);

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, first_address, first, sizeof(first));
    qtest_memwrite(global_qtest, final_address, final, sizeof(final));
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(SHA1_IRQ - 32));

    writel(SHA1_BASE + SHA1_RESET, 1);
    writel(SHA1_BASE + SHA1_RESET, 0);
    writel(SHA1_BASE + SHA1_DMA_CONTROL, 1);
    writel(SHA1_BASE + SHA1_DMA_ADDRESS, first_address);
    writel(SHA1_BASE + SHA1_DMA_LENGTH, sizeof(first));
    writel(SHA1_BASE + SHA1_CONFIG, SHA1_START);
    g_assert_cmphex(readl(SHA1_BASE + SHA1_CONFIG) &
                    (BIT(0) | SHA1_START), ==, 0);

    writel(SHA1_BASE + SHA1_IRQ_ENABLE, 1);
    writel(SHA1_BASE + SHA1_DMA_ADDRESS, final_address);
    writel(SHA1_BASE + SHA1_DMA_LENGTH, sizeof(final));
    writel(SHA1_BASE + SHA1_CONFIG,
           SHA1_START | SHA1_COMPLETION_IRQ | SHA1_CUSTOM_IV);

    for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(readl(SHA1_BASE + SHA1_DIGEST + i * 4), ==,
                        expected[i]);
    }
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(SHA1_IRQ - 32));
    writel(SHA1_BASE + SHA1_IRQ_ACK, 1);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_sha1_pio_read_modify_write(void)
{
    static const uint32_t expected[] = {
        0xd5ffea5b, 0x219ee5ac, 0x9522aa7a, 0x42350322, 0x812ea431,
    };
    uint8_t padded[768] = { 0 };

    for (size_t i = 0; i < 740; i++) {
        padded[i] = i * 37 + 11;
    }
    padded[740] = 0x80;
    stq_be_p(padded + sizeof(padded) - 8, 740 * 8);

    qtest_system_reset(global_qtest);
    writel(SHA1_BASE + SHA1_RESET, 1);
    writel(SHA1_BASE + SHA1_RESET, 0);

    for (size_t block = 0; block < sizeof(padded) / 64; block++) {
        uint32_t config = readl(SHA1_BASE + SHA1_CONFIG);

        if (block == 0) {
            config &= ~SHA1_CUSTOM_IV;
        } else {
            config |= SHA1_CUSTOM_IV;
        }
        writel(SHA1_BASE + SHA1_CONFIG, config);
        for (size_t word = 0; word < 16; word++) {
            size_t offset = block * 64 + word * sizeof(uint32_t);

            writel(SHA1_BASE + SHA1_DATA + word * sizeof(uint32_t),
                   ldl_le_p(padded + offset));
        }
        writel(SHA1_BASE + SHA1_CONFIG, config | SHA1_START);
        g_assert_cmphex(readl(SHA1_BASE + SHA1_CONFIG) &
                        (BIT(0) | SHA1_START), ==, 0);
    }

    for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(readl(SHA1_BASE + SHA1_DIGEST + i * 4), ==,
                        expected[i]);
    }
}

static void test_pke_rsa_public(void)
{
    static const struct {
        uint32_t config;
        uint32_t bytes;
    } sizes[] = {
        { 0x01, 256 },
        { 0x41, 128 },
        { 0x81, 64 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
        uint64_t segment1 = PKE_BASE + PKE_SEGMENT_BASE + sizes[i].bytes;

        writel(PKE_BASE + PKE_SEGMENT_BASE, 3233);
        writel(segment1, 855);
        writel(PKE_BASE + PKE_SEGMENT_CONFIG, sizes[i].config);
        writel(PKE_BASE + PKE_OPERATION, 0x01050003);
        writel(PKE_BASE + PKE_COMMAND, 9);
        writel(PKE_BASE + PKE_SEGMENT_CONFIG, sizes[i].config | BIT(1));
        writel(PKE_BASE + PKE_OPERATION, 0x02020001);
        writel(PKE_BASE + PKE_COMMAND, 1);

        g_assert_cmphex(readl(segment1), ==, 1892);
        g_assert_cmphex(readl(segment1 + 4), ==, 0);
    }
}

static void run_adm_fmc_command(void)
{
    writel(ADM_BASE + ADM_COMMAND, 2);
    qtest_clock_step(global_qtest, ADM_FMC_COMPLETION_NS);
}

static void assert_adm_fmc_event_result(uint16_t command_result,
                                        uint16_t media_result,
                                        uint16_t read_result)
{
    g_assert_cmphex(readl(ADM_BASE + ADM_EVENT_DATA), ==, command_result);
    g_assert_cmphex(readl(ADM_BASE + ADM_EVENT_DATA + 4), ==, media_result);
    g_assert_cmphex(readl(ADM_BASE + ADM_EVENT_DATA + 8), ==, read_result);
    g_assert_cmphex(readl(ADM_BASE + ADM_EVENT_DATA + 12), ==, 0);
}

static void build_adm_fmc_compact_command(uint8_t *command,
                                          uint16_t opcode,
                                          unsigned page_count,
                                          unsigned ce_count,
                                          uint32_t page,
                                          uint32_t data_address)
{
    g_assert_cmpuint(page_count, <=, ADM_FMC_MAX_PAGES);
    g_assert_cmpuint(ce_count, <=, ADM_FMC_BANK_SLOTS);
    g_assert_cmpuint(ce_count, <=, page_count);

    memset(command, 0, ADM_FMC_TEST_COMMAND_SIZE);
    stw_le_p(command + ADM_FMC_OPCODE, opcode);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, page_count);
    stw_be_p(command + ADM_FMC_PAD_SIZE, 12);
    stw_be_p(command + ADM_FMC_CE_COUNT, ce_count);
    for (unsigned i = 0; i < ce_count; i++) {
        command[ADM_FMC_BANKS + i] = i;
        stl_be_p(command + ADM_FMC_PAGES + i * sizeof(uint32_t), page);
    }
    memset(command + ADM_FMC_BANKS + ce_count, 0xff,
           page_count - ce_count);
    for (unsigned i = ce_count; i < page_count; i++) {
        stl_be_p(command + ADM_FMC_PAGES + i * sizeof(uint32_t),
                 UINT32_MAX);
    }
    stl_be_p(command + ADM_FMC_DESCRIPTORS, data_address);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             page_count * NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
}

static void test_adm_registers_and_interrupts(void)
{
    qtest_system_reset(global_qtest);

    g_assert_cmphex(readl(ADM_BASE + ADM_CONTROL), ==, ADM_CONTROL_READY);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);

    writel(ADM_BASE + ADM_UPLOAD_DATA, 0x11223344);
    writel(ADM_BASE + ADM_EVENT_DATA, 0x55667788);
    writel(ADM_BASE + ADM_UPLOAD_ACTION0, 0x08001000);
    writel(ADM_BASE + ADM_EVENT_ACTION0, 0x08002000);
    writel(ADM_BASE + ADM_UPLOAD_ACTION2, 0x08003000);
    writel(ADM_BASE + ADM_EVENT_ACTION2, 0x08004000);
    g_assert_cmphex(readl(ADM_BASE + ADM_UPLOAD_DATA), ==, 0x11223344);
    g_assert_cmphex(readl(ADM_BASE + ADM_EVENT_DATA), ==, 0x55667788);
    g_assert_cmphex(readl(ADM_BASE + ADM_UPLOAD_ACTION0), ==, 0x08001000);
    g_assert_cmphex(readl(ADM_BASE + ADM_EVENT_ACTION0), ==, 0x08002000);
    g_assert_cmphex(readl(ADM_BASE + ADM_UPLOAD_ACTION2), ==, 0x08003000);
    g_assert_cmphex(readl(ADM_BASE + ADM_EVENT_ACTION2), ==, 0x08004000);

    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RESET);
    g_assert_cmphex(readl(ADM_BASE + ADM_CONTROL), ==, ADM_CONTROL_READY);
    g_assert_cmphex(readl(ADM_BASE + ADM_UPLOAD_DATA), ==, 0);
    g_assert_cmphex(readl(ADM_BASE + ADM_EVENT_ACTION2), ==, 0);

    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(ADM_IRQ - 32));
    writel(ADM_BASE + ADM_CONTROL,
           ADM_CONTROL_RUNNING | ADM_IRQ_EVENT | ADM_IRQ_COMMAND |
           ADM_IRQ_UPLOAD);
    g_assert_cmphex(readl(ADM_BASE + ADM_CONTROL), ==,
                    ADM_CONTROL_RUNNING | ADM_CONTROL_READY |
                    ADM_IRQ_EVENT | ADM_IRQ_COMMAND | ADM_IRQ_UPLOAD);

    writel(ADM_BASE + ADM_COMMAND, 0);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, ADM_IRQ_COMMAND);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(ADM_IRQ - 32));
    writel(ADM_BASE + ADM_COMMAND, 5);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);

    writel(ADM_BASE + ADM_COMMAND, 2);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
    qtest_clock_step(global_qtest, ADM_FMC_COMPLETION_NS);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, ADM_IRQ_EVENT);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(ADM_IRQ - 32));
    writel(ADM_BASE + ADM_COMMAND, 4);
    writel(ADM_BASE + ADM_COMMAND, 1);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);

    writel(ADM_BASE + ADM_CONTROL, 0);
    g_assert_cmphex(readl(ADM_BASE + ADM_CONTROL), ==, ADM_CONTROL_READY);
}

static void test_adm_fmc_startup_device_ids(void)
{
    const uint32_t data2 = RAM_ALIAS_BASE + 0x0025f000;
    const uint32_t data3 = RAM_ALIAS_BASE + 0x00260000;
    uint32_t device_ids[ADM_FMC_BANK_SLOTS];

    qtest_system_reset(global_qtest);
    qtest_writeb(global_qtest, data2, 0);
    memset(device_ids, 0xa5, sizeof(device_ids));
    qtest_memwrite(global_qtest, data3, device_ids, sizeof(device_ids));
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(ADM_IRQ - 32));
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_EVENT_ACTION3, data3);
    writel(ADM_BASE + ADM_CONTROL,
           ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);
    g_assert_cmphex(qtest_readb(global_qtest, data2), ==, 0x50);
    qtest_memread(global_qtest, data3, device_ids, sizeof(device_ids));

    for (unsigned bank = 0; bank < ADM_FMC_BANK_SLOTS; bank++) {
        g_assert_cmphex(device_ids[bank], ==,
                        bank < 4 ? NAND_DEVICE_ID : 0);
    }
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, ADM_IRQ_EVENT);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(ADM_IRQ - 32));
    writel(ADM_BASE + ADM_COMMAND, 4);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);

    /*
     * The driver can restart CalmADMFMC without resetting the application
     * processor.  Rewriting RUNNING must execute a fresh POST transaction
     * even when the control bit was already set.
     */
    qtest_writeb(global_qtest, data2, 0);
    memset(device_ids, 0xa5, sizeof(device_ids));
    qtest_memwrite(global_qtest, data3, device_ids, sizeof(device_ids));
    writel(ADM_BASE + ADM_CONTROL,
           ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);
    g_assert_cmphex(qtest_readb(global_qtest, data2), ==, 0x50);
    qtest_memread(global_qtest, data3, device_ids, sizeof(device_ids));
    for (unsigned bank = 0; bank < ADM_FMC_BANK_SLOTS; bank++) {
        g_assert_cmphex(device_ids[bank], ==,
                        bank < 4 ? NAND_DEVICE_ID : 0);
    }
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, ADM_IRQ_EVENT);
}

static void test_adm_fmc_init_command(void)
{
    const uint32_t data2 = RAM_ALIAS_BASE + 0x00268000;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    uint8_t command[ADM_FMC_PAD_SIZE + sizeof(uint32_t)] = { 0 };
    uint8_t result[sizeof(uint32_t)];

    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_INIT);
    memset(command + ADM_FMC_PAGE_COUNT, 0xff, sizeof(uint32_t));
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 0);
    stw_be_p(command + ADM_FMC_PAD_SIZE, 0);

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(ADM_IRQ - 32));
    writel(ADM_BASE + ADM_CONTROL,
           ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);
    run_adm_fmc_command();

    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    assert_adm_fmc_event_result(0, 0, 0);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, ADM_IRQ_EVENT);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(ADM_IRQ - 32));
}

static void test_adm_fmc_empty_page_result(void)
{
    const uint32_t data2 = RAM_ALIAS_BASE + 0x0026c000;
    const uint32_t output = RAM_ALIAS_BASE + 0x0026e000;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    uint8_t command[ADM_FMC_DESCRIPTORS + 2 * 8] = { 0 };
    uint8_t actual[NAND_PAGE_DATA_SIZE];
    uint8_t result[sizeof(uint32_t)];

    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, 0);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);

    qtest_system_reset(global_qtest);
    memset(actual, 0, sizeof(actual));
    qtest_memwrite(global_qtest, output, actual, sizeof(actual));
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);
    run_adm_fmc_command();

    qtest_memread(global_qtest, output, actual, sizeof(actual));
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    for (size_t i = 0; i < sizeof(actual); i++) {
        g_assert_cmphex(actual[i], ==, 0xff);
    }
    g_assert_cmphex(lduw_le_p(result), ==, 0);
    g_assert_cmphex(lduw_le_p(result + 2), ==, 0);
    assert_adm_fmc_event_result(0, 0, ADM_FMC_READ_RESULT_EMPTY);
}

static void test_adm_fmc_backing_page_read(void)
{
    static const uint32_t sentinel[] = {
        0x01234567, 0x89abcdef, 0xa5a5a5a5, 0x5a5a5a5a,
    };
    const uint32_t data2 = RAM_ALIAS_BASE + 0x00270000;
    const uint32_t data3 = RAM_ALIAS_BASE + 0x00280000;
    const uint32_t output = RAM_ALIAS_BASE + 0x00290000;
    const uint32_t lli = RAM_ALIAS_BASE + 0x00298000;
    const uint32_t next_lli = lli + 16;
    const uint32_t next_source = RAM_ALIAS_BASE + 0x00299000;
    const uint32_t next_destination = RAM_ALIAS_BASE + 0x0029a000;
    const uint32_t channel = DMAC0_BASE + DMAC_CHANNEL0 +
                             5 * DMAC_CHANNEL_STRIDE;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const uint32_t dma_control = NAND_PAGE_DATA_SIZE / sizeof(uint32_t) |
                                 DMAC_CONTROL_DEST_INC |
                                 DMAC_CONTROL_WORD_SRC |
                                 DMAC_CONTROL_WORD_DST;
    const uint32_t tail_control = 1 | DMAC_CONTROL_TC_IRQ |
                                  DMAC_CONTROL_SRC_INC |
                                  DMAC_CONTROL_DEST_INC |
                                  DMAC_CONTROL_WORD_SRC |
                                  DMAC_CONTROL_WORD_DST;
    const uint32_t next_control = 2 | DMAC_CONTROL_TC_IRQ |
                                  DMAC_CONTROL_SRC_INC |
                                  DMAC_CONTROL_DEST_INC |
                                  DMAC_CONTROL_WORD_SRC |
                                  DMAC_CONTROL_WORD_DST;
    uint8_t command[ADM_FMC_DESCRIPTORS + 2 * 8] = { 0 };
    uint8_t dma_descriptors[32] = { 0 };
    uint8_t actual[NAND_PAGE_DATA_SIZE];
    uint8_t expected[NAND_PAGE_DATA_SIZE];
    uint8_t spare[NAND_PAGE_SPARE_SIZE];
    uint8_t result[sizeof(uint32_t)];

    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    stw_be_p(command + ADM_FMC_PAD_SIZE, NAND_PAGE_SPARE_SIZE);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, 1);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             2 * NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    stl_le_p(dma_descriptors, NAND_BASE + NAND_FIFO);
    stl_le_p(dma_descriptors + 4, data3);
    stl_le_p(dma_descriptors + 8, next_lli);
    stl_le_p(dma_descriptors + 12, tail_control);
    stl_le_p(dma_descriptors + 16, next_source);
    stl_le_p(dma_descriptors + 20, next_destination);
    stl_le_p(dma_descriptors + 24, 0);
    stl_le_p(dma_descriptors + 28, next_control);

    memset(expected, 0xff, sizeof(expected));
    for (size_t i = 0; i < ARRAY_SIZE(sentinel); i++) {
        stl_le_p(expected + i * sizeof(uint32_t), sentinel[i]);
    }

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    qtest_memwrite(global_qtest, lli, dma_descriptors,
                   sizeof(dma_descriptors));
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(DMAC0_IRQ));
    writel(DMAC0_BASE + DMAC_CONFIG, 1);
    writel(channel + DMAC_CHANNEL_SRC, NAND_BASE + NAND_FIFO);
    writel(channel + DMAC_CHANNEL_DEST, output);
    writel(channel + DMAC_CHANNEL_LLI, lli);
    writel(channel + DMAC_CHANNEL_CONTROL, dma_control);
    writel(channel + DMAC_CHANNEL_CONFIG,
           DMAC_CHANNEL_TC_IRQ | DMAC_FLOW_P2M |
           DMAC_SOURCE_PERIPH(DMAC_NAND_REQUEST) | DMAC_CHANNEL_ENABLE);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, BIT(5));
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_EVENT_ACTION3, data3);
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(ADM_IRQ - 32));
    writel(ADM_BASE + ADM_CONTROL,
           ADM_CONTROL_RUNNING | ADM_IRQ_EVENT | ADM_IRQ_COMMAND |
           ADM_IRQ_UPLOAD);
    g_assert_cmphex(qtest_readb(global_qtest, data2), ==, 0x50);
    writel(ADM_BASE + ADM_COMMAND, 4);

    writel(ADM_BASE + ADM_COMMAND, 1);
    qtest_clock_step(global_qtest, ADM_FMC_DMA_COMPLETION_NS);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, BIT(5));
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, 0);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_CONTROL), ==, dma_control);
    writel(ADM_BASE + ADM_COMMAND, 2);
    qtest_clock_step(global_qtest, ADM_FMC_COMPLETION_NS);
    qtest_clock_step_next(global_qtest);
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    qtest_memread(global_qtest, data3, spare, sizeof(spare));
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    for (size_t i = 0; i < sizeof(spare); i++) {
        g_assert_cmphex(spare[i], ==, 0xff);
    }
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    assert_adm_fmc_event_result(0, 0, 0);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, BIT(5));
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(5));
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_CONTROL), ==, next_control);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_SRC), ==,
                    next_source);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_DEST), ==,
                    next_destination);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_LLI), ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(DMAC0_IRQ));
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, ADM_IRQ_EVENT);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(ADM_IRQ - 32));
    qtest_clock_step_next(global_qtest);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, BIT(5));
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_CONTROL), ==, next_control);
    writel(ADM_BASE + ADM_COMMAND, 4);
    writel(ADM_BASE + ADM_COMMAND, 1);
    writel(DMAC0_BASE + DMAC_INT_TC_CLEAR, BIT(5));
    qtest_clock_step_next(global_qtest);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, 0);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(5));
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_CONTROL) & 0xfff, ==, 0);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_SRC), ==,
                    next_source + 2 * sizeof(uint32_t));
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_DEST), ==,
                    next_destination + 2 * sizeof(uint32_t));
    writel(DMAC0_BASE + DMAC_INT_TC_CLEAR, BIT(5));
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_adm_fmc_late_read_dma_arm(void)
{
    const uint32_t data2 = RAM_ALIAS_BASE + 0x002e0000;
    const uint32_t output = RAM_ALIAS_BASE + 0x002f0000;
    const uint32_t channel = DMAC0_BASE + DMAC_CHANNEL0 +
                             5 * DMAC_CHANNEL_STRIDE;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const uint32_t dma_control = NAND_PAGE_DATA_SIZE / sizeof(uint32_t) |
                                 DMAC_CONTROL_TC_IRQ |
                                 DMAC_CONTROL_DEST_INC |
                                 DMAC_CONTROL_WORD_SRC |
                                 DMAC_CONTROL_WORD_DST;
    uint8_t command[ADM_FMC_DESCRIPTORS + 2 * 8] = { 0 };
    uint8_t result[sizeof(uint32_t)];

    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, 0);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);
    run_adm_fmc_command();
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(lduw_le_p(result), ==, 0);
    g_assert_cmphex(lduw_le_p(result + 2), ==, 0);
    assert_adm_fmc_event_result(0, 0, ADM_FMC_READ_RESULT_EMPTY);

    writel(ADM_BASE + ADM_COMMAND, 4);
    writel(ADM_BASE + ADM_COMMAND, 1);
    qtest_clock_step(global_qtest, ADM_FMC_DMA_COMPLETION_NS);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);

    writel(DMAC0_BASE + DMAC_CONFIG, 1);
    writel(channel + DMAC_CHANNEL_SRC, NAND_BASE + NAND_FIFO);
    writel(channel + DMAC_CHANNEL_DEST, output);
    writel(channel + DMAC_CHANNEL_LLI, 0);
    writel(channel + DMAC_CHANNEL_CONTROL, dma_control);
    writel(channel + DMAC_CHANNEL_CONFIG,
           DMAC_CHANNEL_TC_IRQ | DMAC_FLOW_P2M |
           DMAC_SOURCE_PERIPH(DMAC_NAND_REQUEST) | DMAC_CHANNEL_ENABLE);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, BIT(5));
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, 0);

    qtest_clock_step(global_qtest, ADM_FMC_DMA_ARM_GRACE_NS);
    qtest_clock_step(global_qtest, ADM_FMC_DMA_COMPLETION_NS);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, 0);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(5));
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_CONTROL) & 0xfff, ==, 0);
}

static void test_adm_fmc_early_write_dma_arm(void)
{
    const uint32_t data2 = RAM_ALIAS_BASE + 0x00304000;
    const uint32_t source = RAM_ALIAS_BASE + 0x00308000;
    const uint32_t output = RAM_ALIAS_BASE + 0x0030a000;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const uint32_t channel = DMAC0_BASE + DMAC_CHANNEL0 +
                             5 * DMAC_CHANNEL_STRIDE;
    const uint32_t page = 4 * NAND_PAGES_PER_BLOCK;
    const uint32_t dma_control = NAND_PAGE_DATA_SIZE / sizeof(uint32_t) |
                                 DMAC_CONTROL_TC_IRQ |
                                 DMAC_CONTROL_SRC_INC |
                                 DMAC_CONTROL_WORD_SRC |
                                 DMAC_CONTROL_WORD_DST;
    uint8_t command[ADM_FMC_DESCRIPTORS + 2 * 8] = { 0 };
    uint8_t expected[NAND_PAGE_DATA_SIZE];
    uint8_t actual[sizeof(expected)];
    uint8_t result[4];

    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = i * 29 + 7;
    }

    qtest_system_reset(global_qtest);
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);

    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_ERASE);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_ERASE_BANK] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    writel(ADM_BASE + ADM_COMMAND, 4);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_WRITE_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, source);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    qtest_memwrite(global_qtest, source, expected, sizeof(expected));

    /* The firmware can arm and run before its PL080 channel is ready. */
    writel(ADM_BASE + ADM_COMMAND, 1);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    writel(ADM_BASE + ADM_COMMAND, 2);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    writel(DMAC0_BASE + DMAC_CONFIG, 1);
    writel(channel + DMAC_CHANNEL_SRC, source);
    writel(channel + DMAC_CHANNEL_DEST, NAND_BASE + NAND_FIFO);
    writel(channel + DMAC_CHANNEL_LLI, 0);
    writel(channel + DMAC_CHANNEL_CONTROL, dma_control);
    writel(channel + DMAC_CHANNEL_CONFIG,
           DMAC_CHANNEL_TC_IRQ | DMAC_FLOW_M2P |
           DMAC_DEST_PERIPH(DMAC_NAND_REQUEST) | DMAC_CHANNEL_ENABLE);

    qtest_clock_step_next(global_qtest);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, 0);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);

    qtest_clock_step_next(global_qtest);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(5));
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(lduw_be_p(result), ==, 1);

    qtest_clock_step_next(global_qtest);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, ADM_IRQ_EVENT);
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    writel(ADM_BASE + ADM_COMMAND, 4);
    writel(DMAC0_BASE + DMAC_INT_TC_CLEAR, BIT(5));

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
}

static void test_adm_fmc_write_run_snapshot(void)
{
    const uint32_t data2 = RAM_ALIAS_BASE + 0x00314000;
    const uint32_t source = RAM_ALIAS_BASE + 0x00318000;
    const uint32_t output = RAM_ALIAS_BASE + 0x0031a000;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const uint32_t page = 6 * NAND_PAGES_PER_BLOCK;
    uint8_t command[ADM_FMC_DESCRIPTORS + 2 * 8] = { 0 };
    uint8_t expected[NAND_PAGE_DATA_SIZE];
    uint8_t actual[sizeof(expected)];
    uint8_t overwritten[sizeof(expected)];

    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = i * 17 + 9;
        overwritten[i] = i * 43 + 5;
    }

    qtest_system_reset(global_qtest);
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);

    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_ERASE);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_ERASE_BANK] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    writel(ADM_BASE + ADM_COMMAND, 4);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_WRITE_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, source);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    qtest_memwrite(global_qtest, source, expected, sizeof(expected));

    writel(ADM_BASE + ADM_COMMAND, 2);

    /* RUN owns the record even if the producer immediately reuses it. */
    command[ADM_FMC_BANKS] = 0xff;
    stl_be_p(command + ADM_FMC_PAGES, UINT32_MAX);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4, UINT32_MAX);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    qtest_memwrite(global_qtest, source, overwritten, sizeof(overwritten));
    qtest_clock_step(global_qtest, ADM_FMC_COMPLETION_NS);
    writel(ADM_BASE + ADM_COMMAND, 4);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
}

static void assert_adm_fmc_dma_before_run_uses_run_record(
    unsigned page_count, unsigned block_offset)
{
    const unsigned ce_count = 4;
    const unsigned spare_size = 12;
    const uint32_t data3 = RAM_ALIAS_BASE + 0x01800000;
    const uint32_t data2 = RAM_ALIAS_BASE + 0x01900000;
    const uint32_t stale_source = RAM_ALIAS_BASE + 0x01a00000;
    const uint32_t committed_source = RAM_ALIAS_BASE + 0x01b00000;
    const uint32_t output = RAM_ALIAS_BASE + 0x01c00000;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const uint32_t channel = DMAC0_BASE + DMAC_CHANNEL0 +
                             5 * DMAC_CHANNEL_STRIDE;
    const uint32_t stale_page = (10 + block_offset) * NAND_PAGES_PER_BLOCK;
    const uint32_t committed_page =
        (12 + block_offset) * NAND_PAGES_PER_BLOCK;
    const uint32_t dma_control = NAND_PAGE_DATA_SIZE / sizeof(uint32_t) |
                                 DMAC_CONTROL_TC_IRQ |
                                 DMAC_CONTROL_SRC_INC |
                                 DMAC_CONTROL_WORD_SRC |
                                 DMAC_CONTROL_WORD_DST;
    const size_t data_size = page_count * NAND_PAGE_DATA_SIZE;
    uint8_t command[ADM_FMC_TEST_COMMAND_SIZE];
    uint8_t result[4];
    g_autofree uint8_t *stale = g_malloc(data_size);
    g_autofree uint8_t *expected = g_malloc(data_size);
    g_autofree uint8_t *actual = g_malloc(data_size);
    g_autofree uint8_t *spares = g_malloc(page_count * spare_size);

    for (size_t i = 0; i < data_size; i++) {
        stale[i] = i * 13 + i / NAND_PAGE_DATA_SIZE;
        expected[i] = i * 47 + 0x5b + i / NAND_PAGE_DATA_SIZE;
    }
    memset(spares, 0xff, page_count * spare_size);
    for (unsigned i = 0; i < page_count; i++) {
        stl_le_p(spares + i * spare_size, i);
        stl_le_p(spares + i * spare_size + 4, 1);
        spares[i * spare_size + 9] = 0x41;
    }

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, stale_source, stale, data_size);
    qtest_memwrite(global_qtest, committed_source, expected, data_size);
    qtest_memwrite(global_qtest, data3, spares, page_count * spare_size);
    qtest_memset(global_qtest, output, 0x6d, data_size);
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_EVENT_ACTION3, data3);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);
    writel(ADM_BASE + ADM_COMMAND, 4);

    for (unsigned bank = 0; bank < ce_count; bank++) {
        for (unsigned block = 0; block < 2; block++) {
            memset(command, 0, sizeof(command));
            stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_ERASE);
            stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
            command[ADM_FMC_ERASE_BANK] = bank;
            stl_be_p(command + ADM_FMC_PAGES,
                     block ? committed_page : stale_page);
            qtest_memwrite(global_qtest, command_address, command,
                           sizeof(command));
            run_adm_fmc_command();
            writel(ADM_BASE + ADM_COMMAND, 4);
        }
    }

    build_adm_fmc_compact_command(command, ADM_FMC_WRITE, page_count,
                                  ce_count, stale_page, stale_source);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    writel(DMAC0_BASE + DMAC_CONFIG, 1);
    writel(channel + DMAC_CHANNEL_SRC, stale_source);
    writel(channel + DMAC_CHANNEL_DEST, NAND_BASE + NAND_FIFO);
    writel(channel + DMAC_CHANNEL_LLI, 0);
    writel(channel + DMAC_CHANNEL_CONTROL, dma_control);
    writel(channel + DMAC_CHANNEL_CONFIG,
           DMAC_CHANNEL_TC_IRQ | DMAC_FLOW_M2P |
           DMAC_DEST_PERIPH(DMAC_NAND_REQUEST) | DMAC_CHANNEL_ENABLE);

    /* DMA readiness may precede the producer's final shared command record. */
    writel(ADM_BASE + ADM_COMMAND, 1);
    qtest_clock_step(global_qtest, ADM_FMC_DMA_COMPLETION_NS);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(5));
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);

    build_adm_fmc_compact_command(command, ADM_FMC_WRITE, page_count,
                                  ce_count, committed_page,
                                  committed_source);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    writel(ADM_BASE + ADM_COMMAND, 4);
    writel(DMAC0_BASE + DMAC_INT_TC_CLEAR, BIT(5));

    build_adm_fmc_compact_command(command, ADM_FMC_READ, page_count,
                                  ce_count, committed_page, output);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    qtest_memread(global_qtest, output, actual, data_size);
    g_assert_cmpmem(actual, data_size, expected, data_size);
}

static void test_adm_fmc_dma_before_run_uses_run_record(void)
{
    assert_adm_fmc_dma_before_run_uses_run_record(60, 0);
    assert_adm_fmc_dma_before_run_uses_run_record(64, 4);
}

static void test_adm_fmc_late_write_dma_arm(void)
{
    const uint32_t data2 = RAM_ALIAS_BASE + 0x0030c000;
    const uint32_t source = RAM_ALIAS_BASE + 0x00310000;
    const uint32_t output = RAM_ALIAS_BASE + 0x00312000;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const uint32_t channel = DMAC0_BASE + DMAC_CHANNEL0 +
                             5 * DMAC_CHANNEL_STRIDE;
    const uint32_t page = 5 * NAND_PAGES_PER_BLOCK;
    const uint32_t dma_control = NAND_PAGE_DATA_SIZE / sizeof(uint32_t) |
                                 DMAC_CONTROL_TC_IRQ |
                                 DMAC_CONTROL_SRC_INC |
                                 DMAC_CONTROL_WORD_SRC |
                                 DMAC_CONTROL_WORD_DST;
    uint8_t command[ADM_FMC_DESCRIPTORS + 2 * 8] = { 0 };
    uint8_t expected[NAND_PAGE_DATA_SIZE];
    uint8_t actual[sizeof(expected)];
    uint8_t result[4];

    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = i * 31 + 3;
    }

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, source, expected, sizeof(expected));
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);

    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_ERASE);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_ERASE_BANK] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    writel(ADM_BASE + ADM_COMMAND, 4);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_WRITE_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, source);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));

    /* A Host pause must not turn a committed write into a lifecycle event. */
    writel(ADM_BASE + ADM_COMMAND, 1);
    writel(ADM_BASE + ADM_COMMAND, 2);
    qtest_clock_step(global_qtest, ADM_FMC_DMA_ARM_GRACE_NS);
    qtest_clock_step(global_qtest, ADM_FMC_DMA_ARM_GRACE_NS);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(lduw_be_p(result), ==, 1);

    qtest_clock_step(global_qtest, ADM_FMC_DMA_ARM_GRACE_NS -
                                   ADM_FMC_DMA_COMPLETION_NS / 2);

    writel(DMAC0_BASE + DMAC_CONFIG, 1);
    writel(channel + DMAC_CHANNEL_SRC, source);
    writel(channel + DMAC_CHANNEL_DEST, NAND_BASE + NAND_FIFO);
    writel(channel + DMAC_CHANNEL_LLI, 0);
    writel(channel + DMAC_CHANNEL_CONTROL, dma_control);
    writel(channel + DMAC_CHANNEL_CONFIG,
           DMAC_CHANNEL_TC_IRQ | DMAC_FLOW_M2P |
           DMAC_DEST_PERIPH(DMAC_NAND_REQUEST) | DMAC_CHANNEL_ENABLE);

    /* Start DMA just before RUN's older grace timer expires. */
    writel(ADM_BASE + ADM_COMMAND, 1);
    qtest_clock_step(global_qtest, ADM_FMC_DMA_COMPLETION_NS / 2);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    qtest_clock_step(global_qtest, ADM_FMC_DMA_COMPLETION_NS / 2);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(5));
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    qtest_clock_step_next(global_qtest);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, ADM_IRQ_EVENT);
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    writel(ADM_BASE + ADM_COMMAND, 4);
    writel(DMAC0_BASE + DMAC_INT_TC_CLEAR, BIT(5));

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
}

static void test_adm_fmc_maximum_scatter_read(void)
{
    static const uint32_t sentinel[] = {
        0x01234567, 0x89abcdef, 0xa5a5a5a5, 0x5a5a5a5a,
    };
    const uint32_t data2 = RAM_ALIAS_BASE + 0x01000000;
    const uint32_t output = RAM_ALIAS_BASE + 0x01100000;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const size_t command_size = ADM_FMC_DESCRIPTORS +
                                (ADM_FMC_MAX_PAGES + 1) * 8;
    uint8_t expected[sizeof(sentinel)];
    uint8_t actual[sizeof(expected)];
    uint8_t result[4];
    g_autofree uint8_t *command = g_malloc0(command_size);

    for (size_t i = 0; i < G_N_ELEMENTS(sentinel); i++) {
        stl_le_p(expected + i * sizeof(uint32_t), sentinel[i]);
    }
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, ADM_FMC_MAX_PAGES);
    for (unsigned i = 0; i < ADM_FMC_MAX_PAGES; i++) {
        command[ADM_FMC_BANKS + i] = 0;
        stl_be_p(command + ADM_FMC_PAGES + i * sizeof(uint32_t), 1);
        stl_be_p(command + ADM_FMC_DESCRIPTORS + i * 8,
                 output + i * NAND_PAGE_DATA_SIZE);
        stl_be_p(command + ADM_FMC_DESCRIPTORS + i * 8 + 4,
                 NAND_PAGE_DATA_SIZE);
    }

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, command_address, command, command_size);
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);
    run_adm_fmc_command();

    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_memread(global_qtest,
                  output + (ADM_FMC_MAX_PAGES - 1) * NAND_PAGE_DATA_SIZE,
                  actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
}

static void test_adm_fmc_trailing_scatter_capacity(void)
{
    static const uint32_t sentinel[] = {
        0x01234567, 0x89abcdef, 0xa5a5a5a5, 0x5a5a5a5a,
    };
    const unsigned page_count = 128;
    const unsigned data_descriptors = 128;
    const unsigned merged_descriptor = 97;
    const uint32_t data2 = RAM_ALIAS_BASE + 0x01400000;
    const uint32_t output = RAM_ALIAS_BASE + 0x01500000;
    const uint32_t unused = output + page_count * NAND_PAGE_DATA_SIZE;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const size_t command_size = ADM_FMC_DESCRIPTORS +
                                (data_descriptors + 1) * 8;
    uint8_t expected[sizeof(sentinel)];
    uint8_t actual[sizeof(expected)];
    uint8_t result[4];
    uint32_t unused_sentinel = 0xdeadbeef;
    unsigned output_page = 0;
    g_autofree uint8_t *command = g_malloc0(command_size);

    for (size_t i = 0; i < G_N_ELEMENTS(sentinel); i++) {
        stl_le_p(expected + i * sizeof(uint32_t), sentinel[i]);
    }
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, page_count);
    for (unsigned i = 0; i + 1 < data_descriptors; i++) {
        uint32_t length = i == merged_descriptor ?
                          2 * NAND_PAGE_DATA_SIZE : NAND_PAGE_DATA_SIZE;

        stl_be_p(command + ADM_FMC_DESCRIPTORS + i * 8,
                 output + output_page * NAND_PAGE_DATA_SIZE);
        stl_be_p(command + ADM_FMC_DESCRIPTORS + i * 8 + 4, length);
        output_page += length / NAND_PAGE_DATA_SIZE;
    }
    g_assert_cmpuint(output_page, ==, page_count);
    stl_be_p(command + ADM_FMC_DESCRIPTORS +
             (data_descriptors - 1) * 8, unused);
    stl_be_p(command + ADM_FMC_DESCRIPTORS +
             (data_descriptors - 1) * 8 + 4, NAND_PAGE_DATA_SIZE);
    for (unsigned i = 0; i < page_count; i++) {
        command[ADM_FMC_BANKS + i] = 0;
        stl_be_p(command + ADM_FMC_PAGES + i * sizeof(uint32_t), 1);
    }

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, command_address, command, command_size);
    qtest_memwrite(global_qtest, unused, &unused_sentinel,
                   sizeof(unused_sentinel));
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);
    run_adm_fmc_command();

    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_memread(global_qtest,
                  output + (page_count - 1) * NAND_PAGE_DATA_SIZE,
                  actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmphex(readl(unused), ==, unused_sentinel);
}

static void test_adm_fmc_backing_page_write_and_erase(void)
{
    const uint32_t data2 = RAM_ALIAS_BASE + 0x002a0000;
    const uint32_t data3 = RAM_ALIAS_BASE + 0x002b0000;
    const uint32_t source = RAM_ALIAS_BASE + 0x002c0000;
    const uint32_t output = RAM_ALIAS_BASE + 0x002d0000;
    const uint32_t lli = RAM_ALIAS_BASE + 0x002e0000;
    const uint32_t channel = DMAC0_BASE + DMAC_CHANNEL0 +
                             5 * DMAC_CHANNEL_STRIDE;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const uint32_t page = NAND_PAGES_PER_BLOCK;
    const uint32_t dma_control = NAND_PAGE_DATA_SIZE / sizeof(uint32_t) |
                                 DMAC_CONTROL_TC_IRQ |
                                 DMAC_CONTROL_SRC_INC |
                                 DMAC_CONTROL_WORD_SRC |
                                 DMAC_CONTROL_WORD_DST;
    const uint32_t tail_control = 1 | DMAC_CONTROL_TC_IRQ |
                                  DMAC_CONTROL_SRC_INC |
                                  DMAC_CONTROL_WORD_SRC |
                                  DMAC_CONTROL_WORD_DST;
    uint8_t command[ADM_FMC_DESCRIPTORS + 2 * 8] = { 0 };
    uint8_t dma_lli[16] = { 0 };
    uint8_t expected[NAND_PAGE_DATA_SIZE];
    uint8_t actual[NAND_PAGE_DATA_SIZE];
    uint8_t pad[16];
    uint8_t actual_pad[sizeof(pad)];
    uint8_t result[4];

    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = i * 37 + 11;
    }
    for (size_t i = 0; i < sizeof(pad); i++) {
        pad[i] = 0xf0 - i * 3;
    }

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, source, expected, sizeof(expected));
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(DMAC0_IRQ));
    writel(DMAC0_BASE + DMAC_CONFIG, 1);
    writel(channel + DMAC_CHANNEL_SRC, source);
    writel(channel + DMAC_CHANNEL_DEST, NAND_BASE + NAND_FIFO);
    writel(channel + DMAC_CHANNEL_LLI, lli);
    writel(channel + DMAC_CHANNEL_CONTROL, dma_control);
    writel(channel + DMAC_CHANNEL_CONFIG,
           DMAC_CHANNEL_TC_IRQ | DMAC_FLOW_M2P |
           DMAC_DEST_PERIPH(DMAC_NAND_REQUEST) | DMAC_CHANNEL_ENABLE);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, BIT(5));
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_EVENT_ACTION3, data3);
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(ADM_IRQ - 32));
    writel(ADM_BASE + ADM_CONTROL,
           ADM_CONTROL_RUNNING | ADM_IRQ_EVENT | ADM_IRQ_COMMAND |
           ADM_IRQ_UPLOAD);
    writel(ADM_BASE + ADM_COMMAND, 4);
    qtest_memwrite(global_qtest, data3, pad, sizeof(pad));

    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_WRITE_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    stw_be_p(command + ADM_FMC_PAD_SIZE, sizeof(pad));
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, source);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    stl_le_p(dma_lli, source);
    stl_le_p(dma_lli + 4, NAND_BASE + NAND_FIFO);
    stl_le_p(dma_lli + 8, 0);
    stl_le_p(dma_lli + 12, tail_control);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    qtest_memwrite(global_qtest, lli, dma_lli, sizeof(dma_lli));
    writel(ADM_BASE + ADM_COMMAND, 1);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, BIT(5));
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, 0);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    qtest_clock_step(global_qtest, ADM_FMC_DMA_COMPLETION_NS);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, BIT(5));
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(5));
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_CONTROL), ==, tail_control);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_SRC), ==,
                    source);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_DEST), ==,
                    NAND_BASE + NAND_FIFO);
    g_assert_cmphex(readl(ADM_BASE + ADM_COMMAND), ==, 0);
    writel(DMAC0_BASE + DMAC_INT_TC_CLEAR, BIT(5));
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);

    /* The first TC arrived before the command descriptor was complete. */
    writel(command_address + ADM_FMC_DESCRIPTORS + 4,
           bswap32(sizeof(expected)));
    qtest_clock_step_next(global_qtest);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, 0);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(5));
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_CONTROL) & 0xfff, ==, 0);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_SRC), ==,
                    source + sizeof(uint32_t));
    writel(DMAC0_BASE + DMAC_INT_TC_CLEAR, BIT(5));

    writel(NAND_BASE + NAND_FMCTRL0, BIT(1) | BIT(0) | BIT(11));
    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_READ0);
    writel(NAND_BASE + NAND_FMANUM, 4);
    writel(NAND_BASE + NAND_FMADDR0, page << 16);
    writel(NAND_BASE + NAND_FMADDR1, page >> 16);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_ADDRESS);
    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_READ_CONFIRM);
    writel(NAND_BASE + NAND_FMDNUM, 15);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_READ);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(readl(NAND_BASE + NAND_FIFO), ==, 0xffffffff);
    }
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_FLUSH);

    writel(ADM_BASE + ADM_COMMAND, 2);
    /* RUN commits the record; the producer may then reuse its buffers. */
    qtest_memset(global_qtest, source, 0, sizeof(expected));
    writel(command_address + ADM_FMC_PAGES, bswap32(page + 1));
    qtest_clock_step(global_qtest, ADM_FMC_COMPLETION_NS);
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_ENABLED_CHANNELS), ==, 0);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, 0);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_CONTROL) & 0xfff, ==, 0);
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_SRC), ==,
                    source + sizeof(uint32_t));
    g_assert_cmphex(readl(channel + DMAC_CHANNEL_DEST), ==,
                    NAND_BASE + NAND_FIFO);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    writel(ADM_BASE + ADM_COMMAND, 4);
    writel(ADM_BASE + ADM_COMMAND, 1);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    stw_be_p(command + ADM_FMC_PAD_SIZE, sizeof(pad));
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4, sizeof(actual));
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    qtest_memread(global_qtest, data3, actual_pad, sizeof(actual_pad));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmpmem(actual_pad, sizeof(actual_pad), pad, sizeof(pad));
    writel(ADM_BASE + ADM_COMMAND, 4);
    writel(ADM_BASE + ADM_COMMAND, 1);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_ERASE);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_ERASE_BANK] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    writel(ADM_BASE + ADM_COMMAND, 4);
    writel(ADM_BASE + ADM_COMMAND, 1);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    stw_be_p(command + ADM_FMC_PAD_SIZE, sizeof(pad));
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4, sizeof(actual));
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    qtest_memread(global_qtest, data3, actual_pad, sizeof(actual_pad));
    for (size_t i = 0; i < sizeof(actual); i++) {
        g_assert_cmphex(actual[i], ==, 0xff);
    }
    for (size_t i = 0; i < sizeof(actual_pad); i++) {
        g_assert_cmphex(actual_pad[i], ==, 0xff);
    }
}

static void test_adm_fmc_target_preflight(void)
{
    const uint32_t data2 = RAM_ALIAS_BASE + 0x002f0000;
    const uint32_t source = RAM_ALIAS_BASE + 0x00300000;
    const uint32_t output = RAM_ALIAS_BASE + 0x00302000;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const uint32_t page = 3 * NAND_PAGES_PER_BLOCK;
    uint8_t command[ADM_FMC_DESCRIPTORS + 2 * 8] = { 0 };
    uint8_t actual[NAND_PAGE_DATA_SIZE];
    uint8_t result[4];

    qtest_system_reset(global_qtest);
    qtest_memset(global_qtest, source, 0xa5, 2 * NAND_PAGE_DATA_SIZE);
    qtest_memset(global_qtest, output, 0x6d, 2 * NAND_PAGE_DATA_SIZE);
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);

    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_ERASE);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_ERASE_BANK] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 2);
    command[ADM_FMC_BANKS] = 0;
    command[ADM_FMC_BANKS + 1] = 4;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_PAGES + 4, page + 1);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             2 * NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 2);
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    for (size_t i = 0; i < sizeof(actual); i++) {
        g_assert_cmphex(actual[i], ==, 0x6d);
    }

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_WRITE_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 2);
    command[ADM_FMC_BANKS] = 0;
    command[ADM_FMC_BANKS + 1] = 4;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_PAGES + 4, page + 1);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, source);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             2 * NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest,
                  command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 2);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ_MAX_ECC);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
    command[ADM_FMC_BANKS] = 0;
    stl_be_p(command + ADM_FMC_PAGES, page);
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4,
             NAND_PAGE_DATA_SIZE);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, output, actual, sizeof(actual));
    for (size_t i = 0; i < sizeof(actual); i++) {
        g_assert_cmphex(actual[i], ==, 0xff);
    }
}

static void test_adm_fmc_compact_targets(void)
{
    const unsigned page_count = 64;
    const unsigned ce_count = 4;
    const uint32_t data2 = RAM_ALIAS_BASE + 0x01600000;
    const uint32_t source = RAM_ALIAS_BASE + 0x01700000;
    const uint32_t output = RAM_ALIAS_BASE + 0x01800000;
    const uint32_t command_address = data2 + ADM_FMC_COMMAND_OFFSET;
    const uint32_t page = 8 * NAND_PAGES_PER_BLOCK;
    const size_t data_size = page_count * NAND_PAGE_DATA_SIZE;
    uint8_t command[ADM_FMC_DESCRIPTORS + 2 * 8] = { 0 };
    uint8_t result[4];
    g_autofree uint8_t *expected = g_malloc(data_size);
    g_autofree uint8_t *actual = g_malloc(data_size);

    for (size_t i = 0; i < data_size; i++) {
        expected[i] = i * 37 + i / NAND_PAGE_DATA_SIZE;
    }

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, source, expected, data_size);
    qtest_memset(global_qtest, output, 0x6d, data_size);
    writel(ADM_BASE + ADM_EVENT_ACTION2, data2);
    writel(ADM_BASE + ADM_CONTROL, ADM_CONTROL_RUNNING | ADM_IRQ_EVENT);

    for (unsigned bank = 0; bank < ce_count; bank++) {
        memset(command, 0, sizeof(command));
        stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_ERASE);
        stw_be_p(command + ADM_FMC_PAGE_COUNT, 1);
        command[ADM_FMC_ERASE_BANK] = bank;
        stl_be_p(command + ADM_FMC_PAGES, page);
        qtest_memwrite(global_qtest, command_address, command,
                       sizeof(command));
        run_adm_fmc_command();
    qtest_memread(global_qtest, command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
        g_assert_cmphex(ldl_le_p(result), ==, 0);
    }

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_WRITE);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, page_count);
    stw_be_p(command + ADM_FMC_CE_COUNT, ce_count);
    for (unsigned i = 0; i < ce_count; i++) {
        command[ADM_FMC_BANKS + i] = i;
        stl_be_p(command + ADM_FMC_PAGES + i * sizeof(uint32_t), page);
    }
    memset(command + ADM_FMC_BANKS + ce_count, 0xff,
           page_count - ce_count);
    for (unsigned i = ce_count; i < page_count; i++) {
        stl_be_p(command + ADM_FMC_PAGES + i * sizeof(uint32_t),
                 UINT32_MAX);
    }
    stl_be_p(command + ADM_FMC_DESCRIPTORS, source);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4, data_size);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);

    memset(command, 0, sizeof(command));
    stw_le_p(command + ADM_FMC_OPCODE, ADM_FMC_READ);
    stw_be_p(command + ADM_FMC_PAGE_COUNT, page_count);
    stw_be_p(command + ADM_FMC_CE_COUNT, ce_count);
    for (unsigned i = 0; i < ce_count; i++) {
        command[ADM_FMC_BANKS + i] = i;
        stl_be_p(command + ADM_FMC_PAGES + i * sizeof(uint32_t), page);
    }
    memset(command + ADM_FMC_BANKS + ce_count, 0xff,
           page_count - ce_count);
    for (unsigned i = ce_count; i < page_count; i++) {
        stl_be_p(command + ADM_FMC_PAGES + i * sizeof(uint32_t),
                 UINT32_MAX);
    }
    stl_be_p(command + ADM_FMC_DESCRIPTORS, output);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 4, data_size);
    stl_be_p(command + ADM_FMC_DESCRIPTORS + 12, 0);
    qtest_memwrite(global_qtest, command_address, command, sizeof(command));
    run_adm_fmc_command();
    qtest_memread(global_qtest, command_address + ADM_FMC_PAGE_COUNT,
                  result, sizeof(result));
    g_assert_cmphex(ldl_le_p(result), ==, 0);
    qtest_memread(global_qtest, output, actual, data_size);
    g_assert_cmpmem(actual, data_size, expected, data_size);
}

static void test_i2c_registers_and_nack(void)
{
    static const uint64_t base[] = { I2C0_BASE, I2C1_BASE };

    qtest_system_reset(global_qtest);
    for (unsigned i = 0; i < ARRAY_SIZE(base); i++) {
        writel(base[i] + I2C_ADDRESS, 0x52 + i);
        writel(base[i] + I2C_LINE_CONTROL, 0xa0 + i);
        g_assert_cmphex(readl(base[i] + I2C_ADDRESS), ==, 0x52 + i);
        g_assert_cmphex(readl(base[i] + I2C_LINE_CONTROL), ==, 0xa0 + i);
    }

    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(I2C0_IRQ));
    writel(I2C0_BASE + I2C_DATA, 0xfe);
    writel(I2C0_BASE + I2C_STATUS,
           I2C_STATUS_MASTER_TX | I2C_STATUS_START | I2C_STATUS_OUTPUT);

    g_assert_cmphex(readl(I2C0_BASE + I2C_OPERATION_STATUS), ==,
                    I2C_OPERATION_TRANSFER);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);

    writel(I2C0_BASE + I2C_OPERATION_IRQ_CONTROL,
           I2C_OPERATION_IRQ_ENABLE);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(I2C0_IRQ));

    writel(I2C0_BASE + I2C_OPERATION_STATUS, I2C_OPERATION_TRANSFER);
    g_assert_cmphex(readl(I2C0_BASE + I2C_CONTROL) & I2C_CONTROL_CONTINUE,
                    ==, I2C_CONTROL_CONTINUE);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(I2C0_IRQ));
    writel(I2C0_BASE + I2C_CONTROL, I2C_CONTROL_IRQ_ENABLE);
    writel(I2C0_BASE + I2C_DATA, 0xfe);
    writel(I2C0_BASE + I2C_STATUS,
           I2C_STATUS_MASTER_TX | I2C_STATUS_START | I2C_STATUS_OUTPUT);

    g_assert_cmphex(readl(I2C0_BASE + I2C_STATUS) & I2C_STATUS_NACK, ==,
                    I2C_STATUS_NACK);
    g_assert_cmphex(readl(I2C0_BASE + I2C_OPERATION_STATUS), ==,
                    I2C_OPERATION_TRANSFER);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(I2C0_IRQ));

    writel(I2C0_BASE + I2C_OPERATION_STATUS, I2C_OPERATION_TRANSFER);
    writel(I2C0_BASE + I2C_CONTROL,
           I2C_CONTROL_IRQ_ENABLE | I2C_CONTROL_CONTINUE);
    g_assert_cmphex(readl(I2C0_BASE + I2C_OPERATION_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);

    writel(I2C0_BASE + I2C_STATUS, I2C_STATUS_MASTER_TX);
    g_assert_cmphex(readl(I2C0_BASE + I2C_OPERATION_STATUS), ==,
                    I2C_OPERATION_CONDITION);
}

static void iphone3g_i2c_start(uint8_t address, bool receive)
{
    writel(I2C0_BASE + I2C_DATA, (address << 1) | receive);
    writel(I2C0_BASE + I2C_STATUS,
           (receive ? I2C_STATUS_MASTER_RX : I2C_STATUS_MASTER_TX) |
           I2C_STATUS_START | I2C_STATUS_OUTPUT);
    g_assert_cmphex(readl(I2C0_BASE + I2C_STATUS) & I2C_STATUS_NACK, ==, 0);
    g_assert_cmphex(readl(I2C0_BASE + I2C_OPERATION_STATUS) &
                    I2C_OPERATION_TRANSFER, ==, I2C_OPERATION_TRANSFER);
    writel(I2C0_BASE + I2C_OPERATION_STATUS, I2C_OPERATION_TRANSFER);
}

static void iphone3g_i2c_send(uint8_t data)
{
    writel(I2C0_BASE + I2C_DATA, data);
    writel(I2C0_BASE + I2C_CONTROL,
           I2C_CONTROL_ACK_GENERATE | I2C_CONTROL_CONTINUE);
    g_assert_cmphex(readl(I2C0_BASE + I2C_STATUS) & I2C_STATUS_NACK, ==, 0);
    g_assert_cmphex(readl(I2C0_BASE + I2C_OPERATION_STATUS) &
                    I2C_OPERATION_TRANSFER, ==, I2C_OPERATION_TRANSFER);
    writel(I2C0_BASE + I2C_OPERATION_STATUS, I2C_OPERATION_TRANSFER);
}

static void iphone3g_i2c_stop(bool receive)
{
    writel(I2C0_BASE + I2C_STATUS,
           receive ? I2C_STATUS_MASTER_RX : I2C_STATUS_MASTER_TX);
    g_assert_cmphex(readl(I2C0_BASE + I2C_OPERATION_STATUS) &
                    I2C_OPERATION_CONDITION, ==, I2C_OPERATION_CONDITION);
    writel(I2C0_BASE + I2C_OPERATION_STATUS, I2C_OPERATION_CONDITION);
    writel(I2C0_BASE + I2C_CONTROL, I2C_CONTROL_CONTINUE);
}

static void i2c_device_write(uint8_t address, uint8_t reg,
                             const uint8_t *data, size_t length)
{
    iphone3g_i2c_start(address, false);
    writel(I2C0_BASE + I2C_CONTROL,
           I2C_CONTROL_ACK_GENERATE | I2C_CONTROL_CONTINUE);
    iphone3g_i2c_send(reg);
    for (size_t i = 0; i < length; i++) {
        iphone3g_i2c_send(data[i]);
    }
    iphone3g_i2c_stop(false);
}

static void i2c_device_read(uint8_t address, uint8_t reg,
                            uint8_t *data, size_t length)
{
    g_assert_cmpuint(length, >, 0);

    iphone3g_i2c_start(address, false);
    writel(I2C0_BASE + I2C_CONTROL,
           I2C_CONTROL_ACK_GENERATE | I2C_CONTROL_CONTINUE);
    iphone3g_i2c_send(reg);

    writel(I2C0_BASE + I2C_STATUS,
           I2C_STATUS_MASTER_RX | I2C_STATUS_START);
    g_assert_cmphex(readl(I2C0_BASE + I2C_OPERATION_STATUS) &
                    I2C_OPERATION_CONDITION, ==, I2C_OPERATION_CONDITION);
    writel(I2C0_BASE + I2C_OPERATION_STATUS, I2C_OPERATION_CONDITION);
    writel(I2C0_BASE + I2C_CONTROL,
           I2C_CONTROL_ACK_GENERATE | I2C_CONTROL_CONTINUE);

    iphone3g_i2c_start(address, true);
    for (size_t i = 0; i < length; i++) {
        uint32_t control = I2C_CONTROL_CONTINUE;

        if (i + 1 < length) {
            control |= I2C_CONTROL_ACK_GENERATE;
        }
        writel(I2C0_BASE + I2C_CONTROL, control);
        g_assert_cmphex(readl(I2C0_BASE + I2C_OPERATION_STATUS) &
                        I2C_OPERATION_TRANSFER, ==,
                        I2C_OPERATION_TRANSFER);
        data[i] = readl(I2C0_BASE + I2C_DATA);
        writel(I2C0_BASE + I2C_OPERATION_STATUS,
               I2C_OPERATION_TRANSFER);
    }
    iphone3g_i2c_stop(true);
}

static void pcf50635_write(uint8_t reg, const uint8_t *data, size_t length)
{
    i2c_device_write(0x73, reg, data, length);
}

static void pcf50635_read(uint8_t reg, uint8_t *data, size_t length)
{
    i2c_device_read(0x73, reg, data, length);
}

static void isl29003_write(uint8_t reg, const uint8_t *data, size_t length)
{
    i2c_device_write(0x44, reg, data, length);
}

static void isl29003_read(uint8_t reg, uint8_t *data, size_t length)
{
    i2c_device_read(0x44, reg, data, length);
}

static bool valid_bcd(uint8_t value)
{
    return (value & 0xf) < 10 && (value >> 4) < 10;
}

static void test_i2c_pcf50635_pmu(void)
{
    const unsigned irq_group = PMU_IRQ_PIN / 32;
    const uint32_t irq_bit = BIT(PMU_IRQ_PIN % 32);
    const uint8_t written[] = { 0x5a, 0xa5 };
    const uint8_t cleared = 0;
    const uint8_t battery_adc_start = 0x01;
    const uint8_t usb_adc_start = 0x2d;
    const uint8_t accessory_adc_start = 0x7f;
    uint8_t interrupt_masks[5] = { 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t readback[sizeof(written)] = { 0 };
    uint8_t rtc[7] = { 0 };
    uint8_t interrupts[5] = { 0 };
    uint8_t adc_upper;
    uint8_t adc_lower;
    uint8_t power_status[2];
    unsigned sample;

    qtest_system_reset(global_qtest);
    writel(SYSIC_BASE + SYSIC_GPIO_INTLEVEL + irq_group * 4, 0);
    writel(SYSIC_BASE + SYSIC_GPIO_INTTYPE + irq_group * 4, irq_bit);
    writel(SYSIC_BASE + SYSIC_GPIO_INTEN + irq_group * 4, irq_bit);
    g_assert_cmphex(readl(GPIO_BASE + PMU_IRQ_PIN / 8 * GPIO_PAD_STRIDE +
                          GPIO_DAT) & BIT(PMU_IRQ_PIN % 8), ==, 0);
    pcf50635_write(0x07, interrupt_masks, sizeof(interrupt_masks));
    g_assert_cmphex(readl(GPIO_BASE + PMU_IRQ_PIN / 8 * GPIO_PAD_STRIDE +
                          GPIO_DAT) & BIT(PMU_IRQ_PIN % 8), ==,
                    BIT(PMU_IRQ_PIN % 8));
    interrupt_masks[0] &= ~BIT(2);
    pcf50635_write(0x07, interrupt_masks, sizeof(interrupt_masks));
    g_assert_cmphex(readl(GPIO_BASE + PMU_IRQ_PIN / 8 * GPIO_PAD_STRIDE +
                          GPIO_DAT) & BIT(PMU_IRQ_PIN % 8), ==, 0);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + irq_group * 4),
                    ==, irq_bit);
    writel(SYSIC_BASE + SYSIC_GPIO_INTEN + irq_group * 4, 0);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + irq_group * 4),
                    ==, 0);
    pcf50635_read(0x02, interrupts, sizeof(interrupts));
    g_assert_cmphex(interrupts[0], ==, BIT(2));
    for (size_t i = 1; i < ARRAY_SIZE(interrupts); i++) {
        g_assert_cmphex(interrupts[i], ==, 0);
    }
    g_assert_cmphex(readl(GPIO_BASE + PMU_IRQ_PIN / 8 * GPIO_PAD_STRIDE +
                          GPIO_DAT) & BIT(PMU_IRQ_PIN % 8), ==,
                    BIT(PMU_IRQ_PIN % 8));
    writel(SYSIC_BASE + SYSIC_GPIO_INTEN + irq_group * 4, irq_bit);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + irq_group * 4),
                    ==, 0);
    memset(interrupts, 0xff, sizeof(interrupts));
    pcf50635_read(0x02, interrupts, sizeof(interrupts));
    for (size_t i = 0; i < ARRAY_SIZE(interrupts); i++) {
        g_assert_cmphex(interrupts[i], ==, 0);
    }

    pcf50635_write(0x67, written, sizeof(written));
    pcf50635_read(0x67, readback, sizeof(readback));
    g_assert_cmpmem(readback, sizeof(readback), written, sizeof(written));

    pcf50635_read(0x4b, power_status, sizeof(power_status));
    g_assert_cmphex(power_status[0] & (BIT(0) | BIT(1)), ==,
                    BIT(0) | BIT(1));
    g_assert_cmphex(power_status[1] & (BIT(4) | BIT(5)), ==, BIT(5));
    pcf50635_write(0x4b, &cleared, 1);
    pcf50635_read(0x4b, power_status, sizeof(power_status));
    g_assert_cmphex(power_status[0] & (BIT(0) | BIT(1)), ==,
                    BIT(0) | BIT(1));
    g_assert_cmphex(power_status[1] & (BIT(4) | BIT(5)), ==, BIT(5));

    qtest_system_reset(global_qtest);
    memset(readback, 0, sizeof(readback));
    pcf50635_read(0x67, readback, sizeof(readback));
    g_assert_cmpmem(readback, sizeof(readback), written, sizeof(written));
    memset(interrupts, 0xff, sizeof(interrupts));
    pcf50635_read(0x02, interrupts, sizeof(interrupts));
    for (size_t i = 0; i < ARRAY_SIZE(interrupts); i++) {
        g_assert_cmphex(interrupts[i], ==, 0);
    }

    pcf50635_write(0x54, &battery_adc_start, 1);
    pcf50635_read(0x55, &adc_upper, 1);
    pcf50635_read(0x57, &adc_lower, 1);
    g_assert_cmphex(adc_lower & BIT(7), ==, BIT(7));
    sample = (adc_upper << 2) | (adc_lower & 3);
    g_assert_cmpuint(sample * 6000 / 1023, ==, 4000);

    pcf50635_write(0x54, &usb_adc_start, 1);
    pcf50635_read(0x55, &adc_upper, 1);
    pcf50635_read(0x57, &adc_lower, 1);
    sample = (adc_upper << 2) | (adc_lower & 3);
    g_assert_cmpuint(sample, ==, 0);
    g_assert_cmphex(adc_lower & BIT(7), ==, BIT(7));

    pcf50635_write(0x54, &accessory_adc_start, 1);
    pcf50635_read(0x55, &adc_upper, 1);
    pcf50635_read(0x57, &adc_lower, 1);
    sample = (adc_upper << 2) | (adc_lower & 3);
    g_assert_cmpuint(sample, ==, 1023);
    g_assert_cmphex(adc_upper >> 4, ==, 0xf);
    g_assert_cmphex(adc_lower & BIT(7), ==, BIT(7));

    pcf50635_read(0x59, rtc, sizeof(rtc));
    for (size_t i = 0; i < ARRAY_SIZE(rtc); i++) {
        if (i != 3) {
            g_assert_true(valid_bcd(rtc[i]));
        }
    }
    g_assert_cmpuint(rtc[0] & 0x7f, <=, 0x59);
    g_assert_cmpuint(rtc[1] & 0x7f, <=, 0x59);
    g_assert_cmpuint(rtc[2] & 0x3f, <=, 0x23);
    g_assert_cmpuint(rtc[3] & 7, <=, 6);
    g_assert_cmpuint(rtc[4] & 0x3f, >=, 1);
    g_assert_cmpuint(rtc[4] & 0x3f, <=, 0x31);
    g_assert_cmpuint(rtc[5] & 0x1f, >=, 1);
    g_assert_cmpuint(rtc[5] & 0x1f, <=, 0x12);
}

static void test_i2c_isl29003_ambient_light_sensor(void)
{
    const unsigned group = ALS_IRQ_PIN / 32;
    const uint32_t als_bit = BIT(ALS_IRQ_PIN % 32);
    const uint8_t threshold_high = 0x10;
    const uint8_t command_enable = BIT(7);
    const uint8_t control_clear = 0;
    uint8_t registers[8];

    qtest_system_reset(global_qtest);

    pcf50635_read(0x02, registers, 5);

    g_assert_cmphex(readl(GPIO_BASE + ALS_IRQ_PIN / 8 * GPIO_PAD_STRIDE +
                          GPIO_DAT) & BIT(ALS_IRQ_PIN % 8), ==,
                    BIT(ALS_IRQ_PIN % 8));
    g_assert_cmphex(readl(GPIO_BASE + PMU_IRQ_PIN / 8 * GPIO_PAD_STRIDE +
                          GPIO_DAT) & BIT(PMU_IRQ_PIN % 8), ==,
                    BIT(PMU_IRQ_PIN % 8));

    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(31));
    writel(SYSIC_BASE + SYSIC_GPIO_INTLEVEL + group * 4, 0);
    writel(SYSIC_BASE + SYSIC_GPIO_INTTYPE + group * 4, als_bit);
    writel(SYSIC_BASE + SYSIC_GPIO_INTEN + group * 4, als_bit);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4),
                    ==, 0);

    isl29003_read(0, registers, sizeof(registers));
    g_assert_cmphex(registers[0], ==, 0);
    g_assert_cmphex(registers[1], ==, 0);
    g_assert_cmphex(registers[2], ==, 0xff);
    g_assert_cmphex(registers[3], ==, 0);

    isl29003_write(2, &threshold_high, 1);
    isl29003_write(0, &command_enable, 1);
    isl29003_read(0, registers, sizeof(registers));
    g_assert_cmphex(registers[1] & BIT(5), ==, BIT(5));
    g_assert_cmphex(registers[4], ==, 0xff);
    g_assert_cmphex(registers[5], ==, 0x7f);
    g_assert_cmphex(registers[6], ==, 0xff);
    g_assert_cmphex(registers[7], ==, 0xff);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4),
                    ==, als_bit);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(31));

    isl29003_write(1, &control_clear, 1);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_GPIO_INTSTAT + group * 4),
                    ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
}

static uint16_t wm8991_read(uint8_t reg)
{
    uint8_t data[2];

    i2c_device_read(0x1b, reg, data, sizeof(data));
    return (data[0] << 8) | data[1];
}

static void test_i2c_wm8991_control(void)
{
    const uint8_t written[] = { 0x12, 0x34, 0xab, 0xcd };
    const uint8_t software_reset[] = { 0xde, 0xad };
    uint8_t readback[sizeof(written)] = { 0 };

    qtest_system_reset(global_qtest);
    g_assert_cmphex(wm8991_read(0), ==, 0x8990);
    g_assert_cmphex(wm8991_read(2), ==, 0x6000);
    g_assert_cmphex(wm8991_read(4), ==, 0x4050);

    i2c_device_write(0x1b, 2, written, sizeof(written));
    i2c_device_read(0x1b, 2, readback, sizeof(readback));
    g_assert_cmpmem(readback, sizeof(readback), written, sizeof(written));

    i2c_device_write(0x1b, 0, software_reset, sizeof(software_reset));
    g_assert_cmphex(wm8991_read(0), ==, 0x8990);
    g_assert_cmphex(wm8991_read(2), ==, 0x6000);

    i2c_device_write(0x1b, 0x40, written, 2);
    g_assert_cmphex(wm8991_read(0), ==, 0x8990);
}

static void test_vic_identity(void)
{
    const uint32_t expected_id[] = { 0x92, 0x11, 0x04, 0x00 };
    const uint32_t bases[] = { VIC0_BASE, VIC1_BASE };

    for (size_t vic = 0; vic < ARRAY_SIZE(bases); vic++) {
        for (size_t id = 0; id < ARRAY_SIZE(expected_id); id++) {
            g_assert_cmphex(readl(bases[vic] + VIC_PERIPH_ID + id * 4), ==,
                            expected_id[id]);
        }
        g_assert_cmphex(readl(bases[vic] + VIC_SW_PRIORITY_MASK), ==,
                        0xffff);
        g_assert_cmphex(readl(bases[vic] + VIC_DAISY_PRIORITY), ==, 0xf);
    }
}

static void test_vic_priority_and_acknowledge(void)
{
    const unsigned low_irq = 7;
    const unsigned high_irq = 3;

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_VECTOR_ADDRESS + low_irq * 4, 0x7000);
    writel(VIC0_BASE + VIC_VECTOR_PRIORITY + low_irq * 4, 5);
    writel(VIC0_BASE + VIC_VECTOR_ADDRESS + high_irq * 4, 0x3000);
    writel(VIC0_BASE + VIC_VECTOR_PRIORITY + high_irq * 4, 2);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(low_irq) | BIT(high_irq));
    writel(VIC0_BASE + VIC_SOFT_INT, BIT(low_irq) | BIT(high_irq));

    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==,
                    BIT(low_irq) | BIT(high_irq));
    g_assert_cmphex(readl(VIC0_BASE + VIC_ADDRESS), ==, 0x3000);

    writel(VIC0_BASE + VIC_SOFT_INT_CLEAR, BIT(high_irq));
    writel(VIC0_BASE + VIC_ADDRESS, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_ADDRESS), ==, 0x7000);
    writel(VIC0_BASE + VIC_SOFT_INT_CLEAR, BIT(low_irq));
    writel(VIC0_BASE + VIC_ADDRESS, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_vic_daisy_chain_acknowledge(void)
{
    const unsigned child_irq = 5;
    const uint32_t vector = 0x80000025;

    qtest_system_reset(global_qtest);
    writel(VIC1_BASE + VIC_VECTOR_ADDRESS + child_irq * 4, vector);
    writel(VIC1_BASE + VIC_VECTOR_PRIORITY + child_irq * 4, 7);
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(child_irq));
    writel(VIC1_BASE + VIC_SOFT_INT, BIT(child_irq));

    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, BIT(child_irq));
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_ADDRESS), ==, vector);

    writel(VIC1_BASE + VIC_SOFT_INT_CLEAR, BIT(child_irq));
    writel(VIC1_BASE + VIC_ADDRESS, 0);
    writel(VIC0_BASE + VIC_ADDRESS, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_edgeic_latched_interrupts(void)
{
    const unsigned low_irq = 5;
    const unsigned high_irq = 37;

    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_CONFIG0), ==, 0);
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_CONFIG1), ==, 0);
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_LOW_STATUS), ==, 0);
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_HIGH_STATUS), ==, 0);

    writel(VIC1_BASE + VIC_INT_ENABLE,
           BIT(EDGEIC_LOW_VIC_IRQ - 32) |
           BIT(EDGEIC_HIGH_VIC_IRQ - 32));
    writel(EDGEIC_BASE + EDGEIC_CONFIG0, BIT(low_irq));
    writel(EDGEIC_BASE + EDGEIC_CONFIG1, BIT(high_irq - 32));

    qtest_set_irq_in(global_qtest, "/machine/edgeic", NULL, low_irq, 1);
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_LOW_STATUS), ==,
                    BIT(low_irq));
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(EDGEIC_LOW_VIC_IRQ - 32));

    writel(EDGEIC_BASE + EDGEIC_LOW_STATUS, BIT(low_irq));
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_LOW_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
    qtest_set_irq_in(global_qtest, "/machine/edgeic", NULL, low_irq, 1);
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_LOW_STATUS), ==, 0);
    qtest_set_irq_in(global_qtest, "/machine/edgeic", NULL, low_irq, 0);
    qtest_set_irq_in(global_qtest, "/machine/edgeic", NULL, low_irq, 1);
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_LOW_STATUS), ==,
                    BIT(low_irq));

    qtest_set_irq_in(global_qtest, "/machine/edgeic", NULL, high_irq, 1);
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_HIGH_STATUS), ==,
                    BIT(high_irq - 32));
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(EDGEIC_LOW_VIC_IRQ - 32) |
                    BIT(EDGEIC_HIGH_VIC_IRQ - 32));

    writel(EDGEIC_BASE + EDGEIC_CONFIG1, 0);
    g_assert_cmphex(readl(EDGEIC_BASE + EDGEIC_HIGH_STATUS), ==,
                    BIT(high_irq - 32));
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(EDGEIC_LOW_VIC_IRQ - 32));
    writel(EDGEIC_BASE + EDGEIC_CONFIG1, BIT(high_irq - 32));
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(EDGEIC_LOW_VIC_IRQ - 32) |
                    BIT(EDGEIC_HIGH_VIC_IRQ - 32));
    writel(EDGEIC_BASE + EDGEIC_LOW_STATUS, BIT(low_irq));
    writel(EDGEIC_BASE + EDGEIC_HIGH_STATUS, BIT(high_irq - 32));
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
}

static uint64_t read_rtc_ticks(void)
{
    uint64_t high = readl(TIMER_BASE + TIMER_TICKS_HIGH);

    return high << 32 | readl(TIMER_BASE + TIMER_TICKS_LOW);
}

static void test_timer_rtc(void)
{
    uint64_t before;

    qtest_system_reset(global_qtest);
    before = read_rtc_ticks();
    qtest_clock_step(global_qtest, 1000000);
    g_assert_cmpuint(read_rtc_ticks() - before, ==, 12000);
}

static void test_timer_irq(void)
{
    const uint32_t timer_irq_status = BIT(16);
    int64_t after_first_millisecond;
    int64_t remaining;

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(TIMER_IRQ));
    writel(TIMER_BASE + TIMER_4 + TIMER_CONFIG, 0x1150);
    writel(TIMER_BASE + TIMER_4 + TIMER_COUNT_BUFFER, 12000);
    writel(TIMER_BASE + TIMER_4 + TIMER_STATE, 3);
    g_assert_cmphex(readl(TIMER_BASE + TIMER_4 + TIMER_STATE), ==, 3);

    after_first_millisecond = qtest_clock_step(global_qtest, 1000000);
    g_assert_cmphex(readl(TIMER_BASE + TIMER_IRQ_STATUS), ==, 0);
    remaining = qtest_clock_step_next(global_qtest) -
                after_first_millisecond;
    g_assert_cmpint(remaining, >=, 999999);
    g_assert_cmpint(remaining, <=, 1000000);
    g_assert_cmphex(readl(TIMER_BASE + TIMER_IRQ_STATUS), ==,
                    timer_irq_status);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==,
                    BIT(TIMER_IRQ));

    writel(TIMER_BASE + TIMER_IRQ_LATCH, timer_irq_status);
    g_assert_cmphex(readl(TIMER_BASE + TIMER_IRQ_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);

    qtest_clock_step(global_qtest, 2000000);
    g_assert_cmphex(readl(TIMER_BASE + TIMER_IRQ_STATUS), ==, 0);

    writel(TIMER_BASE + TIMER_4 + TIMER_STATE, 3);
    qtest_clock_step(global_qtest, 2000000);
    g_assert_cmphex(readl(TIMER_BASE + TIMER_IRQ_STATUS), ==,
                    timer_irq_status);
}

static void test_timer_periodic_irq(void)
{
    const uint32_t timer_irq_status = BIT(16);

    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(TIMER_IRQ));
    writel(TIMER_BASE + TIMER_4 + TIMER_CONFIG, 0x7040);
    writel(TIMER_BASE + TIMER_4 + TIMER_COUNT_BUFFER, 120000);
    writel(TIMER_BASE + TIMER_4 + TIMER_STATE, 2);
    writel(TIMER_BASE + TIMER_4 + TIMER_STATE, 1);

    qtest_clock_step(global_qtest, 10000100);
    g_assert_cmphex(readl(TIMER_BASE + TIMER_IRQ_STATUS), ==,
                    timer_irq_status);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==,
                    BIT(TIMER_IRQ));
    writel(TIMER_BASE + TIMER_IRQ_LATCH, timer_irq_status);
    qtest_clock_step(global_qtest, 10000000);
    g_assert_cmphex(readl(TIMER_BASE + TIMER_IRQ_STATUS), ==,
                    timer_irq_status);
}

static void test_clock_reset_contract(void)
{
    static const uint32_t pll_con[] = {
        0x08005000, 0x06006700, 0x35009c02, 0x08004801,
    };

    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(CLOCK1_BASE + CLOCK1_CONFIG0), ==, 0x01021000);
    g_assert_cmphex(readl(CLOCK1_BASE + CLOCK1_CONFIG1), ==, 0x51135103);
    g_assert_cmphex(readl(CLOCK1_BASE + CLOCK1_CONFIG2), ==, 0x31010000);
    for (size_t i = 0; i < ARRAY_SIZE(pll_con); i++) {
        g_assert_cmphex(readl(CLOCK1_BASE + CLOCK1_PLL0CON + i * 4), ==,
                        pll_con[i]);
    }
    g_assert_cmphex(readl(CLOCK1_BASE + CLOCK1_PLLLOCK), ==, 0xf);
    g_assert_cmphex(readl(CLOCK1_BASE + CLOCK1_PLLMODE), ==, 0x000a003a);

    writel(CLOCK0_BASE + CLOCK0_CONFIG, 5);
    writel(CLOCK0_BASE + CLOCK0_ADJ1, 0x12345678);
    writel(CLOCK0_BASE + CLOCK0_ADJ2, 0x89abcdef);
    writel(CLOCK1_BASE + CLOCK1_CL2_GATES, BIT(12));
    g_assert_cmphex(readl(CLOCK0_BASE + CLOCK0_CONFIG), ==, 5);
    g_assert_cmphex(readl(CLOCK0_BASE + CLOCK0_ADJ1), ==, 0x12345678);
    g_assert_cmphex(readl(CLOCK0_BASE + CLOCK0_ADJ2), ==, 0x89abcdef);
    g_assert_cmphex(readl(CLOCK1_BASE + CLOCK1_CL2_GATES), ==, BIT(12));

    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(CLOCK0_BASE + CLOCK0_CONFIG), ==, 0);
    g_assert_cmphex(readl(CLOCK1_BASE + CLOCK1_CL2_GATES), ==, 0);
}

static void test_chipid_contract(void)
{
    g_assert_cmphex(readl(CHIPID_BASE), ==, 0);
    g_assert_cmphex(readl(CHIPID_BASE + CHIPID_SPI_CLOCK_TYPE), ==,
                    0x02000000);
    g_assert_cmphex(readl(CHIPID_BASE + CHIPID_SECURITY_INFO), ==,
                    0x89000105);
}

static void test_lcd_panel_contract(void)
{
    const uint32_t vertical_timing = (3 << 16) | (3 << 8) | 3;
    const uint32_t horizontal_timing = (14 << 16) | (14 << 8) | 15;
    const uint32_t active_size = ((LCD_WIDTH - 1) << 16) |
                                 (LCD_HEIGHT - 1);

    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(LCD_BASE + LCD_ENABLE), ==, 0);
    g_assert_cmphex(readl(LCD_BASE + LCD_INTERRUPT_ENABLE), ==, 0);
    g_assert_cmphex(readl(LCD_BASE + LCD_INTERRUPT_STATUS), ==, 0);

    writel(LCD_BASE + LCD_VIDCON1, BIT(3));
    writel(LCD_BASE + LCD_VIDTCON0, vertical_timing);
    writel(LCD_BASE + LCD_VIDTCON1, horizontal_timing);
    writel(LCD_BASE + LCD_VIDTCON2, active_size);
    writel(LCD_BASE + LCD_VIDTCON3, 1);

    g_assert_cmphex(readl(LCD_BASE + LCD_VIDCON1), ==, 0);
    writel(LCD_BASE + LCD_VIDCON0, BIT(6));
    g_assert_cmphex(readl(LCD_BASE + LCD_VIDCON1), ==, BIT(3));
    g_assert_cmphex(readl(LCD_BASE + LCD_VIDTCON0), ==,
                    vertical_timing);
    g_assert_cmphex(readl(LCD_BASE + LCD_VIDTCON1), ==,
                    horizontal_timing);
    g_assert_cmphex(readl(LCD_BASE + LCD_VIDTCON2), ==, active_size);
    g_assert_cmphex(readl(LCD_BASE + LCD_VIDTCON3), ==, 1);
}

static void test_lcd_vsync_irq(void)
{
    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(LCD_IRQ));
    writel(LCD_BASE + LCD_INTERRUPT_ENABLE, LCD_INTERRUPT_FRAME);
    writel(LCD_BASE + LCD_CON2, LCD_CONTROL_ENABLE);
    writel(LCD_BASE + LCD_VIDCON0, 1);
    writel(LCD_BASE + LCD_ENABLE, 1);

    qtest_clock_step(global_qtest, LCD_FRAME_NS - 1);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    qtest_clock_step(global_qtest, 1);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(LCD_IRQ));
    g_assert_cmphex(readl(LCD_BASE + LCD_INTERRUPT_STATUS), ==,
                    LCD_INTERRUPT_FRAME);

    writel(LCD_BASE + LCD_INTERRUPT_STATUS, LCD_INTERRUPT_FRAME);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    g_assert_cmphex(readl(LCD_BASE + LCD_INTERRUPT_STATUS), ==, 0);
    qtest_clock_step(global_qtest, LCD_FRAME_NS);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(LCD_IRQ));

    writel(LCD_BASE + LCD_INTERRUPT_STATUS, LCD_INTERRUPT_FRAME);
    writel(LCD_BASE + LCD_INTERRUPT_ENABLE, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    qtest_clock_step(global_qtest, LCD_FRAME_NS + 1);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    g_assert_cmphex(readl(LCD_BASE + LCD_INTERRUPT_STATUS), ==,
                    LCD_INTERRUPT_FRAME);

    writel(LCD_BASE + LCD_INTERRUPT_STATUS, LCD_INTERRUPT_FRAME);
    writel(LCD_BASE + LCD_INTERRUPT_ENABLE, LCD_INTERRUPT_FRAME);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
    qtest_clock_step(global_qtest, LCD_FRAME_NS);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(LCD_IRQ));
    writel(LCD_BASE + LCD_INTERRUPT_STATUS, LCD_INTERRUPT_FRAME);
    writel(LCD_BASE + LCD_DISABLE, 1);
}

static void test_lcd_rgb565_scanout(void)
{
    const uint32_t framebuffer = RAM_ALIAS_BASE + 0x00100000;
    const uint32_t stride = LCD_WIDTH * sizeof(uint16_t);
    const char ppm_header[] = "P6\n320 480\n255\n";
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *contents = NULL;
    g_autofree uint16_t *pixels = NULL;
    g_autofree uint32_t *argb_pixels = NULL;
    gsize length;
    size_t pixel_count = LCD_WIDTH * LCD_HEIGHT;
    size_t image_offset = sizeof(ppm_header) - 1;

    qtest_system_reset(global_qtest);
    pixels = g_new0(uint16_t, pixel_count);
    pixels[0] = cpu_to_le16(0xf800);
    pixels[1] = cpu_to_le16(0x07e0);
    pixels[pixel_count - 1] = cpu_to_le16(0x001f);
    qtest_memwrite(global_qtest, framebuffer, pixels,
                   pixel_count * sizeof(*pixels));

    writel(LCD_BASE + LCD_WINDOW2 + LCD_WINDOW_HSPAN, stride);
    writel(LCD_BASE + LCD_WINDOW2 + LCD_WINDOW_FORMAT,
           BIT(16) | (LCD_RGB565 << 8));
    writel(LCD_BASE + LCD_WINDOW2 + LCD_WINDOW_ADDRESS, framebuffer);
    writel(LCD_BASE + LCD_WINDOW2 + LCD_WINDOW_SIZE,
           (LCD_WIDTH << 16) | LCD_HEIGHT);
    writel(LCD_BASE + LCD_CON2, LCD_CONTROL_ENABLE | LCD_WINDOW2_ENABLE);
    writel(LCD_BASE + LCD_VIDCON0, 1);
    writel(LCD_BASE + LCD_ENABLE, 1);

    directory = g_dir_make_tmp("qemu-iphone3g-lcd-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "scanout.ppm", NULL);
    qtest_qmp_assert_success(
        global_qtest,
        "{'execute':'screendump','arguments':{'filename': %s}}",
        filename);

    g_assert_true(g_file_get_contents(filename, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(length, ==,
                     image_offset + LCD_WIDTH * LCD_HEIGHT * 3);
    g_assert_cmpmem(contents, image_offset, ppm_header, image_offset);
    g_assert_cmpmem(contents + image_offset, 3, "\xff\x00\x00", 3);
    g_assert_cmpmem(contents + image_offset + 3, 3, "\x00\xff\x00", 3);
    g_assert_cmpmem(contents + length - 3, 3, "\x00\x00\xff", 3);

    argb_pixels = g_new0(uint32_t, pixel_count);
    argb_pixels[0] = cpu_to_le32(0xffff0000);
    argb_pixels[1] = cpu_to_le32(0xff00ff00);
    argb_pixels[pixel_count - 1] = cpu_to_le32(0xff0000ff);
    qtest_memwrite(global_qtest, framebuffer, argb_pixels,
                   pixel_count * sizeof(*argb_pixels));
    writel(LCD_BASE + LCD_WINDOW2 + LCD_WINDOW_HSPAN,
           LCD_WIDTH * sizeof(uint32_t));
    writel(LCD_BASE + LCD_WINDOW2 + LCD_WINDOW_FORMAT,
           BIT(16) | (LCD_ARGB8888 << 8));
    qtest_qmp_assert_success(
        global_qtest,
        "{'execute':'screendump','arguments':{'filename': %s}}",
        filename);

    g_clear_pointer(&contents, g_free);
    g_assert_true(g_file_get_contents(filename, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(contents + image_offset, 3, "\xff\x00\x00", 3);
    g_assert_cmpmem(contents + image_offset + 3, 3, "\x00\xff\x00", 3);
    g_assert_cmpmem(contents + length - 3, 3, "\x00\x00\xff", 3);

    g_assert_cmpint(g_unlink(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    writel(LCD_BASE + LCD_DISABLE, 1);
}

static void test_lcd_window_composition(void)
{
    const uint32_t background = RAM_ALIAS_BASE + 0x00100000;
    const uint32_t foreground = RAM_ALIAS_BASE + 0x00200000;
    const uint32_t stride = LCD_WIDTH * sizeof(uint32_t);
    const char ppm_header[] = "P6\n320 480\n255\n";
    const size_t image_offset = sizeof(ppm_header) - 1;
    const size_t pixel_count = LCD_WIDTH * LCD_HEIGHT;
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *contents = NULL;
    g_autofree uint32_t *pixels = g_new(uint32_t, pixel_count);
    gsize length;

    qtest_system_reset(global_qtest);
    for (size_t i = 0; i < pixel_count; i++) {
        pixels[i] = cpu_to_le32(0xffff0000);
    }
    qtest_memwrite(global_qtest, background, pixels,
                   pixel_count * sizeof(*pixels));
    memset(pixels, 0, pixel_count * sizeof(*pixels));
    pixels[0] = cpu_to_le32(0xff0000ff);
    pixels[1] = cpu_to_le32(0x80000080);
    qtest_memwrite(global_qtest, foreground, pixels,
                   pixel_count * sizeof(*pixels));

    writel(LCD_BASE + LCD_WINDOW1 + LCD_WINDOW_HSPAN, stride);
    writel(LCD_BASE + LCD_WINDOW1 + LCD_WINDOW_FORMAT,
           BIT(16) | (LCD_ARGB8888 << 8));
    writel(LCD_BASE + LCD_WINDOW1 + LCD_WINDOW_ADDRESS, background);
    writel(LCD_BASE + LCD_WINDOW1 + LCD_WINDOW_SIZE,
           (LCD_WIDTH << 16) | LCD_HEIGHT);
    writel(LCD_BASE + LCD_WINDOW3 + LCD_WINDOW_HSPAN, stride);
    writel(LCD_BASE + LCD_WINDOW3 + LCD_WINDOW_FORMAT,
           BIT(16) | (LCD_ARGB8888 << 8));
    writel(LCD_BASE + LCD_WINDOW3 + LCD_WINDOW_ADDRESS, foreground);
    writel(LCD_BASE + LCD_WINDOW3 + LCD_WINDOW_SIZE,
           (LCD_WIDTH << 16) | LCD_HEIGHT);
    writel(LCD_BASE + LCD_CON2,
           LCD_CONTROL_ENABLE | LCD_WINDOW1_ENABLE | LCD_WINDOW3_ENABLE);
    writel(LCD_BASE + LCD_VIDCON0, 1);
    writel(LCD_BASE + LCD_ENABLE, 1);

    directory = g_dir_make_tmp("qemu-iphone3g-lcd-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "foreground.ppm", NULL);
    qtest_qmp_assert_success(
        global_qtest,
        "{'execute':'screendump','arguments':{'filename': %s}}",
        filename);

    g_assert_true(g_file_get_contents(filename, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(length, ==,
                     image_offset + LCD_WIDTH * LCD_HEIGHT * 3);
    g_assert_cmpmem(contents + image_offset, 3, "\x00\x00\xff", 3);
    g_assert_cmpmem(contents + image_offset + 3, 3, "\x7f\x00\x80", 3);
    g_assert_cmpmem(contents + image_offset + 6, 3, "\xff\x00\x00", 3);
    g_assert_cmpmem(contents + length - 3, 3, "\xff\x00\x00", 3);

    pixels[2] = cpu_to_le32(0xff00ff00);
    qtest_memwrite(global_qtest, foreground + 2 * sizeof(*pixels),
                   &pixels[2], sizeof(*pixels));
    qtest_qmp_assert_success(
        global_qtest,
        "{'execute':'screendump','arguments':{'filename': %s}}",
        filename);
    g_clear_pointer(&contents, g_free);
    g_assert_true(g_file_get_contents(filename, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(contents + image_offset + 3, 3, "\x7f\x00\x80", 3);
    g_assert_cmpmem(contents + image_offset + 6, 3, "\x00\xff\x00", 3);
    g_assert_cmpmem(contents + length - 3, 3, "\xff\x00\x00", 3);

    g_assert_cmpint(g_unlink(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    writel(LCD_BASE + LCD_DISABLE, 1);
}

static void test_dmac_memory_copy(void)
{
    const uint32_t source = RAM_ALIAS_BASE + 0x00200000;
    const uint32_t destination = RAM_ALIAS_BASE + 0x00201000;
    const uint32_t words[] = {
        0x01234567, 0x89abcdef, 0xa5a5a5a5, 0x5a5a5a5a,
    };
    uint32_t copied[ARRAY_SIZE(words)] = { 0 };
    uint32_t control = ARRAY_SIZE(words) | DMAC_CONTROL_TC_IRQ |
                       DMAC_CONTROL_DEST_INC | DMAC_CONTROL_SRC_INC |
                       DMAC_CONTROL_WORD_SRC | DMAC_CONTROL_WORD_DST;

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, source, words, sizeof(words));
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(16));
    writel(DMAC0_BASE + DMAC_CONFIG, 1);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_SRC, source);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_DEST, destination);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_LLI, 0);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_CONTROL, control);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_CONFIG,
           DMAC_CHANNEL_TC_IRQ | DMAC_CHANNEL_ENABLE);

    qtest_memread(global_qtest, destination, copied, sizeof(copied));
    g_assert_cmpmem(copied, sizeof(copied), words, sizeof(words));
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_STATUS), ==, BIT(0));
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(0));
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(16));

    writel(DMAC0_BASE + DMAC_INT_TC_CLEAR, BIT(0));
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_dmac_identity(void)
{
    const uint32_t expected_id[] = {
        0x80, 0x10, 0x04, 0x0a, 0x0d, 0xf0, 0x05, 0xb1,
    };
    const uint32_t bases[] = { DMAC0_BASE, DMAC1_BASE };

    qtest_system_reset(global_qtest);
    for (size_t dmac = 0; dmac < ARRAY_SIZE(bases); dmac++) {
        for (size_t id = 0; id < ARRAY_SIZE(expected_id); id++) {
            g_assert_cmphex(readl(bases[dmac] + 0xfe0 + id * 4), ==,
                            expected_id[id]);
        }
    }
}

static void i2s_configure_dma(uint32_t dmac, uint32_t source,
                              uint32_t destination, uint32_t control,
                              uint32_t config)
{
    writel(dmac + DMAC_CONFIG, 1);
    writel(dmac + DMAC_CHANNEL0 + DMAC_CHANNEL_SRC, source);
    writel(dmac + DMAC_CHANNEL0 + DMAC_CHANNEL_DEST, destination);
    writel(dmac + DMAC_CHANNEL0 + DMAC_CHANNEL_LLI, 0);
    writel(dmac + DMAC_CHANNEL0 + DMAC_CHANNEL_CONTROL, control);
    writel(dmac + DMAC_CHANNEL0 + DMAC_CHANNEL_CONFIG,
           config | DMAC_CHANNEL_TC_IRQ | DMAC_CHANNEL_ENABLE);
}

static void test_i2s_registers(void)
{
    const uint32_t bases[] = { I2S0_BASE, I2S1_BASE };
    const uint32_t txcon = BIT(24) | BIT(20) | (3 << 8) | BIT(0);

    qtest_system_reset(global_qtest);
    for (size_t i = 0; i < ARRAY_SIZE(bases); i++) {
        uint32_t base = bases[i];

        g_assert_cmphex(readl(base + I2S_STATUS), ==, 0);
        writel(base + I2S_TXCON, txcon);
        writel(base + I2S_RXCON, 0x12345678);
        writel(base + I2S_TXCOM, I2S_INTERFACE_ENABLE);
        writel(base + I2S_RXCOM, I2S_DMA_ENABLE);
        writel(base + I2S_CLKCON, I2S_CLOCK_ENABLE);
        g_assert_cmphex(readl(base + I2S_TXCON), ==, txcon);
        g_assert_cmphex(readl(base + I2S_RXCON), ==, 0x12345678);
        g_assert_cmphex(readl(base + I2S_TXCOM), ==,
                        I2S_INTERFACE_ENABLE);
        g_assert_cmphex(readl(base + I2S_RXCOM), ==, I2S_DMA_ENABLE);
        g_assert_cmphex(readl(base + I2S_CLKCON), ==, I2S_CLOCK_ENABLE);
        g_assert_cmphex(readl(base + I2S_RXDATA), ==, 0);
    }
}

static void test_i2s0_dma_routing(void)
{
    const uint32_t dmacs[] = { DMAC0_BASE, DMAC1_BASE };
    const uint32_t source = RAM_ALIAS_BASE + 0x00260000;
    const uint16_t samples[] = { 0x0123, 0x4567, 0x89ab, 0xcdef };
    const uint32_t control = ARRAY_SIZE(samples) | DMAC_CONTROL_TC_IRQ |
                             DMAC_CONTROL_SRC_INC |
                             DMAC_CONTROL_HALF_SRC |
                             DMAC_CONTROL_HALF_DST;
    const uint32_t config = DMAC_FLOW_M2P | DMAC_DEST_PERIPH(0);

    for (size_t i = 0; i < ARRAY_SIZE(dmacs); i++) {
        uint32_t dmac = dmacs[i];

        qtest_system_reset(global_qtest);
        qtest_memwrite(global_qtest, source, samples, sizeof(samples));
        i2s_configure_dma(dmac, source, I2S0_BASE + I2S_TXDATA,
                          control, config);
        g_assert_cmphex(readl(dmac + DMAC_CHANNEL0 + DMAC_CHANNEL_SRC), ==,
                        source);

        writel(I2S0_BASE + I2S_CLKCON, I2S_CLOCK_ENABLE);
        writel(I2S0_BASE + I2S_TXCOM,
               I2S_DMA_ENABLE | I2S_INTERFACE_ENABLE);

        g_assert_cmphex(readl(dmac + DMAC_CHANNEL0 + DMAC_CHANNEL_SRC), ==,
                        source + sizeof(samples));
        g_assert_cmphex(readl(dmac + DMAC_CHANNEL0 + DMAC_CHANNEL_DEST), ==,
                        I2S0_BASE + I2S_TXDATA);
        g_assert_cmphex(readl(dmac + DMAC_CHANNEL0 + DMAC_CHANNEL_CONTROL) &
                        0xfff, ==, 0);
        g_assert_cmphex(readl(dmac + DMAC_INT_TC_STATUS), ==, BIT(0));
    }
}

static void test_i2s1_dma_routing(void)
{
    const uint32_t source = RAM_ALIAS_BASE + 0x00261000;
    const uint32_t destination = RAM_ALIAS_BASE + 0x00262000;
    const uint16_t samples[] = { 0x1357, 0x2468, 0xabcd, 0xef01 };
    uint16_t received[ARRAY_SIZE(samples)];
    uint32_t tx_control = ARRAY_SIZE(samples) | DMAC_CONTROL_TC_IRQ |
                          DMAC_CONTROL_SRC_INC | DMAC_CONTROL_HALF_SRC |
                          DMAC_CONTROL_HALF_DST;
    uint32_t rx_control = ARRAY_SIZE(samples) | DMAC_CONTROL_TC_IRQ |
                          DMAC_CONTROL_DEST_INC | DMAC_CONTROL_HALF_SRC |
                          DMAC_CONTROL_HALF_DST;

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, source, samples, sizeof(samples));
    i2s_configure_dma(DMAC0_BASE, source, I2S1_BASE + I2S_TXDATA,
                      tx_control, DMAC_FLOW_M2P | DMAC_DEST_PERIPH(2));
    writel(I2S1_BASE + I2S_CLKCON, I2S_CLOCK_ENABLE);
    writel(I2S1_BASE + I2S_TXCOM,
           I2S_DMA_ENABLE | I2S_INTERFACE_ENABLE);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_SRC), ==,
                    source);
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, 0);

    qtest_system_reset(global_qtest);
    qtest_memwrite(global_qtest, source, samples, sizeof(samples));
    i2s_configure_dma(DMAC1_BASE, source, I2S1_BASE + I2S_TXDATA,
                      tx_control, DMAC_FLOW_M2P | DMAC_DEST_PERIPH(2));
    writel(I2S1_BASE + I2S_CLKCON, I2S_CLOCK_ENABLE);
    writel(I2S1_BASE + I2S_TXCOM,
           I2S_DMA_ENABLE | I2S_INTERFACE_ENABLE);
    g_assert_cmphex(readl(DMAC1_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_SRC), ==,
                    source + sizeof(samples));
    g_assert_cmphex(readl(DMAC1_BASE + DMAC_INT_TC_STATUS), ==, BIT(0));

    qtest_system_reset(global_qtest);
    memset(received, 0xff, sizeof(received));
    qtest_memwrite(global_qtest, destination, received, sizeof(received));
    i2s_configure_dma(DMAC1_BASE, I2S1_BASE + I2S_RXDATA, destination,
                      rx_control, DMAC_FLOW_P2M | DMAC_SOURCE_PERIPH(3));
    writel(I2S1_BASE + I2S_CLKCON, I2S_CLOCK_ENABLE);
    writel(I2S1_BASE + I2S_RXCOM,
           I2S_DMA_ENABLE | I2S_INTERFACE_ENABLE);
    qtest_memread(global_qtest, destination, received, sizeof(received));
    for (size_t i = 0; i < ARRAY_SIZE(received); i++) {
        g_assert_cmphex(received[i], ==, 0);
    }
    g_assert_cmphex(readl(DMAC1_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_DEST), ==,
                    destination + sizeof(received));
    g_assert_cmphex(readl(DMAC1_BASE + DMAC_INT_TC_STATUS), ==, BIT(0));
}

static void test_nand_identity(void)
{
    const uint32_t completion_mask = NAND_STATUS_CMD_DONE |
                                     NAND_STATUS_ADDR_DONE |
                                     NAND_STATUS_XFER_DONE |
                                     NAND_STATUS_BANK_READY_MASK;

    qtest_system_reset(global_qtest);
    writel(NAND_BASE + NAND_FMCTRL0, BIT(1) | BIT(0) | BIT(11));
    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_RESET);
    g_assert_cmphex(readl(NAND_BASE + NAND_FMCSTAT) &
                    (NAND_STATUS_READY | completion_mask), ==,
                    NAND_STATUS_READY | NAND_STATUS_CMD_DONE |
                    NAND_STATUS_BANK_READY(0));
    writel(NAND_BASE + NAND_FMCSTAT,
           NAND_STATUS_CMD_DONE | NAND_STATUS_BANK_READY(0));

    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_READ_ID);
    g_assert_cmphex(readl(NAND_BASE + NAND_FMCSTAT) & completion_mask, ==,
                    NAND_STATUS_CMD_DONE | NAND_STATUS_BANK_READY(0));
    writel(NAND_BASE + NAND_FMCSTAT,
           NAND_STATUS_CMD_DONE | NAND_STATUS_BANK_READY(0));
    writel(NAND_BASE + NAND_FMANUM, 0);
    writel(NAND_BASE + NAND_FMADDR0, 0);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_ADDRESS);
    g_assert_cmphex(readl(NAND_BASE + NAND_FMCSTAT) & completion_mask, ==,
                    NAND_STATUS_ADDR_DONE);
    writel(NAND_BASE + NAND_FMDNUM, 8);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_READ);
    g_assert_cmphex(readl(NAND_BASE + NAND_FMCSTAT) &
                    NAND_STATUS_XFER_DONE, ==, NAND_STATUS_XFER_DONE);
    g_assert_cmphex(readl(NAND_BASE + NAND_FIFO), ==, NAND_DEVICE_ID);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_FLUSH);

    writel(NAND_BASE + NAND_FMCTRL0, BIT(5) | BIT(0) | BIT(11));
    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_RESET);
    g_assert_cmphex(readl(NAND_BASE + NAND_FMCSTAT) &
                    (NAND_STATUS_CMD_DONE | NAND_STATUS_BANK_READY(4)), ==,
                    NAND_STATUS_CMD_DONE);
    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_READ_ID);
    writel(NAND_BASE + NAND_FMANUM, 0);
    writel(NAND_BASE + NAND_FMADDR0, 0);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_ADDRESS);
    writel(NAND_BASE + NAND_FMDNUM, 8);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_READ);
    g_assert_cmphex(readl(NAND_BASE + NAND_FIFO), ==, 0);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_FLUSH);
}

static void test_nand_dma_page_read(void)
{
    const uint32_t destination = RAM_ALIAS_BASE + 0x00202000;
    uint32_t words[4] = { 0 };
    uint32_t control = ARRAY_SIZE(words) |
                       DMAC_CONTROL_TC_IRQ | DMAC_CONTROL_DEST_INC |
                       DMAC_CONTROL_WORD_SRC | DMAC_CONTROL_WORD_DST;
    uint32_t config = DMAC_CHANNEL_TC_IRQ | DMAC_FLOW_P2M |
                      DMAC_SOURCE_PERIPH(2) | DMAC_CHANNEL_ENABLE;

    qtest_system_reset(global_qtest);
    writel(NAND_BASE + NAND_FMCTRL0, BIT(1) | BIT(0) | BIT(11));
    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_READ0);
    writel(NAND_BASE + NAND_FMANUM, 4);
    writel(NAND_BASE + NAND_FMADDR0, 0);
    writel(NAND_BASE + NAND_FMADDR1, 0);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_ADDRESS);
    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_READ_CONFIRM);
    writel(NAND_BASE + NAND_FMDNUM, sizeof(words) - 1);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_READ);

    writel(DMAC0_BASE + DMAC_CONFIG, 1);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_SRC,
           NAND_BASE + NAND_FIFO);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_DEST, destination);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_LLI, 0);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_CONTROL, control);
    writel(DMAC0_BASE + DMAC_CHANNEL0 + DMAC_CHANNEL_CONFIG, config);

    qtest_memread(global_qtest, destination, words, sizeof(words));
    for (size_t i = 0; i < ARRAY_SIZE(words); i++) {
        g_assert_cmphex(words[i], ==, UINT32_MAX);
    }
    g_assert_cmphex(readl(DMAC0_BASE + DMAC_INT_TC_STATUS), ==, BIT(0));
    g_assert_cmphex(readl(NAND_BASE + NAND_FMCSTAT) &
                    NAND_STATUS_XFER_DONE, ==, NAND_STATUS_XFER_DONE);
    writel(DMAC0_BASE + DMAC_INT_TC_CLEAR, BIT(0));
    writel(NAND_BASE + NAND_FMCSTAT, NAND_STATUS_XFER_DONE);
}

static void test_nand_backing_page_read(void)
{
    static const uint32_t expected[] = {
        0x01234567, 0x89abcdef, 0xa5a5a5a5, 0x5a5a5a5a,
    };

    qtest_system_reset(global_qtest);
    writel(NAND_BASE + NAND_FMCTRL0, BIT(1) | BIT(0) | BIT(11));
    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_READ0);
    writel(NAND_BASE + NAND_FMANUM, 4);
    writel(NAND_BASE + NAND_FMADDR0, 1U << 16);
    writel(NAND_BASE + NAND_FMADDR1, 0);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_ADDRESS);
    writel(NAND_BASE + NAND_COMMAND, NAND_CMD_READ_CONFIRM);
    writel(NAND_BASE + NAND_FMDNUM, sizeof(expected) - 1);
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_READ);

    for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(readl(NAND_BASE + NAND_FIFO), ==, expected[i]);
    }
    writel(NAND_BASE + NAND_FMCTRL1, NAND_FMCTRL1_FLUSH);
}

static void test_nand_ecc_irq(void)
{
    const uint32_t data = RAM_ALIAS_BASE + 0x00203000;
    const uint32_t ecc = RAM_ALIAS_BASE + 0x00203800;

    qtest_system_reset(global_qtest);
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(11));
    writel(NAND_ECC_BASE + NAND_ECC_DATA, data);
    writel(NAND_ECC_BASE + NAND_ECC_CODE, ecc);
    writel(NAND_ECC_BASE + NAND_ECC_SETUP, 0);
    writel(NAND_ECC_BASE + NAND_ECC_START, 1);

    g_assert_cmphex(readl(NAND_ECC_BASE + NAND_ECC_STATUS), ==, 0);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, BIT(11));
    writel(NAND_ECC_BASE + NAND_ECC_CLEARINT, 1);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_sdio_no_device_command(void)
{
    const uint32_t command = 5 | BIT(6);

    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_STATE), ==, 0);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_DATA_STATUS), ==,
                    SDIO_COMMAND_READY);

    writel(SDIO_BASE + SDIO_CTRL, 1);
    writel(SDIO_BASE + SDIO_DCTRL, 3);
    writel(SDIO_BASE + SDIO_CLKDIV, 0x20);
    writel(SDIO_BASE + SDIO_CSR, 2);
    writel(SDIO_BASE + SDIO_ARGUMENT, 0);
    writel(SDIO_BASE + SDIO_COMMAND, command);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_STATE) & 0x70, ==, 0);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_DATA_STATUS), ==,
                    SDIO_COMMAND_READY);

    writel(SDIO_BASE + SDIO_COMMAND, command | SDIO_COMMAND_START);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_DATA_STATUS), ==,
                    SDIO_COMMAND_READY | SDIO_COMMAND_DONE);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_RESPONSE0), ==, 0);

    writel(SDIO_BASE + SDIO_STATUS_ACK,
           readl(SDIO_BASE + SDIO_DATA_STATUS));
    g_assert_cmphex(readl(SDIO_BASE + SDIO_DATA_STATUS), ==,
                    SDIO_COMMAND_READY);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_CTRL), ==, 1);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_DCTRL), ==, 3);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_CLKDIV), ==, 0x20);
    g_assert_cmphex(readl(SDIO_BASE + SDIO_CSR), ==, 2);
}

static void test_watchdog_interrupt_and_reload(void)
{
    const uint32_t control = WATCHDOG_ENABLE | WATCHDOG_INTERRUPT |
                             (15 << 16) | (4 << 12) | WATCHDOG_CLEAR;

    qtest_system_reset(global_qtest);
    writel(VIC1_BASE + VIC_INT_ENABLE, BIT(WATCHDOG_IRQ - 32));
    writel(WATCHDOG_BASE + WATCHDOG_CONTROL, control);
    g_assert_cmphex(readl(WATCHDOG_BASE + WATCHDOG_CONTROL), ==, control);
    g_assert_cmpuint(readl(WATCHDOG_BASE + WATCHDOG_COUNT), >, 0);

    qtest_clock_step(global_qtest, WATCHDOG_PERIOD_NS - 1);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
    qtest_clock_step(global_qtest, 1);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==,
                    BIT(WATCHDOG_IRQ - 32));
    g_assert_cmphex(readl(WATCHDOG_BASE + WATCHDOG_COUNT), ==, 0);

    writel(WATCHDOG_BASE + WATCHDOG_CONTROL, control);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
    writel(WATCHDOG_BASE + WATCHDOG_CONTROL,
           WATCHDOG_CLEAR | WATCHDOG_DISABLE);
    qtest_clock_step(global_qtest, WATCHDOG_PERIOD_NS + 1);
    g_assert_cmphex(readl(VIC1_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_usb_hardware_contract(void)
{
    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(USB_BASE + USB_GHWCFG1), ==, 0x00000000);
    g_assert_cmphex(readl(USB_BASE + USB_GHWCFG2), ==, 0x7a8f60d0);
    g_assert_cmphex(readl(USB_BASE + USB_GHWCFG3), ==, 0x082000e8);
    g_assert_cmphex(readl(USB_BASE + USB_GHWCFG4), ==, 0x01f08024);

    writel(USB_BASE + USB_GRSTCTL, BIT(0) | BIT(5));
    g_assert_cmphex(readl(USB_BASE + USB_GRSTCTL), ==, USB_AHB_IDLE);
}

static void test_usb_nak_commands(void)
{
    uint32_t dctl;

    qtest_system_reset(global_qtest);
    dctl = readl(USB_BASE + USB_DCTL);
    writel(USB_BASE + USB_DCTL,
           dctl | USB_SET_GLOBAL_IN_NAK | USB_SET_GLOBAL_OUT_NAK);
    dctl = readl(USB_BASE + USB_DCTL);
    g_assert_cmphex(dctl & (USB_GLOBAL_IN_NAK_STATUS |
                           USB_GLOBAL_OUT_NAK_STATUS), ==,
                    USB_GLOBAL_IN_NAK_STATUS | USB_GLOBAL_OUT_NAK_STATUS);
    g_assert_cmphex(dctl & (USB_SET_GLOBAL_IN_NAK |
                           USB_SET_GLOBAL_OUT_NAK), ==, 0);

    writel(USB_BASE + USB_DCTL,
           dctl | USB_CLEAR_GLOBAL_IN_NAK | USB_CLEAR_GLOBAL_OUT_NAK);
    dctl = readl(USB_BASE + USB_DCTL);
    g_assert_cmphex(dctl & (USB_GLOBAL_IN_NAK_STATUS |
                           USB_GLOBAL_OUT_NAK_STATUS), ==, 0);

    writel(USB_BASE + USB_IN_EP0_CONTROL,
           USB_EP_ACTIVE | USB_EP_SET_NAK);
    g_assert_cmphex(readl(USB_BASE + USB_IN_EP0_CONTROL), ==,
                    USB_EP_ACTIVE | USB_EP_NAK_STATUS);
    g_assert_cmphex(readl(USB_BASE + USB_IN_EP0_INTERRUPT), ==,
                    USB_IN_NAK_EFFECTIVE);

    writel(USB_BASE + USB_IN_EP0_CONTROL,
           USB_EP_ACTIVE | USB_EP_ENABLE | USB_EP_DISABLE |
           USB_EP_CLEAR_NAK);
    g_assert_cmphex(readl(USB_BASE + USB_IN_EP0_CONTROL), ==,
                    USB_EP_ACTIVE);
    g_assert_cmphex(readl(USB_BASE + USB_IN_EP0_INTERRUPT), ==,
                    USB_IN_NAK_EFFECTIVE | USB_EP_DISABLED);

    writel(USB_BASE + USB_OUT_EP0_CONTROL,
           USB_EP_ACTIVE | USB_EP_SET_NAK);
    g_assert_cmphex(readl(USB_BASE + USB_OUT_EP0_CONTROL), ==,
                    USB_EP_ACTIVE | USB_EP_NAK_STATUS);
}

static void test_usb_irq_to_vic(void)
{
    qtest_system_reset(global_qtest);
    writel(VIC0_BASE + VIC_INT_ENABLE, BIT(19));
    writel(USB_BASE + USB_GAHBCFG, USB_GLOBAL_IRQ);
    writel(USB_BASE + USB_GINTMSK, USB_IN_EP_IRQ);
    writel(USB_BASE + USB_DIEPMSK, USB_IN_NAK_EFFECTIVE);
    writel(USB_BASE + USB_DAINTMSK, BIT(0));
    writel(USB_BASE + USB_IN_EP0_CONTROL,
           USB_EP_ACTIVE | USB_EP_SET_NAK);

    g_assert_cmphex(readl(USB_BASE + USB_GINTSTS) & USB_IN_EP_IRQ, ==,
                    USB_IN_EP_IRQ);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, BIT(19));

    writel(USB_BASE + USB_IN_EP0_INTERRUPT, USB_IN_NAK_EFFECTIVE);
    g_assert_cmphex(readl(VIC0_BASE + VIC_IRQ_STATUS), ==, 0);
}

static void test_usb_phy_registers(void)
{
    qtest_system_reset(global_qtest);
    writel(USB_PHY_BASE + 0x00, 0x1f);
    writel(USB_PHY_BASE + 0x04, 0x03);
    writel(USB_PHY_BASE + 0x08, 0x07);
    g_assert_cmphex(readl(USB_PHY_BASE + 0x00), ==, 0x1f);
    g_assert_cmphex(readl(USB_PHY_BASE + 0x04), ==, 0x03);
    g_assert_cmphex(readl(USB_PHY_BASE + 0x08), ==, 0x07);
}

static void test_vrom_transition(void)
{
    const uint32_t marker = 0x12345678;

    writel(RAM_ALIAS_BASE, marker);
    g_assert_cmphex(readl(0), ==, 0);

    writel(SYSIC_BASE + SYSIC_POWER_OFFCTRL, SYSIC_POWER_VROM);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_POWER_STATE), ==,
                    SYSIC_POWER_VROM);
    g_assert_cmphex(readl(0), ==, marker);

    writel(SYSIC_BASE + SYSIC_POWER_ONCTRL, SYSIC_POWER_VROM);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_POWER_STATE), ==, 0);
    g_assert_cmphex(readl(0), ==, 0);
}

static void test_ram_windows(void)
{
    const uint32_t offset = 0x10000;

    writel(RAM_ALIAS_BASE + offset, 0x0badc0de);
    writel(IBOOT_RAM_BASE + offset, 0x13579bdf);
    writel(SRAM_BASE, 0x11223344);
    writel(SRAM_BASE + SRAM_SIZE - sizeof(uint32_t), 0x55667788);
    g_assert_cmphex(readl(RAM_ALIAS_BASE + offset), ==, 0x0badc0de);
    g_assert_cmphex(readl(offset), ==, 0x0badc0de);
    g_assert_cmphex(readl(IBOOT_RAM_BASE + offset), ==, 0x13579bdf);
    g_assert_cmphex(readl(SRAM_BASE), ==, 0x11223344);
    g_assert_cmphex(readl(SRAM_BASE + SRAM_SIZE - sizeof(uint32_t)), ==,
                    0x55667788);
}

static void test_reset_restores_vrom(void)
{
    writel(RAM_ALIAS_BASE, 0xa5a5a5a5);
    writel(SYSIC_BASE + SYSIC_POWER_OFFCTRL, SYSIC_POWER_VROM);
    g_assert_cmphex(readl(0), ==, 0xa5a5a5a5);

    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(SYSIC_BASE + SYSIC_POWER_STATE), ==, 0);
    g_assert_cmphex(readl(0), ==, 0);
}

int main(int argc, char **argv)
{
    static const uint32_t sentinel[] = {
        0x01234567, 0x89abcdef, 0xa5a5a5a5, 0x5a5a5a5a,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *nand_path = NULL;
    g_autofree char *nor_path = NULL;
    g_autofree char *kbag_path = NULL;
    g_autofree char *machine_args = NULL;
    g_autofree uint8_t *pages = NULL;
    g_autofree uint8_t *nor = NULL;
    int fd;
    int ret;
    uint8_t kbag_bundle[AES_KBAG_BUNDLE_SIZE] = { 0 };

    g_test_init(&argc, &argv, NULL);

    fd = g_file_open_tmp("qemu-iphone3g-nand-XXXXXX", &nand_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, NAND_BACKING_SIZE), ==, 0);
    pages = g_malloc(2 * NAND_PAGE_TOTAL_SIZE);
    memset(pages, 0xff, 2 * NAND_PAGE_TOTAL_SIZE);
    for (size_t i = 0; i < ARRAY_SIZE(sentinel); i++) {
        stl_le_p(pages + NAND_PAGE_TOTAL_SIZE + i * sizeof(uint32_t),
                 sentinel[i]);
    }
    g_assert_cmpint(qemu_write_full(fd, pages, 2 * NAND_PAGE_TOTAL_SIZE), ==,
                    2 * NAND_PAGE_TOTAL_SIZE);
    g_assert_cmpint(lseek(fd,
                         (off_t)NAND_PAGES_PER_BLOCK * NAND_PAGE_TOTAL_SIZE,
                         SEEK_SET), ==,
                    (off_t)NAND_PAGES_PER_BLOCK * NAND_PAGE_TOTAL_SIZE);
    g_assert_cmpint(qemu_write_full(fd, pages, NAND_PAGE_TOTAL_SIZE), ==,
                    NAND_PAGE_TOTAL_SIZE);
    g_assert_cmpint(close(fd), ==, 0);

    fd = g_file_open_tmp("qemu-iphone3g-kbags-XXXXXX", &kbag_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    memcpy(kbag_bundle, "S5KBG01", 7);
    stl_le_p(kbag_bundle + 8, 1);
    stl_le_p(kbag_bundle + 12, 32);
    memcpy(kbag_bundle + 16, aes_test_kbag_wrapped,
           sizeof(aes_test_kbag_wrapped));
    memcpy(kbag_bundle + 48, aes_test_kbag_clear,
           sizeof(aes_test_kbag_clear));
    g_assert_cmpint(qemu_write_full(fd, kbag_bundle, sizeof(kbag_bundle)),
                    ==, sizeof(kbag_bundle));
    g_assert_cmpint(close(fd), ==, 0);

    fd = g_file_open_tmp("qemu-iphone3g-nor-XXXXXX", &nor_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    nor = g_malloc(NOR_SIZE);
    memset(nor, 0xff, NOR_SIZE);
    stl_le_p(nor + NOR_SENTINEL_OFFSET, sentinel[0]);
    g_assert_cmpint(qemu_write_full(fd, nor, NOR_SIZE), ==, NOR_SIZE);
    g_assert_cmpint(close(fd), ==, 0);

    qtest_add_func("/iphone3g/sysic/identity", test_sysic_identity);
    qtest_add_func("/iphone3g/sysic/memory-ready",
                   test_sysic_memory_ready);
    qtest_add_func("/iphone3g/gpio/edge-and-level-interrupts",
                   test_gpio_edge_and_level_interrupts);
    qtest_add_func("/iphone3g/gpio/function-select-output",
                   test_gpio_function_select_output);
    qtest_add_func("/iphone3g/gpio/n82-buttons",
                   test_gpio_n82_buttons);
    qtest_add_func("/iphone3g/spi/nor-contract", test_spi_nor_contract);
    qtest_add_func("/iphone3g/spi/nor-full-duplex",
                   test_spi_nor_full_duplex);
    qtest_add_func("/iphone3g/spi/nor-apple-refill-read",
                   test_spi_nor_apple_refill_read);
    qtest_add_func("/iphone3g/spi/apple-service-start",
                   test_spi_apple_service_start);
    qtest_add_func("/iphone3g/spi/nor-apple-aai-word-program",
                   test_spi_nor_apple_aai_word_program);
    qtest_add_func("/iphone3g/spi/merlot-panel-id",
                   test_spi_merlot_panel_id);
    qtest_add_func("/iphone3g/spi/merlot-apple-driver-phasing",
                   test_spi_merlot_apple_driver_phasing);
    qtest_add_func("/iphone3g/spi/touch-control",
                   test_spi_touch_control);
    qtest_add_func("/iphone3g/spi/touch-hbpp-full-duplex",
                   test_spi_touch_hbpp_full_duplex);
    qtest_add_func("/iphone3g/spi/touch-dma-bootload",
                   test_spi_touch_dma_bootload);
    qtest_add_func("/iphone3g/spi/touch-apple-refill",
                   test_spi_touch_apple_refill);
    qtest_add_func("/iphone3g/spi/touch-frames",
                   test_spi_touch_frames);
    qtest_add_func("/iphone3g/spi/touch-atn-routing",
                   test_spi_touch_atn_routing);
    qtest_add_func("/iphone3g/uart/registers-and-loopback",
                   test_uart_registers_and_loopback);
    qtest_add_func("/iphone3g/uart/fifo-full-and-reset",
                   test_uart_fifo_full_and_reset);
    qtest_add_func("/iphone3g/aes/custom-key-dma", test_aes_custom_key_dma);
    qtest_add_func("/iphone3g/aes/polled-completion",
                   test_aes_polled_completion);
    qtest_add_func("/iphone3g/aes/segmented-dma", test_aes_segmented_dma);
    qtest_add_func("/iphone3g/aes/gid-secret", test_aes_gid_secret);
    qtest_add_func("/iphone3g/aes/ios2-ramdisk-dma-span",
                   test_aes_ios2_ramdisk_dma_span);
    qtest_add_func("/iphone3g/aes/ios2-devicetree-partial-block",
                   test_aes_ios2_devicetree_partial_block);
    qtest_add_func("/iphone3g/aes/gid-kbag-oracle",
                   test_aes_gid_kbag_oracle);
    qtest_add_func("/iphone3g/aes/key-iv-subword-access",
                   test_aes_key_iv_subword_access);
    qtest_add_func("/iphone3g/mbx/wrapper-contract",
                   test_mbx_wrapper_contract);
    qtest_add_func("/iphone3g/mbx/2d-copy", test_mbx_2d_copy);
    qtest_add_func("/iphone3g/mbx/ta-capture-and-handshakes",
                   test_mbx_ta_capture_and_handshakes);
    qtest_add_func("/iphone3g/tvout/ordered-apertures",
                   test_tvout_ordered_apertures);
    qtest_add_func("/iphone3g/sha1/dma-continuation",
                   test_sha1_dma_continuation);
    qtest_add_func("/iphone3g/sha1/pio-read-modify-write",
                   test_sha1_pio_read_modify_write);
    qtest_add_func("/iphone3g/pke/rsa-public", test_pke_rsa_public);
    qtest_add_func("/iphone3g/adm/registers-and-interrupts",
                   test_adm_registers_and_interrupts);
    qtest_add_func("/iphone3g/adm/fmc-startup-device-ids",
                   test_adm_fmc_startup_device_ids);
    qtest_add_func("/iphone3g/adm/fmc-init-command",
                   test_adm_fmc_init_command);
    qtest_add_func("/iphone3g/adm/fmc-empty-page-result",
                   test_adm_fmc_empty_page_result);
    qtest_add_func("/iphone3g/adm/fmc-backing-page-read",
                   test_adm_fmc_backing_page_read);
    qtest_add_func("/iphone3g/adm/fmc-late-read-dma-arm",
                   test_adm_fmc_late_read_dma_arm);
    qtest_add_func("/iphone3g/adm/fmc-early-write-dma-arm",
                   test_adm_fmc_early_write_dma_arm);
    qtest_add_func("/iphone3g/adm/fmc-write-run-snapshot",
                   test_adm_fmc_write_run_snapshot);
    qtest_add_func("/iphone3g/adm/fmc-dma-before-run-uses-run-record",
                   test_adm_fmc_dma_before_run_uses_run_record);
    qtest_add_func("/iphone3g/adm/fmc-late-write-dma-arm",
                   test_adm_fmc_late_write_dma_arm);
    qtest_add_func("/iphone3g/adm/fmc-maximum-scatter-read",
                   test_adm_fmc_maximum_scatter_read);
    qtest_add_func("/iphone3g/adm/fmc-trailing-scatter-capacity",
                   test_adm_fmc_trailing_scatter_capacity);
    qtest_add_func("/iphone3g/adm/fmc-backing-page-write-and-erase",
                   test_adm_fmc_backing_page_write_and_erase);
    qtest_add_func("/iphone3g/adm/fmc-target-preflight",
                   test_adm_fmc_target_preflight);
    qtest_add_func("/iphone3g/adm/fmc-compact-targets",
                   test_adm_fmc_compact_targets);
    qtest_add_func("/iphone3g/i2c/registers-and-nack",
                   test_i2c_registers_and_nack);
    qtest_add_func("/iphone3g/i2c/pcf50635-pmu",
                   test_i2c_pcf50635_pmu);
    qtest_add_func("/iphone3g/i2c/isl29003-ambient-light-sensor",
                   test_i2c_isl29003_ambient_light_sensor);
    qtest_add_func("/iphone3g/i2c/wm8991-control",
                   test_i2c_wm8991_control);
    qtest_add_func("/iphone3g/vic/identity", test_vic_identity);
    qtest_add_func("/iphone3g/vic/priority-and-acknowledge",
                   test_vic_priority_and_acknowledge);
    qtest_add_func("/iphone3g/vic/daisy-chain-acknowledge",
                   test_vic_daisy_chain_acknowledge);
    qtest_add_func("/iphone3g/edgeic/latched-interrupts",
                   test_edgeic_latched_interrupts);
    qtest_add_func("/iphone3g/timer/rtc", test_timer_rtc);
    qtest_add_func("/iphone3g/timer/irq", test_timer_irq);
    qtest_add_func("/iphone3g/timer/periodic-irq",
                   test_timer_periodic_irq);
    qtest_add_func("/iphone3g/clock/reset-contract",
                   test_clock_reset_contract);
    qtest_add_func("/iphone3g/chipid/contract", test_chipid_contract);
    qtest_add_func("/iphone3g/lcd/panel-contract",
                   test_lcd_panel_contract);
    qtest_add_func("/iphone3g/lcd/vsync-irq", test_lcd_vsync_irq);
    qtest_add_func("/iphone3g/lcd/rgb565-scanout",
                   test_lcd_rgb565_scanout);
    qtest_add_func("/iphone3g/lcd/window-composition",
                   test_lcd_window_composition);
    qtest_add_func("/iphone3g/dmac/identity", test_dmac_identity);
    qtest_add_func("/iphone3g/dmac/memory-copy", test_dmac_memory_copy);
    qtest_add_func("/iphone3g/i2s/registers", test_i2s_registers);
    qtest_add_func("/iphone3g/i2s/i2s0-dma-routing",
                   test_i2s0_dma_routing);
    qtest_add_func("/iphone3g/i2s/i2s1-dma-routing",
                   test_i2s1_dma_routing);
    qtest_add_func("/iphone3g/nand/identity", test_nand_identity);
    qtest_add_func("/iphone3g/nand/dma-page-read",
                   test_nand_dma_page_read);
    qtest_add_func("/iphone3g/nand/backing-page-read",
                   test_nand_backing_page_read);
    qtest_add_func("/iphone3g/nand/ecc-irq", test_nand_ecc_irq);
    qtest_add_func("/iphone3g/sdio/no-device-command",
                   test_sdio_no_device_command);
    qtest_add_func("/iphone3g/watchdog/interrupt-and-reload",
                   test_watchdog_interrupt_and_reload);
    qtest_add_func("/iphone3g/usb/hardware-contract",
                   test_usb_hardware_contract);
    qtest_add_func("/iphone3g/usb/nak-commands", test_usb_nak_commands);
    qtest_add_func("/iphone3g/usb/irq-to-vic", test_usb_irq_to_vic);
    qtest_add_func("/iphone3g/usb/phy-registers", test_usb_phy_registers);
    qtest_add_func("/iphone3g/memory/ram-windows", test_ram_windows);
    qtest_add_func("/iphone3g/sysic/vrom-transition", test_vrom_transition);
    qtest_add_func("/iphone3g/reset/restores-vrom", test_reset_restores_vrom);

    machine_args = g_strdup_printf(
        "-machine iphone3g -drive if=mtd,format=raw,file=%s "
        "-drive if=pflash,format=raw,file=%s "
        "-object secret,id=iphone3g-test-gid,format=base64,"
        "data=K34VFiiu0qar9xWICc9PPA== "
        "-object secret,id=iphone3g-test-kbags,file=%s "
        "-global s5l8900-aes.gid-key-secret=iphone3g-test-gid "
        "-global s5l8900-aes.gid-kbag-secret=iphone3g-test-kbags "
        "-chardev null,id=iphone3g-test-usboip "
        "-global s5l8900-usb.chardev=iphone3g-test-usboip",
        nand_path, nor_path, kbag_path);
    qtest_start(machine_args);
    ret = g_test_run();
    qtest_end();
    g_assert_cmpint(g_unlink(nand_path), ==, 0);
    g_assert_cmpint(g_unlink(nor_path), ==, 0);
    g_assert_cmpint(g_unlink(kbag_path), ==, 0);
    return ret;
}
