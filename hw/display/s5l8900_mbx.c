/*
 * Apple S5L8900 PowerVR MBX wrapper
 *
 * N82AP exposes a 16 MiB aperture.  The initial model implements the wrapper
 * identity, command-address, cache-capacity, bounded 2D packets, and measured
 * MBX3D object-list families consumed by the iOS 4.2.1 AppleMBX driver.
 *
 * Copyright (c) 2026 QEMU iPhone contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The bounded MBX2D decoder is derived from S5LBox commit
 * 6f203ba550b49afadee008c7eb55373a838eed33, Copyright (c) 2026
 * j0shua-SYSON, used under the MIT License.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/s5l8900_mbx.h"
#include "hw/display/s5l8900_mbx_metal.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "trace.h"

#define MBX_STATUS             0x012c
#define MBX_INTERRUPT_MASK     0x0130
#define MBX_STATUS_ACK         0x0134
#define MBX_BACKGROUND_TAG     0x06d8
#define MBX_STARTRENDER        0x0680
#define MBX_TA_START           0x0800
#define MBX_TA_RENDER_ID       0x0810
#define MBX_TA_CONTEXT_LOAD    0x0814
#define MBX_TA_CONTEXT_STORE   0x0818
#define MBX_TA_CONTEXT_RESET   0x081c
#define MBX_TA_OBJECT_DATABASE 0x083c
#define MBX_TA_TAILPTR_BASE    0x0840
#define MBX_TA_REGION_BASE     0x0844
#define MBX_RGNBASE            0x0608
#define MBX_OBJBASE            0x060c
#define MBX_3DPIXSAMP          0x061c
#define MBX_FBCTL              0x0650
#define MBX_FBXCLIP            0x0654
#define MBX_FBYCLIP            0x0658
#define MBX_FBSTART            0x065c
#define MBX_FBLINESTRIDE       0x0660
#define MBX_ID                 0x0f00
#define MBX_RESET              0x1020

#define MBX_ID_S5L8900         0x01020000
#define MBX_STATUS_EVM_DALLOC  BIT(6)
#define MBX_STATUS_RENDER_COMPLETE BIT(2)
#define MBX_STATUS_ISP         BIT(3)
#define MBX_STATUS_TA_COMPLETE BIT(4)
#define MBX_STATUS_TA_CONTEXT  BIT(8)
#define MBX_RESET_REQUEST      BIT(0)
#define MBX_RESET_DONE         BIT(16)

#define MBX_3D_DATA_FIFO       0x00800000
#define MBX_3D_SUBMIT          0xf0000000
#define MBX_TA_CAPTURE_MAX_WORDS 16384

#define MBX_3D_WIDTH             320u
#define MBX_3D_SOURCE_STRIDE     0x20u
#define MBX_3D_TARGET_STRIDE     0x500u
#define MBX_3D_ADDRESS_MASK      0x0003ffffu

#define MBX_2D_RING_BASE       0x00a00000
#define MBX_2D_RING_SIZE       0x00010000
#define MBX_2D_COMMAND_HEADER  0xa0060500
#define MBX_2D_SUBMIT          0xf0000000
#define MBX_2D_COPY_WORDS      16
#define MBX_2D_BLEND_WORDS     18
#define MBX_2D_END             0x70000000
#define MBX_2D_BLEND_TAG       0x20000004
#define MBX_2D_BLEND_EQUATION  0x095ff000
#define MBX_2D_GLOBAL_FACTORS  0x0d500000
#define MBX_2D_GLOBAL_ALPHA_MASK 0x000ff000
#define MBX_2D_BLEND_MODE      0x8002cccc
#define MBX_2D_FILL_MODE       0x8000f0f0
#define MBX_2D_SOURCE_FORMAT_MASK 0xffff8000
#define MBX_2D_SOURCE_BGRA8    0x94060000
#define MBX_2D_SOURCE_ARGB1555 0x94048000
#define MBX_2D_STRIDE          0x500
#define MBX_2D_WIDTH           320
#define MBX_2D_HEIGHT          480
#define MBX_2D_SURFACE_BYTES   (MBX_2D_STRIDE * MBX_2D_HEIGHT)
#define MBX_STATUS_2D_SYNC     BIT(10)

static uint32_t *s5l8900_mbx_reg(S5L8900MBXState *s, hwaddr offset)
{
    return &s->regs[offset / sizeof(uint32_t)];
}

static void s5l8900_mbx_update_irq(S5L8900MBXState *s)
{
    qemu_set_irq(s->irq,
                 (s->status & *s5l8900_mbx_reg(s, MBX_INTERRUPT_MASK)) != 0);
}

static bool s5l8900_mbx_ram_ptr(S5L8900MBXState *s, hwaddr address,
                                size_t length, bool is_write,
                                MemoryRegionSection *section,
                                uint8_t **pointer)
{
    if (!s->system_memory || !length) {
        return false;
    }

    *section = memory_region_find(s->system_memory, address, length);
    if (!section->mr || int128_get64(section->size) < length ||
        !memory_region_is_ram(section->mr) ||
        (is_write && section->mr->readonly)) {
        if (section->mr) {
            memory_region_unref(section->mr);
            section->mr = NULL;
        }
        return false;
    }

    *pointer = memory_region_get_ram_ptr(section->mr) +
               section->offset_within_region;
    return true;
}

static bool s5l8900_mbx_physical_read(S5L8900MBXState *s, hwaddr address,
                                      uint8_t *destination, size_t length)
{
    MemoryRegionSection section = { 0 };
    uint8_t *source;

    if (!s5l8900_mbx_ram_ptr(s, address, length, false, &section, &source)) {
        return false;
    }
    memcpy(destination, source, length);
    memory_region_unref(section.mr);
    return true;
}

static bool s5l8900_mbx_physical_write(S5L8900MBXState *s, hwaddr address,
                                       const uint8_t *source, size_t length)
{
    MemoryRegionSection section = { 0 };
    uint8_t *destination;

    if (!s5l8900_mbx_ram_ptr(s, address, length, true,
                             &section, &destination)) {
        return false;
    }
    memcpy(destination, source, length);
    memory_region_set_dirty(section.mr, section.offset_within_region, length);
    memory_region_unref(section.mr);
    return true;
}

static bool s5l8900_mbx_gart_span(S5L8900MBXState *s, uint32_t gpu_address,
                                  uint32_t *physical, uint32_t *available)
{
    uint32_t chunk = gpu_address >> 22;
    uint32_t root;
    uint32_t pte;
    uint8_t bytes[4];

    if (chunk >= 8) {
        return false;
    }
    root = *s5l8900_mbx_reg(s, 0x1000 + chunk * 4);
    if (!root || (root & 0xfff) ||
        !s5l8900_mbx_physical_read(
            s, root + (((gpu_address >> 12) & 0x3ff) * 4),
            bytes, sizeof(bytes))) {
        return false;
    }
    pte = ldl_le_p(bytes);
    if (!pte || (pte & 0xfff)) {
        return false;
    }

    *physical = pte + (gpu_address & 0xfff);
    *available = 0x1000 - (gpu_address & 0xfff);
    return true;
}

static bool s5l8900_mbx_gart_read(S5L8900MBXState *s, uint32_t gpu_address,
                                  uint8_t *destination, uint32_t length)
{
    while (length) {
        uint32_t physical;
        uint32_t span;

        if (!s5l8900_mbx_gart_span(s, gpu_address, &physical, &span)) {
            return false;
        }
        span = MIN(span, length);
        if (!s5l8900_mbx_physical_read(s, physical, destination, span)) {
            return false;
        }
        gpu_address += span;
        destination += span;
        length -= span;
    }
    return true;
}

static bool s5l8900_mbx_gart_validate(S5L8900MBXState *s,
                                      uint32_t gpu_address,
                                      uint32_t length, bool is_write)
{
    while (length) {
        MemoryRegionSection section = { 0 };
        uint8_t *pointer;
        uint32_t physical;
        uint32_t span;

        if (!s5l8900_mbx_gart_span(s, gpu_address, &physical, &span)) {
            return false;
        }
        span = MIN(span, length);
        if (!s5l8900_mbx_ram_ptr(s, physical, span, is_write,
                                 &section, &pointer)) {
            return false;
        }
        memory_region_unref(section.mr);
        gpu_address += span;
        length -= span;
    }
    return true;
}

static bool s5l8900_mbx_gart_write(S5L8900MBXState *s, uint32_t gpu_address,
                                   const uint8_t *source, uint32_t length)
{
    while (length) {
        uint32_t physical;
        uint32_t span;

        if (!s5l8900_mbx_gart_span(s, gpu_address, &physical, &span)) {
            return false;
        }
        span = MIN(span, length);
        if (!s5l8900_mbx_physical_write(s, physical, source, span)) {
            return false;
        }
        gpu_address += span;
        source += span;
        length -= span;
    }
    return true;
}

static bool s5l8900_mbx_gart_u32(S5L8900MBXState *s, uint32_t gpu_address,
                                 uint32_t *value)
{
    uint8_t bytes[4];

    if (!s5l8900_mbx_gart_read(s, gpu_address, bytes, sizeof(bytes))) {
        return false;
    }
    *value = ldl_le_p(bytes);
    return true;
}

/* Adapter boundary for the bounded, measured software MBX3D decoder derived
 * from the MIT-licensed S5LBox source identified in the file header.  Keep the
 * imported decoder names private to this translation unit and route every
 * Guest access through this model's checked GART helpers. */
