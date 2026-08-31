/*
 * Bounded PowerVR MBX3D object-list decoders.
 *
 * Mechanically adapted from S5LBox commit
 * 6f203ba550b49afadee008c7eb55373a838eed33 under its MIT license.  The full
 * copyright and permission notice is in the including s5l8900_mbx.c file.
 */

static bool mbx_gart_u32(const s5l_mbx_t *m, const arm_bus_t *bus,
                         uint32_t gpu_va, uint32_t *value,
                         const char **why) {
    uint8_t bytes[4];
    if (!mbx_gart_read(m, bus, gpu_va, bytes, sizeof bytes, why)) return false;
    *value = mbx_load_le32(bytes);
    return true;
}

static uint32_t mbx_3d_decode_address(uint32_t word) {
    return (word & MBX_3D_ADDRESS_MASK) << 7;
}

static void mbx_store_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/* QuartzCore selects source ONE and destination ONE_MINUS_SRC_ALPHA for this
 * object. Its shipped software compositor at 0x3122dcf4 uses the same 8-bit
 * fixed-point equation, with 256-alpha rather than an inferred /255 rule.
 * Fixed-function blending clamps each resulting component. Premultiplied
 * sources never reach that clamp, while arbitrary BGRA8 texture bytes can. */
static uint32_t mbx_source_over_clamped(uint32_t dst, uint32_t src) {
    uint32_t inv = 256u - (src >> 24);
    uint32_t out = 0u;
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        uint32_t s = (src >> shift) & 0xffu;
        uint32_t d = (dst >> shift) & 0xffu;
        uint32_t blended = s + ((d * inv) >> 8);
        if (blended > 0xffu) blended = 0xffu;
        out |= blended << shift;
    }
    return out;
}

/* QuartzCore's software rasterizer modulates a sampled channel as
 * `vertex * (texture + 1) >> 8`. The r180 software frame and r382 MBX frame
 * independently pin that equation for these status sprites: an unmodulated
 * source channel 255 becomes 191 under the captured 0xbf vertex alpha, and
 * 183 becomes 137. Applying one alpha to every premultiplied BGRA8 channel
 * preserves the source-over invariant and the identity endpoint at 255. */
static uint32_t mbx_modulate_vertex_alpha(uint32_t src, uint32_t alpha) {
    uint32_t out = 0u;
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        uint32_t component = (src >> shift) & 0xffu;
        out |= (((component + 1u) * alpha) >> 8) << shift;
    }
    return out;
}

/* QuartzCore's shipped sw_sample_linear_BGRA8 implementation at
 * 0x3122bc08..0x3122bcb4 interpolates BGRA8 in two packed byte lanes.  Keep
 * the unsigned wrapping multiply/adds: for a descending channel they are what
 * reproduces the native floor-toward-minus-infinity result after the mask. */
static uint32_t mbx_linear_bgra8(uint32_t first, uint32_t second,
                                 uint32_t weight) {
    const uint32_t lanes = UINT32_C(0x00ff00ff);
    uint32_t first_even = first & lanes;
    uint32_t second_even = second & lanes;
    uint32_t first_odd = (first >> 8) & lanes;
    uint32_t second_odd = (second >> 8) & lanes;
    uint32_t even = first_even +
                    ((weight * (second_even - first_even)) >> 8);
    uint32_t odd = first_odd +
                   ((weight * (second_odd - first_odd)) >> 8);

    return (even & lanes) | ((odd & lanes) << 8);
}

struct mbx_bilinear_axis {
    uint32_t first;
    uint32_t second;
    uint32_t weight;
};

static bool mbx_bilinear_coordinate(float coordinate, uint32_t dimension,
                                    struct mbx_bilinear_axis *axis) {
    if (!axis || !dimension || dimension > 1024u || coordinate < 0.0f)
        return false;
    float fixed_float = coordinate * 65536.0f;
    if (fixed_float > (float)INT32_MAX) return false;

    int64_t raw = (int64_t)(int32_t)fixed_float - INT64_C(32768);
    int64_t neighbour = raw + INT64_C(65536);
    uint32_t maximum = (dimension << 16) - 1u;
    uint32_t first_fixed = raw < 0 ? 0u :
        (uint64_t)raw > maximum ? maximum : (uint32_t)raw;
    uint32_t second_fixed = neighbour < 0 ? 0u :
        (uint64_t)neighbour > maximum ? maximum : (uint32_t)neighbour;

    axis->first = first_fixed >> 16;
    axis->second = second_fixed >> 16;
    axis->weight = (first_fixed & UINT32_C(0xffff)) >> 8;
    return true;
}

/* Convert one covered destination pixel centre into the same clamped 16.16
 * tap pair consumed by QuartzCore's software sampler.  The MBX interpolator's
 * undocumented sub-LSB precision cannot be hardware-oracled here; binary32
 * interpolation is the producer/software-renderer reference and is kept
 * explicit instead of pretending the old contiguous-row shortcut was exact. */
static bool mbx_bilinear_axis(float origin, float span,
                              float texel_origin, float texel_span,
                              uint32_t pixel, uint32_t dimension,
                              struct mbx_bilinear_axis *axis) {
    if (!axis || !dimension || dimension > 1024u || span <= 0.0f ||
        texel_origin < 0.0f || texel_span <= 0.0f)
        return false;

    float step = texel_span / span;
    float coordinate = texel_origin +
                       ((float)pixel + 0.5f - origin) * step;
    return mbx_bilinear_coordinate(coordinate, dimension, axis);
}

struct mbx_3d_word {
    uint16_t off;
    uint32_t value;
};

struct mbx_3d_background_form {
    uint32_t xclip, yclip;
    uint32_t tile_x0, tile_x1, tile_y0, tile_y1;
    uint32_t left, top, width, height;
    uint32_t source_row0;
    uint32_t boundary[8];
};

/* The first entry is r369's full 320x96 overlay. r379 captured the next redraw
 * as two literal dirty rectangles over the same quad/source: source rows 0..76
 * across the full width, then rows 77..88 inset by eight pixels. Keeping all
 * three forms exact makes those clipped redraws possible without accepting an
 * arbitrary tile stream or inferring a generic PowerVR rasterizer. */
static const struct mbx_3d_background_form mbx_3d_background_forms[] = {
    {
        .xclip = 0x01400000u, .yclip = 0x00800010u,
        .tile_x0 = 0u, .tile_x1 = 39u,
        .tile_y0 = 1u, .tile_y1 = 7u,
        .left = 0u, .top = 20u, .width = 320u, .height = 96u,
        .source_row0 = 0u,
        .boundary = {
            0x00000000u, 0x42e80000u, 0x00000000u, 0x41a00000u,
            0x43a00000u, 0x42e80000u, 0x43a00000u, 0x41a00000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x00700010u,
        .tile_x0 = 0u, .tile_x1 = 39u,
        .tile_y0 = 1u, .tile_y1 = 6u,
        .left = 0u, .top = 20u, .width = 320u, .height = 77u,
        .source_row0 = 0u,
        .boundary = {
            0x00000000u, 0x42c20000u, 0x00000000u, 0x41a00000u,
            0x43a00000u, 0x42c20000u, 0x43a00000u, 0x41a00000u,
        },
    },
    {
        .xclip = 0x01380008u, .yclip = 0x00700060u,
        .tile_x0 = 1u, .tile_x1 = 38u,
        .tile_y0 = 6u, .tile_y1 = 6u,
        .left = 8u, .top = 97u, .width = 304u, .height = 12u,
        .source_row0 = 77u,
        .boundary = {
            0x41000000u, 0x42da0000u, 0x41000000u, 0x42c20000u,
            0x439c0000u, 0x42da0000u, 0x439c0000u, 0x42c20000u,
        },
    },
};

static const struct mbx_3d_background_form *
mbx_3d_find_background_form(const s5l_mbx_t *m) {
    uint32_t xclip = m->reg[S5L_MBX_FBXCLIP / 4u];
    uint32_t yclip = m->reg[S5L_MBX_FBYCLIP / 4u];
    for (unsigned i = 0;
         i < sizeof mbx_3d_background_forms /
             sizeof mbx_3d_background_forms[0]; i++) {
        const struct mbx_3d_background_form *form =
            &mbx_3d_background_forms[i];
        if (form->xclip == xclip && form->yclip == yclip) return form;
    }
    return NULL;
}

static uint32_t mbx_3d_boundary_fixed_expected(uint32_t off) {
    static const struct mbx_3d_word nonzero[] = {
        {0x080u, 0x22206f80u}, {0x088u, 0x45800000u},
        {0x094u, 0x45800000u}, {0x098u, 0x45800000u},
        {0x09cu, 0x45800000u}, {0x0b4u, 0x22207f80u},
    };
    for (unsigned i = 0; i < sizeof nonzero / sizeof nonzero[0]; i++)
        if (nonzero[i].off == off) return nonzero[i].value;
    return 0u;
}

static uint32_t mbx_3d_background_boundary_expected(
    const struct mbx_3d_background_form *form, uint32_t off) {
    if (off >= 0x0b8u && off <= 0x0d4u)
        return form->boundary[(off - 0x0b8u) / 4u];
    return mbx_3d_boundary_fixed_expected(off);
}

static bool mbx_execute_first_tiled_over(s5l_mbx_t *m,
                                         const arm_bus_t *bus,
                                         const char **why,
                                         uint32_t *pixels_blended) {
    uint32_t region = m->reg[S5L_MBX_RGNBASE / 4u];
    uint32_t object = m->reg[S5L_MBX_OBJBASE / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];
    const struct mbx_3d_background_form *form =
        mbx_3d_find_background_form(m);

    if ((region & 3u) || (object & 3u) || (target & 3u) ||
        object > UINT32_MAX - 0x2a0u) {
        if (why) *why = "region, object, or framebuffer base is invalid";
        return false;
    }
    if (m->reg[S5L_MBX_3DPIXSAMP / 4u] != 0x00020007u ||
        m->reg[S5L_MBX_FBCTL / 4u] != 0x00000006u ||
        m->reg[S5L_MBX_FBLINESTRIDE / 4u] != MBX_3D_WIDTH || !form) {
        if (why) *why = "render registers are not a captured BGRA8 background form";
        return false;
    }

    /* Each captured form is an ordered rectangle of 8x16 tiles. Every tile
     * points at the same list, and only the final tile carries bit 31. */
    uint32_t list = object + 0x68u;
    uint32_t tile_count = (form->tile_x1 - form->tile_x0 + 1u) *
                          (form->tile_y1 - form->tile_y0 + 1u);
    uint32_t tile_index = 0u;
    for (uint32_t y = form->tile_y0; y <= form->tile_y1; y++) {
        for (uint32_t x = form->tile_x0; x <= form->tile_x1; x++) {
            uint32_t pair = tile_index * 8u;
            uint32_t code, pointer;
            if (!mbx_gart_u32(m, bus, region + pair, &code, why) ||
                !mbx_gart_u32(m, bus, region + pair + 4u, &pointer, why))
                return false;
            uint32_t expected = (y << 8) | x;
            if (tile_index + 1u == tile_count) expected |= 0x80000000u;
            if (code != expected || pointer != list) {
                if (why) *why = "region tile list differs from its captured background stream";
                return false;
            }
            tile_index++;
        }
    }

    static const uint32_t list_words[4] = {
        0x60200020u, 0x6020002du, 0x61a0007cu, 0xf0000000u
    };
    for (unsigned i = 0; i < 4u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, list + i * 4u, &value, why)) return false;
        if (value != list_words[i]) {
            if (why) *why = "object list is not the captured three-object list";
            return false;
        }
    }

    static const uint32_t background[26] = {
        0xe0000000u, 0xa7718000u, 0u, 0xd6887610u,
        0x22220e80u, 0u, 0u, 0x45000000u,
        0u, 0u, 0x45000000u, 0x3f800000u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0x3f800000u, 0u, 0u, 0u,
        0u, 0x40000000u, 0u, 0u,
        0u, 0x40000000u,
    };
    for (unsigned i = 0; i < 26u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + i * 4u, &value, why)) return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "background texture does not resolve to FBSTART";
                return false;
            }
        } else if (value != background[i]) {
            if (why) *why = "background object differs from the captured form";
            return false;
        }
    }

    /* The first two list entries are consecutive 13-word boundary objects at
     * +0x80 and +0xb4. Bytes from +0xe8 to the pointer-selected third object
     * are not referenced and may retain an older command. */
    for (uint32_t off = 0x80u; off < 0x0e8u; off += 4u) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + off, &value, why)) return false;
        if (value != mbx_3d_background_boundary_expected(form, off)) {
            if (why) *why = "rectangle boundary object differs from the captured form";
            return false;
        }
    }

    static const uint32_t quad[44] = {
        0xe0000000u, 0xa0418001u, 0u, 0xa6884710u,
        0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
        0x00000000u, 0x41a00000u, 0x43a00000u, 0x41a00000u,
        0x00000000u, 0x42e80000u, 0x43a00000u, 0x42e80000u,
        0u, 0u, 0u, 0u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0xff000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x3ca00000u, 0xff000000u, 0x3d800000u, 0x00000000u,
        0x3ea00000u, 0x3ca00000u, 0xff000000u, 0x00000000u,
        0x3f400000u, 0x00000000u, 0x3de80000u, 0xff000000u,
        0x3d800000u, 0x3f400000u, 0x3ea00000u, 0x3de80000u,
    };
    uint32_t source = 0u;
    for (unsigned i = 0; i < 44u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + 0x1f0u + i * 4u,
                          &value, why))
            return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x8e000000u) {
                if (why) *why = "source texture address word has an unknown format";
                return false;
            }
            source = mbx_3d_decode_address(value);
        } else if (i == 5u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "destination texture does not resolve to FBSTART";
                return false;
            }
        } else if (value != quad[i]) {
            if (why) *why = "textured quad differs from the captured source-over form";
            return false;
        }
    }
    if (!source) {
        if (why) *why = "source texture resolves to GPU address zero";
        return false;
    }

    uint32_t row_bytes = form->width * 4u;
    uint32_t total = row_bytes * form->height;
    uint64_t source_end = (uint64_t)source +
                          (uint64_t)(form->source_row0 + form->height - 1u) *
                              MBX_3D_SOURCE_STRIDE + 4u;
    uint64_t target_end = (uint64_t)target +
                          (uint64_t)(form->top + form->height - 1u) *
                              MBX_3D_TARGET_STRIDE +
                          (uint64_t)(form->left + form->width) * 4u;
    if (source_end > UINT32_MAX || target_end > UINT32_MAX) {
        if (why) *why = "captured background rectangle overflows its surface";
        return false;
    }
    for (uint32_t row = 0; row < form->height; row++) {
        uint32_t src = source +
                       (form->source_row0 + row) * MBX_3D_SOURCE_STRIDE;
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        if (!mbx_gart_validate(m, bus, src, 4u, why) ||
            !mbx_gart_validate(m, bus, dst, row_bytes, why))
            return false;
    }

    uint8_t *pixels = malloc(total);
    if (!pixels) {
        if (why) *why = "host allocation for staged 3D pixels failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < form->height && ok; row++) {
        uint8_t src_bytes[4];
        uint32_t src_addr = source +
                            (form->source_row0 + row) * MBX_3D_SOURCE_STRIDE;
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        ok = mbx_gart_read(m, bus, src_addr, src_bytes,
                           sizeof src_bytes, why) &&
             mbx_gart_read(m, bus, dst, pixels + row * row_bytes,
                           row_bytes, why);
        if (!ok) break;
        uint32_t src = mbx_load_le32(src_bytes);
        uint32_t alpha = src >> 24;
        if ((src & 0xffu) > alpha || ((src >> 8) & 0xffu) > alpha ||
            ((src >> 16) & 0xffu) > alpha) {
            if (why) *why = "source texture is not premultiplied BGRA8";
            ok = false;
            break;
        }
        for (uint32_t x = 0; x < form->width; x++) {
            uint8_t *pixel = pixels + row * row_bytes + x * 4u;
            uint32_t blended = mbx_source_over_clamped(
                mbx_load_le32(pixel), src);
            pixel[0] = (uint8_t)blended;
            pixel[1] = (uint8_t)(blended >> 8);
            pixel[2] = (uint8_t)(blended >> 16);
            pixel[3] = (uint8_t)(blended >> 24);
        }
    }
    for (uint32_t row = 0; row < form->height && ok; row++) {
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        ok = mbx_gart_write(m, bus, dst, pixels + row * row_bytes,
                            row_bytes, why);
    }
    free(pixels);
    if (ok && pixels_blended) *pixels_blended = form->width * form->height;
    return ok;
}

