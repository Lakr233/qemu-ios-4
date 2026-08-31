/*
 * Apple S5L8900 LCD controller
 *
 * The fixed panel contract comes from the iPhone 3G (N82AP) producer:
 * 320x480 active pixels, 15/15/16 horizontal porch/sync, 4/4/4 vertical
 * porch/sync and a 10.8 MHz pixel clock.
 *
 * Copyright (c) 2026 QEMU iPhone contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The start/stop/gate contract follows S5LBox commit
 * 6f203ba550b49afadee008c7eb55373a838eed33 (MIT), Copyright (c) 2026
 * j0shua-SYSON.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/s5l8900_lcd.h"
#include "hw/display/framebuffer.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "trace.h"
#include "ui/pixel_ops.h"

#define LCD_ENABLE                   0x000
#define LCD_DISABLE                  0x004
#define LCD_WINDOW_ENABLE            0x008
#define LCD_INTERRUPT_ENABLE         0x014
#define LCD_INTERRUPT_STATUS         0x018
#define LCD_VIDCON0                  0x200
#define LCD_VIDCON1                  0x204

#define LCD_WINDOW_HSPAN             0x00
#define LCD_WINDOW_FORMAT            0x04
#define LCD_WINDOW_ADDRESS           0x08
#define LCD_WINDOW_SIZE              0x0c

#define LCD_VIDCON0_ENABLE           BIT(0)
#define LCD_CONTROL_ENABLE           BIT(0)
#define LCD_INTERRUPT_FRAME          BIT(0)

#define LCD_FORMAT_SHIFT             8
#define LCD_FORMAT_MASK              0x7
#define LCD_FORMAT_RGB565            3
#define LCD_FORMAT_RGB888            6
#define LCD_FORMAT_ARGB8888          7

#define IPHONE3G_LCD_WIDTH            320
#define IPHONE3G_LCD_HEIGHT           480
#define IPHONE3G_LCD_H_BACK_PORCH     15
#define IPHONE3G_LCD_H_FRONT_PORCH    15
#define IPHONE3G_LCD_H_SYNC           16
#define IPHONE3G_LCD_V_BACK_PORCH     4
#define IPHONE3G_LCD_V_FRONT_PORCH    4
#define IPHONE3G_LCD_V_SYNC           4
#define IPHONE3G_LCD_PIXEL_CLOCK_HZ    10800000

#define IPHONE3G_LCD_PIXELS_PER_LINE \
    (IPHONE3G_LCD_WIDTH + IPHONE3G_LCD_H_BACK_PORCH + \
     IPHONE3G_LCD_H_FRONT_PORCH + IPHONE3G_LCD_H_SYNC)
#define IPHONE3G_LCD_LINES_PER_FRAME \
    (IPHONE3G_LCD_HEIGHT + IPHONE3G_LCD_V_BACK_PORCH + \
     IPHONE3G_LCD_V_FRONT_PORCH + IPHONE3G_LCD_V_SYNC)
#define IPHONE3G_LCD_PIXELS_PER_FRAME \
    (IPHONE3G_LCD_PIXELS_PER_LINE * IPHONE3G_LCD_LINES_PER_FRAME)
#define IPHONE3G_LCD_FRAME_NUMERATOR \
    ((uint64_t)IPHONE3G_LCD_PIXELS_PER_FRAME * NANOSECONDS_PER_SECOND)
#define IPHONE3G_LCD_FRAME_NS \
    (IPHONE3G_LCD_FRAME_NUMERATOR / IPHONE3G_LCD_PIXEL_CLOCK_HZ)
#define IPHONE3G_LCD_FRAME_REMAINDER \
    (IPHONE3G_LCD_FRAME_NUMERATOR % IPHONE3G_LCD_PIXEL_CLOCK_HZ)

static const hwaddr s5l8900_lcd_window_base[] = {
    0x058, 0x070, 0x088, 0x0a0, 0x0b8,
};
QEMU_BUILD_BUG_ON(ARRAY_SIZE(s5l8900_lcd_window_base) !=
                  S5L8900_LCD_WINDOWS);

typedef struct S5L8900LCDScanout {
    hwaddr base;
    uint32_t stride;
    uint8_t format;
    uint8_t bytes_per_pixel;
    uint8_t window;
} S5L8900LCDScanout;

static uint32_t *s5l8900_lcd_reg(S5L8900LCDState *s, hwaddr offset)
{
    return &s->regs[offset / sizeof(uint32_t)];
}

static bool s5l8900_lcd_enabled(S5L8900LCDState *s)
{
    return s->scanning &&
           (*s5l8900_lcd_reg(s, LCD_WINDOW_ENABLE) & LCD_CONTROL_ENABLE) &&
           (*s5l8900_lcd_reg(s, LCD_VIDCON0) & LCD_VIDCON0_ENABLE);
}

static void s5l8900_lcd_update_irq(S5L8900LCDState *s)
{
    uint32_t enabled = *s5l8900_lcd_reg(s, LCD_INTERRUPT_ENABLE);
    uint32_t status = *s5l8900_lcd_reg(s, LCD_INTERRUPT_STATUS);

    qemu_set_irq(s->irq, enabled & status);
}

static void s5l8900_lcd_schedule_frame(S5L8900LCDState *s)
{
    uint64_t fraction = s->frame_remainder +
                        IPHONE3G_LCD_FRAME_REMAINDER;
    int64_t delta = IPHONE3G_LCD_FRAME_NS;

    delta += fraction / IPHONE3G_LCD_PIXEL_CLOCK_HZ;
    s->frame_remainder = fraction % IPHONE3G_LCD_PIXEL_CLOCK_HZ;
    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delta);
}

static void s5l8900_lcd_frame(void *opaque)
{
    S5L8900LCDState *s = opaque;

    if (!s5l8900_lcd_enabled(s)) {
        return;
    }

    *s5l8900_lcd_reg(s, LCD_INTERRUPT_STATUS) |= LCD_INTERRUPT_FRAME;
    s5l8900_lcd_update_irq(s);
    s5l8900_lcd_schedule_frame(s);
}

static void s5l8900_lcd_invalidate(S5L8900LCDState *s)
{
    s->invalidate = true;
    s->blanked = false;
    qemu_console_hw_invalidate(s->con);
}

static void s5l8900_lcd_draw_rgb565(void *opaque, uint8_t *dst,
                                    const uint8_t *src, int width,
                                    int dststep)
{
    uint32_t *dst32 = (uint32_t *)dst;

    while (width--) {
        uint16_t pixel = lduw_le_p(src);
        uint8_t r5 = (pixel >> 11) & 0x1f;
        uint8_t g6 = (pixel >> 5) & 0x3f;
        uint8_t b5 = pixel & 0x1f;

        *dst32++ = rgb_to_pixel32((r5 << 3) | (r5 >> 2),
                                  (g6 << 2) | (g6 >> 4),
                                  (b5 << 3) | (b5 >> 2));
        src += 2;
    }
}

static void s5l8900_lcd_draw_rgb888(void *opaque, uint8_t *dst,
                                    const uint8_t *src, int width,
                                    int dststep)
{
    uint32_t *dst32 = (uint32_t *)dst;

    while (width--) {
        uint32_t pixel = ldl_le_p(src);

        *dst32++ = rgb_to_pixel32((pixel >> 16) & 0xff,
                                  (pixel >> 8) & 0xff,
                                  pixel & 0xff);
        src += 4;
    }
}

static void s5l8900_lcd_blank(S5L8900LCDState *s)
{
    DisplaySurface *surface;

    if (s->blanked) {
        return;
    }

    surface = qemu_console_surface(s->con);
    memset(surface_data(surface), 0,
           surface_stride(surface) * surface_height(surface));
    qemu_console_update(s->con, 0, 0, IPHONE3G_LCD_WIDTH,
                        IPHONE3G_LCD_HEIGHT);
    s->blanked = true;
}

static unsigned s5l8900_lcd_scanouts(
    S5L8900LCDState *s,
    S5L8900LCDScanout scanouts[ARRAY_SIZE(s5l8900_lcd_window_base)])
{
    uint32_t enabled = *s5l8900_lcd_reg(s, LCD_WINDOW_ENABLE);
    unsigned count = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(s5l8900_lcd_window_base); i++) {
        unsigned window = i + 1;
        hwaddr base = s5l8900_lcd_window_base[i];
        S5L8900LCDScanout *scanout = &scanouts[count];
        uint32_t format_reg;
        uint32_t size;
        uint32_t width;
        uint32_t height;
        uint64_t length;

        if (!(enabled & BIT(7 - window))) {
            continue;
        }

        format_reg = *s5l8900_lcd_reg(s, base + LCD_WINDOW_FORMAT);
        scanout->format = (format_reg >> LCD_FORMAT_SHIFT) &
                          LCD_FORMAT_MASK;
        switch (scanout->format) {
        case LCD_FORMAT_RGB565:
            scanout->bytes_per_pixel = 2;
            break;
        case LCD_FORMAT_RGB888:
        case LCD_FORMAT_ARGB8888:
            scanout->bytes_per_pixel = 4;
            break;
        default:
            continue;
        }

        size = *s5l8900_lcd_reg(s, base + LCD_WINDOW_SIZE);
        width = size >> 16;
        height = size & 0xffff;
        if (width != IPHONE3G_LCD_WIDTH ||
            height != IPHONE3G_LCD_HEIGHT) {
            continue;
        }

        scanout->stride = *s5l8900_lcd_reg(s,
                                           base + LCD_WINDOW_HSPAN);
        if (scanout->stride < width * scanout->bytes_per_pixel) {
            continue;
        }

        scanout->base = *s5l8900_lcd_reg(s,
                                         base + LCD_WINDOW_ADDRESS);
        length = (uint64_t)scanout->stride * height;
        if (length > UINT32_MAX || scanout->base > UINT32_MAX - length) {
            continue;
        }
        scanout->window = i;
        count++;
    }

    return count;
}

static void s5l8900_lcd_release_section(MemoryRegionSection *section)
{
    if (!section->mr) {
        return;
    }
    memory_region_set_log(section->mr, false, DIRTY_MEMORY_VGA);
    memory_region_unref(section->mr);
    section->mr = NULL;
}

static void s5l8900_lcd_release_composite_scanouts(S5L8900LCDState *s)
{
    for (unsigned i = 0; i < S5L8900_LCD_WINDOWS; i++) {
        s5l8900_lcd_release_section(&s->composite_fbsections[i]);
        g_clear_pointer(&s->composite_shadow[i], g_free);
    }
}

static bool s5l8900_lcd_map_composite_scanout(
    S5L8900LCDState *s, const S5L8900LCDScanout *scanout)
{
    unsigned window = scanout->window;
    MemoryRegionSection *section = &s->composite_fbsections[window];
    size_t length = (size_t)scanout->stride * IPHONE3G_LCD_HEIGHT;

    if (!section->mr ||
        s->composite_cached_fb_base[window] != scanout->base ||
        s->composite_cached_stride[window] != scanout->stride ||
        s->composite_cached_format[window] != scanout->format) {
        framebuffer_update_memory_section(section, s->fbmem,
                                          scanout->base,
                                          IPHONE3G_LCD_HEIGHT,
                                          scanout->stride);
        if (section->mr) {
            /*
             * The iPhone framebuffer lives in system RAM.  VGA dirty
             * logging is MemoryRegion-wide, so keeping it enabled here
             * instruments every Guest RAM write, not just this surface.
             * Compare against a bounded Host shadow instead.
             */
            memory_region_set_log(section->mr, false, DIRTY_MEMORY_VGA);
        }
        s->composite_cached_fb_base[window] = scanout->base;
        s->composite_cached_stride[window] = scanout->stride;
        s->composite_cached_format[window] = scanout->format;
        g_clear_pointer(&s->composite_shadow[window], g_free);
        s->invalidate = true;
    }
    if (section->mr && !s->composite_shadow[window]) {
        s->composite_shadow[window] = g_malloc(length);
        s->invalidate = true;
    }
    return section->mr != NULL;
}