typedef S5L8900MBXState s5l_mbx_t;
typedef void arm_bus_t;

static bool mbx_try_metal_source_over(s5l_mbx_t *s, uint8_t *destination,
                                      const uint8_t *source, uint32_t pixels)
{
    if (!s->metal) {
        return false;
    }
    if (!s5l8900_mbx_metal_source_over(s->metal, destination, source,
                                        pixels)) {
        trace_s5l8900_mbx_metal_failure(pixels);
        return false;
    }
    s->metal_submissions++;
    trace_s5l8900_mbx_metal_complete(
        pixels, s->metal_submissions,
        s5l8900_mbx_metal_device_name(s->metal));
    return true;
}

static bool mbx_gart_read(const s5l_mbx_t *s, const arm_bus_t *unused,
                          uint32_t address, uint8_t *destination,
                          uint32_t length, const char **why)
{
    bool ok = s5l8900_mbx_gart_read((S5L8900MBXState *)s, address,
                                    destination, length);
    if (!ok && why) {
        *why = "GART read is outside mapped Guest RAM";
    }
    return ok;
}

static bool mbx_gart_write(const s5l_mbx_t *s, const arm_bus_t *unused,
                           uint32_t address, const uint8_t *source,
                           uint32_t length, const char **why)
{
    bool ok = s5l8900_mbx_gart_write((S5L8900MBXState *)s, address,
                                     source, length);
    if (!ok && why) {
        *why = "GART write is outside writable Guest RAM";
    }
    return ok;
}

static bool mbx_gart_validate(const s5l_mbx_t *s, const arm_bus_t *unused,
                              uint32_t address, uint32_t length,
                              const char **why)
{
    bool ok = s5l8900_mbx_gart_validate((S5L8900MBXState *)s, address,
                                        length, false);
    if (!ok && why) {
        *why = "GART span is outside mapped Guest RAM";
    }
    return ok;
}

static int mbx_trace_state;

#define reg regs
#define S5L_MBX_RGNBASE MBX_RGNBASE
#define S5L_MBX_OBJBASE MBX_OBJBASE
#define S5L_MBX_3DPIXSAMP MBX_3DPIXSAMP
#define S5L_MBX_FBCTL MBX_FBCTL
#define S5L_MBX_FBXCLIP MBX_FBXCLIP
#define S5L_MBX_FBYCLIP MBX_FBYCLIP
#define S5L_MBX_FBSTART MBX_FBSTART
#define S5L_MBX_FBLINESTRIDE MBX_FBLINESTRIDE
#define mbx_load_le32 ldl_le_p
#include "s5l8900_mbx3d.inc.c"
#undef mbx_load_le32
#undef S5L_MBX_FBLINESTRIDE
#undef S5L_MBX_FBSTART
#undef S5L_MBX_FBYCLIP
#undef S5L_MBX_FBXCLIP
#undef S5L_MBX_FBCTL
#undef S5L_MBX_3DPIXSAMP
#undef S5L_MBX_OBJBASE
#undef S5L_MBX_RGNBASE
#undef reg