struct mbx_3d_status_form {
    uint32_t xclip, yclip;
    uint32_t target;
    bool variable_vertex_alpha;
    bool boundary_override;
    uint32_t tile_x0, tile_x1, tile_y0, tile_y1;
    uint32_t left, top, width, height;
    uint32_t source_row0;
    uint32_t source_stride;
    uint32_t source_control;
    uint32_t boundary[8];
    uint32_t quad[44];
};

/* These are literal transcriptions of the live object streams. Words 2 and 5
 * are address fields and are validated separately against each form's control
 * bits and FBSTART; every other word must match exactly. A zero target accepts
 * either surface used by the earlier status forms.
 *
 * The slider label is the one variable-alpha exception: r385/r387/r389
 * measured the same word at all four vertices while its high byte stepped b4,
 * 8a, 61, 37 and its low 24 bits stayed zero. Only those four words may vary,
 * must remain identical, and are consumed as the per-vertex alpha established
 * by the software-renderer pixel oracle. r402-r406's tutorial layers retain
 * their literal b7/05 alpha words; one capture is not treated as proof of an
 * arbitrary opacity range. Five former 0x612 forms were removed after r414
 * proved that they followed the stale +0x1f0 texture instead of the object
 * selected by the list pointer. */
static const struct mbx_3d_status_form mbx_3d_status_forms[] = {
    {
        .xclip = 0x00a80098u, .yclip = 0x00200000u,
        .tile_x0 = 0x13u, .tile_x1 = 0x14u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 155u, .top = 0u, .width = 10u, .height = 20u,
        .source_stride = 0x40u, .source_control = 0x0e040000u,
        .quad = {
            0xe0000000u, 0xa1218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x431b0000u, 0x41a00000u, 0x431b0000u, 0x00000000u,
            0x43250000u, 0x41a00000u, 0x43250000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x3e1b0000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x3e1b0000u, 0x00000000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3e250000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x00000000u, 0x3e250000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x00500000u, .yclip = 0x00200000u,
        .tile_x0 = 0u, .tile_x1 = 9u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 4u, .top = 1u, .width = 76u, .height = 16u,
        .source_stride = 0x140u, .source_control = 0x0e140000u,
        .quad = {
            0xe0000000u, 0xa4118000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x40800000u, 0x41880000u, 0x40800000u, 0x3f800000u,
            0x42a00000u, 0x41880000u, 0x42a00000u, 0x3f800000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f800000u, 0x3b800000u,
            0x3c880000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x3b800000u, 0x3a800000u, 0xbf000000u, 0x3f180000u,
            0x3f800000u, 0x3da00000u, 0x3c880000u, 0xbf000000u,
            0x3f180000u, 0x00000000u, 0x3da00000u, 0x3a800000u,
        },
    },
    {
        .xclip = 0x01400128u, .yclip = 0x00200000u,
        .tile_x0 = 0x25u, .tile_x1 = 0x27u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 296u, .top = 0u, .width = 21u, .height = 20u,
        .source_stride = 0x60u, .source_control = 0x0e040000u,
        .quad = {
            0xe0000000u, 0xa2218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x43940000u, 0x41a00000u, 0x43940000u, 0x00000000u,
            0x439e8000u, 0x41a00000u, 0x439e8000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x3e940000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x3e940000u, 0x00000000u, 0xbf000000u, 0x3f280000u,
            0x3f200000u, 0x3e9e8000u, 0x3ca00000u, 0xbf000000u,
            0x3f280000u, 0x00000000u, 0x3e9e8000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x00a80098u, .yclip = 0x00200010u,
        .tile_x0 = 0x13u, .tile_x1 = 0x14u,
        .tile_y0 = 1u, .tile_y1 = 1u,
        .left = 155u, .top = 16u, .width = 10u, .height = 4u,
        .source_row0 = 16u,
        .source_stride = 0x40u, .source_control = 0x0e040000u,
        .quad = {
            0xe0000000u, 0xa1218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x431b0000u, 0x41a00000u, 0x431b0000u, 0x41800000u,
            0x43250000u, 0x41a00000u, 0x43250000u, 0x41800000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x3e1b0000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x3f000000u,
            0x3e1b0000u, 0x3c800000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3e250000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x3f000000u, 0x3e250000u, 0x3c800000u,
        },
    },
    {
        .xclip = 0x01400128u, .yclip = 0x00200010u,
        .tile_x0 = 0x25u, .tile_x1 = 0x27u,
        .tile_y0 = 1u, .tile_y1 = 1u,
        .left = 296u, .top = 16u, .width = 21u, .height = 4u,
        .source_row0 = 16u,
        .source_stride = 0x60u, .source_control = 0x0e040000u,
        .quad = {
            0xe0000000u, 0xa2218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x43940000u, 0x41a00000u, 0x43940000u, 0x41800000u,
            0x439e8000u, 0x41a00000u, 0x439e8000u, 0x41800000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x3e940000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x3f000000u,
            0x3e940000u, 0x3c800000u, 0xbf000000u, 0x3f280000u,
            0x3f200000u, 0x3e9e8000u, 0x3ca00000u, 0xbf000000u,
            0x3f280000u, 0x3f000000u, 0x3e9e8000u, 0x3c800000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x00200000u,
        .target = 0x00998000u,
        .tile_x0 = 0u, .tile_x1 = 0x27u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 0u, .top = 0u, .width = 320u, .height = 20u,
        .source_stride = 0x500u, .source_control = 0x0e500000u,
        .quad = {
            0xe0000000u, 0xa6218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x00000000u, 0x41a00000u, 0x00000000u, 0x00000000u,
            0x43a00000u, 0x41a00000u, 0x43a00000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x00000000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x00000000u, 0x00000000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3ea00000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x00000000u, 0x3ea00000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x00200000u,
        .target = 0x00897000u,
        .tile_x0 = 0u, .tile_x1 = 0x27u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 0u, .top = 0u, .width = 320u, .height = 20u,
        .source_stride = 0x500u, .source_control = 0x0e500000u,
        .quad = {
            0xe0000000u, 0xa6218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x00000000u, 0x41a00000u, 0x00000000u, 0x00000000u,
            0x43a00000u, 0x41a00000u, 0x43a00000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x00000000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x00000000u, 0x00000000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3ea00000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x00000000u, 0x3ea00000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x00200000u,
        .target = 0x00a41000u,
        .tile_x0 = 0u, .tile_x1 = 0x27u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 0u, .top = 0u, .width = 320u, .height = 20u,
        .source_stride = 0x500u, .source_control = 0x0e500000u,
        .quad = {
            0xe0000000u, 0xa6218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x00000000u, 0x41a00000u, 0x00000000u, 0x00000000u,
            0x43a00000u, 0x41a00000u, 0x43a00000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x00000000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x00000000u, 0x00000000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3ea00000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x00000000u, 0x3ea00000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x01e00010u,
        .target = 0x00a41000u,
        .tile_x0 = 0u, .tile_x1 = 0x27u,
        .tile_y0 = 1u, .tile_y1 = 0x1du,
        .left = 0u, .top = 20u, .width = 320u, .height = 460u,
        .source_row0 = 20u,
        .source_stride = 0x500u, .source_control = 0x0e500000u,
        .quad = {
            0xe0000000u, 0xa6618000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x00000000u, 0x43f00000u, 0x00000000u, 0x41a00000u,
            0x43a00000u, 0x43f00000u, 0x43a00000u, 0x41a00000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xb7000000u, 0x00000000u, 0x3f700000u, 0x00000000u,
            0x3ef00000u, 0xb7000000u, 0x00000000u, 0x3d200000u,
            0x00000000u, 0x3ca00000u, 0xb7000000u, 0x3f200000u,
            0x3f700000u, 0x3ea00000u, 0x3ef00000u, 0xb7000000u,
            0x3f200000u, 0x3d200000u, 0x3ea00000u, 0x3ca00000u,
        },
    },
    {
        .xclip = 0x01300010u, .yclip = 0x01800080u,
        .target = 0x00a41000u,
        .tile_x0 = 2u, .tile_x1 = 0x25u,
        .tile_y0 = 8u, .tile_y1 = 0x17u,
        .left = 18u, .top = 130u, .width = 284u, .height = 241u,
        .source_stride = 0x480u, .source_control = 0x0e480000u,
        .quad = {
            0xe0000000u, 0xa6518000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x41900000u, 0x43b98000u, 0x41900000u, 0x43020000u,
            0x43970000u, 0x43b98000u, 0x43970000u, 0x43020000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0x05000000u, 0x00000000u, 0x3f710000u, 0x3c900000u,
            0x3eb98000u, 0x05000000u, 0x00000000u, 0x00000000u,
            0x3c900000u, 0x3e020000u, 0x05000000u, 0x3f0e0000u,
            0x3f710000u, 0x3e970000u, 0x3eb98000u, 0x05000000u,
            0x3f0e0000u, 0x00000000u, 0x3e970000u, 0x3e020000u,
        },
    },
    {
        .xclip = 0x01280018u, .yclip = 0x00b00090u,
        .target = 0x00a41000u,
        .tile_x0 = 3u, .tile_x1 = 0x24u,
        .tile_y0 = 9u, .tile_y1 = 0x0au,
        .left = 30u, .top = 145u, .width = 260u, .height = 23u,
        .source_stride = 0x420u, .source_control = 0x0e400000u,
        .quad = {
            0xe0000000u, 0xa6218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x41f00000u, 0x43280000u, 0x41f00000u, 0x43110000u,
            0x43910000u, 0x43280000u, 0x43910000u, 0x43110000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0x05000000u, 0x00000000u, 0x3f380000u, 0x3cf00000u,
            0x3e280000u, 0x05000000u, 0x00000000u, 0x00000000u,
            0x3cf00000u, 0x3e110000u, 0x05000000u, 0x3f020000u,
            0x3f380000u, 0x3e910000u, 0x3e280000u, 0x05000000u,
            0x3f020000u, 0x00000000u, 0x3e910000u, 0x3e110000u,
        },
    },
    {
        .xclip = 0x01280018u, .yclip = 0x013000a0u,
        .target = 0x00a41000u,
        .tile_x0 = 3u, .tile_x1 = 0x24u,
        .tile_y0 = 0x0au, .tile_y1 = 0x12u,
        .left = 30u, .top = 175u, .width = 260u, .height = 121u,
        .source_stride = 0x420u, .source_control = 0x0e400000u,
        .quad = {
            0xe0000000u, 0xa6418001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x41f00000u, 0x43940000u, 0x41f00000u, 0x432f0000u,
            0x43910000u, 0x43940000u, 0x43910000u, 0x432f0000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0x05000000u, 0x00000000u, 0x3f720000u, 0x3cf00000u,
            0x3e940000u, 0x05000000u, 0x00000000u, 0x00000000u,
            0x3cf00000u, 0x3e2f0000u, 0x05000000u, 0x3f020000u,
            0x3f720000u, 0x3e910000u, 0x3e940000u, 0x05000000u,
            0x3f020000u, 0x00000000u, 0x3e910000u, 0x3e2f0000u,
        },
    },
    {
        .xclip = 0x01280018u, .yclip = 0x01700130u,
        .target = 0x00a41000u,
        .tile_x0 = 3u, .tile_x1 = 0x24u,
        .tile_y0 = 0x13u, .tile_y1 = 0x16u,
        .left = 29u, .top = 312u, .width = 262u, .height = 43u,
        .source_stride = 0x420u, .source_control = 0x0e400000u,
        .quad = {
            0xe0000000u, 0xa6318001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x41e80000u, 0x43b18000u, 0x41e80000u, 0x439c0000u,
            0x43918000u, 0x43b18000u, 0x43918000u, 0x439c0000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0x05000000u, 0x00000000u, 0x3f2c0000u, 0x3ce80000u,
            0x3eb18000u, 0x05000000u, 0x00000000u, 0x00000000u,
            0x3ce80000u, 0x3e9c0000u, 0x05000000u, 0x3f030000u,
            0x3f2c0000u, 0x3e918000u, 0x3eb18000u, 0x05000000u,
            0x3f030000u, 0x00000000u, 0x3e918000u, 0x3e9c0000u,
        },
    },
    {
        .xclip = 0x01180070u, .yclip = 0x01c001a0u,
        .target = 0x00897000u,
        .variable_vertex_alpha = true,
        .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
        .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
        .left = 114u, .top = 417u, .width = 161u, .height = 30u,
        .source_stride = 0x2a0u, .source_control = 0x0e280000u,
        .quad = {
            0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
            0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0u, 0u, 0x3f700000u, 0x3de40000u,
            0x3edf8000u, 0u, 0u, 0u,
            0x3de40000u, 0x3ed08000u, 0u, 0x3f210000u,
            0x3f700000u, 0x3e898000u, 0x3edf8000u, 0u,
            0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
        },
    },
    {
        .xclip = 0x01180070u, .yclip = 0x01c001a0u,
        .target = 0x00a33000u,
        .variable_vertex_alpha = true,
        .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
        .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
        .left = 114u, .top = 417u, .width = 161u, .height = 30u,
        .source_stride = 0x2a0u, .source_control = 0x0e280000u,
        .quad = {
            0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
            0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0u, 0u, 0x3f700000u, 0x3de40000u,
            0x3edf8000u, 0u, 0u, 0u,
            0x3de40000u, 0x3ed08000u, 0u, 0x3f210000u,
            0x3f700000u, 0x3e898000u, 0x3edf8000u, 0u,
            0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
        },
    },
    {
        .xclip = 0x01180070u, .yclip = 0x01c001a0u,
        .target = 0x00998000u,
        .variable_vertex_alpha = true,
        .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
        .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
        .left = 114u, .top = 417u, .width = 161u, .height = 30u,
        .source_stride = 0x2a0u, .source_control = 0x0e280000u,
        .quad = {
            0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
            0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0u, 0u, 0x3f700000u, 0x3de40000u,
            0x3edf8000u, 0u, 0u, 0u,
            0x3de40000u, 0x3ed08000u, 0u, 0x3f210000u,
            0x3f700000u, 0x3e898000u, 0x3edf8000u, 0u,
            0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
        },
    },
};

static const struct mbx_3d_status_form *
mbx_3d_find_status_form(const s5l_mbx_t *m) {
    uint32_t xclip = m->reg[S5L_MBX_FBXCLIP / 4u];
    uint32_t yclip = m->reg[S5L_MBX_FBYCLIP / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];
    for (unsigned i = 0;
         i < sizeof mbx_3d_status_forms / sizeof mbx_3d_status_forms[0]; i++) {
        const struct mbx_3d_status_form *form = &mbx_3d_status_forms[i];
        if (form->xclip == xclip && form->yclip == yclip &&
            (!form->target || form->target == target))
            return form;
    }
    return NULL;
}

static uint32_t
mbx_3d_status_boundary_expected(const struct mbx_3d_status_form *form,
                                 uint32_t off) {
    if (off >= 0x0b8u && off <= 0x0d4u)
        return form->boundary_override
            ? form->boundary[(off - 0x0b8u) / 4u]
            : form->quad[8u + (off - 0x0b8u) / 4u];
    return mbx_3d_boundary_fixed_expected(off);
}

static bool mbx_execute_status_sprite(s5l_mbx_t *m,
                                      const arm_bus_t *bus,
                                      const char **why,
                                      uint32_t *pixels_blended) {
    uint32_t region = m->reg[S5L_MBX_RGNBASE / 4u];
    uint32_t object = m->reg[S5L_MBX_OBJBASE / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];

    if ((region & 3u) || (object & 3u) || (target & 3u) ||
        object > UINT32_MAX - 0x2a0u) {
        if (why) *why = "status-sprite region, object, or framebuffer base is invalid";
        return false;
    }
    const struct mbx_3d_status_form *form = mbx_3d_find_status_form(m);
    if (m->reg[S5L_MBX_3DPIXSAMP / 4u] != 0x00020007u ||
        m->reg[S5L_MBX_FBCTL / 4u] != 0x00000006u ||
        m->reg[S5L_MBX_FBLINESTRIDE / 4u] != MBX_3D_WIDTH || !form) {
        if (why) *why = "render registers are not a captured status-sprite form";
        return false;
    }

    uint32_t list = object + 0x68u;
    uint32_t tile_count = (form->tile_x1 - form->tile_x0 + 1u) *
                          (form->tile_y1 - form->tile_y0 + 1u);
    uint32_t tile_index = 0u;
    for (uint32_t y = form->tile_y0; y <= form->tile_y1; y++) {
        for (uint32_t x = form->tile_x0; x <= form->tile_x1; x++) {
            uint32_t code, pointer;
            if (!mbx_gart_u32(m, bus, region + tile_index * 8u,
                              &code, why) ||
                !mbx_gart_u32(m, bus, region + tile_index * 8u + 4u,
                              &pointer, why))
                return false;
            uint32_t expected = (y << 8) | x;
            if (tile_index + 1u == tile_count) expected |= 0x80000000u;
            if (code != expected || pointer != list) {
                if (why) *why = "status-sprite region list differs from its captured tiles";
                return false;
            }
            tile_index++;
        }
    }

    const uint32_t list_words[4] = {
        0x60200020u, 0x6020002du, 0x61a0007cu, 0xf0000000u
    };
    for (unsigned i = 0; i < 4u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, list + i * 4u, &value, why)) return false;
        if (value != list_words[i]) {
            if (why) *why = "status-sprite list is not the captured three-object list";
            return false;
        }
    }

    static const uint32_t background[26] = {
        0xe0000000u, 0xa7718000u, 0u, 0xd6887610u,
        0x22220e80u, 0u, 0u, 0x45000000u,
        0u, 0u, 0x45000000u, 0x3f800000u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0x3f800000u, 0u, 0u, 0u,
        0u, 0x40000000u, 0u, 0u,
        0u, 0x40000000u,
    };
    for (unsigned i = 0; i < 26u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + i * 4u, &value, why)) return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "status background does not resolve to FBSTART";
                return false;
            }
        } else if (value != background[i]) {
            if (why) *why = "status background object differs from the captured form";
            return false;
        }
    }

    for (uint32_t off = 0x80u; off < 0x0e8u; off += 4u) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + off, &value, why)) return false;
        if (value != mbx_3d_status_boundary_expected(form, off)) {
            if (why) *why = "status boundary object differs from the captured form";
            return false;
        }
    }

    uint32_t source = 0u;
    uint32_t vertex_alpha_word = 0u;
    bool vertex_alpha_seen = false;
    bool quad_matches = true;
    for (unsigned i = 0; i < 44u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + 0x1f0u + i * 4u,
                          &value, why))
            return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != form->source_control) {
                if (why) *why = "status source address word has an unknown format";
                return false;
            }
            source = mbx_3d_decode_address(value);
        } else if (i == 5u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "status blend surface differs from the captured form";
                return false;
            }
        } else if (i == 24u || i == 29u || i == 34u || i == 39u) {
            if (!vertex_alpha_seen) {
                vertex_alpha_word = value;
                vertex_alpha_seen = true;
            }
            if (form->variable_vertex_alpha) {
                if (value & 0x00ffffffu) {
                    if (why)
                        *why = "status vertex alpha has nonzero colour bits";
                    return false;
                }
                if (value != vertex_alpha_word) {
                    if (why)
                        *why = "status vertex alpha differs between vertices";
                    return false;
                }
            } else {
                quad_matches = quad_matches && value == form->quad[i];
            }
        } else {
            quad_matches = quad_matches && value == form->quad[i];
        }
    }
    if (!source) {
        if (why) *why = "status source resolves to GPU address zero";
        return false;
    }
    if (!vertex_alpha_seen) {
        if (why) *why = "status sprite has no vertex alpha";
        return false;
    }
    if (!quad_matches) {
        if (why) *why = "textured status sprite differs from its captured form";
        return false;
    }

    uint32_t row_bytes = form->width * 4u;
    uint32_t total = row_bytes * form->height;
    uint64_t source_end = (uint64_t)source +
                          (uint64_t)(form->source_row0 + form->height - 1u) *
                              form->source_stride + row_bytes;
    uint64_t target_end = (uint64_t)target +
                          (uint64_t)(form->top + form->height - 1u) *
                              MBX_3D_TARGET_STRIDE +
                           (uint64_t)(form->left + form->width) * 4u;
    if (row_bytes > form->source_stride || source_end > UINT32_MAX ||
        target_end > UINT32_MAX) {
        if (why) *why = "status source or destination rectangle overflows";
        return false;
    }
    for (uint32_t row = 0; row < form->height; row++) {
        uint32_t src = source +
                       (form->source_row0 + row) * form->source_stride;
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        uint32_t background_address = target +
                                      (form->top + row) * MBX_3D_TARGET_STRIDE +
                                      form->left * 4u;
        if (!mbx_gart_validate(m, bus, src, row_bytes, why) ||
            !mbx_gart_validate(m, bus, background_address, row_bytes, why) ||
            !mbx_gart_validate(m, bus, dst, row_bytes, why))
            return false;
    }

    uint8_t *source_pixels = malloc(total);
    uint8_t *pixels = malloc(total);
    if (!source_pixels || !pixels) {
        free(source_pixels);
        free(pixels);
        if (why) *why = "host allocation for staged status sprite failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < form->height && ok; row++) {
        uint32_t src = source +
                       (form->source_row0 + row) * form->source_stride;
        uint32_t background_address = target +
                                      (form->top + row) * MBX_3D_TARGET_STRIDE +
                                      form->left * 4u;
        ok = mbx_gart_read(m, bus, src, source_pixels + row * row_bytes,
                           row_bytes, why) &&
             mbx_gart_read(m, bus, background_address, pixels + row * row_bytes,
                           row_bytes, why);
    }
    for (uint32_t i = 0; i < total && ok; i += 4u) {
        uint32_t src = mbx_load_le32(source_pixels + i);
        uint32_t alpha = src >> 24;
        if ((src & 0xffu) > alpha || ((src >> 8) & 0xffu) > alpha ||
            ((src >> 16) & 0xffu) > alpha) {
            if (why) *why = "status source is not premultiplied BGRA8";
            ok = false;
            break;
        }
        src = mbx_modulate_vertex_alpha(src, vertex_alpha_word >> 24);
        mbx_store_le32(source_pixels + i, src);
    }
    if (ok && !mbx_try_metal_source_over(
            m, pixels, source_pixels, total / 4u)) {
        for (uint32_t i = 0; i < total; i += 4u) {
            uint32_t blended = mbx_source_over_clamped(
                mbx_load_le32(pixels + i),
                mbx_load_le32(source_pixels + i));
            mbx_store_le32(pixels + i, blended);
        }
    }
    for (uint32_t row = 0; row < form->height && ok; row++) {
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        ok = mbx_gart_write(m, bus, dst, pixels + row * row_bytes,
                            row_bytes, why);
    }
    free(source_pixels);
    free(pixels);
    if (ok && pixels_blended)
        *pixels_blended = form->width * form->height;
    return ok;
}