static void s5l8900_lcd_composite(S5L8900LCDState *s,
                                  S5L8900LCDScanout *scanouts,
                                  unsigned count)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    MemoryRegionSection *sections[S5L8900_LCD_WINDOWS] = { 0 };
    const uint8_t *sources[ARRAY_SIZE(s5l8900_lcd_window_base)] = { 0 };
    uint32_t *destination = (uint32_t *)surface_data(surface);
    bool dirty[IPHONE3G_LCD_HEIGHT];
    unsigned mapped = 0;
    int first = -1;
    int last = -1;

    for (unsigned i = 0; i < count; i++) {
        unsigned window = scanouts[i].window;

        if (!s5l8900_lcd_map_composite_scanout(s, &scanouts[i])) {
            continue;
        }
        sections[mapped] = &s->composite_fbsections[window];
        sources[mapped] = memory_region_get_ram_ptr(sections[mapped]->mr) +
                          sections[mapped]->offset_within_region;
        if (mapped != i) {
            scanouts[mapped] = scanouts[i];
        }
        mapped++;
    }

    if (!mapped) {
        s5l8900_lcd_blank(s);
        return;
    }

    memset(dirty, s->invalidate, sizeof(dirty));
    for (unsigned i = 0; i < mapped; i++) {
        const S5L8900LCDScanout *scanout = &scanouts[i];
        uint8_t *shadow = s->composite_shadow[scanout->window];

        for (unsigned y = 0; y < IPHONE3G_LCD_HEIGHT; y++) {
            const uint8_t *source = sources[i] +
                                    (size_t)y * scanout->stride;
            uint8_t *saved = shadow + (size_t)y * scanout->stride;

            if (s->invalidate ||
                memcmp(source, saved, scanout->stride) != 0) {
                dirty[y] = true;
                memcpy(saved, source, scanout->stride);
            }
        }
    }

    for (unsigned y = 0; y < IPHONE3G_LCD_HEIGHT; y++) {
        uint32_t *line = destination +
                         y * surface_stride(surface) / sizeof(*line);

        if (!dirty[y]) {
            continue;
        }
        if (first < 0) {
            first = y;
        }
        last = y;
        for (unsigned x = 0; x < IPHONE3G_LCD_WIDTH; x++) {
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;

            for (unsigned i = 0; i < mapped; i++) {
                const S5L8900LCDScanout *scanout = &scanouts[i];
                const uint8_t *source = sources[i] + y * scanout->stride +
                                        x * scanout->bytes_per_pixel;
                uint32_t pixel;
                uint32_t source_r;
                uint32_t source_g;
                uint32_t source_b;
                uint32_t alpha = 255;

                if (scanout->format == LCD_FORMAT_RGB565) {
                    uint16_t rgb565 = lduw_le_p(source);
                    uint32_t r5 = (rgb565 >> 11) & 0x1f;
                    uint32_t g6 = (rgb565 >> 5) & 0x3f;
                    uint32_t b5 = rgb565 & 0x1f;

                    source_r = (r5 << 3) | (r5 >> 2);
                    source_g = (g6 << 2) | (g6 >> 4);
                    source_b = (b5 << 3) | (b5 >> 2);
                } else {
                    pixel = ldl_le_p(source);
                    source_r = (pixel >> 16) & 0xff;
                    source_g = (pixel >> 8) & 0xff;
                    source_b = pixel & 0xff;
                    if (scanout->format == LCD_FORMAT_ARGB8888) {
                        alpha = pixel >> 24;
                    }
                }

                if (!alpha) {
                    continue;
                }
                if (alpha == 255) {
                    r = source_r;
                    g = source_g;
                    b = source_b;
                } else {
                    uint32_t inverse = 255 - alpha;

                    r = MIN(255, source_r + (r * inverse + 127) / 255);
                    g = MIN(255, source_g + (g * inverse + 127) / 255);
                    b = MIN(255, source_b + (b * inverse + 127) / 255);
                }
            }
            line[x] = rgb_to_pixel32(r, g, b);
        }
    }

    if (first >= 0) {
        qemu_console_update(s->con, 0, first, IPHONE3G_LCD_WIDTH,
                            last - first + 1);
    }
    s->invalidate = false;
    s->blanked = false;
}