static bool s5l8900_mbx_execute_3d(S5L8900MBXState *s)
{
    const char *tiled_why = "unknown tiled form";
    const char *status_why = "unknown status form";
    const char *sprite_why = "unknown sprite form";
    const char *solid_why = "unknown solid form";
    const char *decoder = "tiled";
    uint32_t pixels = 0;
    bool accepted;

    accepted = mbx_execute_first_tiled_over(s, NULL, &tiled_why, &pixels);
    if (!accepted) {
        decoder = "status";
        accepted = mbx_execute_status_sprite(s, NULL, &status_why, &pixels);
    }
    if (!accepted) {
        decoder = "sprite";
        accepted = mbx_execute_textured_sprite(s, NULL, &sprite_why, &pixels);
    }
    if (!accepted) {
        decoder = "solid";
        accepted = mbx_execute_solid_quad(s, NULL, &solid_why, &pixels);
    }
    if (!accepted) {
        trace_s5l8900_mbx_3d_reject(sprite_why, 0);
        return false;
    }

    uint32_t target = *s5l8900_mbx_reg(s, MBX_FBSTART);
    uint32_t physical = UINT32_MAX;
    uint32_t available;
    uint32_t top_left = 0;
    uint32_t center = 0;
    uint32_t bottom_right = 0;

    s5l8900_mbx_gart_span(s, target, &physical, &available);
    trace_s5l8900_mbx_3d_target(target, physical);
    s5l8900_mbx_gart_u32(s, target, &top_left);
    s5l8900_mbx_gart_u32(s, target + 240u * MBX_3D_TARGET_STRIDE +
                         160u * 4u, &center);
    s5l8900_mbx_gart_u32(s, target + 479u * MBX_3D_TARGET_STRIDE +
                         319u * 4u, &bottom_right);
    trace_s5l8900_mbx_3d_complete(decoder, pixels, target, top_left,
                                  center, bottom_right);

    /* AppleMBX enters its wait after STARTRENDER returns.  Preserve a real
     * virtual-time boundary so completion cannot clear/wake before that wait
     * is armed.  The pixels above are already committed atomically. */
    s->render_completion_pending = true;
    timer_mod(s->render_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
    return true;
}

static void s5l8900_mbx_render_complete(void *opaque)
{
    S5L8900MBXState *s = opaque;

    if (!s->render_completion_pending) {
        return;
    }
    s->render_completion_pending = false;
    s->status |= MBX_STATUS_ISP | MBX_STATUS_RENDER_COMPLETE |
                 MBX_STATUS_EVM_DALLOC;
    s5l8900_mbx_update_irq(s);
}

static void s5l8900_mbx_trace_3d_object(S5L8900MBXState *s)
{
    uint32_t object = *s5l8900_mbx_reg(s, MBX_OBJBASE);
    uint32_t region = *s5l8900_mbx_reg(s, MBX_RGNBASE);

    /* The fixed bound covers the longest compact record currently selected
     * by the iOS 4 object list while the submitting context owns its GART. */
    for (uint32_t i = 0; i < 208; i++) {
        uint32_t value;

        if (!s5l8900_mbx_gart_u32(s, object + i * 4, &value)) {
            trace_s5l8900_mbx_3d_reject("object witness mapping", i);
            break;
        }
        trace_s5l8900_mbx_3d_object_word(i, value);
    }
    for (uint32_t i = 0; i < 32; i++) {
        uint32_t value;

        if (!s5l8900_mbx_gart_u32(s, region + i * 4, &value)) {
            trace_s5l8900_mbx_3d_reject("region witness mapping", i);
            break;
        }
        trace_s5l8900_mbx_3d_region_word(i, value);
    }
}

static void s5l8900_mbx_trace_3d_start(S5L8900MBXState *s)
{
    trace_s5l8900_mbx_3d_ta_start(
        *s5l8900_mbx_reg(s, MBX_TA_RENDER_ID),
        *s5l8900_mbx_reg(s, MBX_TA_OBJECT_DATABASE),
        *s5l8900_mbx_reg(s, MBX_TA_TAILPTR_BASE),
        *s5l8900_mbx_reg(s, MBX_TA_REGION_BASE),
        *s5l8900_mbx_reg(s, MBX_RGNBASE),
        *s5l8900_mbx_reg(s, MBX_OBJBASE),
        *s5l8900_mbx_reg(s, MBX_FBSTART));
}

static uint64_t s5l8900_mbx_fifo_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    return 0;
}

static void s5l8900_mbx_fifo_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    S5L8900MBXState *s = opaque;
    uint32_t word = value;
    uint32_t index = s->ta_capture_count;

    trace_s5l8900_mbx_3d_fifo(index, word);
    if (!s->ta_in_flight) {
        trace_s5l8900_mbx_3d_reject("FIFO write without TA_START", index);
        return;
    }

    if (index >= MBX_TA_CAPTURE_MAX_WORDS) {
        s->ta_capture_failed = true;
    } else {
        uint32_t object = *s5l8900_mbx_reg(s, MBX_TA_OBJECT_DATABASE);
        uint8_t bytes[4];

        stl_le_p(bytes, word);
        if (!object || object > UINT32_MAX - index * 4 ||
            !s5l8900_mbx_gart_write(s, object + index * 4,
                                    bytes, sizeof(bytes))) {
            s->ta_capture_failed = true;
        } else {
            s->ta_capture_count++;
        }
    }

    if (word == MBX_3D_SUBMIT) {
        s->ta_in_flight = false;
        *s5l8900_mbx_reg(s, MBX_TA_START) = 0;
        s->status |= MBX_STATUS_TA_COMPLETE;
        s5l8900_mbx_update_irq(s);
        trace_s5l8900_mbx_3d_ta_complete(
            s->ta_capture_count, s->ta_capture_failed, s->status);
    }
}