/* r409-r412 initially supplied four literal app-icon/label forms. Reading the
 * shipped _mbx3DCtxQuadCopyPerspective producer explains the shared packet
 * instead: it is a BGRA8 textured sprite whose independently encoded
 * destination, UV, texture-power, pitch, clip and tile bounds must agree.
 *
 * The texture header stores log2(power-of-two dimension)-3 in nibbles 6 and 5.
 * Linear pitch is split because the source address consumes bits 0..17 of its
 * word: pitch_bytes/16 bits 2..7 remain in source-control bits 18..23, while
 * bit 1 is relocated to texture-header bit 0. All measured linear textures
 * use an eight-pixel padded pitch, and that reconstruction yields every
 * captured 0x40..0x500 stride exactly. r415 adds the producer's second vertex
 * order: control 0x0e uses full width/power and height/power UV extents, while
 * control 0x8e uses the previously recovered half-texel extents. QuartzCore's
 * transform_filter_bits path proves that the latter is its filtered form, and
 * the co-shipped software sampler supplies the clamped 8-bit bilinear kernel.
 * r420 then contributes one exact uniformly minified 320x460 form and a third
 * measured state pair. Both vertex orders redundantly encode the same
 * destination and are checked in full. r434 and r438 add the filtered affine
 * subset: the direct sampler remains rigid 1:1, while the modulated sampler may
 * apply a positive uniform similarity transform. The captured samplers carry
 * one uniform alpha byte, which uses the already recovered channel-modulation
 * equation. The background object, blend object and FBSTART must all resolve to
 * the same mapped target, but its GPU address is not a rendering semantic and
 * is therefore not whitelisted. This is still not a perspective or arbitrary
 * affine rasterizer: shear, nonuniform affine scale, four-point warps and
 * coloured vertices remain rejected. r416's partly off-screen label is
 * filtered only when its normalized coordinates, integer boundary, clip and
 * tiles all independently agree. */