static bool s5l8900_lcd_update_display(void *opaque)
{
    S5L8900LCDState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    S5L8900LCDScanout scanouts[ARRAY_SIZE(s5l8900_lcd_window_base)];
    S5L8900LCDScanout *scanout = &scanouts[0];
    drawfn draw;
    unsigned count;
    int first = 0;
    int last = 0;

    count = s5l8900_lcd_enabled(s) ?
            s5l8900_lcd_scanouts(s, scanouts) : 0;
    if (!count) {
        s5l8900_lcd_release_section(&s->fbsection);
        s5l8900_lcd_release_composite_scanouts(s);
        s5l8900_lcd_blank(s);
        return true;
    }
    if (count > 1) {
        s5l8900_lcd_release_section(&s->fbsection);
        s5l8900_lcd_composite(s, scanouts, count);
        return true;
    }
    s5l8900_lcd_release_composite_scanouts(s);

    draw = scanout->format == LCD_FORMAT_RGB565 ?
           s5l8900_lcd_draw_rgb565 : s5l8900_lcd_draw_rgb888;
    if (!s->fbsection.mr || s->invalidate ||
        s->cached_fb_base != scanout->base ||
        s->cached_stride != scanout->stride ||
        s->cached_format != scanout->format) {
        framebuffer_update_memory_section(&s->fbsection, s->fbmem,
                                          scanout->base,
                                          IPHONE3G_LCD_HEIGHT,
                                          scanout->stride);
        s->cached_fb_base = scanout->base;
        s->cached_stride = scanout->stride;
        s->cached_format = scanout->format;
    }

    if (!s->fbsection.mr) {
        s5l8900_lcd_blank(s);
        return true;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               IPHONE3G_LCD_WIDTH,
                               IPHONE3G_LCD_HEIGHT,
                               scanout->stride,
                               surface_stride(surface), 0,
                               s->invalidate, draw, s, &first, &last);
    if (first >= 0) {
        qemu_console_update(s->con, 0, first, IPHONE3G_LCD_WIDTH,
                            last - first + 1);
    }

    s->invalidate = false;
    s->blanked = false;
    return true;
}