static const MemoryRegionOps s5l8900_mbx_fifo_ops = {
    .read = s5l8900_mbx_fifo_read,
    .write = s5l8900_mbx_fifo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static uint32_t s5l8900_mbx_ring_word(S5L8900MBXState *s, uint32_t offset)
{
    uint8_t *aperture = memory_region_get_ram_ptr(&s->aperture);

    return ldl_le_p(aperture + offset);
}

static uint32_t s5l8900_mbx_source_over(uint32_t destination,
                                        uint32_t source)
{
    uint32_t inverse = 256 - (source >> 24);
    uint32_t output = 0;

    for (unsigned shift = 0; shift < 32; shift += 8) {
        uint32_t component = ((source >> shift) & 0xff) +
            ((((destination >> shift) & 0xff) * inverse) >> 8);

        output |= MIN(component, 0xff) << shift;
    }
    return output;
}

static uint32_t s5l8900_mbx_modulate_alpha(uint32_t source, uint32_t alpha)
{
    uint32_t output = 0;

    for (unsigned shift = 0; shift < 32; shift += 8) {
        uint32_t component = (source >> shift) & 0xff;

        output |= (((component + 1) * alpha) >> 8) << shift;
    }
    return output;
}

typedef struct S5L8900MBX2DJob {
    uint32_t target;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t row_bytes;
    uint32_t total_bytes;
    uint8_t *pixels;
} S5L8900MBX2DJob;

static void s5l8900_mbx_2d_job_clear(S5L8900MBX2DJob *job)
{
    g_free(job->pixels);
    memset(job, 0, sizeof(*job));
}

static bool s5l8900_mbx_2d_validate_rows(S5L8900MBXState *s,
                                         uint32_t base, uint32_t x,
                                         uint32_t y, uint32_t height,
                                         uint32_t stride,
                                         uint32_t pixel_bytes,
                                         uint32_t row_bytes, bool is_write)
{
    for (uint32_t row = 0; row < height; row++) {
        uint64_t address = (uint64_t)base + (uint64_t)(y + row) * stride +
                           (uint64_t)x * pixel_bytes;

        if (address + row_bytes > (uint64_t)UINT32_MAX + 1 ||
            !s5l8900_mbx_gart_validate(s, address, row_bytes, is_write)) {
            return false;
        }
    }
    return true;
}

static bool s5l8900_mbx_2d_commit(S5L8900MBXState *s,
                                  const S5L8900MBX2DJob *job)
{
    uint32_t first = job->target + job->y * MBX_2D_STRIDE + job->x * 4;
    uint32_t physical = UINT32_MAX;
    uint32_t available;

    s5l8900_mbx_gart_span(s, first, &physical, &available);
    trace_s5l8900_mbx_2d_commit(job->target, physical, job->x, job->y,
                                job->width, job->height);
    for (uint32_t row = 0; row < job->height; row++) {
        uint32_t destination = job->target +
            (job->y + row) * MBX_2D_STRIDE + job->x * 4;

        if (!s5l8900_mbx_gart_write(
                s, destination, job->pixels + row * job->row_bytes,
                job->row_bytes)) {
            return false;
        }
    }
    return true;
}

static bool s5l8900_mbx_2d_simple_copy(S5L8900MBXState *s,
                                        const uint32_t *words,
                                        S5L8900MBX2DJob *job,
                                        const char **reason)
{
    uint32_t format = words[2] & MBX_2D_SOURCE_FORMAT_MASK;
    bool bgra8 = format == MBX_2D_SOURCE_BGRA8;
    bool argb1555 = format == MBX_2D_SOURCE_ARGB1555;
    uint32_t source_pixel_bytes = bgra8 ? 4 : 2;
    uint32_t source_stride = words[2] & 0x7fff;
    uint32_t source_x = (words[4] >> 14) & 0x1fff;
    uint32_t source_y = words[4] & 0x1fff;
    uint32_t destination_x = (words[8] >> 16) & 0x1fff;
    uint32_t destination_y = words[8] & 0x1fff;
    uint32_t destination_x1 = (words[9] >> 16) & 0x1fff;
    uint32_t destination_y1 = words[9] & 0x1fff;

    if ((!bgra8 && !argb1555) ||
        (words[4] & 0xf8002000) != 0x30000000 ||
        words[5] != 0x60800200 || words[6] != 0x8000cccc ||
        words[7] != 0xffffffff ||
        (words[8] & ~0x1fff1fff) || (words[9] & ~0x1fff1fff)) {
        *reason = "not a decoded BGRA8/ARGB1555 unity copy";
        return false;
    }
    for (unsigned i = 10; i < MBX_2D_COPY_WORDS; i++) {
        if (words[i] != MBX_2D_END) {
            *reason = "copy terminator mismatch";
            return false;
        }
    }
    if (!source_stride || source_stride % source_pixel_bytes ||
        source_stride > MBX_2D_WIDTH * source_pixel_bytes ||
        destination_x1 <= destination_x ||
        destination_y1 <= destination_y) {
        *reason = "copy stride or rectangle is invalid";
        return false;
    }

    uint32_t width = destination_x1 - destination_x;
    uint32_t height = destination_y1 - destination_y;
    uint32_t source_width = source_stride / source_pixel_bytes;
    uint32_t source_row_bytes = width * source_pixel_bytes;
    uint32_t row_bytes = width * 4;

    if (destination_x1 > MBX_2D_WIDTH || destination_y1 > MBX_2D_HEIGHT ||
        source_x > source_width || width > source_width - source_x ||
        source_y > MBX_2D_HEIGHT || height > MBX_2D_HEIGHT - source_y ||
        (words[1] & 3) || (words[3] & 3) ||
        !s5l8900_mbx_2d_validate_rows(
            s, words[3], source_x, source_y, height, source_stride,
            source_pixel_bytes, source_row_bytes, false) ||
        !s5l8900_mbx_2d_validate_rows(
            s, words[1], destination_x, destination_y, height,
            MBX_2D_STRIDE, 4, row_bytes, true)) {
        *reason = "copy surface lies outside the bounded GART mapping";
        return false;
    }

    job->pixels = g_try_malloc((size_t)row_bytes * height);
    if (!job->pixels) {
        *reason = "copy staging allocation failed";
        return false;
    }
    if (bgra8) {
        for (uint32_t row = 0; row < height; row++) {
            uint32_t source = words[3] +
                (source_y + row) * source_stride + source_x * 4;

            if (!s5l8900_mbx_gart_read(
                    s, source, job->pixels + row * row_bytes, row_bytes)) {
                *reason = "copy source mapping changed while staging";
                s5l8900_mbx_2d_job_clear(job);
                return false;
            }
        }
    } else {
        g_autofree uint8_t *packed = g_try_malloc(source_row_bytes);

        if (!packed) {
            *reason = "ARGB1555 row allocation failed";
            s5l8900_mbx_2d_job_clear(job);
            return false;
        }
        for (uint32_t row = 0; row < height; row++) {
            uint32_t source = words[3] +
                (source_y + row) * source_stride + source_x * 2;

            if (!s5l8900_mbx_gart_read(s, source, packed,
                                       source_row_bytes)) {
                *reason = "ARGB1555 source mapping changed while staging";
                s5l8900_mbx_2d_job_clear(job);
                return false;
            }
            for (uint32_t x = 0; x < width; x++) {
                uint16_t pixel = lduw_le_p(packed + x * 2);
                uint8_t *bgra = job->pixels + row * row_bytes + x * 4;

                for (unsigned component = 0; component < 3; component++) {
                    uint8_t value = (pixel >> (component * 5)) & 0x1f;
                    bgra[component] = (value << 3) | (value >> 2);
                }
                bgra[3] = (pixel & 0x8000) ? 0xff : 0;
            }
        }
    }

    job->target = words[1];
    job->x = destination_x;
    job->y = destination_y;
    job->width = width;
    job->height = height;
    job->row_bytes = row_bytes;
    job->total_bytes = row_bytes * height;
    return true;
}

static bool s5l8900_mbx_2d_fill(S5L8900MBXState *s,
                                 const uint32_t *words,
                                 S5L8900MBX2DJob *job,
                                 const char **reason)
{
    uint32_t left = (words[8] >> 16) & 0x1fff;
    uint32_t top = words[8] & 0x1fff;
    uint32_t right = (words[9] >> 16) & 0x1fff;
    uint32_t bottom = words[9] & 0x1fff;

    if (words[2] != 0x94060500 || words[3] != 0 ||
        words[4] != 0x30000000 || words[5] != 0x60800200 ||
        words[6] != MBX_2D_FILL_MODE ||
        (words[8] & ~0x1fff1fff) || (words[9] & ~0x1fff1fff)) {
        *reason = "not a decoded BGRA8 fill";
        return false;
    }
    for (unsigned i = 10; i < MBX_2D_COPY_WORDS; i++) {
        if (words[i] != MBX_2D_END) {
            *reason = "fill terminator mismatch";
            return false;
        }
    }
    if (right <= left || bottom <= top || right > MBX_2D_WIDTH ||
        bottom > MBX_2D_HEIGHT || (words[1] & 3)) {
        *reason = "fill rectangle is outside 320x480";
        return false;
    }

    uint32_t width = right - left;
    uint32_t height = bottom - top;
    uint32_t row_bytes = width * 4;

    if (!s5l8900_mbx_2d_validate_rows(
            s, words[1], left, top, height, MBX_2D_STRIDE, 4,
            row_bytes, true)) {
        *reason = "fill target lies outside the bounded GART mapping";
        return false;
    }
    job->pixels = g_try_malloc((size_t)row_bytes * height);
    if (!job->pixels) {
        *reason = "fill staging allocation failed";
        return false;
    }
    for (uint32_t offset = 0; offset < row_bytes * height; offset += 4) {
        stl_le_p(job->pixels + offset, words[7]);
    }
    job->target = words[1];
    job->x = left;
    job->y = top;
    job->width = width;
    job->height = height;
    job->row_bytes = row_bytes;
    job->total_bytes = row_bytes * height;
    return true;
}

static bool s5l8900_mbx_2d_blend(S5L8900MBXState *s,
                                  const uint32_t *words,
                                  S5L8900MBX2DJob *job,
                                  const char **reason)
{
    bool premultiplied = words[6] == MBX_2D_BLEND_EQUATION;
    bool global = (words[6] & ~MBX_2D_GLOBAL_ALPHA_MASK) ==
                  MBX_2D_GLOBAL_FACTORS;
    uint32_t source_stride = words[2] & 0x7fff;
    uint32_t source_x = (words[4] >> 14) & 0x1fff;
    uint32_t source_y = words[4] & 0x1fff;
    uint32_t destination_x = (words[10] >> 16) & 0x1fff;
    uint32_t destination_y = words[10] & 0x1fff;
    uint32_t destination_x1 = (words[11] >> 16) & 0x1fff;
    uint32_t destination_y1 = words[11] & 0x1fff;

    if ((words[2] & MBX_2D_SOURCE_FORMAT_MASK) != MBX_2D_SOURCE_BGRA8 ||
        (words[4] & 0xf8002000) != 0x30000000 ||
        words[5] != MBX_2D_BLEND_TAG || (!premultiplied && !global) ||
        words[7] != 0x60800200 || words[8] != MBX_2D_BLEND_MODE ||
        words[9] != 0xffffffff ||
        (words[10] & ~0x1fff1fff) || (words[11] & ~0x1fff1fff)) {
        *reason = "not a decoded premultiplied BGRA8 blend";
        return false;
    }
    for (unsigned i = 12; i < MBX_2D_BLEND_WORDS; i++) {
        if (words[i] != MBX_2D_END) {
            *reason = "blend terminator mismatch";
            return false;
        }
    }
    if (!source_stride || (source_stride & 3) ||
        destination_x1 <= destination_x ||
        destination_y1 <= destination_y) {
        *reason = "blend stride or rectangle is invalid";
        return false;
    }

    uint32_t width = destination_x1 - destination_x;
    uint32_t height = destination_y1 - destination_y;
    uint32_t source_width = source_stride / 4;
    uint32_t row_bytes = width * 4;

    if (destination_x1 > MBX_2D_WIDTH || destination_y1 > MBX_2D_HEIGHT ||
        source_x > source_width || width > source_width - source_x ||
        (words[1] & 3) || (words[3] & 3) ||
        !s5l8900_mbx_2d_validate_rows(
            s, words[3], source_x, source_y, height, source_stride, 4,
            row_bytes, false) ||
        !s5l8900_mbx_2d_validate_rows(
            s, words[1], destination_x, destination_y, height,
            MBX_2D_STRIDE, 4, row_bytes, true)) {
        *reason = "blend surface lies outside the bounded GART mapping";
        return false;
    }

    g_autofree uint8_t *source = g_try_malloc((size_t)row_bytes * height);
    job->pixels = g_try_malloc((size_t)row_bytes * height);
    if (!source || !job->pixels) {
        *reason = "blend staging allocation failed";
        s5l8900_mbx_2d_job_clear(job);
        return false;
    }
    for (uint32_t row = 0; row < height; row++) {
        uint32_t source_address = words[3] +
            (source_y + row) * source_stride + source_x * 4;
        uint32_t destination_address = words[1] +
            (destination_y + row) * MBX_2D_STRIDE + destination_x * 4;

        if (!s5l8900_mbx_gart_read(
                s, source_address, source + row * row_bytes, row_bytes) ||
            !s5l8900_mbx_gart_read(
                s, destination_address, job->pixels + row * row_bytes,
                row_bytes)) {
            *reason = "blend mapping changed while staging";
            s5l8900_mbx_2d_job_clear(job);
            return false;
        }
    }

    for (uint32_t offset = 0; offset < row_bytes * height; offset += 4) {
        uint32_t source_pixel = ldl_le_p(source + offset);
        uint32_t alpha = source_pixel >> 24;

        if (!global && ((source_pixel & 0xff) > alpha ||
                        ((source_pixel >> 8) & 0xff) > alpha ||
                        ((source_pixel >> 16) & 0xff) > alpha)) {
            *reason = "blend source is not premultiplied";
            s5l8900_mbx_2d_job_clear(job);
            return false;
        }
        if (global) {
            source_pixel |= 0xff000000;
            source_pixel = s5l8900_mbx_modulate_alpha(
                source_pixel,
                (words[6] & MBX_2D_GLOBAL_ALPHA_MASK) >> 12);
        }
        stl_le_p(job->pixels + offset,
                 s5l8900_mbx_source_over(
                     ldl_le_p(job->pixels + offset), source_pixel));
    }

    job->target = words[1];
    job->x = destination_x;
    job->y = destination_y;
    job->width = width;
    job->height = height;
    job->row_bytes = row_bytes;
    job->total_bytes = row_bytes * height;
    return true;
}

static bool s5l8900_mbx_2d_stage_packet(S5L8900MBXState *s,
                                         uint32_t offset,
                                         S5L8900MBX2DJob *job,
                                         uint32_t *packet_words,
                                         const char **reason)
{
    uint32_t words[MBX_2D_BLEND_WORDS];
    uint32_t expected = offset == MBX_2D_RING_BASE ?
                        MBX_2D_SUBMIT : MBX_2D_COMMAND_HEADER;

    memset(job, 0, sizeof(*job));
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        words[i] = s5l8900_mbx_ring_word(s, offset + i * 4);
    }
    if (words[0] != expected) {
        *reason = "2D command heads are not contiguous";
        return false;
    }

    if (words[5] == MBX_2D_BLEND_TAG) {
        *packet_words = MBX_2D_BLEND_WORDS;
        return s5l8900_mbx_2d_blend(s, words, job, reason);
    }
    *packet_words = MBX_2D_COPY_WORDS;
    if (words[6] == MBX_2D_FILL_MODE) {
        return s5l8900_mbx_2d_fill(s, words, job, reason);
    }
    return s5l8900_mbx_2d_simple_copy(s, words, job, reason);
}