static bool mbx_3d_word_to_finite_float(uint32_t word, float *value) {
    if ((word & 0x7f800000u) == 0x7f800000u ||
        sizeof *value != sizeof word)
        return false;
    memcpy(value, &word, sizeof *value);
    return true;
}

static bool mbx_3d_word_to_nonnegative_float(uint32_t word, float *value) {
    return !(word & 0x80000000u) &&
           mbx_3d_word_to_finite_float(word, value);
}

static uint32_t mbx_3d_float_to_word(float value) {
    uint32_t word = 0u;
    memcpy(&word, &value, sizeof word);
    return word;
}

static int32_t mbx_3d_ceil_to_i32(float value) {
    int32_t integer = (int32_t)value;
    return integer + ((float)integer < value);
}

struct mbx_affine_transform {
    float origin_x, origin_y;
    float u_x, u_y;
    float v_x, v_y;
    float determinant;
};

static bool mbx_affine_pixel(const struct mbx_affine_transform *transform,
                             uint32_t x, uint32_t y,
                             float *u_fraction, float *v_fraction) {
    if (!transform || !u_fraction || !v_fraction ||
        transform->determinant <= 0.0f)
        return false;
    float dx = (float)x + 0.5f - transform->origin_x;
    float dy = (float)y + 0.5f - transform->origin_y;
    float u = (dx * transform->v_y - dy * transform->v_x) /
              transform->determinant;
    float v = (transform->u_x * dy - transform->u_y * dx) /
              transform->determinant;
    if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f)
        return false;
    *u_fraction = u;
    *v_fraction = v;
    return true;
}

/* r414 exposed why the object-list word is not merely another packet
 * discriminator.  Its low twenty bits are a word offset from OBJBASE:
 * 0x61a0007c references the textured object at +0x1f0, while 0x612000a8
 * references a different object at +0x2a0.  The latter is the five-record
 * form emitted by the shipped _mbx3DCtxQuadColor producer.  The apparently
 * related texture object at +0x1f0 is stale and must not be executed.
 *
 * This decoder follows that pointer, then requires the independently encoded
 * main geometry, normalized coordinates, boundary object, clip registers and
 * row-major tile list to agree.  Disassembly of the shipped _mbx3DQuadColor
 * wrapper and producer proves that its second argument is copied to the main
 * record before every normalized vertex pair.  Those four words must be one
 * uniform premultiplied A8R8G8B8 colour.  The four trailing parameter records
 * carry fixed all-one words and exact controls; they are not the quad colour.
 * Non-axis-aligned quads remain rejected. */
static bool mbx_execute_solid_quad(s5l_mbx_t *m,
                                   const arm_bus_t *bus,
                                   const char **why,
                                   uint32_t *pixels_filled) {
    uint32_t region = m->reg[S5L_MBX_RGNBASE / 4u];
    uint32_t object = m->reg[S5L_MBX_OBJBASE / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];
    if ((region & 3u) || (object & 3u) || (target & 3u) ||
        object > UINT32_MAX - 0x68u) {
        if (why) *why = "solid-quad region, object, or framebuffer base is invalid";
        return false;
    }
    if (m->reg[S5L_MBX_3DPIXSAMP / 4u] != 0x00020007u ||
        m->reg[S5L_MBX_FBCTL / 4u] != 0x00000006u ||
        m->reg[S5L_MBX_FBLINESTRIDE / 4u] != MBX_3D_WIDTH) {
        if (why) *why = "render registers are not the measured solid-quad family";
        return false;
    }

    uint32_t list = object + 0x68u;
    uint32_t list_words[4];
    for (unsigned i = 0; i < 4u; i++)
        if (!mbx_gart_u32(m, bus, list + i * 4u, &list_words[i], why))
            return false;
    bool solid_pointer =
        (list_words[2] & 0xfff00000u) == 0x61200000u;
    if (list_words[0] != 0x60200020u ||
        list_words[1] != 0x6020002du || !solid_pointer ||
        list_words[3] != 0xf0000000u) {
        /* Preserve the textured decoder's more relevant reason when this is
         * plainly its 0x61a pointer family. */
        if (why && solid_pointer)
            *why = "solid-quad list is not the measured pointer form";
        return false;
    }
    uint64_t solid64 = (uint64_t)object +
                       (uint64_t)(list_words[2] & 0x000fffffu) * 4u;
    if (solid64 < (uint64_t)object + 0x1f0u ||
        solid64 + 5u * 33u * 4u > (uint64_t)UINT32_MAX + 1u) {
        if (why) *why = "solid-quad object pointer is outside its safe span";
        return false;
    }
    uint32_t solid = (uint32_t)solid64;

    uint32_t main[33];
    for (unsigned i = 0; i < 33u; i++)
        if (!mbx_gart_u32(m, bus, solid + i * 4u, &main[i], why))
            return false;
    if (main[0] != 0xe0000000u || main[1] != 0xa7718000u ||
        main[3] != 0xa6104620u || main[4] != 0x22220e80u) {
        if (why) *why = "solid-quad main record has unknown controls";
        return false;
    }
    if ((main[2] & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
        mbx_3d_decode_address(main[2]) != target) {
        if (why) *why = "solid-quad main record does not resolve to FBSTART";
        return false;
    }
    for (unsigned i = 13u; i < 17u; i++) {
        if (main[i] != 0u) {
            if (why) *why = "solid-quad perspective terms are nonzero";
            return false;
        }
    }
    for (unsigned i = 17u; i < 21u; i++) {
        if (main[i] != 0x3f800000u) {
            if (why) *why = "solid-quad perspective divisors are not one";
            return false;
        }
    }
    if (main[5] != main[7] || main[5] == main[9] ||
        main[9] != main[11] || main[6] != main[10] ||
        main[6] == main[8] || main[8] != main[12]) {
        if (why) *why = "solid-quad destination is not an axis-aligned rectangle";
        return false;
    }

    float x0, y0, x1, y1;
    if (!mbx_3d_word_to_nonnegative_float(main[5], &x0) ||
        !mbx_3d_word_to_nonnegative_float(main[8], &y0) ||
        !mbx_3d_word_to_nonnegative_float(main[9], &x1) ||
        !mbx_3d_word_to_nonnegative_float(main[6], &y1) ||
        x0 >= x1 || y0 >= y1 || x1 > 320.0f || y1 > 480.0f) {
        if (why) *why = "solid-quad destination coordinates are invalid";
        return false;
    }

    static const unsigned x_words[4] = {5u, 7u, 9u, 11u};
    static const unsigned y_words[4] = {6u, 8u, 10u, 12u};
    uint32_t colour = main[21];
    uint32_t alpha = colour >> 24;
    if ((colour & 0xffu) > alpha || ((colour >> 8) & 0xffu) > alpha ||
        ((colour >> 16) & 0xffu) > alpha) {
        if (why) *why = "solid-quad colour is not premultiplied A8R8G8B8";
        return false;
    }
    for (unsigned vertex = 0; vertex < 4u; vertex++) {
        float x, y;
        unsigned attribute = 21u + vertex * 3u;
        if (!mbx_3d_word_to_nonnegative_float(main[x_words[vertex]], &x) ||
            !mbx_3d_word_to_nonnegative_float(main[y_words[vertex]], &y) ||
            main[attribute] != colour ||
            main[attribute + 1u] != mbx_3d_float_to_word(x / 1024.0f) ||
            main[attribute + 2u] != mbx_3d_float_to_word(y / 1024.0f)) {
            if (why) *why =
                "solid-quad colour or normalized coordinates differ between vertices";
            return false;
        }
    }

    const float lower_bias = 0.468505859375f; /* producer 0x3eefe000 */
    const float upper_bias = 0.531494140625f; /* producer 0x3f081000 */
    uint32_t left = (uint32_t)(x0 + lower_bias);
    uint32_t top = (uint32_t)(y0 + lower_bias);
    uint32_t right = (uint32_t)(x1 + upper_bias);
    uint32_t bottom = (uint32_t)(y1 + upper_bias);
    if (left >= right || top >= bottom || right > MBX_3D_WIDTH ||
        bottom > 480u) {
        if (why) *why = "solid-quad producer bounds leave the 320x480 surface";
        return false;
    }
    uint32_t clip_left = left & ~7u;
    uint32_t clip_right = (right + 7u) & ~7u;
    uint32_t clip_top = top & ~15u;
    uint32_t clip_bottom = (bottom + 15u) & ~15u;
    if (m->reg[S5L_MBX_FBXCLIP / 4u] !=
            ((clip_right << 16) | clip_left) ||
        m->reg[S5L_MBX_FBYCLIP / 4u] !=
            ((clip_bottom << 16) | clip_top)) {
        if (why) *why = "solid-quad clip registers disagree with producer geometry";
        return false;
    }

    uint32_t tile_x0 = clip_left / 8u;
    uint32_t tile_x1 = clip_right / 8u - 1u;
    uint32_t tile_y0 = clip_top / 16u;
    uint32_t tile_y1 = clip_bottom / 16u - 1u;
    uint32_t tile_count = (tile_x1 - tile_x0 + 1u) *
                          (tile_y1 - tile_y0 + 1u);
    if (!tile_count || tile_count > 40u * 30u ||
        (uint64_t)region + (uint64_t)tile_count * 8u > UINT32_MAX) {
        if (why) *why = "solid-quad tile rectangle is invalid";
        return false;
    }
    uint32_t tile_index = 0u;
    for (uint32_t y = tile_y0; y <= tile_y1; y++) {
        for (uint32_t x = tile_x0; x <= tile_x1; x++) {
            uint32_t code, pointer;
            if (!mbx_gart_u32(m, bus, region + tile_index * 8u,
                              &code, why) ||
                !mbx_gart_u32(m, bus, region + tile_index * 8u + 4u,
                              &pointer, why))
                return false;
            uint32_t expected = (y << 8) | x;
            if (tile_index + 1u == tile_count) expected |= 0x80000000u;
            if (code != expected || pointer != list) {
                if (why) *why = "solid-quad region list disagrees with its clip tiles";
                return false;
            }
            tile_index++;
        }
    }

    static const uint32_t background[26] = {
        0xe0000000u, 0xa7718000u, 0u, 0xd6887610u,
        0x22220e80u, 0u, 0u, 0x45000000u,
        0u, 0u, 0x45000000u, 0x3f800000u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0x3f800000u, 0u, 0u, 0u,
        0u, 0x40000000u, 0u, 0u,
        0u, 0x40000000u,
    };
    for (unsigned i = 0; i < 26u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + i * 4u, &value, why))
            return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "solid-quad background does not resolve to FBSTART";
                return false;
            }
        } else if (value != background[i]) {
            if (why) *why = "solid-quad background object is unknown";
            return false;
        }
    }

    for (uint32_t off = 0x80u; off < 0x0e8u; off += 4u) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + off, &value, why)) return false;
        uint32_t expected = mbx_3d_boundary_fixed_expected(off);
        if (off >= 0x0b8u && off <= 0x0d4u)
            expected = main[5u + (off - 0x0b8u) / 4u];
        if (value != expected) {
            if (why) *why = "solid-quad boundary object disagrees with its geometry";
            return false;
        }
    }

    static const uint32_t parameter_controls[4] = {
        0x22620ea0u, 0x46622ea0u, 0x66622ea0u, 0x82622ea0u
    };
    for (unsigned record = 0; record < 4u; record++) {
        uint32_t base = solid + (record + 1u) * 33u * 4u;
        for (unsigned i = 0; i < 33u; i++) {
            uint32_t value;
            if (!mbx_gart_u32(m, bus, base + i * 4u, &value, why))
                return false;
            uint32_t expected = 0u;
            if (i == 0u) expected = 0xe0000000u;
            else if (i == 3u) expected = 0x86084610u;
            else if (i == 4u) expected = parameter_controls[record];
            else if (i >= 17u && i <= 20u) expected = 0x3f800000u;
            else if (i >= 21u && (i - 21u) % 3u == 0u)
                expected = 0xffffffffu;
            if (value != expected) {
                if (why) *why = "solid-quad parameter records are inconsistent";
                return false;
            }
        }
    }

    uint32_t width = right - left;
    uint32_t height = bottom - top;
    uint32_t row_bytes = width * 4u;
    uint32_t total = row_bytes * height;
    uint64_t target_end = (uint64_t)target +
        (uint64_t)(bottom - 1u) * MBX_3D_TARGET_STRIDE +
        (uint64_t)right * 4u;
    if (target_end > UINT32_MAX) {
        if (why) *why = "solid-quad destination rectangle overflows";
        return false;
    }
    for (uint32_t row = 0; row < height; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        if (!mbx_gart_validate(m, bus, dst, row_bytes, why)) return false;
    }

    uint8_t *source_pixels = malloc(total);
    uint8_t *pixels = malloc(total);
    if (!source_pixels || !pixels) {
        free(source_pixels);
        free(pixels);
        if (why) *why = "host allocation for staged solid quad failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        ok = mbx_gart_read(m, bus, dst, pixels + row * row_bytes,
                           row_bytes, why);
    }
    for (uint32_t i = 0; i < total; i += 4u) {
        mbx_store_le32(source_pixels + i, colour);
    }
    if (ok && !mbx_try_metal_source_over(
            m, pixels, source_pixels, total / 4u)) {
        for (uint32_t i = 0; i < total; i += 4u) {
            uint32_t blended = mbx_source_over_clamped(
                mbx_load_le32(pixels + i), colour);
            mbx_store_le32(pixels + i, blended);
        }
    }
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        ok = mbx_gart_write(m, bus, dst, pixels + row * row_bytes,
                            row_bytes, why);
    }
    free(source_pixels);
    free(pixels);
    if (ok && pixels_filled) *pixels_filled = width * height;
    return ok;
}