static void s5l8900_lcd_invalidate_display(void *opaque)
{
    S5L8900LCDState *s = opaque;

    s->invalidate = true;
    s->blanked = false;
}

static const GraphicHwOps s5l8900_lcd_graphic_ops = {
    .invalidate = s5l8900_lcd_invalidate_display,
    .gfx_update = s5l8900_lcd_update_display,
};

static uint64_t s5l8900_lcd_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    S5L8900LCDState *s = opaque;

    switch (offset) {
    case LCD_VIDCON1:
        return (*s5l8900_lcd_reg(s, LCD_VIDCON0) & 0xc0) == 0x40 ? 8 : 0;
    default:
        return *s5l8900_lcd_reg(s, offset);
    }
}

static void s5l8900_lcd_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    S5L8900LCDState *s = opaque;
    bool was_enabled = s5l8900_lcd_enabled(s);

    if (offset == LCD_INTERRUPT_STATUS) {
        *s5l8900_lcd_reg(s, offset) &= ~(uint32_t)value;
        s5l8900_lcd_update_irq(s);
        trace_s5l8900_lcd_write(offset, size, value,
                                s5l8900_lcd_enabled(s));
        return;
    }

    if (offset == LCD_VIDCON1) {
        return;
    }

    *s5l8900_lcd_reg(s, offset) = value;

    if (offset == LCD_ENABLE && (value & 1)) {
        s->scanning = true;
    } else if (offset == LCD_DISABLE && (value & 1)) {
        s->scanning = false;
    }

    if (offset == LCD_INTERRUPT_ENABLE) {
        s5l8900_lcd_update_irq(s);
        return;
    }

    if (offset == LCD_ENABLE || offset == LCD_DISABLE ||
        offset == LCD_WINDOW_ENABLE || offset == LCD_VIDCON0) {
        bool enabled = s5l8900_lcd_enabled(s);

        if (!was_enabled && enabled) {
            s->frame_remainder = 0;
            s5l8900_lcd_schedule_frame(s);
        } else if (was_enabled && !enabled) {
            timer_del(s->frame_timer);
        }
    }

    if (offset < LCD_VIDCON0 + 0x20) {
        s5l8900_lcd_invalidate(s);
    }
    trace_s5l8900_lcd_write(offset, size, value,
                            s5l8900_lcd_enabled(s));
}