static void s5l8900_mbx_process_2d(S5L8900MBXState *s)
{
    const char *reason = "unknown 2D packet";
    uint32_t offset = s->pending_2d_offset;
    uint32_t count = s->pending_2d_count;
    uint64_t committed = 0;

    s->pending_2d_valid = false;
    s->pending_2d_count = 0;
    if (!count || count > 255 || offset < MBX_2D_RING_BASE ||
        offset >= MBX_2D_RING_BASE + MBX_2D_RING_SIZE) {
        trace_s5l8900_mbx_2d_reject(offset, count,
                                    "invalid pending command count/offset");
        return;
    }

    for (uint32_t command = 0; command < count; command++) {
        S5L8900MBX2DJob job;
        uint32_t packet_words = 0;

        if (!s5l8900_mbx_2d_stage_packet(
                s, offset, &job, &packet_words, &reason)) {
            trace_s5l8900_mbx_2d_reject(offset, count, reason);
            return;
        }
        if (!s5l8900_mbx_2d_commit(s, &job)) {
            s5l8900_mbx_2d_job_clear(&job);
            trace_s5l8900_mbx_2d_reject(
                offset, count, "destination mapping changed during commit");
            return;
        }
        committed += job.total_bytes;
        s5l8900_mbx_2d_job_clear(&job);
        offset += packet_words * 4;
        if (offset >= MBX_2D_RING_BASE + MBX_2D_RING_SIZE) {
            offset = MBX_2D_RING_BASE;
        }
    }

    s->status |= MBX_STATUS_2D_SYNC;
    s5l8900_mbx_update_irq(s);
    trace_s5l8900_mbx_2d_complete(count, committed);
}