/* MBX2D has two related textured producers. _mbx3DCtxQuadCopyPerspective
 * emits the measured 44-word source-over record selected by 0x61a. The
 * shipped _mbx3DCtxBlitCopy fast path emits a compact 33-word opaque-copy
 * record selected by 0x612: it omits the blend-surface state and the four
 * redundant normalized-destination pairs, but retains independently encoded
 * geometry, UVs, texture allocation, boundary, clip and tiles. Decode both
 * through one sampler so their filtering and guard rules cannot drift. */
static bool mbx_execute_textured_sprite(s5l_mbx_t *m,
                                        const arm_bus_t *bus,
                                        const char **why,
                                        uint32_t *pixels_blended) {
    uint32_t region = m->reg[S5L_MBX_RGNBASE / 4u];
    uint32_t object = m->reg[S5L_MBX_OBJBASE / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];
    if ((region & 3u) || (object & 3u) || (target & 3u) ||
        object > UINT32_MAX - 0x0e8u) {
        if (why) *why = "sprite region, object, or framebuffer base is invalid";
        return false;
    }
    if (m->reg[S5L_MBX_3DPIXSAMP / 4u] != 0x00020007u ||
        m->reg[S5L_MBX_FBCTL / 4u] != 0x00000006u ||
        m->reg[S5L_MBX_FBLINESTRIDE / 4u] != MBX_3D_WIDTH) {
        if (why) *why =
            "render registers are not the measured textured-sprite family";
        return false;
    }

    uint32_t list = object + 0x68u;
    uint32_t list_words[4];
    for (unsigned i = 0; i < 4u; i++)
        if (!mbx_gart_u32(m, bus, list + i * 4u, &list_words[i], why))
            return false;
    bool perspective_copy = list_words[2] == 0x61a0007cu;
    bool compact_copy =
        (list_words[2] & 0xfff00000u) == 0x61200000u;
    if (list_words[0] != 0x60200020u ||
        list_words[1] != 0x6020002du ||
        (!perspective_copy && !compact_copy) ||
        list_words[3] != 0xf0000000u) {
        if (why) *why = "sprite list is not a supported three-object pointer form";
        return false;
    }

    if (perspective_copy) {
        static const uint32_t background[26] = {
            0xe0000000u, 0xa7718000u, 0u, 0xd6887610u,
            0x22220e80u, 0u, 0u, 0x45000000u,
            0u, 0u, 0x45000000u, 0x3f800000u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0x3f800000u, 0u, 0u, 0u,
            0u, 0x40000000u, 0u, 0u,
            0u, 0x40000000u,
        };
        for (unsigned i = 0; i < 26u; i++) {
            uint32_t value;
            if (!mbx_gart_u32(m, bus, object + i * 4u, &value, why))
                return false;
            if (i == 2u) {
                if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                    mbx_3d_decode_address(value) != target) {
                    if (why) *why =
                        "sprite background does not resolve to FBSTART";
                    return false;
                }
            } else if (value != background[i]) {
                if (why) *why = "sprite background object is unknown";
                return false;
            }
        }
    }

    uint32_t quad[44] = {0};
    if (perspective_copy) {
        if (object > UINT32_MAX - 0x2a0u) {
            if (why) *why = "perspective sprite object span overflows";
            return false;
        }
        for (unsigned i = 0; i < 44u; i++)
            if (!mbx_gart_u32(m, bus, object + 0x1f0u + i * 4u,
                              &quad[i], why))
                return false;
    } else {
        uint64_t compact64 = (uint64_t)object +
            (uint64_t)(list_words[2] & 0x000fffffu) * 4u;
        if (compact64 < (uint64_t)object + 0x0e8u ||
            compact64 + 33u * 4u > (uint64_t)UINT32_MAX + 1u) {
            if (why) *why = "compact-copy object pointer is outside its safe span";
            return false;
        }
        uint32_t compact = (uint32_t)compact64;
        uint32_t record[33];
        for (unsigned i = 0; i < 33u; i++)
            if (!mbx_gart_u32(m, bus, compact + i * 4u,
                              &record[i], why))
                return false;
        for (unsigned i = 0; i < 4u; i++) quad[i] = record[i];
        quad[7] = record[4];
        for (unsigned i = 0; i < 16u; i++)
            quad[8u + i] = record[5u + i];
        for (unsigned vertex = 0; vertex < 4u; vertex++) {
            unsigned compact_attribute = 21u + vertex * 3u;
            unsigned quad_attribute = 24u + vertex * 5u;
            quad[quad_attribute] = record[compact_attribute];
            quad[quad_attribute + 1u] = record[compact_attribute + 1u];
            quad[quad_attribute + 2u] = record[compact_attribute + 2u];
        }
    }
    bool compact_modulated_sampler = compact_copy &&
        (quad[3] == 0xa6104620u || quad[3] == 0xa1104020u);
    bool direct_sampler = compact_copy
        ? quad[3] == 0xa6887610u
        : quad[3] == 0xa6884710u && quad[6] == 0xae504ea0u;
    bool modulated_sampler = compact_modulated_sampler ||
        (quad[3] == 0xcd206c40u && quad[6] == 0xae504ea0u);
    bool scaled_sampler = quad[3] == 0xd6887610u &&
                          quad[6] == 0xa3104620u;
    if (quad[0] != 0xe0000000u ||
        (!direct_sampler && !modulated_sampler && !scaled_sampler) ||
        (compact_copy
            ? quad[7] != 0x22220e80u
            : quad[4] != 0xa7718000u || quad[7] != 0x22250e80u)) {
        if (why) *why = "sprite quad setup is not a measured BGRA8 copy form";
        return false;
    }
    if (!compact_copy &&
        ((quad[5] & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
         mbx_3d_decode_address(quad[5]) != target)) {
        if (why) *why = "sprite blend surface differs from FBSTART";
        return false;
    }
    for (unsigned i = 16u; i < 20u; i++) {
        if (quad[i] != 0u) {
            if (why) *why = "sprite perspective terms are nonzero";
            return false;
        }
    }
    for (unsigned i = 20u; i < 24u; i++) {
        if (quad[i] != 0x3f800000u) {
            if (why) *why = "sprite perspective divisors are not one";
            return false;
        }
    }
    static const unsigned alpha_words[4] = {24u, 29u, 34u, 39u};
    uint32_t vertex_alpha_word = quad[alpha_words[0]];
    /* Opaque compact copies bypass vertex modulation in the commit loop. The
     * producer has emitted both all-zero unused colour words (Safari) and the
     * older all-opaque form; require one of those exact uniform encodings. */
    bool compact_unused_colour = compact_copy &&
        (vertex_alpha_word == 0u || vertex_alpha_word == 0xff000000u);
    if ((vertex_alpha_word & 0x00ffffffu) ||
        (direct_sampler && !compact_unused_colour &&
         vertex_alpha_word != 0xff000000u)) {
        if (mbx_trace_state == 1) {
            fprintf(stderr,
                    "MBX3D sprite vertex modulation=%08x,%08x,%08x,%08x "
                    "direct=%u compact=%u\n",
                    quad[alpha_words[0]], quad[alpha_words[1]],
                    quad[alpha_words[2]], quad[alpha_words[3]],
                    direct_sampler ? 1u : 0u, compact_copy ? 1u : 0u);
        }
        if (why) *why = "sprite vertex modulation is invalid for its sampler";
        return false;
    }
    for (unsigned i = 1u; i < 4u; i++) {
        if (quad[alpha_words[i]] != vertex_alpha_word) {
            if (why) *why = "sprite vertex modulation differs between vertices";
            return false;
        }
    }

    uint32_t source_control = quad[2] & ~MBX_3D_ADDRESS_MASK;
    bool half_texel_layout = (source_control & 0x80000000u) != 0u;
    /* Texture filtering and corner order are independent producer choices.
     * Direct packets retain row-major p00,p10,p01,p11 corners in both the
     * half-texel 0x8e form and Spotlight's measured full-extent 0x0e form.
     * The older modulated 0x0e producer uses the alternate ordering below.
     * No full-extent packet has established alternate-sampler semantics. */
    bool row_major_corners = half_texel_layout || direct_sampler;
    if (!half_texel_layout && scaled_sampler) {
        if (why) *why = "full-extent alternate sampler is unmeasured";
        return false;
    }
    const float epsilon = 0.0009765625f;
    float destination_x[4], destination_y[4];
    for (unsigned i = 0; i < 4u; i++) {
        if (!mbx_3d_word_to_finite_float(quad[8u + i * 2u],
                                         &destination_x[i]) ||
            !mbx_3d_word_to_finite_float(quad[9u + i * 2u],
                                         &destination_y[i])) {
            if (why) *why = "sprite destination coordinates are not finite";
            return false;
        }
    }
    bool axis_aligned = row_major_corners
        ? quad[8] == quad[12] && quad[10] == quad[14] &&
          quad[9] == quad[11] && quad[13] == quad[15]
        : quad[8] == quad[10] && quad[12] == quad[14] &&
          quad[9] == quad[13] && quad[11] == quad[15];
    unsigned p00 = row_major_corners ? 0u : 1u;
    unsigned p10 = row_major_corners ? 1u : 3u;
    unsigned p01 = row_major_corners ? 2u : 0u;
    unsigned p11 = row_major_corners ? 3u : 2u;
    float x0 = destination_x[p00], y0 = destination_y[p00];
    float x1 = destination_x[p11], y1 = destination_y[p11];
    struct mbx_affine_transform affine = {0};
    bool affine_sprite = !axis_aligned;
    if (compact_copy && affine_sprite) {
        if (why) *why = "compact blit-copy destination is not axis-aligned";
        return false;
    }
    if (axis_aligned) {
        if (x0 >= x1 || y0 >= y1) {
            if (why) *why = "sprite destination coordinates are invalid";
            return false;
        }
    } else {
        /* r434's first wiggle-mode render is a rigid affine instance of the
         * direct filtered producer. r438 contributes the modulated producer's
         * uniformly scaled affine form. Both have zero perspective and four
         * corners that close to a parallelogram. Keep unfiltered, alternate-
         * sampler, perspective, shear, and arbitrary four-point warps
         * rejected. */
        float closure_x = destination_x[p00] + destination_x[p11] -
                          destination_x[p10] - destination_x[p01];
        float closure_y = destination_y[p00] + destination_y[p11] -
                          destination_y[p10] - destination_y[p01];
        if (!half_texel_layout ||
            (!direct_sampler && !modulated_sampler) ||
            closure_x < -epsilon || closure_x > epsilon ||
            closure_y < -epsilon || closure_y > epsilon) {
            if (why) *why =
                "sprite destination is neither axis-aligned nor a measured affine quad";
            return false;
        }
        affine.origin_x = destination_x[p00];
        affine.origin_y = destination_y[p00];
        affine.u_x = destination_x[p10] - destination_x[p00];
        affine.u_y = destination_y[p10] - destination_y[p00];
        affine.v_x = destination_x[p01] - destination_x[p00];
        affine.v_y = destination_y[p01] - destination_y[p00];
        affine.determinant = affine.u_x * affine.v_y -
                             affine.u_y * affine.v_x;
        if (affine.determinant <= 0.0f) {
            if (why) *why = "affine sprite orientation is degenerate or reversed";
            return false;
        }
        x0 = x1 = destination_x[0];
        y0 = y1 = destination_y[0];
        for (unsigned i = 1u; i < 4u; i++) {
            if (destination_x[i] < x0) x0 = destination_x[i];
            if (destination_x[i] > x1) x1 = destination_x[i];
            if (destination_y[i] < y0) y0 = destination_y[i];
            if (destination_y[i] > y1) y1 = destination_y[i];
        }
        if (x0 >= x1 || y0 >= y1) {
            if (why) *why = "affine sprite has empty destination bounds";
            return false;
        }
    }

    if (x0 <= -(float)INT32_MAX || y0 <= -(float)INT32_MAX ||
        x1 >= (float)INT32_MAX || y1 >= (float)INT32_MAX) {
        if (why) *why = "sprite destination coordinates are invalid";
        return false;
    }

    static const unsigned destination_words[8] = {
        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u
    };
    static const unsigned normalized_words[8] = {
        27u, 28u, 32u, 33u, 37u, 38u, 42u, 43u
    };
    for (unsigned i = 0; !compact_copy && i < 8u; i++) {
        float coordinate;
        if (!mbx_3d_word_to_finite_float(
                quad[destination_words[i]], &coordinate) ||
            quad[normalized_words[i]] !=
                mbx_3d_float_to_word(coordinate / 1024.0f)) {
            if (why) *why = "sprite normalized destination disagrees with its quad";
            return false;
        }
    }

    /* These are the exact positive float literals loaded at 0x30e1d258/25c
     * by _mbx3DCtxQuadCopyPerspective before VCVT.U32.F32 and subsequent 8x16
     * tile alignment. The producer first intersects its float extrema with the
     * context bounds. The surface intersection below avoids relying on a C
     * conversion whose negative-input behaviour differs from ARM saturation;
     * the boundary object then supplies any stricter integer context scissor. */
    const float lower_bias = 0.468505859375f; /* 0x3eefe000 */
    const float upper_bias = 0.531494140625f; /* 0x3f081000 */
    float dx = x1 - x0;
    float dy = y1 - y0;
    /* UI transitions can emit a positive subpixel extent on one axis while
     * still covering one or more pixel centres. Empty and reversed geometry
     * was rejected above; the scale family, pixel-centre coverage, boundary,
     * clip, tile, source and target checks below remain authoritative. */
    if (dx > 512.0f + epsilon || dy > 512.0f + epsilon) {
        if (why) *why = "sprite transform exceeds the bounded textured family";
        return false;
    }

    /* Source dimensions are encoded independently of destination geometry.
     * Invert the producer's normalized UV extent first, then decode the
     * power-of-two allocation and split pitch independently.  This is what
     * distinguishes r420's 320x460 texture from its 28.8x41.4 destination. */
    uint32_t header_width_field = (quad[1] >> 24) & 7u;
    uint32_t header_height_field = (quad[1] >> 20) & 7u;
    uint32_t header_texture_width = 8u << header_width_field;
    uint32_t header_texture_height = 8u << header_height_field;
    uint32_t u0_word = quad[25];
    uint32_t v0_word = row_major_corners ? quad[26] : quad[31];
    uint32_t u1_word = row_major_corners ? quad[30] : quad[35];
    uint32_t v1_word = row_major_corners ? quad[36] : quad[26];
    bool uv_rectangle = row_major_corners
        ? quad[35] == u0_word && quad[31] == v0_word &&
          quad[40] == u1_word && quad[41] == v1_word
        : quad[30] == u0_word && quad[41] == v0_word &&
          quad[40] == u1_word && quad[36] == v1_word;
    float normalized_u0, normalized_v0, normalized_u1, normalized_v1;
    if (!uv_rectangle ||
        !mbx_3d_word_to_nonnegative_float(u0_word, &normalized_u0) ||
        !mbx_3d_word_to_nonnegative_float(v0_word, &normalized_v0) ||
        !mbx_3d_word_to_nonnegative_float(u1_word, &normalized_u1) ||
        !mbx_3d_word_to_nonnegative_float(v1_word, &normalized_v1)) {
        if (why) *why =
            "sprite UVs are not an axis-aligned finite rectangle";
        return false;
    }
    float u_texel_start =
        normalized_u0 * (float)header_texture_width;
    float v_texel_start =
        normalized_v0 * (float)header_texture_height;
    float u_texel_end =
        normalized_u1 * (float)header_texture_width;
    float v_texel_end =
        normalized_v1 * (float)header_texture_height;
    if (u_texel_start >= u_texel_end ||
        v_texel_start >= v_texel_end ||
        u_texel_end > 512.0f || v_texel_end > 512.0f ||
        u_texel_end > (float)header_texture_width ||
        v_texel_end > (float)header_texture_height) {
        if (why) *why = "sprite UV rectangle leaves the supported texture bounds";
        return false;
    }
    uint32_t source_left = (uint32_t)u_texel_start;
    uint32_t source_top = (uint32_t)v_texel_start;
    uint32_t source_right = (uint32_t)u_texel_end;
    uint32_t source_bottom = (uint32_t)v_texel_end;
    source_right += (float)source_right < u_texel_end;
    source_bottom += (float)source_bottom < v_texel_end;
    uint32_t source_width = source_right - source_left;
    uint32_t source_height = source_bottom - source_top;
    float u_texel_span = u_texel_end - u_texel_start;
    float v_texel_span = v_texel_end - v_texel_start;
    bool compact_full_extent_uniform_minification = false;

    /* The texture allocation, UV rectangle and linear row pitch are independent
     * producer inputs.  _mbx3DCtxQuadCopyPerspective derives the header power
     * from its surface-width argument at 0x30e1ced4..0x30e1cf18, while the row
     * bytes arrive independently through context+0x14.  Retained Safari unlock
     * packets place 8- or 16-pixel allocations inside 72- or 320-pixel rows.
     * Requiring header width to equal the smallest power covering the pitch
     * therefore rejects valid padded-row sub-rectangles.  Decode both fields
     * independently, while requiring the sampled UV footprint to fit both.
     *
     * BGRA8 rows are eight-pixel aligned, so pitch_bytes/16 is even.  Bits
     * 2..7 stay in source-control bits 18..23 and bit 1 moves to header bit 0.
     * Bit 0 is consequently zero rather than an omitted unknown. */
    uint32_t pitch_units = ((source_control >> 16) & 0xfcu) |
                           ((quad[1] & 1u) << 1);
    uint32_t source_stride = pitch_units * 16u;
    uint32_t source_pitch_pixels = source_stride / 4u;
    if (!pitch_units || source_pitch_pixels > 512u ||
        header_texture_width > 1024u || header_texture_height > 1024u ||
        source_right > source_pitch_pixels ||
        source_bottom > header_texture_height) {
        if (why) *why =
            "sprite UV rectangle exceeds its encoded texture allocation";
        return false;
    }
    uint32_t expected_header = 0xa0018000u |
        (header_width_field << 24) | (header_height_field << 20) |
        ((pitch_units & 2u) >> 1);
    uint32_t expected_source_control =
        (half_texel_layout ? 0x8e000000u : 0x0e000000u) |
        ((pitch_units & ~3u) << 16);
    if (quad[1] != expected_header ||
        (quad[2] & ~MBX_3D_ADDRESS_MASK) != expected_source_control) {
        if (why) *why =
            "sprite texture allocation or split pitch is inconsistent";
        return false;
    }

    if (affine_sprite) {
        float expected_u2 = (float)source_width * (float)source_width;
        float expected_v2 = (float)source_height * (float)source_height;
        float expected_det = (float)source_width * (float)source_height;
        float u2 = affine.u_x * affine.u_x + affine.u_y * affine.u_y;
        float v2 = affine.v_x * affine.v_x + affine.v_y * affine.v_y;
        float dot = affine.u_x * affine.v_x + affine.u_y * affine.v_y;
        if (dot < 0.0f) dot = -dot;
        if (source_width > MBX_3D_WIDTH || source_height > 480u) {
            if (why) *why = "affine sprite source is outside measured bounds";
            return false;
        }
        if (direct_sampler) {
            float u_error = u2 - expected_u2;
            float v_error = v2 - expected_v2;
            float det_error = affine.determinant - expected_det;
            float tolerance =
                ((float)source_width + (float)source_height) * epsilon;
            if (u_error < 0.0f) u_error = -u_error;
            if (v_error < 0.0f) v_error = -v_error;
            if (det_error < 0.0f) det_error = -det_error;
            if (u_error > tolerance || v_error > tolerance ||
                dot > tolerance || det_error > tolerance) {
                if (why) *why =
                    "direct affine sprite is not the measured rigid unity transform";
                return false;
            }
        } else {
            float scale2 = u2 / expected_u2;
            float v_error = v2 - expected_v2 * scale2;
            float det_error = affine.determinant - expected_det * scale2;
            float tolerance =
                ((float)source_width + (float)source_height) * epsilon *
                (scale2 > 1.0f ? scale2 : 1.0f);
            if (v_error < 0.0f) v_error = -v_error;
            if (det_error < 0.0f) det_error = -det_error;
            if (scale2 <= 0.0f || v_error > tolerance ||
                dot > tolerance || det_error > tolerance) {
                if (why) *why =
                    "modulated affine sprite is not a measured uniform similarity transform";
                return false;
            }
        }
    } else {
        bool unity_transform =
            dx >= (float)source_width - epsilon &&
            dx <= (float)source_width + epsilon &&
            dy >= (float)source_height - epsilon &&
            dy <= (float)source_height + epsilon;
        if (!unity_transform) {
            float scale_x = dx / (float)source_width;
            float scale_y = dy / (float)source_height;
            float scale_difference = scale_x > scale_y
                ? scale_x - scale_y : scale_y - scale_x;
            /* The device unlock capture is a direct-filtered 67x20 source at
             * 0.724135x in both axes. Rejecting every direct minification left
             * 3DIdle false and sent AppleMBX's watchdog into an endless
             * Graphics Recovery Event loop. Preserve the previously measured
             * nonuniform magnification family, but admit minification only
             * when both axes carry the captured uniform-scale invariant. */
            bool direct_magnification = direct_sampler && half_texel_layout &&
                scale_x >= 1.0f - epsilon && scale_y >= 1.0f - epsilon;
            bool direct_uniform_minification =
                direct_sampler && half_texel_layout &&
                scale_x > 0.0f && scale_y > 0.0f &&
                scale_x <= 1.0f + epsilon && scale_y <= 1.0f + epsilon &&
                scale_difference <= 0.00001f;
            /* Safari's keyboard/address transition contributes a direct-
             * filtered row operation with a wide source rectangle. It keeps
             * the conservative source height exactly 1:1 while reducing only
             * X. Treat that one-dimensional resample as its own family: this
             * does not admit vertical minification, mixed two-axis scaling,
             * or an unchecked generic textured quad. */
            bool direct_horizontal_minification =
                direct_sampler && half_texel_layout && source_width > 2u &&
                scale_x > 0.0f && scale_x <= 1.0f + epsilon &&
                scale_y >= 1.0f - epsilon && scale_y <= 1.0f + epsilon;
            /* The compact blit producer used while unlocking back into
             * Safari carries the same direct filtered sampler as the older
             * compact records, but selects the 0x0e texture-coordinate layout.
             * Its independently encoded UV rectangle starts on integer texels
             * and ends exactly one half texel inside both conservative source
             * bounds.  Two retained phases uniformly reduce the same 320x356
             * source to different subpixel rectangles.  Keep this distinct
             * from the genuinely unfiltered 0x0e perspective producer: both
             * axes must be strict, positive, uniform minification and neither
             * source axis may collapse into the narrow-strip family. */
            bool compact_half_texel_envelope =
                compact_copy && direct_sampler && !half_texel_layout &&
                source_width > 2u && source_height > 2u &&
                u_texel_start == (float)source_left &&
                v_texel_start == (float)source_top &&
                u_texel_end == (float)source_right - 0.5f &&
                v_texel_end == (float)source_bottom - 0.5f;
            compact_full_extent_uniform_minification =
                compact_half_texel_envelope &&
                scale_x > 0.0f && scale_y > 0.0f &&
                scale_x < 1.0f - epsilon &&
                scale_y < 1.0f - epsilon &&
                scale_difference <= 0.00001f;
            /* Retained direct, alternate and modulated producer packets all
             * use a one-texel-or-narrower horizontal UV strip. They magnify
             * that strip in X while retaining or reducing its rows in Y. The
             * Spotlight return packet's normalized float round-trip lands
             * 0.00000191 texels above one, so its conservative floor/ceil
             * envelope spans two columns. Classify the sampled UV span, while
             * bounding that envelope to two columns; genuinely wider strips
             * and vertical magnification still require another measured
             * family. */
            bool filtered_narrow_strip_resample =
                (direct_sampler || modulated_sampler || scaled_sampler) &&
                half_texel_layout && source_width <= 2u &&
                u_texel_span <= 1.0f + epsilon &&
                scale_x >= 1.0f - epsilon &&
                scale_y > 0.0f && scale_y <= 1.0f + epsilon;
            bool modulated_uniform_scale =
                modulated_sampler && half_texel_layout &&
                scale_x > 0.0f && scale_y > 0.0f &&
                scale_difference <= 0.00001f;
            bool alternate_uniform_minification =
                scaled_sampler && half_texel_layout &&
                scale_x > 0.0f && scale_y > 0.0f &&
                scale_x <= 1.0f + epsilon && scale_y <= 1.0f + epsilon &&
                scale_difference <= 0.00001f;
            if (source_width > MBX_3D_WIDTH || source_height > 480u ||
                (!direct_magnification && !direct_uniform_minification &&
                 !direct_horizontal_minification &&
                 !compact_full_extent_uniform_minification &&
                 !filtered_narrow_strip_resample &&
                 !modulated_uniform_scale &&
                 !alternate_uniform_minification)) {
                if (mbx_trace_state == 1) {
                    fprintf(stderr,
                            "MBX3D transform reject: sampler=%s half=%u "
                            "source=%ux%u uv-span=%.9gx%.9g "
                            "destination=%.9gx%.9g scale=%.9gx%.9g "
                            "difference=%.9g quad=%08x/%08x/%08x/%08x/%08x\n",
                            direct_sampler ? "direct" :
                            (modulated_sampler ? "modulated" : "scaled"),
                            half_texel_layout ? 1u : 0u,
                            source_width, source_height,
                            (double)u_texel_span, (double)v_texel_span,
                            (double)dx, (double)dy,
                            (double)scale_x, (double)scale_y,
                            (double)scale_difference,
                            quad[1], quad[2], quad[3], quad[6], quad[7]);
                }
                if (why) *why =
                    "filtered transform is outside its measured sampler scale family";
                return false;
            }
        } else if (scaled_sampler) {
            if (why) *why =
                "alternate filtered state lacks its measured minification";
            return false;
        }
    }
    bool filtered_sampling = half_texel_layout ||
                             compact_full_extent_uniform_minification;
    float bounded_x0 = x0 < 0.0f ? 0.0f : x0;
    float bounded_y0 = y0 < 0.0f ? 0.0f : y0;
    float bounded_x1 = x1 > (float)MBX_3D_WIDTH
        ? (float)MBX_3D_WIDTH : x1;
    float bounded_y1 = y1 > 480.0f ? 480.0f : y1;
    if (bounded_x0 >= bounded_x1 || bounded_y0 >= bounded_y1) {
        if (why) *why = "sprite does not intersect the 320x480 surface";
        return false;
    }

    uint32_t natural_left = (uint32_t)bounded_x0;
    uint32_t natural_top = (uint32_t)bounded_y0;
    uint32_t natural_right = (uint32_t)bounded_x1 +
        ((float)(uint32_t)bounded_x1 != bounded_x1);
    uint32_t natural_bottom = (uint32_t)bounded_y1 +
        ((float)(uint32_t)bounded_y1 != bounded_y1);
    uint32_t boundary[8] = {0};
    for (uint32_t off = 0x80u; off < 0x0e8u; off += 4u) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + off, &value, why)) return false;
        if (off >= 0x0b8u && off <= 0x0d4u) {
            float coordinate;
            if (!mbx_3d_word_to_nonnegative_float(value, &coordinate) ||
                coordinate > 480.0f ||
                coordinate != (float)(uint32_t)coordinate) {
                if (why) *why =
                    "sprite boundary object is not an integer clipped quad";
                return false;
            }
            boundary[(off - 0x0b8u) / 4u] = (uint32_t)coordinate;
        } else if (value != mbx_3d_boundary_fixed_expected(off)) {
            if (why) *why = "sprite boundary object setup is unknown";
            return false;
        }
    }
    uint32_t boundary_left = boundary[0];
    uint32_t boundary_bottom = boundary[1];
    uint32_t boundary_top = boundary[3];
    uint32_t boundary_right = boundary[4];
    if (boundary[2] != boundary_left || boundary[5] != boundary_bottom ||
        boundary[6] != boundary_right || boundary[7] != boundary_top ||
        boundary_left >= boundary_right || boundary_top >= boundary_bottom ||
        boundary_right > MBX_3D_WIDTH || boundary_bottom > 480u ||
        boundary_left + 1u < natural_left ||
        boundary_top + 1u < natural_top ||
        boundary_right > natural_right + 1u ||
        boundary_bottom > natural_bottom + 1u) {
        if (why) *why =
            "sprite boundary object is not a clipped subset of its quad";
        return false;
    }

    /* An inward edge is the integer context scissor measured in r418. Natural
     * floor/ceil edges retain the original float extrema so the producer's
     * asymmetric guard rounding remains independently checkable. */
    float producer_x0 = boundary_left > natural_left
        ? (float)boundary_left : bounded_x0;
    float producer_y0 = boundary_top > natural_top
        ? (float)boundary_top : bounded_y0;
    float producer_x1 = boundary_right < natural_right
        ? (float)boundary_right : bounded_x1;
    float producer_y1 = boundary_bottom < natural_bottom
        ? (float)boundary_bottom : bounded_y1;
    uint32_t guard_left = (uint32_t)(producer_x0 + lower_bias);
    uint32_t guard_top = (uint32_t)(producer_y0 + lower_bias);
    uint32_t guard_right = (uint32_t)(producer_x1 + upper_bias);
    uint32_t guard_bottom = (uint32_t)(producer_y1 + upper_bias);

    /* Standard pixel-centre coverage can be empty even when the producer's
     * conservative, tile-aligned region is non-empty.  Work it out before
     * rejecting a collapsed integer guard: the fully validated command must
     * still complete so AppleMBX can return to 3DIdle. */
    int32_t raster_left_unclipped = mbx_3d_ceil_to_i32(x0 - 0.5f);
    int32_t raster_top_unclipped = mbx_3d_ceil_to_i32(y0 - 0.5f);
    int32_t raster_right_unclipped = mbx_3d_ceil_to_i32(x1 - 0.5f);
    int32_t raster_bottom_unclipped = mbx_3d_ceil_to_i32(y1 - 0.5f);
    int32_t raster_left = raster_left_unclipped < 0
        ? 0 : raster_left_unclipped;
    int32_t raster_top = raster_top_unclipped < 0
        ? 0 : raster_top_unclipped;
    int32_t raster_right = raster_right_unclipped > (int32_t)MBX_3D_WIDTH
        ? (int32_t)MBX_3D_WIDTH : raster_right_unclipped;
    int32_t raster_bottom = raster_bottom_unclipped > 480
        ? 480 : raster_bottom_unclipped;
    if (raster_left < (int32_t)boundary_left)
        raster_left = (int32_t)boundary_left;
    if (raster_top < (int32_t)boundary_top)
        raster_top = (int32_t)boundary_top;
    if (raster_right > (int32_t)boundary_right)
        raster_right = (int32_t)boundary_right;
    if (raster_bottom > (int32_t)boundary_bottom)
        raster_bottom = (int32_t)boundary_bottom;
    bool zero_coverage =
        raster_left >= raster_right || raster_top >= raster_bottom;
    uint32_t left = (uint32_t)raster_left;
    uint32_t top = (uint32_t)raster_top;
    uint32_t right = (uint32_t)raster_right;
    uint32_t bottom = (uint32_t)raster_bottom;
    uint32_t width = zero_coverage ? 0u : right - left;
    uint32_t height = zero_coverage ? 0u : bottom - top;

    bool collapsed_guard =
        guard_left >= guard_right || guard_top >= guard_bottom;
    if ((collapsed_guard && !zero_coverage) ||
        guard_right > MBX_3D_WIDTH || guard_bottom > 480u) {
        if (why) *why = "sprite producer bounds leave the 320x480 surface";
        return false;
    }

    uint32_t source_x0 = 0u, source_y0 = 0u;
    if (!filtered_sampling && !zero_coverage) {
        /* The remaining 0x0e producer order is unfiltered.  Its integer-sized
         * unity transform must still select one strict contiguous source crop.
         * The crop does not have to begin at texture origin: r430 copies
         * source rows 20..479 to destination rows 20..479 from a 320x480
         * surface inside a 512x512 allocation.  Require integer UV edges so
         * this remains a direct texel copy rather than inventing nearest-
         * neighbour semantics for fractional unfiltered coordinates. */
        int32_t full_raster_width =
            raster_right_unclipped - raster_left_unclipped;
        int32_t full_raster_height =
            raster_bottom_unclipped - raster_top_unclipped;
        bool integer_source_rectangle =
            u_texel_start == (float)source_left &&
            v_texel_start == (float)source_top &&
            u_texel_end == (float)source_right &&
            v_texel_end == (float)source_bottom;
        if (!integer_source_rectangle ||
            full_raster_width != (int32_t)source_width ||
            full_raster_height != (int32_t)source_height ||
            raster_left < raster_left_unclipped ||
            raster_top < raster_top_unclipped ||
            raster_right > raster_right_unclipped ||
            raster_bottom > raster_bottom_unclipped) {
            if (why) *why =
                "unfiltered sprite coverage is not a contiguous 1:1 crop";
            return false;
        }
        source_x0 = source_left +
                    (uint32_t)(raster_left - raster_left_unclipped);
        source_y0 = source_top +
                    (uint32_t)(raster_top - raster_top_unclipped);
        if (source_x0 + width > source_right ||
            source_y0 + height > source_bottom) {
            if (why) *why = "unfiltered sprite crop exceeds its source";
            return false;
        }
    }

    uint32_t source = mbx_3d_decode_address(quad[2]);
    if (!source) {
        if (why) *why = "sprite source resolves to GPU address zero";
        return false;
    }

    uint32_t clip_left = guard_left & ~7u;
    uint32_t clip_right = (guard_right + 7u) & ~7u;
    uint32_t clip_top = guard_top & ~15u;
    uint32_t clip_bottom = (guard_bottom + 15u) & ~15u;
    if (clip_left >= clip_right || clip_top >= clip_bottom ||
        clip_right > MBX_3D_WIDTH || clip_bottom > 480u) {
        if (why) {
            *why = "sprite aligned clip rectangle is empty or out of bounds";
        }
        return false;
    }
    uint32_t expected_xclip = (clip_right << 16) | clip_left;
    uint32_t expected_yclip = (clip_bottom << 16) | clip_top;
    if (m->reg[S5L_MBX_FBXCLIP / 4u] != expected_xclip ||
        m->reg[S5L_MBX_FBYCLIP / 4u] != expected_yclip) {
        if (why) *why = "sprite clip registers disagree with producer geometry";
        return false;
    }

    uint32_t tile_x0 = clip_left / 8u;
    uint32_t tile_x1 = clip_right / 8u - 1u;
    uint32_t tile_y0 = clip_top / 16u;
    uint32_t tile_y1 = clip_bottom / 16u - 1u;
    uint32_t tile_count = (tile_x1 - tile_x0 + 1u) *
                          (tile_y1 - tile_y0 + 1u);
    if (!tile_count || tile_count > 40u * 30u ||
        (uint64_t)region + (uint64_t)tile_count * 8u > UINT32_MAX) {
        if (why) *why = "sprite tile rectangle is invalid";
        return false;
    }
    uint32_t tile_index = 0u;
    for (uint32_t y = tile_y0; y <= tile_y1; y++) {
        for (uint32_t x = tile_x0; x <= tile_x1; x++) {
            uint32_t code, pointer;
            if (!mbx_gart_u32(m, bus, region + tile_index * 8u,
                              &code, why) ||
                !mbx_gart_u32(m, bus, region + tile_index * 8u + 4u,
                              &pointer, why))
                return false;
            uint32_t expected = (y << 8) | x;
            if (tile_index + 1u == tile_count) expected |= 0x80000000u;
            if (code != expected || pointer != list) {
                if (why) *why = "sprite region list disagrees with its clip tiles";
                return false;
            }
            tile_index++;
        }
    }

    /* A conservative producer boundary can survive clipping even when the
     * subpixel quad covers no pixel centre.  That is a valid no-op draw, not
     * a rejected submission: the guest still waits for its completion event.
     * Keep this lifecycle rule independent of any captured address or packet,
     * but do not let it hide malformed state.  The texture allocation,
     * render-target boundary, clip registers and complete tile list must all
     * be valid before the command completes without reading or writing a
     * pixel. */
    if (zero_coverage) {
        uint64_t source_end = (uint64_t)source +
            (uint64_t)header_texture_height * source_stride;
        if (source_end > UINT32_MAX) {
            if (why) *why = "zero-coverage sprite source allocation overflows";
            return false;
        }
        for (uint32_t row = 0; row < header_texture_height; row++) {
            uint32_t src = source + row * source_stride;
            if (!mbx_gart_validate(m, bus, src, source_stride, why))
                return false;
        }

        uint32_t target_row_bytes =
            (boundary_right - boundary_left) * 4u;
        uint64_t target_end = (uint64_t)target +
            (uint64_t)(boundary_bottom - 1u) * MBX_3D_TARGET_STRIDE +
            (uint64_t)boundary_right * 4u;
        if (target_end > UINT32_MAX) {
            if (why) *why = "zero-coverage sprite target boundary overflows";
            return false;
        }
        for (uint32_t row = boundary_top; row < boundary_bottom; row++) {
            uint32_t dst = target + row * MBX_3D_TARGET_STRIDE +
                           boundary_left * 4u;
            if (!mbx_gart_validate(m, bus, dst, target_row_bytes, why))
                return false;
        }
        *pixels_blended = 0u;
        return true;
    }

    uint32_t row_bytes = width * 4u;
    uint32_t total = row_bytes * height;
    uint64_t target_end = (uint64_t)target +
        (uint64_t)(top + height - 1u) * MBX_3D_TARGET_STRIDE +
        (uint64_t)(left + width) * 4u;
    if (target_end > UINT32_MAX) {
        if (why) *why = "sprite destination rectangle overflows";
        return false;
    }
    for (uint32_t row = 0; row < height; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        if (!mbx_gart_validate(m, bus, dst, row_bytes, why))
            return false;
    }

    struct mbx_bilinear_axis x_axis[MBX_3D_WIDTH];
    struct mbx_bilinear_axis y_axis[480u];
    uint32_t source_stage_x0 = source_x0;
    uint32_t source_stage_y0 = source_y0;
    uint32_t source_stage_width = width;
    uint32_t source_stage_height = height;
    if (filtered_sampling) {
        uint32_t minimum_x = UINT32_MAX, minimum_y = UINT32_MAX;
        uint32_t maximum_x = 0u, maximum_y = 0u;
        if (!affine_sprite) {
            for (uint32_t x = 0; x < width; x++) {
                if (!mbx_bilinear_axis(x0, dx, u_texel_start, u_texel_span,
                                       left + x, source_pitch_pixels,
                                       &x_axis[x])) {
                    if (why) *why =
                        "filtered sprite has an invalid horizontal sample";
                    return false;
                }
                if (x_axis[x].first < minimum_x) minimum_x = x_axis[x].first;
                if (x_axis[x].second > maximum_x) maximum_x = x_axis[x].second;
            }
            for (uint32_t y = 0; y < height; y++) {
                if (!mbx_bilinear_axis(y0, dy, v_texel_start, v_texel_span,
                                       top + y, header_texture_height,
                                       &y_axis[y])) {
                    if (why) *why =
                        "filtered sprite has an invalid vertical sample";
                    return false;
                }
                if (y_axis[y].first < minimum_y) minimum_y = y_axis[y].first;
                if (y_axis[y].second > maximum_y) maximum_y = y_axis[y].second;
            }
        } else {
            /* The inverse transform already proves every covered fragment has
             * u/v in [0, 1).  A bilinear tap can extend at most one texel past
             * the UV rectangle's floor/ceil envelope, so stage that bounded
             * superset directly.  The old path traversed the whole destination
             * once to discover this window and then repeated the same affine
             * divisions and coordinate conversion while rendering.  Staging
             * a conservative rectangle removes that entire discovery pass
             * without caching or changing any rendered sample calculation. */
            source_stage_x0 = source_left ? source_left - 1u : 0u;
            source_stage_y0 = source_top ? source_top - 1u : 0u;
            uint32_t source_stage_right = source_right < source_pitch_pixels
                ? source_right + 1u : source_pitch_pixels;
            uint32_t source_stage_bottom =
                source_bottom < header_texture_height
                    ? source_bottom + 1u : header_texture_height;
            source_stage_width = source_stage_right - source_stage_x0;
            source_stage_height = source_stage_bottom - source_stage_y0;
        }
        if (!affine_sprite) {
            source_stage_x0 = minimum_x;
            source_stage_y0 = minimum_y;
            source_stage_width = maximum_x - minimum_x + 1u;
            source_stage_height = maximum_y - minimum_y + 1u;
        }
    }

    uint32_t source_row_bytes = source_stage_width * 4u;
    uint32_t source_total = source_row_bytes * source_stage_height;
    uint64_t source_end = (uint64_t)source +
        (uint64_t)(source_stage_y0 + source_stage_height - 1u) * source_stride +
        (uint64_t)(source_stage_x0 + source_stage_width) * 4u;
    if (source_row_bytes > source_stride || source_end > UINT32_MAX) {
        if (why) *why = "sprite source sample window overflows";
        return false;
    }
    for (uint32_t row = 0; row < source_stage_height; row++) {
        uint32_t src = source + (source_stage_y0 + row) * source_stride +
                       source_stage_x0 * 4u;
        if (!mbx_gart_validate(m, bus, src, source_row_bytes, why)) return false;
    }

    uint8_t *source_pixels = malloc(source_total);
    uint8_t *pixels = malloc(total);
    if (!source_pixels || !pixels) {
        free(source_pixels);
        free(pixels);
        if (why) *why = "host allocation for staged sprite failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < source_stage_height && ok; row++) {
        uint32_t src = source + (source_stage_y0 + row) * source_stride +
                       source_stage_x0 * 4u;
        ok = mbx_gart_read(m, bus, src,
                           source_pixels + row * source_row_bytes,
                           source_row_bytes, why);
    }
    /* The compact producer is an opaque copy and the validated axis-aligned
     * raster loop below writes every byte in `pixels` before the commit.  Its
     * previous destination read therefore moved an entire rectangle across
     * the GART only to overwrite it.  Blended perspective sprites still need
     * the original destination, and keeping the shared output buffer preserves
     * the all-validation-before-first-write transaction boundary. */
    bool opaque_compact_copy = compact_copy &&
                               !compact_modulated_sampler;
    for (uint32_t row = 0; !opaque_compact_copy && row < height && ok; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        ok = mbx_gart_read(m, bus, dst, pixels + row * row_bytes,
                           row_bytes, why);
    }
    /* Axis-aligned filtered sprites reuse the same vertical interpolation for
     * every destination x that names a given source column.  The literal
     * transcription used to recompute both vertical lanes for every output
     * pixel and only then apply the horizontal filter.  Cache that exact
     * vertical result once per source column when the staged source is no
     * wider than the output.  Minification can leave large gaps between taps,
     * so it keeps the literal path rather than scanning unused source columns.
     * The order *inside each sample* remains vertical then horizontal, which
     * preserves the shipped packed-byte rounding exactly. */
    uint32_t vertical_pixels[MBX_3D_WIDTH + 2u];
    bool cache_vertical = filtered_sampling && !affine_sprite &&
                          source_stage_width <= width + 2u;
    uint32_t affine_rendered_pixels = 0u;
    for (uint32_t y = 0; y < height && ok; y++) {
        if (cache_vertical) {
            const struct mbx_bilinear_axis *sample_y = &y_axis[y];
            uint32_t y0_offset = sample_y->first - source_stage_y0;
            uint32_t y1_offset = sample_y->second - source_stage_y0;
            const uint8_t *top_row = source_pixels +
                                     y0_offset * source_row_bytes;
            const uint8_t *bottom_row = source_pixels +
                                        y1_offset * source_row_bytes;
            for (uint32_t sx = 0; sx < source_stage_width; sx++) {
                uint32_t top_pixel = mbx_load_le32(top_row + sx * 4u);
                uint32_t bottom_pixel = mbx_load_le32(bottom_row + sx * 4u);
                vertical_pixels[sx] = top_pixel == bottom_pixel
                    ? top_pixel
                    : mbx_linear_bgra8(top_pixel, bottom_pixel,
                                       sample_y->weight);
            }
        }
        for (uint32_t x = 0; x < width; x++) {
            uint32_t src;
            if (filtered_sampling) {
                struct mbx_bilinear_axis affine_x, affine_y;
                const struct mbx_bilinear_axis *sample_x = &x_axis[x];
                const struct mbx_bilinear_axis *sample_y = &y_axis[y];
                if (affine_sprite) {
                    float u_fraction, v_fraction;
                    if (!mbx_affine_pixel(&affine, left + x, top + y,
                                          &u_fraction, &v_fraction))
                        continue;
                    float u_coordinate =
                        u_texel_start + u_fraction * u_texel_span;
                    float v_coordinate =
                        v_texel_start + v_fraction * v_texel_span;
                    if (!mbx_bilinear_coordinate(
                            u_coordinate, source_pitch_pixels, &affine_x) ||
                        !mbx_bilinear_coordinate(
                            v_coordinate, header_texture_height, &affine_y)) {
                        if (why) *why =
                            "affine sprite sample changed during staging";
                        ok = false;
                        break;
                    }
                    sample_x = &affine_x;
                    sample_y = &affine_y;
                    affine_rendered_pixels++;
                }
                if (sample_x->first < source_stage_x0 ||
                    sample_x->second >=
                        source_stage_x0 + source_stage_width ||
                    sample_y->first < source_stage_y0 ||
                    sample_y->second >=
                        source_stage_y0 + source_stage_height) {
                    if (why) *why =
                        "filtered sprite sample escaped its staged source window";
                    ok = false;
                    break;
                }
                uint32_t x0_offset = sample_x->first - source_stage_x0;
                uint32_t x1_offset = sample_x->second - source_stage_x0;
                uint32_t y0_offset = sample_y->first - source_stage_y0;
                uint32_t y1_offset = sample_y->second - source_stage_y0;
                uint32_t vertical_left, vertical_right;
                if (cache_vertical) {
                    vertical_left = vertical_pixels[x0_offset];
                    vertical_right = vertical_pixels[x1_offset];
                } else {
                    uint32_t top_left = mbx_load_le32(source_pixels +
                        y0_offset * source_row_bytes + x0_offset * 4u);
                    uint32_t bottom_left = mbx_load_le32(source_pixels +
                        y1_offset * source_row_bytes + x0_offset * 4u);
                    uint32_t top_right = mbx_load_le32(source_pixels +
                        y0_offset * source_row_bytes + x1_offset * 4u);
                    uint32_t bottom_right = mbx_load_le32(source_pixels +
                        y1_offset * source_row_bytes + x1_offset * 4u);
                    vertical_left = top_left == bottom_left
                        ? top_left
                        : mbx_linear_bgra8(top_left, bottom_left,
                                           sample_y->weight);
                    vertical_right = top_right == bottom_right
                        ? top_right
                        : mbx_linear_bgra8(top_right, bottom_right,
                                           sample_y->weight);
                }
                src = vertical_left == vertical_right
                    ? vertical_left
                    : mbx_linear_bgra8(vertical_left, vertical_right,
                                       sample_x->weight);
            } else {
                src = mbx_load_le32(source_pixels + y * source_row_bytes +
                                    x * 4u);
            }
            uint32_t pixel_offset = y * row_bytes + x * 4u;
            uint32_t output = src;
            if (!opaque_compact_copy) {
                src = mbx_modulate_vertex_alpha(
                    src, vertex_alpha_word >> 24);
                output = mbx_source_over_clamped(
                    mbx_load_le32(pixels + pixel_offset), src);
            }
            pixels[pixel_offset] = (uint8_t)output;
            pixels[pixel_offset + 1u] = (uint8_t)(output >> 8);
            pixels[pixel_offset + 2u] = (uint8_t)(output >> 16);
            pixels[pixel_offset + 3u] = (uint8_t)(output >> 24);
        }
    }
    if (ok && affine_sprite && !affine_rendered_pixels) {
        if (why) *why = "affine sprite covers no destination pixel centres";
        ok = false;
    }
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        ok = mbx_gart_write(m, bus, dst, pixels + row * row_bytes,
                            row_bytes, why);
    }
    free(source_pixels);
    free(pixels);
    if (ok && pixels_blended) {
        *pixels_blended = affine_sprite
            ? affine_rendered_pixels : width * height;
    }
    return ok;
}

/* The PIO TA stream retained from Voice Memos is not an object list: it is
 * the input from which real MBX hardware would build the region and object
 * buffers. The stream nevertheless describes ordinary BGRA8 quads using the
 * same texture header, pitch, sampler, vertex-alpha and source-over
 * conventions already proved by the object-list decoders above. Decode only
 * that measured subset. In particular, an unfamiliar state block, control
 * word, vertex layout, transform, colour modulation, or texture alias rejects
 * the entire scene before the first target write. */
#define MBX_TA_STATE_WORDS 14u

enum mbx_ta_parse_result {
    MBX_TA_NOT_DRAW = 0,
    MBX_TA_DRAW_OK,
    MBX_TA_DRAW_BAD,
};