static const MemoryRegionOps s5l8900_lcd_ops = {
    .read = s5l8900_lcd_read,
    .write = s5l8900_lcd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_lcd_reset(DeviceState *dev)
{
    S5L8900LCDState *s = S5L8900_LCD(dev);

    s5l8900_lcd_release_section(&s->fbsection);
    s5l8900_lcd_release_composite_scanouts(s);
    memset(s->regs, 0, sizeof(s->regs));
    s->scanning = false;
    s->frame_remainder = 0;
    timer_del(s->frame_timer);
    s5l8900_lcd_update_irq(s);
    s->cached_fb_base = UINT64_MAX;
    s->cached_stride = 0;
    s->cached_format = 0;
    for (unsigned i = 0; i < S5L8900_LCD_WINDOWS; i++) {
        s->composite_cached_fb_base[i] = UINT64_MAX;
        s->composite_cached_stride[i] = 0;
        s->composite_cached_format[i] = 0;
    }
    s->invalidate = true;
    s->blanked = false;
    s5l8900_lcd_blank(s);
}

static int s5l8900_lcd_post_load(void *opaque, int version_id)
{
    S5L8900LCDState *s = opaque;

    if (version_id < 2) {
        s->scanning =
            (*s5l8900_lcd_reg(s, LCD_VIDCON0) & LCD_VIDCON0_ENABLE) != 0;
    }

    s5l8900_lcd_release_section(&s->fbsection);
    s5l8900_lcd_release_composite_scanouts(s);
    s->cached_fb_base = UINT64_MAX;
    s->cached_stride = 0;
    s->cached_format = 0;
    for (unsigned i = 0; i < S5L8900_LCD_WINDOWS; i++) {
        s->composite_cached_fb_base[i] = UINT64_MAX;
        s->composite_cached_stride[i] = 0;
        s->composite_cached_format[i] = 0;
    }
    s->invalidate = true;
    s->blanked = false;
    s5l8900_lcd_update_irq(s);
    if (s5l8900_lcd_enabled(s)) {
        if (!timer_pending(s->frame_timer)) {
            s5l8900_lcd_schedule_frame(s);
        }
        s5l8900_lcd_invalidate(s);
    } else {
        s5l8900_lcd_blank(s);
    }
    return 0;
}

static const VMStateDescription vmstate_s5l8900_lcd = {
    .name = TYPE_S5L8900_LCD,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = s5l8900_lcd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8900LCDState,
                             S5L8900_LCD_REGISTER_COUNT),
        VMSTATE_UINT32(frame_remainder, S5L8900LCDState),
        VMSTATE_UNUSED(1),
        VMSTATE_TIMER_PTR(frame_timer, S5L8900LCDState),
        VMSTATE_BOOL_V(scanning, S5L8900LCDState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s5l8900_lcd_properties[] = {
    DEFINE_PROP_LINK("framebuffer-memory", S5L8900LCDState, fbmem,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void s5l8900_lcd_realize(DeviceState *dev, Error **errp)
{
    S5L8900LCDState *s = S5L8900_LCD(dev);

    if (!s->fbmem) {
        error_setg(errp, "'framebuffer-memory' property was not set");
        return;
    }

    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  s5l8900_lcd_frame, s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &s5l8900_lcd_ops, s,
                          TYPE_S5L8900_LCD, S5L8900_LCD_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->con = qemu_graphic_console_create(dev, 0,
                                         &s5l8900_lcd_graphic_ops, s);
    qemu_console_resize(s->con, IPHONE3G_LCD_WIDTH, IPHONE3G_LCD_HEIGHT);
    s->blanked = false;
    s5l8900_lcd_blank(s);
}

static void s5l8900_lcd_unrealize(DeviceState *dev)
{
    S5L8900LCDState *s = S5L8900_LCD(dev);

    timer_del(s->frame_timer);
    timer_free(s->frame_timer);
    s->frame_timer = NULL;

    s5l8900_lcd_release_section(&s->fbsection);
    s5l8900_lcd_release_composite_scanouts(s);
    if (s->con) {
        qemu_graphic_console_close(s->con);
        s->con = NULL;
    }
}

static void s5l8900_lcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8900_lcd_realize;
    dc->unrealize = s5l8900_lcd_unrealize;
    dc->vmsd = &vmstate_s5l8900_lcd;
    dc->desc = "Apple S5L8900 LCD controller";
    device_class_set_legacy_reset(dc, s5l8900_lcd_reset);
    device_class_set_props(dc, s5l8900_lcd_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo s5l8900_lcd_info = {
    .name = TYPE_S5L8900_LCD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900LCDState),
    .class_init = s5l8900_lcd_class_init,
};

static void s5l8900_lcd_register_types(void)
{
    type_register_static(&s5l8900_lcd_info);
}

type_init(s5l8900_lcd_register_types)