static uint64_t s5l8900_mbx_ring_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    S5L8900MBXState *s = opaque;
    uint8_t *aperture = memory_region_get_ram_ptr(&s->aperture);

    return ldl_le_p(aperture + MBX_2D_RING_BASE + offset);
}

static void s5l8900_mbx_ring_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    S5L8900MBXState *s = opaque;
    uint32_t aperture_offset = MBX_2D_RING_BASE + offset;
    uint8_t *aperture = memory_region_get_ram_ptr(&s->aperture);

    stl_le_p(aperture + aperture_offset, value);
    memory_region_set_dirty(&s->aperture, aperture_offset, sizeof(uint32_t));

    if (value == MBX_2D_COMMAND_HEADER) {
        if (!s->pending_2d_valid) {
            s->pending_2d_offset = aperture_offset;
            s->pending_2d_count = 1;
            s->pending_2d_valid = true;
        } else if (s->pending_2d_count != UINT32_MAX) {
            s->pending_2d_count++;
        }
        return;
    }
    if (offset == 0 && value == MBX_2D_SUBMIT && s->pending_2d_valid) {
        s5l8900_mbx_process_2d(s);
    }
}

static const MemoryRegionOps s5l8900_mbx_ring_ops = {
    .read = s5l8900_mbx_ring_read,
    .write = s5l8900_mbx_ring_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_mbx_trace_read(S5L8900MBXState *s, hwaddr offset,
                                   unsigned size, uint32_t value)
{
    if (s->trace_read_count != 0 && s->trace_read_offset == offset &&
        s->trace_read_size == size && s->trace_read_value == value) {
        s->trace_read_count++;
    } else {
        s->trace_read_offset = offset;
        s->trace_read_size = size;
        s->trace_read_value = value;
        s->trace_read_count = 1;
    }

    if ((s->trace_read_count & (s->trace_read_count - 1)) == 0) {
        trace_s5l8900_mbx_read_sample(offset, size, value,
                                     s->trace_read_count);
    }
}

static uint64_t s5l8900_mbx_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    S5L8900MBXState *s = opaque;
    uint32_t value = *s5l8900_mbx_reg(s, offset);

    if (offset == MBX_STATUS) {
        value = s->status;
    } else if (offset == MBX_RESET) {
        value &= ~MBX_RESET_DONE;
        if (s->reset_done) {
            value |= MBX_RESET_DONE;
        }
    }
    s5l8900_mbx_trace_read(s, offset, size, value);
    return value;
}

static void s5l8900_mbx_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    S5L8900MBXState *s = opaque;

    trace_s5l8900_mbx_write(offset, size, value);
    switch (offset) {
    case MBX_ID:
        return;
    case MBX_STATUS:
        s->status |= value;
        s5l8900_mbx_update_irq(s);
        return;
    case MBX_STATUS_ACK:
        s->status &= ~value;
        s5l8900_mbx_update_irq(s);
        return;
    case MBX_INTERRUPT_MASK:
        *s5l8900_mbx_reg(s, offset) = value;
        s5l8900_mbx_update_irq(s);
        return;
    case MBX_TA_CONTEXT_RESET:
    case MBX_TA_CONTEXT_STORE:
    case MBX_TA_CONTEXT_LOAD:
        *s5l8900_mbx_reg(s, offset) = value;
        if (value == 1) {
            s->status |= MBX_STATUS_TA_CONTEXT;
            s5l8900_mbx_update_irq(s);
        }
        return;
    case MBX_TA_START:
        *s5l8900_mbx_reg(s, offset) = value;
        if (value == 1) {
            s->ta_capture_count = 0;
            s->ta_capture_failed = false;
            s->ta_in_flight = true;
            s5l8900_mbx_trace_3d_start(s);
        }
        return;
    case MBX_STARTRENDER:
        *s5l8900_mbx_reg(s, offset) = value;
        if (value == 1) {
            s5l8900_mbx_trace_3d_object(s);
            trace_s5l8900_mbx_3d_render(
                *s5l8900_mbx_reg(s, MBX_RGNBASE),
                *s5l8900_mbx_reg(s, MBX_OBJBASE),
                *s5l8900_mbx_reg(s, MBX_FBSTART),
                *s5l8900_mbx_reg(s, MBX_FBXCLIP),
                *s5l8900_mbx_reg(s, MBX_FBYCLIP),
                *s5l8900_mbx_reg(s, MBX_3DPIXSAMP),
                *s5l8900_mbx_reg(s, MBX_FBCTL),
                *s5l8900_mbx_reg(s, MBX_FBLINESTRIDE));
            /* Every STARTRENDER is a hardware submission.  In particular,
             * SpringBoard switches to a new IOSurface before issuing its
             * first narrow dirty rectangles, so waiting for a later full-
             * panel signature leaves both the pixels and completion absent.
             * Keep acceptance bounded in the decoders, but never suppress a
             * submission based on the shape of an earlier or future frame. */
            s5l8900_mbx_execute_3d(s);
        }
        return;
    case MBX_BACKGROUND_TAG:
    case MBX_TA_OBJECT_DATABASE:
        *s5l8900_mbx_reg(s, offset) = value;
        /*
         * Both observed firmware generations synchronously wait for EVM
         * deallocation after their final setup write.  The older producer
         * uses BACKGROUND_TAG; 8C148 ends at TA_OBJECT_DATABASE instead.
         */
        s->status |= MBX_STATUS_EVM_DALLOC;
        s5l8900_mbx_update_irq(s);
        return;
    case MBX_RESET:
        *s5l8900_mbx_reg(s, offset) = value & ~MBX_RESET_DONE;
        s->reset_done = (value & MBX_RESET_REQUEST) != 0;
        return;
    default:
        *s5l8900_mbx_reg(s, offset) = value;
        return;
    }
}

static const MemoryRegionOps s5l8900_mbx_ops = {
    .read = s5l8900_mbx_read,
    .write = s5l8900_mbx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void s5l8900_mbx_reset(DeviceState *dev)
{
    S5L8900MBXState *s = S5L8900_MBX(dev);
    uint8_t *aperture = memory_region_get_ram_ptr(&s->aperture);

    memset(s->regs, 0, sizeof(s->regs));
    memset(aperture, 0, S5L8900_MBX_APERTURE_SIZE);
    memory_region_set_dirty(&s->aperture, 0, S5L8900_MBX_APERTURE_SIZE);
    s->status = 0;
    s->reset_done = false;
    s->pending_2d_valid = false;
    s->pending_2d_count = 0;
    s->ta_capture_count = 0;
    s->ta_capture_failed = false;
    s->ta_in_flight = false;
    timer_del(s->render_timer);
    s->render_completion_pending = false;
    s->render_3d_enabled = false;
    s->metal_submissions = 0;
    s->trace_read_count = 0;
    *s5l8900_mbx_reg(s, MBX_ID) = MBX_ID_S5L8900;
    qemu_set_irq(s->irq, 0);
}

static int s5l8900_mbx_post_load(void *opaque, int version_id)
{
    S5L8900MBXState *s = opaque;

    if (version_id < 2) {
        s->reset_done =
            (*s5l8900_mbx_reg(s, MBX_RESET) & MBX_RESET_REQUEST) != 0;
    }
    if (version_id < 3) {
        s->status = *s5l8900_mbx_reg(s, MBX_STATUS);
    }
    if (version_id < 4) {
        s->pending_2d_valid = false;
        s->pending_2d_count = 0;
    }
    if (version_id < 5) {
        s->ta_capture_count = 0;
        s->ta_capture_failed = false;
        s->ta_in_flight = false;
    }
    if (version_id < 6) {
        timer_del(s->render_timer);
        s->render_completion_pending = false;
    }
    if (version_id < 7) {
        s->render_3d_enabled = false;
    }
    *s5l8900_mbx_reg(s, MBX_STATUS) = 0;
    *s5l8900_mbx_reg(s, MBX_RESET) &= ~MBX_RESET_DONE;
    s5l8900_mbx_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8900_mbx = {
    .name = TYPE_S5L8900_MBX,
    .version_id = 7,
    .minimum_version_id = 1,
    .post_load = s5l8900_mbx_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8900MBXState,
                             S5L8900_MBX_REGISTER_COUNT),
        VMSTATE_BOOL_V(reset_done, S5L8900MBXState, 2),
        VMSTATE_UINT32_V(status, S5L8900MBXState, 3),
        VMSTATE_UINT32_V(pending_2d_offset, S5L8900MBXState, 4),
        VMSTATE_UINT32_V(pending_2d_count, S5L8900MBXState, 4),
        VMSTATE_BOOL_V(pending_2d_valid, S5L8900MBXState, 4),
        VMSTATE_UINT32_V(ta_capture_count, S5L8900MBXState, 5),
        VMSTATE_BOOL_V(ta_capture_failed, S5L8900MBXState, 5),
        VMSTATE_BOOL_V(ta_in_flight, S5L8900MBXState, 5),
        VMSTATE_TIMER_PTR_V(render_timer, S5L8900MBXState, 6),
        VMSTATE_BOOL_V(render_completion_pending, S5L8900MBXState, 6),
        VMSTATE_BOOL_V(render_3d_enabled, S5L8900MBXState, 7),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8900_mbx_realize(DeviceState *dev, Error **errp)
{
    S5L8900MBXState *s = S5L8900_MBX(dev);

    if (!s->system_memory) {
        error_setg(errp, TYPE_S5L8900_MBX
                   " requires a system-memory link");
        return;
    }

    s->render_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   s5l8900_mbx_render_complete, s);

    memory_region_init(&s->iomem, OBJECT(dev), TYPE_S5L8900_MBX,
                       S5L8900_MBX_APERTURE_SIZE);
    if (!memory_region_init_ram(&s->aperture, OBJECT(dev),
                                "s5l8900-mbx.command-memory",
                                S5L8900_MBX_APERTURE_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(&s->iomem, 0, &s->aperture);
    memory_region_init_io(&s->control, OBJECT(dev), &s5l8900_mbx_ops, s,
                          "s5l8900-mbx.control",
                          S5L8900_MBX_CONTROL_SIZE);
    memory_region_add_subregion_overlap(&s->iomem, 0, &s->control, 1);
    memory_region_init_io(&s->ring, OBJECT(dev), &s5l8900_mbx_ring_ops, s,
                          "s5l8900-mbx.2d-ring", MBX_2D_RING_SIZE);
    memory_region_add_subregion_overlap(&s->iomem, MBX_2D_RING_BASE,
                                        &s->ring, 2);
    memory_region_init_io(&s->fifo, OBJECT(dev), &s5l8900_mbx_fifo_ops, s,
                          "s5l8900-mbx.3d-fifo", sizeof(uint32_t));
    memory_region_add_subregion_overlap(&s->iomem, MBX_3D_DATA_FIFO,
                                        &s->fifo, 2);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    if (s->metal_enabled) {
        s->metal = s5l8900_mbx_metal_create(errp);
        if (!s->metal) {
            return;
        }
    }
}

static void s5l8900_mbx_unrealize(DeviceState *dev)
{
    S5L8900MBXState *s = S5L8900_MBX(dev);

    timer_free(s->render_timer);
    s->render_timer = NULL;
    s5l8900_mbx_metal_destroy(s->metal);
    s->metal = NULL;
}

static const Property s5l8900_mbx_properties[] = {
    DEFINE_PROP_LINK("system-memory", S5L8900MBXState, system_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_BOOL("metal", S5L8900MBXState, metal_enabled, false),
};

static void s5l8900_mbx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8900_mbx_realize;
    dc->unrealize = s5l8900_mbx_unrealize;
    dc->vmsd = &vmstate_s5l8900_mbx;
    dc->desc = "Apple S5L8900 PowerVR MBX wrapper";
    device_class_set_props(dc, s5l8900_mbx_properties);
    device_class_set_legacy_reset(dc, s5l8900_mbx_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo s5l8900_mbx_info = {
    .name = TYPE_S5L8900_MBX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900MBXState),
    .class_init = s5l8900_mbx_class_init,
};

static void s5l8900_mbx_register_types(void)
{
    type_register_static(&s5l8900_mbx_info);
}

type_init(s5l8900_mbx_register_types)
