/*
 * QEMU NVIDIA Quadro2 Pro (NV15GL) emulation -- D3D engine core
 *
 * Based on the Bochs GeForce emulation:
 *   Copyright (C) 2025-2026  The Bochs Project
 * QEMU port:
 *   Copyright (c) 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include <math.h>
#include "geforce_int.h"

static bool gf_d3d_scissor_clip(GeForceState *s, gf_channel *ch,
                                uint32_t *x, uint32_t *y,
                                uint32_t *width, uint32_t *height)
{
    if (s->card_type >= 0x35) {
        int32_t surf_x2 = *x + *width;
        int32_t surf_y2 = *y + *height;
        int32_t scissor_x1 = (int32_t)ch->d3d_scissor_x +
                             ch->d3d_window_offset_x;
        int32_t scissor_y1 = (int32_t)ch->d3d_scissor_y +
                             ch->d3d_window_offset_y;
        int32_t scissor_x2 = scissor_x1 + (int32_t)ch->d3d_scissor_width;
        int32_t scissor_y2 = scissor_y1 + (int32_t)ch->d3d_scissor_height;
        if (scissor_x1 >= surf_x2 || scissor_x2 <= (int32_t)*x ||
            scissor_y1 >= surf_y2 || scissor_y2 <= (int32_t)*y) {
            return false;
        }
        *x = MAX((int32_t)*x, scissor_x1);
        *y = MAX((int32_t)*y, scissor_y1);
        *width = MIN(surf_x2, scissor_x2) - *x;
        *height = MIN(surf_y2, scissor_y2) - *y;
    }
    return true;
}

static bool gf_d3d_viewport_clip(GeForceState *s, gf_channel *ch,
                                 uint32_t *x, uint32_t *y,
                                 uint32_t *width, uint32_t *height)
{
    if (s->card_type >= 0x35) {
        int32_t surf_x2 = *x + *width;
        int32_t surf_y2 = *y + *height;
        int32_t viewport_x1 = (int32_t)ch->d3d_viewport_x +
                              ch->d3d_window_offset_x;
        int32_t viewport_y1 = (int32_t)ch->d3d_viewport_y +
                              ch->d3d_window_offset_y;
        int32_t viewport_x2 = viewport_x1 + (int32_t)ch->d3d_viewport_width;
        int32_t viewport_y2 = viewport_y1 + (int32_t)ch->d3d_viewport_height;
        if (viewport_x1 >= surf_x2 || viewport_x2 <= (int32_t)*x ||
            viewport_y1 >= surf_y2 || viewport_y2 <= (int32_t)*y) {
            return false;
        }
        *x = MAX((int32_t)*x, viewport_x1);
        *y = MAX((int32_t)*y, viewport_y1);
        *width = MIN(surf_x2, viewport_x2) - *x;
        *height = MIN(surf_y2, viewport_y2) - *y;
    }
    return true;
}

static bool gf_d3d_window_clip(GeForceState *s, gf_channel *ch,
                               uint32_t *x, uint32_t *y,
                               uint32_t *width, uint32_t *height)
{
    if (s->card_type >= 0x35) {
        int32_t surf_x2 = *x + *width;
        int32_t surf_y2 = *y + *height;
        int32_t window_x1 = (int32_t)ch->d3d_window_clip_x1[0] +
                            ch->d3d_window_offset_x;
        int32_t window_y1 = (int32_t)ch->d3d_window_clip_y1[0] +
                            ch->d3d_window_offset_y;
        int32_t window_x2 = (int32_t)ch->d3d_window_clip_x2[0] +
                            ch->d3d_window_offset_x + 1;
        int32_t window_y2 = (int32_t)ch->d3d_window_clip_y2[0] +
                            ch->d3d_window_offset_y + 1;
        if (window_x1 >= surf_x2 || window_x2 <= (int32_t)*x ||
            window_y1 >= surf_y2 || window_y2 <= (int32_t)*y) {
            return false;
        }
        *x = MAX((int32_t)*x, window_x1);
        *y = MAX((int32_t)*y, window_y1);
        *width = MIN(surf_x2, window_x2) - *x;
        *height = MIN(surf_y2, window_y2) - *y;
    }
    return true;
}

static uint32_t gf_d3d_get_surface_pitch_z(GeForceState *s, gf_channel *ch)
{
    if (s->card_type <= 0x35) {
        return ch->d3d_surface_pitch_a >> 16;
    } else {
        return ch->d3d_surface_pitch_z;
    }
}

void gf_d3d_clear_surface_op(GeForceState *s, gf_channel *ch)
{
    uint32_t dx = ch->d3d_clip_horizontal & 0xFFFF;
    uint32_t dy = ch->d3d_clip_vertical & 0xFFFF;
    uint32_t width = ch->d3d_clip_horizontal >> 16;
    uint32_t height = ch->d3d_clip_vertical >> 16;
    bool depth_clear;
    bool stencil_clear;
    uint32_t x, y;

    if (!gf_d3d_scissor_clip(s, ch, &dx, &dy, &width, &height)) {
        return;
    }
    if (ch->d3d_clear_surface & 0x000000F0) {
        uint32_t pitch = ch->d3d_surface_pitch_a & 0xFFFF;
        uint32_t draw_offset = ch->d3d_surface_color_offset +
            dy * pitch + dx * ch->d3d_color_bytes;
        uint32_t redraw_offset = gf_dma_lin_lookup(s, ch->d3d_color_obj,
                                                   draw_offset);
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                if (ch->d3d_color_bytes == 2) {
                    gf_dma_write16(s, ch->d3d_color_obj, draw_offset + x * 2,
                                   ch->d3d_color_clear_value);
                } else {
                    gf_dma_write32(s, ch->d3d_color_obj, draw_offset + x * 4,
                                   ch->d3d_color_clear_value);
                }
            }
            draw_offset += pitch;
        }
        gf_redraw_area(s, redraw_offset, width, height);
    }
    depth_clear = (ch->d3d_clear_surface & 0x00000001) != 0;
    stencil_clear = (ch->d3d_clear_surface & 0x00000002) != 0;
    if (depth_clear || stencil_clear) {
        uint32_t pitch = gf_d3d_get_surface_pitch_z(s, ch);
        uint32_t draw_offset = ch->d3d_surface_zeta_offset +
            dy * pitch + dx * ch->d3d_depth_bytes;
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                if (ch->d3d_depth_bytes == 2) {
                    if (depth_clear) {
                        gf_dma_write16(s, ch->d3d_zeta_obj,
                                       draw_offset + x * 2,
                                       ch->d3d_zstencil_clear_value);
                    }
                } else {
                    if (depth_clear) {
                        if (stencil_clear) {
                            gf_dma_write32(s, ch->d3d_zeta_obj,
                                           draw_offset + x * 4,
                                           ch->d3d_zstencil_clear_value);
                        } else {
                            gf_dma_write8(s, ch->d3d_zeta_obj,
                                draw_offset + x * 4 + 1,
                                (uint8_t)(ch->d3d_zstencil_clear_value >> 8));
                            gf_dma_write16(s, ch->d3d_zeta_obj,
                                draw_offset + x * 4 + 2,
                                (uint16_t)(ch->d3d_zstencil_clear_value >>
                                           16));
                        }
                    } else {
                        gf_dma_write8(s, ch->d3d_zeta_obj, draw_offset + x * 4,
                                      (uint8_t)ch->d3d_zstencil_clear_value);
                    }
                }
            }
            draw_offset += pitch;
        }
    }
}

/* v2 may point at a 2-element x/y pair; only [0] and [1] are read */
static double gf_edge_function(const float *v0, const float *v1,
                               const float *v2)
{
    return ((double)v1[0] - v0[0]) * ((double)v2[1] - v0[1]) -
           ((double)v1[1] - v0[1]) * ((double)v2[0] - v0[0]);
}

float gf_uint32_as_float(uint32_t val)
{
    union {
        uint32_t ui32;
        float f;
    } conv;
    conv.ui32 = val;
    return conv.f;
}

void gf_d3d_texture_process_format(gf_texture *tex)
{
    static bool unknown_format_reported;

    tex->linear = false;
    tex->unnormalized = false;
    tex->compressed = false;
    tex->dxt_alpha_data = false;
    tex->dxt_alpha_explicit = false;
    if ((tex->format & 0x80) != 0) {
        if ((tex->format & 0x20) != 0) {
            tex->linear = true;
        }
        if ((tex->format & 0x40) != 0) {
            tex->unnormalized = true;
        }
        tex->format &= 0x9f;
    } else if (tex->format == 0x12 ||
               tex->format == 0x1b ||
               tex->format == 0x1e) {
        tex->linear = true;
        tex->unnormalized = true;
    }
    switch (tex->format) {
    case 0x0c: /* DXT1 */
    case 0x0e: /* DXT23 */
    case 0x0f: /* DXT45 */
    case 0x86: /* DXT1 */
    case 0x87: /* DXT23 */
    case 0x88: /* DXT45 */
        tex->compressed = true;
        tex->dxt_alpha_data = tex->format != 0x0c && tex->format != 0x86;
        tex->dxt_alpha_explicit = tex->format == 0x0e || tex->format == 0x87;
        tex->color_bytes = tex->dxt_alpha_data ? 16 : 8;
        break;
    case 0x02: /* A1R5G5B5 */
    case 0x03: /* X1R5G5B5 */
    case 0x04: /* A4R4G4B4 */
    case 0x05: /* R5G6B5 */
    case 0x27: /* R6G5B5 */
    case 0x28: /* G8B8 */
    case 0x82: /* A1R5G5B5 */
    case 0x83: /* A4R4G4B4 */
    case 0x84: /* R5G6B5 */
    case 0x8b: /* G8B8 */
    case 0x8f: /* R6G5B5 */
        tex->color_bytes = 2;
        break;
    case 0x06: /* A8R8G8B8 */
    case 0x07: /* X8R8G8B8 */
    case 0x12: /* A8R8G8B8 */
    case 0x1e: /* X8R8G8B8 */
    case 0x3a: /* A8B8G8R8 */
    case 0x85: /* A8R8G8B8 */
        tex->color_bytes = 4;
        break;
    default:
        if (!unknown_format_reported) {
            qemu_log_mask(LOG_UNIMP,
                          "geforce: unknown texture format 0x%02x\n",
                          tex->format);
            unknown_format_reported = true;
        }
        /* fallthrough */
    case 0x00: /* Y8 */
    case 0x01: /* AY8 */
    case 0x0b: /* I8_A8R8G8B8 */
    case 0x1b: /* AY8 */
    case 0x81: /* B8 */
        tex->color_bytes = 1;
        break;
    }
}

void gf_texture_update_size(gf_texture *tex, uint32_t cls)
{
    uint32_t lw, lh, i;

    if (tex->linear || cls >= 0x4097) {
        tex->size[0] = tex->image_rect >> 16;
        tex->size[1] = tex->image_rect & 0x0000ffff;
    } else {
        tex->size[0] = 1 << tex->base_size[0];
        tex->size[1] = 1 << tex->base_size[1];
    }
    lw = tex->size[0];
    lh = tex->size[1];
    tex->face_bytes = 0;
    for (i = 0; i < tex->levels; i++) {
        uint32_t level_bytes = lw * lh * tex->color_bytes;
        if (tex->compressed) {
            level_bytes /= 16;
        }
        tex->face_bytes += level_bytes;
        lw /= 2;
        lh /= 2;
        if (lw == 0) {
            lw = 1;
        }
        if (lh == 0) {
            lh = 1;
        }
    }
    tex->face_bytes = GF_ALIGN(tex->face_bytes, 128);
}

static void gf_d3d_sample_texture(GeForceState *s, gf_channel *ch,
                                  gf_texture *tex, float coords_in[3],
                                  float color[4])
{
    float *coords;
    float coords_cubemap[3];
    uint32_t tex_ofs = tex->offset;
    uint32_t xy[2];
    int32_t color_int[4] = { 0 };
    float color_scale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    int i;

    if (tex->cubemap) {
        uint32_t face;
        float coords_abs[3];
        for (i = 0; i < 3; i++) {
            coords_abs[i] = fabs(coords_in[i]);
        }
        if (coords_abs[0] > coords_abs[1] && coords_abs[0] > coords_abs[2]) {
            coords_cubemap[0] = coords_cubemap[1] = 1.0f / coords_abs[0];
            if (coords_in[0] > 0.0f) {
                face = 0;
                coords_cubemap[0] *= -coords_in[2];
                coords_cubemap[1] *= -coords_in[1];
            } else {
                face = 1;
                coords_cubemap[0] *= coords_in[2];
                coords_cubemap[1] *= -coords_in[1];
            }
        } else if (coords_abs[1] > coords_abs[0] &&
                   coords_abs[1] > coords_abs[2]) {
            coords_cubemap[0] = coords_cubemap[1] = 1.0f / coords_abs[1];
            if (coords_in[1] > 0.0f) {
                face = 2;
                coords_cubemap[0] *= coords_in[0];
                coords_cubemap[1] *= coords_in[2];
            } else {
                face = 3;
                coords_cubemap[0] *= coords_in[0];
                coords_cubemap[1] *= -coords_in[2];
            }
        } else {
            coords_cubemap[0] = coords_cubemap[1] = 1.0f / coords_abs[2];
            if (coords_in[2] > 0.0f) {
                face = 4;
                coords_cubemap[0] *= coords_in[0];
                coords_cubemap[1] *= -coords_in[1];
            } else {
                face = 5;
                coords_cubemap[0] *= -coords_in[0];
                coords_cubemap[1] *= -coords_in[1];
            }
        }
        coords_cubemap[0] = (coords_cubemap[0] + 1.0f) * 0.5f;
        coords_cubemap[1] = (coords_cubemap[1] + 1.0f) * 0.5f;
        coords_cubemap[2] = 0.0f;
        coords = coords_cubemap;
        tex_ofs += face * tex->face_bytes;
    } else {
        coords = coords_in;
    }
    for (i = 0; i < 2; i++) {
        if (tex->size[i] == 0) {
            xy[i] = 0;
            continue;
        }
        if (tex->unnormalized) {
            int32_t c = coords[i];
            uint32_t size = tex->size[i];
            if (c < 0 || (uint32_t)c >= size) {
                switch (tex->wrap[i]) {
                case 1:  /* WRAP */
                    c %= (int32_t)size;
                    if (c < 0) {
                        c += size;
                    }
                    break;
                case 2:  /* MIRROR */
                    c %= (int32_t)(size * 2);
                    if (c < 0) {
                        c += size * 2;
                    }
                    if ((uint32_t)c >= size) {
                        c = size * 2 - c - 1;
                    }
                    break;
                default: /* CLAMP_TO_EDGE */
                    c = c < 0 ? 0 : size - 1;
                    break;
                }
            }
            xy[i] = c;
        } else {
            float c = coords[i];
            if (c < 0.0f || c > 1.0f) {
                switch (tex->wrap[i]) {
                case 1:  /* WRAP */
                    c = c - floor(c);
                    break;
                case 2:  /* MIRROR */
                    c = fmod(c, 2.0f);
                    if (c < 0.0f) {
                        c += 2.0f;
                    }
                    if (c > 1.0f) {
                        c = 2.0f - c;
                    }
                    break;
                default: /* CLAMP_TO_EDGE */
                    c = c < 0.0f ? 0.0f : 1.0f;
                    break;
                }
            }
            xy[i] = c == 1.0f ? tex->size[i] - 1 : c * tex->size[i];
        }
    }
    if (tex->compressed) {
        uint32_t pitch = tex->size[0] * (tex->dxt_alpha_data ? 4 : 2);
        uint32_t bx = xy[0] >> 2;
        uint32_t by = xy[1] >> 2;
        tex_ofs += by * pitch + bx * tex->color_bytes;
    } else if (tex->linear) {
        uint32_t pitch;
        if (s->card_type >= 0x40) {
            pitch = tex->control3 & 0x000fffff;
        } else {
            pitch = tex->control1 >> 16;
        }
        tex_ofs += xy[1] * pitch + xy[0] * tex->color_bytes;
    } else {
        tex_ofs += gf_swizzle(xy[0], xy[1], tex->size[0], tex->size[1]) *
                   tex->color_bytes;
    }
    switch (tex->format) {
    case 0x0c:   /* DXT1 */
    case 0x0e:   /* DXT23 */
    case 0x0f:   /* DXT45 */
    case 0x86:   /* DXT1 */
    case 0x87:   /* DXT23 */
    case 0x88: { /* DXT45 */
        uint32_t ox = xy[0] & 3;
        uint32_t oy = xy[1] & 3;
        uint64_t color_word;
        uint32_t color_index;
        if (tex->dxt_alpha_data) {
            uint64_t alpha_word = gf_dma_read64(s, tex->dma_obj, tex_ofs);
            if (tex->dxt_alpha_explicit) {
                color_int[0] = (alpha_word >> (oy * 16 + ox * 4)) & 0xf;
                color_scale[0] = 1.0f / 15.0f;
            } else {
                uint32_t alpha_index =
                    (alpha_word >> (16 + oy * 12 + ox * 3)) & 7;
                uint8_t alpha0 = (uint8_t)alpha_word;
                uint8_t alpha1 = (uint8_t)(alpha_word >> 8);
                switch (alpha_index) {
                case 0:
                    color_int[0] = alpha0;
                    color_scale[0] = 1.0f / 255.0f;
                    break;
                case 1:
                    color_int[0] = alpha1;
                    color_scale[0] = 1.0f / 255.0f;
                    break;
                case 2:
                    if (alpha0 > alpha1) {
                        color_int[0] = 6 * alpha0 + alpha1;
                        color_scale[0] = 1.0f / 1785.0f;
                    } else {
                        color_int[0] = 4 * alpha0 + alpha1;
                        color_scale[0] = 1.0f / 1275.0f;
                    }
                    break;
                case 3:
                    if (alpha0 > alpha1) {
                        color_int[0] = 5 * alpha0 + 2 * alpha1;
                        color_scale[0] = 1.0f / 1785.0f;
                    } else {
                        color_int[0] = 3 * alpha0 + 2 * alpha1;
                        color_scale[0] = 1.0f / 1275.0f;
                    }
                    break;
                case 4:
                    if (alpha0 > alpha1) {
                        color_int[0] = 4 * alpha0 + 3 * alpha1;
                        color_scale[0] = 1.0f / 1785.0f;
                    } else {
                        color_int[0] = 2 * alpha0 + 3 * alpha1;
                        color_scale[0] = 1.0f / 1275.0f;
                    }
                    break;
                case 5:
                    if (alpha0 > alpha1) {
                        color_int[0] = 3 * alpha0 + 4 * alpha1;
                        color_scale[0] = 1.0f / 1785.0f;
                    } else {
                        color_int[0] = alpha0 + 4 * alpha1;
                        color_scale[0] = 1.0f / 1275.0f;
                    }
                    break;
                case 6:
                    if (alpha0 > alpha1) {
                        color_int[0] = 2 * alpha0 + 5 * alpha1;
                        color_scale[0] = 1.0f / 1785.0f;
                    } else {
                        color_int[0] = 0;
                        color_scale[0] = 1.0f;
                    }
                    break;
                case 7:
                    if (alpha0 > alpha1) {
                        color_int[0] = alpha0 + 6 * alpha1;
                        color_scale[0] = 1.0f / 1785.0f;
                    } else {
                        color_int[0] = 1;
                        color_scale[0] = 1.0f;
                    }
                    break;
                }
            }
        } else {
            color_int[0] = 1;
            color_scale[0] = 1.0f;
        }
        color_word = gf_dma_read64(s, tex->dma_obj,
                                   tex_ofs + (tex->dxt_alpha_data ? 8 : 0));
        color_index = (color_word >> (32 + oy * 8 + ox * 2)) & 3;
        {
            uint16_t color0 = (uint16_t)color_word;
            uint16_t color1 = (uint16_t)(color_word >> 16);
            switch (color_index) {
            case 0:
                color_int[1] = (color0 >> 11) & 0x1f;
                color_scale[1] = 1.0f / 31.0f;
                color_int[2] = (color0 >> 5) & 0x3f;
                color_scale[2] = 1.0f / 63.0f;
                color_int[3] = (color0 >> 0) & 0x1f;
                color_scale[3] = 1.0f / 31.0f;
                break;
            case 1:
                color_int[1] = (color1 >> 11) & 0x1f;
                color_scale[1] = 1.0f / 31.0f;
                color_int[2] = (color1 >> 5) & 0x3f;
                color_scale[2] = 1.0f / 63.0f;
                color_int[3] = (color1 >> 0) & 0x1f;
                color_scale[3] = 1.0f / 31.0f;
                break;
            case 2:
                if (color0 > color1) {
                    color_int[1] = 2 * ((color0 >> 11) & 0x1f) +
                                   ((color1 >> 11) & 0x1f);
                    color_scale[1] = 1.0f / 93.0f;
                    color_int[2] = 2 * ((color0 >> 5) & 0x3f) +
                                   ((color1 >> 5) & 0x3f);
                    color_scale[2] = 1.0f / 189.0f;
                    color_int[3] = 2 * ((color0 >> 0) & 0x1f) +
                                   ((color1 >> 0) & 0x1f);
                    color_scale[3] = 1.0f / 93.0f;
                } else {
                    color_int[1] = ((color0 >> 11) & 0x1f) +
                                   ((color1 >> 11) & 0x1f);
                    color_scale[1] = 1.0f / 62.0f;
                    color_int[2] = ((color0 >> 5) & 0x3f) +
                                   ((color1 >> 5) & 0x3f);
                    color_scale[2] = 1.0f / 126.0f;
                    color_int[3] = ((color0 >> 0) & 0x1f) +
                                   ((color1 >> 0) & 0x1f);
                    color_scale[3] = 1.0f / 62.0f;
                }
                break;
            case 3:
                if (color0 > color1) {
                    color_int[1] = 2 * ((color1 >> 11) & 0x1f) +
                                   ((color0 >> 11) & 0x1f);
                    color_scale[1] = 1.0f / 93.0f;
                    color_int[2] = 2 * ((color1 >> 5) & 0x3f) +
                                   ((color0 >> 5) & 0x3f);
                    color_scale[2] = 1.0f / 189.0f;
                    color_int[3] = 2 * ((color1 >> 0) & 0x1f) +
                                   ((color0 >> 0) & 0x1f);
                    color_scale[3] = 1.0f / 93.0f;
                } else {
                    color_int[0] = 0;
                    color_scale[0] = 1.0f;
                    color_int[1] = 0;
                    color_scale[1] = 1.0f;
                    color_int[2] = 0;
                    color_scale[2] = 1.0f;
                    color_int[3] = 0;
                    color_scale[3] = 1.0f;
                }
                break;
            }
        }
        break;
    }
    case 0x04:
    case 0x83: { /* A4R4G4B4 */
        uint16_t value = gf_dma_read16(s, tex->dma_obj, tex_ofs);
        color_int[0] = (value >> 12) & 0xf;
        color_scale[0] = 1.0f / 15.0f;
        color_int[1] = (value >> 8) & 0xf;
        color_scale[1] = 1.0f / 15.0f;
        color_int[2] = (value >> 4) & 0xf;
        color_scale[2] = 1.0f / 15.0f;
        color_int[3] = (value >> 0) & 0xf;
        color_scale[3] = 1.0f / 15.0f;
        break;
    }
    case 0x05:
    case 0x84: { /* R5G6B5 */
        uint16_t value = gf_dma_read16(s, tex->dma_obj, tex_ofs);
        color_int[0] = 1;
        color_scale[0] = 1.0f;
        color_int[1] = (value >> 11) & 0x1f;
        color_scale[1] = 1.0f / 31.0f;
        color_int[2] = (value >> 5) & 0x3f;
        color_scale[2] = 1.0f / 63.0f;
        color_int[3] = (value >> 0) & 0x1f;
        color_scale[3] = 1.0f / 31.0f;
        break;
    }
    case 0x02:
    case 0x82: { /* A1R5G5B5 */
        uint16_t value = gf_dma_read16(s, tex->dma_obj, tex_ofs);
        if ((tex->control0 & 3) != 0 && value == tex->key_color) {
            color_int[0] = 0;
        } else {
            color_int[0] = (value >> 15) & 1;
        }
        color_scale[0] = 1.0f;
        color_int[1] = (value >> 10) & 0x1f;
        color_scale[1] = 1.0f / 31.0f;
        color_int[2] = (value >> 5) & 0x1f;
        color_scale[2] = 1.0f / 31.0f;
        color_int[3] = (value >> 0) & 0x1f;
        color_scale[3] = 1.0f / 31.0f;
        break;
    }
    case 0x03: { /* X1R5G5B5 */
        uint16_t value = gf_dma_read16(s, tex->dma_obj, tex_ofs);
        color_int[0] = 1;
        color_scale[0] = 1.0f;
        color_int[1] = (value >> 10) & 0x1f;
        color_scale[1] = 1.0f / 31.0f;
        color_int[2] = (value >> 5) & 0x1f;
        color_scale[2] = 1.0f / 31.0f;
        color_int[3] = (value >> 0) & 0x1f;
        color_scale[3] = 1.0f / 31.0f;
        break;
    }
    case 0x27:
    case 0x8f: { /* R6G5B5 */
        uint16_t value = gf_dma_read16(s, tex->dma_obj, tex_ofs);
        color_int[0] = 1;
        color_scale[0] = 1.0f;
        color_int[1] = (value >> 10) & 0x3f;
        color_scale[1] = 1.0f / 63.0f;
        color_int[2] = (value >> 5) & 0x1f;
        color_scale[2] = 1.0f / 31.0f;
        color_int[3] = (value >> 0) & 0x1f;
        color_scale[3] = 1.0f / 31.0f;
        break;
    }
    case 0x28:
    case 0x8b: { /* G8B8 */
        uint16_t value = gf_dma_read16(s, tex->dma_obj, tex_ofs);
        color_int[0] = 1;
        color_scale[0] = 1.0f;
        color_int[1] = 1;
        color_scale[1] = 1.0f;
        color_int[2] = (value >> 8) & 0xff;
        color_scale[2] = 1.0f / 255.0f;
        color_int[3] = (value >> 0) & 0xff;
        color_scale[3] = 1.0f / 255.0f;
        break;
    }
    case 0x06:
    case 0x12:
    case 0x85: { /* A8R8G8B8 */
        uint32_t value = gf_dma_read32(s, tex->dma_obj, tex_ofs);
        color_int[0] = (value >> 24) & 0xff;
        color_scale[0] = 1.0f / 255.0f;
        color_int[1] = (value >> 16) & 0xff;
        color_scale[1] = 1.0f / 255.0f;
        color_int[2] = (value >> 8) & 0xff;
        color_scale[2] = 1.0f / 255.0f;
        color_int[3] = (value >> 0) & 0xff;
        color_scale[3] = 1.0f / 255.0f;
        break;
    }
    case 0x3a: { /* A8B8G8R8 */
        uint32_t value = gf_dma_read32(s, tex->dma_obj, tex_ofs);
        color_int[0] = (value >> 24) & 0xff;
        color_scale[0] = 1.0f / 255.0f;
        color_int[1] = (value >> 0) & 0xff;
        color_scale[1] = 1.0f / 255.0f;
        color_int[2] = (value >> 8) & 0xff;
        color_scale[2] = 1.0f / 255.0f;
        color_int[3] = (value >> 16) & 0xff;
        color_scale[3] = 1.0f / 255.0f;
        break;
    }
    case 0x07:
    case 0x1e: { /* X8R8G8B8 */
        uint32_t value = gf_dma_read32(s, tex->dma_obj, tex_ofs);
        color_int[0] = 1;
        color_scale[0] = 1.0f;
        color_int[1] = (value >> 16) & 0xff;
        color_scale[1] = 1.0f / 255.0f;
        color_int[2] = (value >> 8) & 0xff;
        color_scale[2] = 1.0f / 255.0f;
        color_int[3] = (value >> 0) & 0xff;
        color_scale[3] = 1.0f / 255.0f;
        break;
    }
    case 0x0b: { /* I8_A8R8G8B8 */
        uint32_t pal_index = gf_dma_read8(s, tex->dma_obj, tex_ofs);
        uint32_t value = gf_dma_read32(s, tex->pal_dma_obj,
                                       tex->pal_ofs + pal_index * 4);
        color_int[0] = (value >> 24) & 0xff;
        color_scale[0] = 1.0f / 255.0f;
        color_int[1] = (value >> 16) & 0xff;
        color_scale[1] = 1.0f / 255.0f;
        color_int[2] = (value >> 8) & 0xff;
        color_scale[2] = 1.0f / 255.0f;
        color_int[3] = (value >> 0) & 0xff;
        color_scale[3] = 1.0f / 255.0f;
        break;
    }
    case 0x00:   /* Y8 */
    case 0x81: { /* B8 */
        uint8_t value = gf_dma_read8(s, tex->dma_obj, tex_ofs);
        color_int[0] = 1;
        color_scale[0] = 1.0f;
        color_int[1] = value;
        color_scale[1] = 1.0f / 255.0f;
        color_int[2] = value;
        color_scale[2] = 1.0f / 255.0f;
        color_int[3] = value;
        color_scale[3] = 1.0f / 255.0f;
        break;
    }
    case 0x01:
    case 0x1b: { /* AY8 */
        uint8_t value = gf_dma_read8(s, tex->dma_obj, tex_ofs);
        color_int[0] = value;
        color_scale[0] = 1.0f / 255.0f;
        color_int[1] = value;
        color_scale[1] = 1.0f / 255.0f;
        color_int[2] = value;
        color_scale[2] = 1.0f / 255.0f;
        color_int[3] = value;
        color_scale[3] = 1.0f / 255.0f;
        break;
    }
    default:
        color_int[0] = 1;
        color_scale[0] = 0.8f;
        color_int[1] = 1;
        color_scale[1] = 0.8f + coords[0] * 0.2f;
        color_int[2] = 1;
        color_scale[2] = 0.6f + coords[1] * 0.2f;
        color_int[3] = 1;
        color_scale[3] = 0.6f + coords[2] * 0.2f;
        break;
    }
    if (tex->signed_any) {
        uint32_t ci;
        for (ci = 0; ci < 4; ci++) {
            if (tex->signed_comp[ci]) {
                color_int[ci] = (int8_t)color_int[ci];
                color_scale[ci] = 1.0f / 128.0f;
            }
        }
    }
    if (s->card_type <= 0x20) {
        uint32_t ci;
        for (ci = 0; ci < 4; ci++) {
            uint32_t j = (ci + 3) & 3;
            color[j] = color_int[ci] * color_scale[ci];
        }
    } else {
        uint16_t s01 = tex->control1;
        uint32_t ci;
        for (ci = 0; ci < 4; ci++) {
            uint32_t j = (ci + 3) & 3;
            switch ((s01 >> (8 + ci * 2)) & 3) {
            case 0:
                color[j] = 0.0f;
                break;
            case 1:
                color[j] = 1.0f;
                break;
            default: {
                uint32_t swz = (s01 >> (ci * 2)) & 3;
                color[j] = color_int[swz] * color_scale[swz];
                break;
            }
            }
        }
    }
}

static float gf_dot3(const float x[3], const float y[3])
{
    return x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
}

static float gf_dot4(const float x[4], const float y[4])
{
    return x[0] * y[0] + x[1] * y[1] + x[2] * y[2] + x[3] * y[3];
}

static void gf_reflection(const float axis[3], const float direction[3],
                          float refl_dir[3])
{
    float k = 2.0f * gf_dot3(axis, direction) / gf_dot3(axis, axis);

    refl_dir[0] = k * axis[0] - direction[0];
    refl_dir[1] = k * axis[1] - direction[1];
    refl_dir[2] = k * axis[2] - direction[2];
}

static void gf_dot_map(uint32_t func, const float src[4], float dst[3])
{
    int ci;

    switch (func) {
    default:
    case 0:
        for (ci = 0; ci < 3; ci++) {
            dst[ci] = src[ci];
        }
        break;
    case 1:
        for (ci = 0; ci < 3; ci++) {
            dst[ci] = (src[ci] * 255.0f - 128.0f) / 127.0f;
        }
        break;
    }
}

static float gf_dot3_map(const float x[3], const float y[4],
                         uint32_t map_func)
{
    float ym[3];

    gf_dot_map(map_func, y, ym);
    return gf_dot3(x, ym);
}

static void gf_d3d_vertex_shader(GeForceState *s, gf_channel *ch,
                                 float in[16][4], float out[16][4])
{
    static bool unknown_opcode_reported;
    int32_t addr_regs[2][4] = { { 0 } };
    float tmp_regs[32][4];
    uint32_t r, ci, op_index;
    int a;

    for (a = 0; a < 16; a++) {
        out[a][0] = 0.0f;
        out[a][1] = 0.0f;
        out[a][2] = 0.0f;
        out[a][3] = 1.0f;
    }
    for (r = 0; r < ch->d3d_vs_temp_regs_count && r < 32; r++) {
        for (ci = 0; ci < 4; ci++) {
            tmp_regs[r][ci] = 0.0f;
        }
    }
    for (op_index = ch->d3d_transform_program_start;
         op_index < 544; op_index++) {
        uint32_t *tokens = ch->d3d_transform_program[op_index];
        float params[3][4];
        uint32_t vec_op;
        uint32_t sca_op;
        bool addr_write = false;
        bool paired_ops;
        float vec_result[4];
        float sca_result[4];
        int p, comp_index;

        for (p = 0; p < 3; p++) {
            uint32_t tmp_index = 0;
            uint32_t reg_type = 0;
            bool absolute = false;
            bool negate = false;
            uint32_t swizzle[4] = { 0 };

            if (p == 0) {
                if (s->card_type >= 0x35) {
                    absolute = (tokens[0] >> 21) & 1;
                }
                if (s->card_type <= 0x35) {
                    reg_type = (tokens[2] >> 26) & 3;
                    tmp_index = (tokens[2] >> 28) & 0xf;
                    negate = (tokens[1] >> 8) & 1;
                    for (int i = 0; i < 4; i++) {
                        swizzle[i] = (tokens[1] >> (6 - i * 2)) & 3;
                    }
                } else {
                    reg_type = (tokens[2] >> 23) & 3;
                    tmp_index = (tokens[2] >> 25) & 0x3f;
                    swizzle[3] = ((tokens[1] & 1) << 1) |
                                 ((tokens[2] >> 31) & 1);
                    swizzle[2] = (tokens[1] >> 1) & 3;
                    swizzle[1] = (tokens[1] >> 3) & 3;
                    swizzle[0] = (tokens[1] >> 5) & 3;
                    negate = (tokens[1] >> 7) & 1;
                }
            } else if (p == 1) {
                if (s->card_type >= 0x35) {
                    absolute = (tokens[0] >> 22) & 1;
                }
                if (s->card_type <= 0x35) {
                    reg_type = (tokens[2] >> 11) & 3;
                    tmp_index = (tokens[2] >> 13) & 0xf;
                    negate = (tokens[2] >> 25) & 1;
                    for (int i = 0; i < 4; i++) {
                        swizzle[i] = (tokens[2] >> (23 - i * 2)) & 3;
                    }
                } else {
                    reg_type = (tokens[2] >> 6) & 3;
                    tmp_index = (tokens[2] >> 8) & 0x3f;
                    for (int i = 0; i < 4; i++) {
                        swizzle[i] = (tokens[2] >> (20 - i * 2)) & 3;
                    }
                    negate = (tokens[2] >> 22) & 1;
                }
            } else {
                if (s->card_type >= 0x35) {
                    absolute = (tokens[0] >> 23) & 1;
                }
                if (s->card_type <= 0x35) {
                    reg_type = (tokens[3] >> 28) & 3;
                    tmp_index = ((tokens[2] & 3) << 2) |
                                ((tokens[3] >> 30) & 3);
                    negate = (tokens[2] >> 10) & 1;
                    for (int i = 0; i < 4; i++) {
                        swizzle[i] = (tokens[2] >> (8 - i * 2)) & 3;
                    }
                } else {
                    reg_type = (tokens[3] >> 21) & 3;
                    tmp_index = (tokens[3] >> 23) & 0x3f;
                    swizzle[3] = (tokens[3] >> 29) & 3;
                    swizzle[2] = ((tokens[2] & 1) << 1) |
                                 ((tokens[3] >> 31) & 1);
                    swizzle[1] = (tokens[2] >> 1) & 3;
                    swizzle[0] = (tokens[2] >> 3) & 3;
                    negate = (tokens[2] >> 5) & 1;
                }
            }
            for (comp_index = 0; comp_index < 4; comp_index++) {
                int comp_index_swizzle = swizzle[comp_index];
                if (reg_type == 1) {
                    if (s->card_type == 0x20 && tmp_index == 12) {
                        params[p][comp_index] = out[0][comp_index_swizzle];
                    } else {
                        params[p][comp_index] =
                            tmp_regs[tmp_index & 0x1f][comp_index_swizzle];
                    }
                } else if (reg_type == 2) {
                    uint32_t in_index;
                    if (s->card_type <= 0x35) {
                        in_index = (tokens[1] >> 9) & 0xf;
                    } else {
                        in_index = (tokens[1] >> 8) & 0xf;
                    }
                    params[p][comp_index] = in[in_index][comp_index_swizzle];
                } else if (reg_type == 3) {
                    uint32_t const_index;
                    if (s->card_type == 0x20) {
                        const_index = (tokens[1] >> 13) & 0xff;
                    } else if (s->card_type == 0x35) {
                        const_index = (tokens[1] >> 14) & 0x1ff;
                    } else {
                        const_index = (tokens[1] >> 12) & 0x1ff;
                    }
                    if (((tokens[3] >> 1) & 1) != 0) {
                        uint32_t addr_reg_sel;
                        uint32_t addr_swz;
                        if (s->card_type == 0x20) {
                            addr_reg_sel = 0;
                            addr_swz = 0;
                        } else {
                            addr_reg_sel = (tokens[0] >> 24) & 1;
                            if (s->card_type == 0x35) {
                                addr_swz = (tokens[0] >> 1) & 3;
                            } else {
                                addr_swz = tokens[0] & 3;
                            }
                        }
                        const_index = (const_index +
                                       addr_regs[addr_reg_sel][addr_swz]) &
                                      0x1ff;
                    }
                    params[p][comp_index] =
                        ch->d3d_transform_constant[const_index]
                                                  [comp_index_swizzle];
                } else {
                    params[p][comp_index] = 0.0f;
                }
                if (absolute) {
                    params[p][comp_index] = fabs(params[p][comp_index]);
                }
                if (negate) {
                    params[p][comp_index] = -params[p][comp_index];
                }
            }
        }
        if (s->card_type == 0x20) {
            vec_op = (tokens[1] >> 21) & 0xf;
        } else if (s->card_type == 0x35) {
            vec_op = (tokens[1] >> 23) & 0x1f;
        } else {
            vec_op = (tokens[1] >> 22) & 0x1f;
        }
        switch (vec_op) {
        case 0: /* NOP */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = 0.0f;
            }
            break;
        case 1: /* MOV */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = params[0][comp_index];
            }
            break;
        case 2: /* MUL */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = params[0][comp_index] *
                                         params[1][comp_index];
            }
            break;
        case 3: /* ADD */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = params[0][comp_index] +
                                         params[2][comp_index];
            }
            break;
        case 4: /* MAD */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = params[0][comp_index] *
                                         params[1][comp_index] +
                                         params[2][comp_index];
            }
            break;
        case 5: { /* DP3 */
            float dp3 = gf_dot3(params[0], params[1]);
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = dp3;
            }
            break;
        }
        case 6: { /* DPH */
            float dph = gf_dot3(params[0], params[1]) + params[1][3];
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = dph;
            }
            break;
        }
        case 7: { /* DP4 */
            float dp4 = gf_dot4(params[0], params[1]);
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = dp4;
            }
            break;
        }
        case 8: /* DST */
            vec_result[0] = 1.0f;
            vec_result[1] = params[0][1] * params[1][1];
            vec_result[2] = params[0][2];
            vec_result[3] = params[1][3];
            break;
        case 9: /* MIN */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = MIN(params[0][comp_index],
                                             params[1][comp_index]);
            }
            break;
        case 0xa: /* MAX */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = MAX(params[0][comp_index],
                                             params[1][comp_index]);
            }
            break;
        case 0xb: /* SLT */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] =
                    params[0][comp_index] < params[1][comp_index] ? 1.0f
                                                                  : 0.0f;
            }
            break;
        case 0xc: /* SGE */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] =
                    params[0][comp_index] >= params[1][comp_index] ? 1.0f
                                                                   : 0.0f;
            }
            break;
        case 0xd: /* ARL */
            addr_write = true;
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = floor(params[0][comp_index]);
            }
            break;
        case 0xe: /* FRC */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = params[0][comp_index] -
                                         floor(params[0][comp_index]);
            }
            break;
        case 0xf: /* FLR */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = floor(params[0][comp_index]);
            }
            break;
        case 0x10: /* SEQ */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] =
                    params[0][comp_index] == params[1][comp_index] ? 1.0f
                                                                   : 0.0f;
            }
            break;
        case 0x11: /* SFL */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = 0.0f;
            }
            break;
        case 0x12: /* SGT */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] =
                    params[0][comp_index] > params[1][comp_index] ? 1.0f
                                                                  : 0.0f;
            }
            break;
        case 0x13: /* SLE */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] =
                    params[0][comp_index] <= params[1][comp_index] ? 1.0f
                                                                   : 0.0f;
            }
            break;
        case 0x14: /* SNE */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] =
                    params[0][comp_index] != params[1][comp_index] ? 1.0f
                                                                   : 0.0f;
            }
            break;
        case 0x15: /* STR */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = 1.0f;
            }
            break;
        case 0x16: /* SSG */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] =
                    params[0][comp_index] == 0.0f ? 0.0f :
                    params[0][comp_index] < 0.0f ? -1.0f : 1.0f;
            }
            break;
        default:
            for (comp_index = 0; comp_index < 4; comp_index++) {
                vec_result[comp_index] = 0.5f;
            }
            if (!unknown_opcode_reported) {
                qemu_log_mask(LOG_UNIMP,
                    "geforce: vertex shader: unknown VEC opcode 0x%02x\n",
                    vec_op);
                unknown_opcode_reported = true;
            }
            break;
        }
        if (s->card_type == 0x20) {
            sca_op = (tokens[1] >> 25) & 7;
        } else if (s->card_type == 0x35) {
            sca_op = ((tokens[0] & 1) << 4) | ((tokens[1] >> 28) & 0x0f);
        } else {
            sca_op = (tokens[1] >> 27) & 0x1f;
        }
        paired_ops = vec_op != 0 && sca_op != 0;
        switch (sca_op) {
        case 0: /* NOP */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                sca_result[comp_index] = 0.0f;
            }
            break;
        case 1: /* MOV */
            for (comp_index = 0; comp_index < 4; comp_index++) {
                sca_result[comp_index] = params[2][comp_index];
            }
            break;
        case 2: /* RCP */
        case 3: { /* RCC */
            float rcp = 1.0f / params[2][0];
            for (comp_index = 0; comp_index < 4; comp_index++) {
                sca_result[comp_index] = rcp;
            }
            break;
        }
        case 4: { /* RSQ */
            float rsq = 1.0f / sqrt(fabs(params[2][0]));
            for (comp_index = 0; comp_index < 4; comp_index++) {
                sca_result[comp_index] = rsq;
            }
            break;
        }
        case 5: { /* EXP */
            float fl = floor(params[2][0]);
            sca_result[0] = exp2(fl);
            sca_result[1] = params[2][0] - fl;
            sca_result[2] = exp2(params[2][0]);
            sca_result[3] = 1.0f;
            break;
        }
        case 6: { /* LOG */
            float fa = fabs(params[2][0]);
            if (fa != 0.0f) {
                if (isinf(fa)) {
                    sca_result[0] = INFINITY;
                    sca_result[1] = 1.0f;
                    sca_result[2] = INFINITY;
                } else {
                    sca_result[0] = floor(log2(fa));
                    sca_result[1] = fa / exp2(floor(log2(fa)));
                    sca_result[2] = log2(fa);
                }
            } else {
                sca_result[0] = -INFINITY;
                sca_result[1] = 1.0f;
                sca_result[2] = -INFINITY;
            }
            sca_result[3] = 1.0f;
            break;
        }
        case 7: { /* LIT */
            float tmpx = params[2][0];
            float tmpy = params[2][1];
            float tmpw = params[2][3];
            float epsilon = 1.0e-6f;
            if (tmpx < 0.0f) {
                tmpx = 0.0f;
            }
            if (tmpy < 0.0f) {
                tmpy = 0.0f;
            }
            if (tmpw < -(128.0f - epsilon)) {
                tmpw = -(128.0f - epsilon);
            } else if (tmpw > 128.0f - epsilon) {
                tmpw = 128.0f - epsilon;
            }
            sca_result[0] = 1.0f;
            sca_result[1] = tmpx;
            sca_result[2] = (tmpx > 0.0f) ? pow(tmpy, tmpw) : 0.0f;
            sca_result[3] = 1.0f;
            break;
        }
        case 0xd: { /* LG2 */
            float lg2 = log2(params[2][0]);
            for (comp_index = 0; comp_index < 4; comp_index++) {
                sca_result[comp_index] = lg2;
            }
            break;
        }
        case 0xe: { /* EX2 */
            float ex2 = exp2(params[2][0]);
            for (comp_index = 0; comp_index < 4; comp_index++) {
                sca_result[comp_index] = ex2;
            }
            break;
        }
        default:
            for (comp_index = 0; comp_index < 4; comp_index++) {
                sca_result[comp_index] = 0.5f;
            }
            if (!unknown_opcode_reported) {
                qemu_log_mask(LOG_UNIMP,
                    "geforce: vertex shader: unknown SCA opcode 0x%02x\n",
                    sca_op);
                unknown_opcode_reported = true;
            }
            break;
        }
        if (s->card_type == 0x20) {
            uint32_t dst_out_reg = (tokens[3] >> 3) & 0xf;
            uint32_t dst_vec_mask = (tokens[3] >> 24) & 0xf;
            uint32_t dst_sca_mask = (tokens[3] >> 16) & 0xf;
            uint32_t dst_out_mask = (tokens[3] >> 12) & 0xf;
            uint32_t dst_tmp_reg = (tokens[3] >> 20) & 0xf;
            bool dst_out_sca = (tokens[3] >> 2) & 1;
            for (comp_index = 0; comp_index < 4; comp_index++) {
                if (addr_write) {
                    if (comp_index == 0) {
                        addr_regs[0][0] = (int32_t)vec_result[0];
                    }
                } else if ((dst_vec_mask & (8 >> comp_index)) != 0) {
                    tmp_regs[dst_tmp_reg][comp_index] =
                        vec_result[comp_index];
                }
                if ((dst_sca_mask & (8 >> comp_index)) != 0) {
                    tmp_regs[paired_ops ? 1 : dst_tmp_reg][comp_index] =
                        sca_result[comp_index];
                }
                if ((dst_out_mask & (8 >> comp_index)) != 0) {
                    out[dst_out_reg][comp_index] =
                        dst_out_sca ? sca_result[comp_index]
                                    : vec_result[comp_index];
                }
            }
        } else if (s->card_type == 0x35) {
            uint32_t dst_out_reg = (tokens[3] >> 2) & 0x1f;
            uint32_t dst_tmp_reg = (tokens[0] >> 16) & 0xf;
            uint32_t dst_vec_out_mask = (tokens[3] >> 12) & 0xf;
            uint32_t dst_sca_out_mask = (tokens[3] >> 16) & 0xf;
            uint32_t dst_vec_tmp_mask = (tokens[3] >> 20) & 0xf;
            uint32_t dst_sca_tmp_mask = (tokens[3] >> 24) & 0xf;
            for (comp_index = 0; comp_index < 4; comp_index++) {
                if (dst_out_reg != 0x1f) {
                    bool sca_out = (dst_sca_out_mask &
                                    (8 >> comp_index)) != 0;
                    bool vec_out = (dst_vec_out_mask &
                                    (8 >> comp_index)) != 0;
                    if (sca_out) {
                        out[dst_out_reg & 0xf][comp_index] =
                            sca_result[comp_index];
                    }
                    if (vec_out) {
                        out[sca_out ? 0 : (dst_out_reg & 0xf)][comp_index] =
                            vec_result[comp_index];
                    }
                }
                if (dst_tmp_reg != 0xf) {
                    if ((dst_vec_tmp_mask & (8 >> comp_index)) != 0) {
                        if (addr_write) {
                            addr_regs[dst_tmp_reg & 1][comp_index] =
                                (int32_t)vec_result[comp_index];
                        } else {
                            tmp_regs[dst_tmp_reg][comp_index] =
                                vec_result[comp_index];
                        }
                    }
                }
                if ((dst_sca_tmp_mask & (8 >> comp_index)) != 0) {
                    if (paired_ops) {
                        tmp_regs[1][comp_index] = sca_result[comp_index];
                    } else if (dst_tmp_reg != 0xf) {
                        tmp_regs[dst_tmp_reg][comp_index] =
                            sca_result[comp_index];
                    }
                }
            }
        } else {
            uint32_t dst_out_reg = (tokens[3] >> 2) & 0x1f;
            uint32_t dst_vec_mask = (tokens[3] >> 13) & 0xf;
            uint32_t dst_sca_mask = (tokens[3] >> 17) & 0xf;
            uint32_t dst_tmp_vec = (tokens[0] >> 15) & 0x3f;
            uint32_t dst_tmp_sca = (tokens[3] >> 7) & 0x3f;
            bool dst_out_vec = (tokens[0] >> 30) & 1;
            for (comp_index = 0; comp_index < 4; comp_index++) {
                if ((dst_vec_mask & (8 >> comp_index)) != 0) {
                    if (dst_out_vec && dst_out_reg != 0x1f) {
                        out[dst_out_reg & 0xf][comp_index] =
                            vec_result[comp_index];
                    }
                    if (dst_tmp_vec != 0x3f) {
                        if (addr_write) {
                            addr_regs[dst_tmp_vec & 1][comp_index] =
                                (int32_t)vec_result[comp_index];
                        } else {
                            tmp_regs[dst_tmp_vec & 0x1f][comp_index] =
                                vec_result[comp_index];
                        }
                    }
                }
                if ((dst_sca_mask & (8 >> comp_index)) != 0) {
                    if (!dst_out_vec && dst_out_reg != 0x1f) {
                        out[dst_out_reg & 0xf][comp_index] =
                            sca_result[comp_index];
                    }
                    if (dst_tmp_sca != 0x3f) {
                        tmp_regs[dst_tmp_sca & 0x1f][comp_index] =
                            sca_result[comp_index];
                    }
                }
            }
        }
        if ((tokens[3] & 1) == 1) {
            break;
        }
    }
}

static float gf_rc_get_var(uint32_t cw, uint32_t shift, float regs[16][4],
                           uint32_t civ)
{
    uint32_t x = cw >> shift;
    uint32_t reg = x & 0xf;
    uint32_t pir = (x >> 4) & 1;
    uint32_t map = (x >> 5) & 7;
    uint32_t cir = pir ? 3 : civ;
    float value = regs[reg][cir];

    switch (map) {
    case 0: /* UNSIGNED_IDENTITY */
        return MAX(0.0f, value);
    case 1: /* UNSIGNED_INVERT */
        return 1.0f - MIN(MAX(value, 0.0f), 1.0f);
    case 2: /* EXPAND_NORMAL */
        return 2.0f * MAX(0.0f, value) - 1.0f;
    case 3: /* EXPAND_NEGATE */
        return -2.0f * MAX(0.0f, value) + 1.0f;
    case 4: /* HALF_BIAS_NORMAL */
        return MAX(0.0f, value) - 0.5f;
    case 5: /* HALF_BIAS_NEGATE */
        return -MAX(0.0f, value) + 0.5f;
    default:
    case 6: /* SIGNED_IDENTITY */
        return value;
    case 7: /* SIGNED_NEGATE */
        return -value;
    }
}

static void gf_d3d_register_combiners(GeForceState *s, gf_channel *ch,
                                      float regs[16][4], float out[4])
{
    uint32_t st, ci, civ;
    float final_vars[6][3];

    for (st = 0; st < ch->d3d_combiner_control_num_stages && st < 8; st++) {
        uint32_t icws[2] = {
            ch->d3d_combiner_color_icw[st],
            ch->d3d_combiner_alpha_icw[st]
        };
        float vars[4][4];
        uint32_t color_ocw, color_cd, color_ab, color_muxsum;
        bool color_cd_dot, color_ab_dot;
        uint32_t alpha_ocw, alpha_cd, alpha_ab, alpha_muxsum;

        if (icws[0] == 0 && icws[1] == 0) {
            continue;
        }
        for (ci = 0; ci < 4; ci++) {
            regs[1][ci] = ch->d3d_combiner_const_color[st][0][ci];
            regs[2][ci] = ch->d3d_combiner_const_color[st][1][ci];
        }
        for (civ = 0; civ < 4; civ++) {
            uint32_t icw = icws[civ == 3];
            vars[0][civ] = gf_rc_get_var(icw, 24, regs, civ);
            vars[1][civ] = gf_rc_get_var(icw, 16, regs, civ);
            vars[2][civ] = gf_rc_get_var(icw, 8, regs, civ);
            vars[3][civ] = gf_rc_get_var(icw, 0, regs, civ);
        }
        color_ocw = ch->d3d_combiner_color_ocw[st];
        color_cd = color_ocw & 0xf;
        color_ab = (color_ocw >> 4) & 0xf;
        color_muxsum = (color_ocw >> 8) & 0xf;
        color_cd_dot = (color_ocw & 0x00001000) != 0;
        color_ab_dot = (color_ocw & 0x00002000) != 0;
        if (color_ab != 0) {
            if (color_ab_dot) {
                float ab_dot = vars[0][0] * vars[1][0] +
                               vars[0][1] * vars[1][1] +
                               vars[0][2] * vars[1][2];
                for (ci = 0; ci < 3; ci++) {
                    regs[color_ab][ci] = ab_dot;
                }
            } else {
                for (ci = 0; ci < 3; ci++) {
                    regs[color_ab][ci] = vars[0][ci] * vars[1][ci];
                }
            }
        }
        if (color_cd != 0) {
            if (color_cd_dot) {
                float cd_dot = vars[2][0] * vars[3][0] +
                               vars[2][1] * vars[3][1] +
                               vars[2][2] * vars[3][2];
                for (ci = 0; ci < 3; ci++) {
                    regs[color_cd][ci] = cd_dot;
                }
            } else {
                for (ci = 0; ci < 3; ci++) {
                    regs[color_cd][ci] = vars[2][ci] * vars[3][ci];
                }
            }
        }
        if (color_muxsum != 0) {
            for (ci = 0; ci < 3; ci++) {
                regs[color_muxsum][ci] = vars[0][ci] * vars[1][ci] +
                                         vars[2][ci] * vars[3][ci];
            }
        }
        alpha_ocw = ch->d3d_combiner_alpha_ocw[st];
        alpha_cd = alpha_ocw & 0xf;
        alpha_ab = (alpha_ocw >> 4) & 0xf;
        alpha_muxsum = (alpha_ocw >> 8) & 0xf;
        if (alpha_ab != 0) {
            regs[alpha_ab][3] = vars[0][3] * vars[1][3];
        }
        if (alpha_cd != 0) {
            regs[alpha_cd][3] = vars[2][3] * vars[3][3];
        }
        if (alpha_muxsum != 0) {
            regs[alpha_muxsum][3] = vars[0][3] * vars[1][3] +
                                    vars[2][3] * vars[3][3];
        }
    }
    for (civ = 0; civ < 3; civ++) {
        final_vars[4][civ] = gf_rc_get_var(ch->d3d_combiner_final[1], 24,
                                           regs, civ);
        final_vars[5][civ] = gf_rc_get_var(ch->d3d_combiner_final[1], 16,
                                           regs, civ);
    }
    for (ci = 0; ci < 3; ci++) {
        regs[0xe][ci] = regs[5][ci] + regs[0xc][ci];
        regs[0xf][ci] = final_vars[4][ci] * final_vars[5][ci];
    }
    for (civ = 0; civ < 3; civ++) {
        final_vars[0][civ] = gf_rc_get_var(ch->d3d_combiner_final[0], 24,
                                           regs, civ);
        final_vars[1][civ] = gf_rc_get_var(ch->d3d_combiner_final[0], 16,
                                           regs, civ);
        final_vars[2][civ] = gf_rc_get_var(ch->d3d_combiner_final[0], 8,
                                           regs, civ);
        final_vars[3][civ] = gf_rc_get_var(ch->d3d_combiner_final[0], 0,
                                           regs, civ);
    }
    out[3] = gf_rc_get_var(ch->d3d_combiner_final[1], 8, regs, 2);
    for (civ = 0; civ < 3; civ++) {
        out[civ] = final_vars[0][civ] * final_vars[1][civ] +
                   (1.0f - final_vars[0][civ]) * final_vars[2][civ] +
                   final_vars[3][civ];
    }
}

static float gf_length(const float v[3])
{
    return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static float gf_normalize_inplace(float v[3])
{
    float l = gf_length(v);
    float scale = 1.0f / l;

    v[0] *= scale;
    v[1] *= scale;
    v[2] *= scale;
    return l;
}

static void gf_normalize_out(const float in[3], float out[3])
{
    float scale = 1.0f / gf_length(in);

    out[0] = in[0] * scale;
    out[1] = in[1] * scale;
    out[2] = in[2] * scale;
}

static bool gf_d3d_pixel_shader(GeForceState *s, gf_channel *ch,
                                float in[16][4], float tmp_regs16[][4],
                                float tmp_regs32[][4])
{
    static bool unknown_opcode_reported;
    uint32_t ps_offset = ch->d3d_shader_offset;
    uint32_t cc[4] = { 0 };

    for (;;) {
        uint32_t dst_word = gf_dma_read32(s, ch->d3d_shader_obj, ps_offset);
        uint32_t src_words[3];
        float cnst[4] = { 0 };
        float params[3][4];
        bool const_loaded = false;
        uint32_t cond;
        bool execute;
        int p, comp_index;

        ps_offset += 4;
        for (p = 0; p < 3; p++) {
            src_words[p] = gf_dma_read32(s, ch->d3d_shader_obj, ps_offset);
            ps_offset += 4;
        }
        for (p = 0; p < 3; p++) {
            uint32_t reg_type = src_words[p] & 3;
            uint32_t swizzle[4];
            bool negate;
            bool src_abs;
            if (reg_type == 2 && !const_loaded) {
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    cnst[comp_index] = gf_uint32_as_float(
                        gf_dma_read32(s, ch->d3d_shader_obj, ps_offset));
                    ps_offset += 4;
                }
                const_loaded = true;
            }
            for (int i = 0; i < 4; i++) {
                swizzle[i] = (src_words[p] >> (9 + i * 2)) & 3;
            }
            negate = (src_words[p] >> 17) & 1;
            src_abs = (p == 0 ? src_words[0] >> 29 : src_words[p] >> 18) & 1;
            for (comp_index = 0; comp_index < 4; comp_index++) {
                int comp_index_swizzle = swizzle[comp_index];
                if (reg_type == 0) {
                    uint32_t tmp_index = (src_words[p] >> 2) & 0x3f;
                    bool fp16 = (src_words[p] >> 8) & 1;
                    params[p][comp_index] = fp16 ?
                        tmp_regs16[tmp_index][comp_index_swizzle] :
                        tmp_regs32[tmp_index][comp_index_swizzle];
                } else if (reg_type == 1) {
                    uint32_t in_index = (dst_word >> 13) & 0xf;
                    params[p][comp_index] = in[in_index][comp_index_swizzle];
                } else if (reg_type == 2) {
                    params[p][comp_index] = cnst[comp_index_swizzle];
                } else { /* reg_type == 3 */
                    params[p][comp_index] = 0;
                }
                if (src_abs) {
                    params[p][comp_index] = fabs(params[p][comp_index]);
                }
                if (negate) {
                    params[p][comp_index] = -params[p][comp_index];
                }
            }
        }
        cond = (src_words[0] >> 18) & 7;
        if (cond == 7) {
            execute = true;
        } else {
            execute = false;
            for (int i = 0; i < 4; i++) {
                uint32_t cond_swizzle = (src_words[0] >> (21 + i * 2)) & 3;
                if ((cc[cond_swizzle] & cond) != 0) {
                    execute = true;
                    break;
                }
            }
        }
        if (execute) {
            uint32_t op = (dst_word >> 24) & 0x3f;
            float op_result[4] = { 0 };
            bool set_cc;
            bool no_dst;
            switch (op) {
            case 0: /* NOP */
                break;
            case 1: /* MOV */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = params[0][comp_index];
                }
                break;
            case 2: /* MUL */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = params[0][comp_index] *
                                            params[1][comp_index];
                }
                break;
            case 3: /* ADD */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = params[0][comp_index] +
                                            params[1][comp_index];
                }
                break;
            case 4: /* MAD */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = params[0][comp_index] *
                                            params[1][comp_index] +
                                            params[2][comp_index];
                }
                break;
            case 5: { /* DP3 */
                float dp3 = gf_dot3(params[0], params[1]);
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = dp3;
                }
                break;
            }
            case 6: { /* DP4 */
                float dp4 = gf_dot4(params[0], params[1]);
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = dp4;
                }
                break;
            }
            case 8: /* MIN */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = MIN(params[0][comp_index],
                                                params[1][comp_index]);
                }
                break;
            case 9: /* MAX */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = MAX(params[0][comp_index],
                                                params[1][comp_index]);
                }
                break;
            case 0xa: /* SLT */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] =
                        params[0][comp_index] < params[1][comp_index] ? 1.0f
                                                                      : 0.0f;
                }
                break;
            case 0xb: /* SGE */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] =
                        params[0][comp_index] >= params[1][comp_index] ?
                            1.0f : 0.0f;
                }
                break;
            case 0xc: /* SLE */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] =
                        params[0][comp_index] <= params[1][comp_index] ?
                            1.0f : 0.0f;
                }
                break;
            case 0xd: /* SGT */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] =
                        params[0][comp_index] > params[1][comp_index] ? 1.0f
                                                                      : 0.0f;
                }
                break;
            case 0xe: /* SNE */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] =
                        params[0][comp_index] != params[1][comp_index] ?
                            1.0f : 0.0f;
                }
                break;
            case 0xf: /* SEQ */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] =
                        params[0][comp_index] == params[1][comp_index] ?
                            1.0f : 0.0f;
                }
                break;
            case 0x10: /* FRC */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = params[0][comp_index] -
                                            floor(params[0][comp_index]);
                }
                break;
            case 0x11: /* FLR */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = floor(params[0][comp_index]);
                }
                break;
            case 0x12: /* KIL */
                return true;
            case 0x18: { /* TXP */
                float winv = 1.0f / params[0][3];
                params[0][0] *= winv;
                params[0][1] *= winv;
                params[0][2] *= winv;
            }
                /* fallthrough */
            case 0x2f: /* TXL: level of detail parameter not implemented */
            case 0x31: /* TXB: bias parameter not implemented */
            case 0x17: { /* TEX */
                uint32_t tex_unit = (dst_word >> 17) & 0xf;
                gf_texture *tex = &ch->d3d_texture[tex_unit];
                gf_d3d_sample_texture(s, ch, tex, params[0], op_result);
                if (((dst_word >> 21) & 1) != 0) {
                    for (comp_index = 0; comp_index < 4; comp_index++) {
                        op_result[comp_index] =
                            op_result[comp_index] * 2.0f - 1.0f;
                    }
                }
                break;
            }
            case 0x1a: { /* RCP */
                float rcp = 1.0f / params[0][0];
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = rcp;
                }
                break;
            }
            case 0x1c: { /* EX2 */
                float ex2 = exp2(params[0][0]);
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = ex2;
                }
                break;
            }
            case 0x1d: { /* LG2 */
                float lg2 = log2(params[0][0]);
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = lg2;
                }
                break;
            }
            case 0x1f: /* LRP */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = params[0][comp_index] *
                        params[1][comp_index] +
                        (1.0f - params[0][comp_index]) *
                        params[2][comp_index];
                }
                break;
            case 0x22: { /* COS */
                float cosv = cos(params[0][0]);
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = cosv;
                }
                break;
            }
            case 0x23: { /* SIN */
                float sinv = sin(params[0][0]);
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = sinv;
                }
                break;
            }
            case 0x26: { /* POW */
                float powv = pow(params[0][0], params[1][0]);
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = powv;
                }
                break;
            }
            case 0x2e: { /* DP2A */
                float dp2a = 0.0f;
                for (comp_index = 0; comp_index < 2; comp_index++) {
                    dp2a += params[0][comp_index] * params[1][comp_index];
                }
                dp2a += params[2][0];
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = dp2a;
                }
                break;
            }
            case 0x34: /* TXPBEM */
                params[0][0] /= params[0][3];
                params[0][1] /= params[0][3];
                /* fallthrough */
            case 0x33: { /* TEXBEM */
                float coords[3];
                uint32_t tex_unit = (dst_word >> 17) & 0xf;
                gf_texture *tex = &ch->d3d_texture[tex_unit];
                coords[0] = params[0][0] + params[1][0] * params[2][0] +
                            params[1][1] * params[2][1];
                coords[1] = params[0][1] + params[1][0] * params[2][2] +
                            params[1][1] * params[2][3];
                coords[2] = 0.0f;
                gf_d3d_sample_texture(s, ch, tex, coords, op_result);
                if (((dst_word >> 21) & 1) != 0) {
                    for (comp_index = 0; comp_index < 4; comp_index++) {
                        op_result[comp_index] =
                            op_result[comp_index] * 2.0f - 1.0f;
                    }
                }
                break;
            }
            case 0x36: /* RFL */
                gf_reflection(params[0], params[1], op_result);
                op_result[3] = 0.0f; /* ignored */
                break;
            case 0x38: { /* DP2 */
                float dp2 = 0.0f;
                for (comp_index = 0; comp_index < 2; comp_index++) {
                    dp2 += params[0][comp_index] * params[1][comp_index];
                }
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = dp2;
                }
                break;
            }
            case 0x39: /* NRM */
                gf_normalize_out(params[0], op_result);
                op_result[3] = 0.0f;
                break;
            case 0x3a: /* DIV */
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = params[0][comp_index] /
                                            params[1][0];
                }
                break;
            default:
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    op_result[comp_index] = 0.5f;
                }
                if (!unknown_opcode_reported) {
                    qemu_log_mask(LOG_UNIMP,
                        "geforce: pixel shader: unknown opcode 0x%02x\n", op);
                    unknown_opcode_reported = true;
                }
                break;
            }
            set_cc = (dst_word >> 8) & 1;
            if (set_cc) {
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    if (op_result[comp_index] < 0.0f) {
                        cc[comp_index] = 1;
                    } else if (op_result[comp_index] == 0.0f) {
                        cc[comp_index] = 2;
                    } else {
                        cc[comp_index] = 4;
                    }
                }
            }
            no_dst = (dst_word >> 30) & 1;
            if (op != 0 && !no_dst) {
                uint32_t mask = (dst_word >> 9) & 0xf;
                uint32_t dst_tmp_reg = (dst_word >> 1) & 0x3f;
                static const float dst_scales[] = {
                    1.0f, 2.0f, 4.0f, 8.0f, 1.0f, 0.5f, 0.25f, 0.125f
                };
                uint32_t dst_scale = (src_words[1] >> 28) & 7;
                bool dst_fp16 = (dst_word >> 7) & 1;
                bool saturate = (dst_word >> 31) & 1;
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    if ((mask & (1 << comp_index)) != 0) {
                        float value = op_result[comp_index] *
                                      dst_scales[dst_scale];
                        if (saturate) {
                            if (value < 0.0f) {
                                value = 0.0f;
                            } else if (value > 1.0f) {
                                value = 1.0f;
                            }
                        }
                        if (dst_fp16) {
                            tmp_regs16[dst_tmp_reg][comp_index] = value;
                        } else {
                            tmp_regs32[dst_tmp_reg][comp_index] = value;
                        }
                    }
                }
            }
        }
        if ((dst_word & 1) == 1) {
            break;
        }
    }
    return false;
}

static float gf_blend_equation(uint16_t equation, float src,
                               float src_factor, float dst, float dst_factor)
{
    switch (equation) {
    case 0x0001: /* ADD */
    case 0x8006: /* FUNC_ADD */
    default:
        return src * src_factor + dst * dst_factor;
    case 0x0002: /* SUBTRACT */
    case 0x800a: /* FUNC_SUBTRACT */
        return src * src_factor - dst * dst_factor;
    case 0x0003: /* REV_SUBTRACT */
    case 0x800b: /* FUNC_REVERSE_SUBTRACT */
        return dst * dst_factor - src * src_factor;
    case 0x0004: /* MIN */
    case 0x8007: /* MIN */
        return MIN(src, dst);
    case 0x0005: /* MAX */
    case 0x8008: /* MAX */
        return MAX(src, dst);
    }
}

static float gf_blend_factor(uint16_t factor, float src_rgb, float src_a,
                             float dst_rgb, float dst_a, float const_rgb,
                             float const_a)
{
    switch (factor) {
    case 0x0000: /* ZERO */
    case 0x1001: /* ZERO */
        return 0.0f;
    case 0x0001: /* ONE */
    case 0x1002: /* ONE */
        return 1.0f;
    case 0x0300: /* SRC_COLOR */
    case 0x1003: /* SRC_COLOR */
        return src_rgb;
    case 0x0301: /* ONE_MINUS_SRC_COLOR */
    case 0x1004: /* INV_SRC_COLOR */
        return 1.0f - src_rgb;
    case 0x0302: /* SRC_ALPHA */
    case 0x1005: /* SRC_ALPHA */
        return src_a;
    case 0x0303: /* ONE_MINUS_SRC_ALPHA */
    case 0x1006: /* INV_SRC_ALPHA */
        return 1.0f - src_a;
    case 0x0304: /* DST_ALPHA */
    case 0x1007: /* DEST_ALPHA */
        return dst_a;
    case 0x0305: /* ONE_MINUS_DST_ALPHA */
    case 0x1008: /* INV_DEST_ALPHA */
        return 1.0f - dst_a;
    case 0x0306: /* DST_COLOR */
    case 0x1009: /* DEST_COLOR */
        return dst_rgb;
    case 0x0307: /* ONE_MINUS_DST_COLOR */
    case 0x100a: /* INV_DEST_COLOR */
        return 1.0f - dst_rgb;
    case 0x0308: /* SRC_ALPHA_SATURATE */
    case 0x100b: /* SRC_ALPHA_SAT */
        return MIN(src_a, 1.0f - dst_a);
    case 0x8001: /* CONSTANT_COLOR */
    case 0x100e: /* BLEND_FACTOR */
        return const_rgb;
    case 0x8002: /* ONE_MINUS_CONSTANT_COLOR */
    case 0x100f: /* INV_BLEND_FACTOR */
        return 1.0f - const_rgb;
    case 0x8003: /* CONSTANT_ALPHA */
        return const_a;
    case 0x8004: /* ONE_MINUS_CONSTANT_ALPHA */
        return 1.0f - const_a;
    default:
        return 0.5f;
    }
}

static bool gf_compare(uint32_t func, uint32_t val1, uint32_t val2)
{
    switch (func) {
    case 1:
    case 0x200: /* NEVER */
        return false;
    case 2:
    case 0x201: /* LESS */
    default:
        return val1 < val2;
    case 3:
    case 0x202: /* EQUAL */
        return val1 == val2;
    case 4:
    case 0x203: /* LEQUAL */
        return val1 <= val2;
    case 5:
    case 0x204: /* GREATER */
        return val1 > val2;
    case 6:
    case 0x205: /* NOTEQUAL */
        return val1 != val2;
    case 7:
    case 0x206: /* GEQUAL */
        return val1 >= val2;
    case 8:
    case 0x207: /* ALWAYS */
        return true;
    }
}

static void gf_position_to_view3(gf_channel *ch, const float p[4],
                                 float pt[3])
{
    float *m = ch->d3d_model_view_matrix[0];

    pt[0] = p[0] * m[0]  + p[1] * m[1]  + p[2] * m[2]  + p[3] * m[3];
    pt[1] = p[0] * m[4]  + p[1] * m[5]  + p[2] * m[6]  + p[3] * m[7];
    pt[2] = p[0] * m[8]  + p[1] * m[9]  + p[2] * m[10] + p[3] * m[11];
}

static void gf_position_to_view4(gf_channel *ch, const float p[4],
                                 float pt[4])
{
    float *m = ch->d3d_model_view_matrix[0];

    pt[0] = p[0] * m[0]  + p[1] * m[1]  + p[2] * m[2]  + p[3] * m[3];
    pt[1] = p[0] * m[4]  + p[1] * m[5]  + p[2] * m[6]  + p[3] * m[7];
    pt[2] = p[0] * m[8]  + p[1] * m[9]  + p[2] * m[10] + p[3] * m[11];
    pt[3] = p[0] * m[12] + p[1] * m[13] + p[2] * m[14] + p[3] * m[15];
}

static void gf_normal_to_view(gf_channel *ch, const float n[3], float nt[3])
{
    float *m = ch->d3d_inverse_model_view_matrix;

    nt[0] = n[0] * m[0] + n[1] * m[1] + n[2] * m[2];
    nt[1] = n[0] * m[4] + n[1] * m[5] + n[2] * m[6];
    nt[2] = n[0] * m[8] + n[1] * m[9] + n[2] * m[10];
    if (ch->d3d_normalize_enable) {
        gf_normalize_inplace(nt);
    }
}

static void gf_d3d_clip_to_screen(GeForceState *s, gf_channel *ch,
                                  const float pos_clip[4],
                                  float pos_screen[4])
{
    pos_screen[3] = 1.0f / pos_clip[3];
    if (((ch->d3d_transform_execution_mode & 3) != 0 &&
         (ch->d3d_transform_execution_mode & 0x100) == 0 &&
         s->card_type >= 0x35) ||
        (ch->d3d_transform_execution_mode & 3) == 0) {
        for (int i = 0; i < 3; i++) {
            pos_screen[i] = pos_clip[i] * pos_screen[3];
            if ((ch->d3d_view_matrix_enable & 1) != 0) {
                pos_screen[i] *= ch->d3d_model_view_matrix[1][i];
                pos_screen[i] += ch->d3d_model_view_matrix[1][i + 4];
            } else {
                if (s->card_type > 0x20) {
                    pos_screen[i] *= ch->d3d_viewport_scale[i];
                }
                pos_screen[i] += ch->d3d_viewport_offset[i];
            }
        }
        pos_screen[0] += ch->d3d_window_offset_x;
        pos_screen[1] += ch->d3d_window_offset_y;
    } else {
        for (int i = 0; i < 3; i++) {
            pos_screen[i] = pos_clip[i];
        }
    }
}

static void gf_d3d_triangle_clipped(GeForceState *s, gf_channel *ch,
                                    float v0[16][4], float v1[16][4],
                                    float v2[16][4])
{
    float sp0[4], sp1[4], sp2[4];
    double b012;
    bool front_face_cw;
    bool clockwise;
    bool front_face;
    uint32_t surf_x1, surf_y1, surf_x2, surf_y2;
    int32_t tri_x1, tri_y1, tri_x2, tri_y2;
    uint32_t draw_x1, draw_y1, draw_x2, draw_y2;
    uint32_t draw_width, draw_height;
    uint32_t pitch, pitch_zeta;
    uint32_t draw_offset, draw_offset_zeta, redraw_offset;
    bool interpolate[16];
    float ps_in[16][4];
    float rc_regs[16][4];
    float fog_factor = 1.0f;
    float xy[2];
    double b012inv;
    bool stencil_test_enable;
    bool zstencil_enable;
    bool ps_enable;
    bool rc_enable;
    float ps_tmp_regs16[64][4];
    float ps_tmp_regs32[64][4];
    float (*ps_tmp_regs_exp)[4] = ps_tmp_regs16;
    uint32_t i;
    uint16_t x, y;
    int a, comp_index;

    gf_d3d_clip_to_screen(s, ch, v0[0], sp0);
    gf_d3d_clip_to_screen(s, ch, v1[0], sp1);
    gf_d3d_clip_to_screen(s, ch, v2[0], sp2);
    b012 = gf_edge_function(sp0, sp1, sp2);
    front_face_cw = ch->d3d_front_face == 0x00000900;
    clockwise = b012 > 0.0;
    front_face = (clockwise != ch->d3d_triangle_flip) == front_face_cw;
    if (ch->d3d_cull_face_enable) {
        if ((ch->d3d_cull_face == 0x00000405 && !front_face) ||
            (ch->d3d_cull_face == 0x00000404 && front_face) ||
            (ch->d3d_cull_face == 0x00000408)) {
            return;
        }
    }
    if (b012 == 0.0) {
        return;
    }
    surf_x1 = ch->d3d_clip_horizontal & 0xFFFF;
    surf_y1 = ch->d3d_clip_vertical & 0xFFFF;
    surf_x2 = surf_x1 + (ch->d3d_clip_horizontal >> 16);
    surf_y2 = surf_y1 + (ch->d3d_clip_vertical >> 16);
    tri_x1 = MIN(MIN(sp0[0], sp1[0]), sp2[0]);
    tri_y1 = MIN(MIN(sp0[1], sp1[1]), sp2[1]);
    tri_x2 = MAX(MAX(sp0[0], sp1[0]), sp2[0]);
    tri_y2 = MAX(MAX(sp0[1], sp1[1]), sp2[1]);
    draw_x1 = MIN(MAX(tri_x1, (int32_t)surf_x1), (int32_t)surf_x2);
    draw_y1 = MIN(MAX(tri_y1, (int32_t)surf_y1), (int32_t)surf_y2);
    draw_x2 = MIN(MAX(tri_x2 + 1, (int32_t)surf_x1), (int32_t)surf_x2);
    draw_y2 = MIN(MAX(tri_y2 + 1, (int32_t)surf_y1), (int32_t)surf_y2);
    if (draw_x2 < draw_x1 || draw_y2 < draw_y1) {
        return; /* overflow */
    }
    draw_width = draw_x2 - draw_x1;
    draw_height = draw_y2 - draw_y1;
    if (!gf_d3d_window_clip(s, ch, &draw_x1, &draw_y1, &draw_width,
                            &draw_height)) {
        return;
    }
    if (!gf_d3d_viewport_clip(s, ch, &draw_x1, &draw_y1, &draw_width,
                              &draw_height)) {
        return;
    }
    if (!gf_d3d_scissor_clip(s, ch, &draw_x1, &draw_y1, &draw_width,
                             &draw_height)) {
        return;
    }
    pitch = ch->d3d_surface_pitch_a & 0xFFFF;
    pitch_zeta = gf_d3d_get_surface_pitch_z(s, ch);
    draw_offset = ch->d3d_surface_color_offset +
        draw_y1 * pitch + draw_x1 * ch->d3d_color_bytes;
    draw_offset_zeta = ch->d3d_surface_zeta_offset +
        draw_y1 * pitch_zeta + draw_x1 * ch->d3d_depth_bytes;
    redraw_offset = gf_dma_lin_lookup(s, ch->d3d_color_obj, draw_offset);
    for (a = 0; a < 16; a++) {
        bool result = false;
        for (comp_index = 0; comp_index < 4; comp_index++) {
            result |= v0[a][comp_index] != v1[a][comp_index];
            result |= v1[a][comp_index] != v2[a][comp_index];
        }
        interpolate[a] = result;
    }
    ps_in[3][1] = fog_factor;
    rc_regs[3][3] = fog_factor;
    for (i = 0; i < 3; i++) {
        rc_regs[3][i] = ch->d3d_fog_color[i];
    }
    for (i = 0; i < 2; i++) {
        if (!interpolate[ch->d3d_attrib_out_color[i]]) {
            for (comp_index = 0; comp_index < 4; comp_index++) {
                ps_in[i + 1][comp_index] =
                    v0[ch->d3d_attrib_out_color[i]][comp_index];
            }
        }
    }
    for (i = 0; i < ch->d3d_tex_coord_count; i++) {
        if (!interpolate[ch->d3d_attrib_out_tex_coord[i]]) {
            for (comp_index = 0; comp_index < 4; comp_index++) {
                ps_in[i + 4][comp_index] =
                    v0[ch->d3d_attrib_out_tex_coord[i]][comp_index];
            }
        }
    }
    xy[1] = draw_y1 + 0.5f;
    b012inv = 1.0 / b012;
    stencil_test_enable = ch->d3d_stencil_test_enable &&
                          ch->d3d_depth_bytes != 2;
    zstencil_enable = ch->d3d_depth_test_enable || stencil_test_enable;
    ps_enable = ch->d3d_shader_obj != 0;
    rc_enable = ch->d3d_combiner_control_num_stages != 0;
    if (ps_enable && ((ch->d3d_shader_control & 0x00000040) != 0)) {
        ps_tmp_regs_exp = ps_tmp_regs32;
    }
    for (y = 0; y < draw_height; y++, xy[1]++) {
        xy[0] = draw_x1 + 0.5f;
        for (x = 0; x < draw_width; x++, xy[0]++) {
            double b0, b1, b2;
            float z;
            uint32_t z_new = 0;
            uint8_t stencil = 0x00;
            float aa, rr, gg, bb;

            b0 = gf_edge_function(sp1, sp2, xy);
            if (clockwise) {
                if (b0 < 0.0) {
                    continue;
                }
            } else {
                if (b0 > 0.0) {
                    continue;
                }
            }
            b1 = gf_edge_function(sp2, sp0, xy);
            if (clockwise) {
                if (b1 < 0.0) {
                    continue;
                }
            } else {
                if (b1 > 0.0) {
                    continue;
                }
            }
            b2 = gf_edge_function(sp0, sp1, xy);
            if (clockwise) {
                if (b2 < 0.0) {
                    continue;
                }
            } else {
                if (b2 > 0.0) {
                    continue;
                }
            }
            b0 *= b012inv;
            b1 *= b012inv;
            b2 *= b012inv;
            z = sp0[2] * b0 + sp1[2] * b1 + sp2[2] * b2;
            if (z > ch->d3d_clip_max) {
                continue;
            }
            if (zstencil_enable) {
                uint32_t z_prev;
                bool depth_test_pass;
                if (ch->d3d_depth_bytes == 2) {
                    z_prev = gf_dma_read16(s, ch->d3d_zeta_obj,
                                           draw_offset_zeta + x * 2);
                } else {
                    uint32_t zstencil = gf_dma_read32(s, ch->d3d_zeta_obj,
                                                      draw_offset_zeta +
                                                      x * 4);
                    z_prev = zstencil >> 8;
                    stencil = (uint8_t)zstencil;
                }
                if (ch->d3d_depth_test_enable) {
                    if (s->card_type <= 0x20) {
                        z_new = z;
                    } else if (ch->d3d_depth_bytes == 2) {
                        z_new = z * 65535.0f;
                    } else {
                        z_new = z * 16777215.0f;
                    }
                    depth_test_pass = gf_compare(ch->d3d_depth_func, z_new,
                                                 z_prev);
                } else {
                    depth_test_pass = true;
                }
                if (stencil_test_enable) {
                    bool stencil_test_pass = gf_compare(ch->d3d_stencil_func,
                        ch->d3d_stencil_func_ref & ch->d3d_stencil_func_mask,
                        stencil & ch->d3d_stencil_func_mask);
                    uint32_t stencil_op;
                    if (stencil_test_pass) {
                        if (depth_test_pass) {
                            stencil_op = ch->d3d_stencil_op_dppass;
                        } else {
                            stencil_op = ch->d3d_stencil_op_dpfail;
                        }
                    } else {
                        stencil_op = ch->d3d_stencil_op_sfail;
                    }
                    switch (stencil_op) {
                    case 0x1e00: /* KEEP */
                    default:
                        break;
                    case 0x0000: /* ZERO */
                        stencil = 0x00;
                        break;
                    case 0x1e01: /* REPLACE */
                        stencil = ch->d3d_stencil_func_ref;
                        break;
                    case 0x1e02: /* INCRSAT */
                        if (stencil < 0xff) {
                            stencil++;
                        }
                        break;
                    case 0x1e03: /* DECRSAT */
                        if (stencil > 0x00) {
                            stencil--;
                        }
                        break;
                    case 0x150a: /* INVERT */
                        stencil = ~stencil;
                        break;
                    case 0x8507: /* INCR */
                        stencil++;
                        break;
                    case 0x8508: /* DECR */
                        stencil--;
                        break;
                    }
                    if (stencil_op != 0x1e00) {
                        stencil &= ch->d3d_stencil_mask;
                        gf_dma_write8(s, ch->d3d_zeta_obj,
                                      draw_offset_zeta + x * 4, stencil);
                    }
                    if (!stencil_test_pass) {
                        continue;
                    }
                }
                if (!depth_test_pass) {
                    continue;
                }
            }
            ps_in[0][3] = sp0[3] * b0 + sp1[3] * b1 + sp2[3] * b2;
            b0 *= sp0[3] / ps_in[0][3];
            b1 *= sp1[3] / ps_in[0][3];
            b2 *= sp2[3] / ps_in[0][3];
            for (i = 0; i < 2; i++) {
                if (interpolate[ch->d3d_attrib_out_color[i]]) {
                    for (comp_index = 0; comp_index < 4; comp_index++) {
                        ps_in[i + 1][comp_index] =
                            v0[ch->d3d_attrib_out_color[i]][comp_index] * b0 +
                            v1[ch->d3d_attrib_out_color[i]][comp_index] * b1 +
                            v2[ch->d3d_attrib_out_color[i]][comp_index] * b2;
                    }
                }
            }
            for (i = 0; i < ch->d3d_tex_coord_count; i++) {
                if (interpolate[ch->d3d_attrib_out_tex_coord[i]]) {
                    for (comp_index = 0; comp_index < 4; comp_index++) {
                        ps_in[i + 4][comp_index] =
                            v0[ch->d3d_attrib_out_tex_coord[i]][comp_index] *
                                b0 +
                            v1[ch->d3d_attrib_out_tex_coord[i]][comp_index] *
                                b1 +
                            v2[ch->d3d_attrib_out_tex_coord[i]][comp_index] *
                                b2;
                    }
                }
            }
            for (comp_index = 0; comp_index < 4; comp_index++) {
                ps_tmp_regs16[0][comp_index] = ps_in[1][comp_index];
            }
            if (ch->d3d_fog_enable) {
                float fog_dist =
                    v0[ch->d3d_attrib_out_fogc][0] * b0 +
                    v1[ch->d3d_attrib_out_fogc][0] * b1 +
                    v2[ch->d3d_attrib_out_fogc][0] * b2;
                switch (ch->d3d_fog_mode) {
                case 0x2601: /* LINEAR */
                    fog_factor = ch->d3d_fog_params[1] * fog_dist +
                                 ch->d3d_fog_params[0] - 1.0f;
                    break;
                case 0x804:  /* LINEAR_ABS */
                    fog_factor = ch->d3d_fog_params[1] * fabs(fog_dist) +
                                 ch->d3d_fog_params[0] - 1.0f;
                    break;
                case 0x800:  /* EXP */
                    fog_factor = exp2(16.0f * (ch->d3d_fog_params[1] *
                        fog_dist + ch->d3d_fog_params[0] - 1.5f));
                    break;
                case 0x802:  /* EXP_ABS */
                    fog_factor = exp2(16.0f * (ch->d3d_fog_params[1] *
                        fabs(fog_dist) + ch->d3d_fog_params[0] - 1.5f));
                    break;
                case 0x801:  /* EXP2 */
                    fog_factor = exp(-pow(4.709f * (ch->d3d_fog_params[1] *
                        fog_dist + ch->d3d_fog_params[0] - 1.5f), 2.0f));
                    break;
                case 0x803:  /* EXP2_ABS */
                    fog_factor = exp(-pow(4.709f * (ch->d3d_fog_params[1] *
                        fabs(fog_dist) + ch->d3d_fog_params[0] - 1.5f),
                        2.0f));
                    break;
                default:     /* not implemented */
                    fog_factor = 0.5f;
                    break;
                }
                if (fog_factor < 0.0f) {
                    fog_factor = 0.0f;
                }
                if (fog_factor > 1.0f) {
                    fog_factor = 1.0f;
                }
                if (ps_enable) {
                    ps_in[3][1] = fog_factor;
                }
                if (rc_enable) {
                    rc_regs[3][3] = fog_factor;
                }
            }
            if (ps_enable) {
                ps_in[0][0] = xy[0] - ch->d3d_window_offset_x;
                ps_in[0][1] = ch->d3d_viewport_height -
                              (xy[1] - ch->d3d_window_offset_y);
                ps_in[0][2] = 0.0f;
                /* eye-ray vector for texm3x3vspec */
                ps_in[15][0] = ps_in[5][3];
                ps_in[15][1] = ps_in[6][3];
                ps_in[15][2] = ps_in[7][3];
                if (gf_d3d_pixel_shader(s, ch, ps_in,
                                        rc_enable ? &rc_regs[8]
                                                  : ps_tmp_regs16,
                                        ps_tmp_regs32)) {
                    continue;
                }
            }
            if (rc_enable) {
                for (i = 0; i < 4; i++) {
                    rc_regs[0][i] = 0.0f;
                    rc_regs[4][i] = ps_in[1][i];
                    rc_regs[5][i] = ps_in[2][i];
                }
                rc_regs[0xe][3] = 0.0f;
                rc_regs[0xf][3] = 0.0f;
                if (!ps_enable) {
                    float uv[2] = { 0.0f, 0.0f };
                    uint32_t t;
                    for (t = 0; t < ch->d3d_tex_coord_count; t++) {
                        switch (ch->d3d_tex_shader_op[t & 3]) {
                        case 0x00:   /* NONE */
                            break;
                        case 0x01:   /* PROJECT2D */
                        case 0x03: { /* CUBEMAP */
                            gf_texture *tex = &ch->d3d_texture[t];
                            gf_d3d_sample_texture(s, ch, tex, ps_in[4 + t],
                                                  rc_regs[8 + t]);
                            break;
                        }
                        case 0x06: { /* BUMPENVMAP */
                            float *in_coords = ps_in[4 + t];
                            float *prev_color =
                                rc_regs[8 + ch->d3d_tex_shader_previous[t &
                                                                        3]];
                            float coords[3];
                            gf_texture *tex = &ch->d3d_texture[t];
                            coords[0] = in_coords[0] / in_coords[3] +
                                tex->offset_matrix[0] * prev_color[2] +
                                tex->offset_matrix[3] * prev_color[1];
                            coords[1] = in_coords[1] / in_coords[3] +
                                tex->offset_matrix[1] * prev_color[2] +
                                tex->offset_matrix[2] * prev_color[1];
                            coords[2] = 0.0f;
                            gf_d3d_sample_texture(s, ch, tex, coords,
                                                  rc_regs[8 + t]);
                            break;
                        }
                        case 0x0c: { /* DOT_RFLCT_SPEC */
                            float *input_tex =
                                rc_regs[8 + ch->d3d_tex_shader_previous[t &
                                                                        3]];
                            float w = gf_dot3_map(ps_in[4 + t], input_tex,
                                ch->d3d_tex_shader_dotmapping[t & 3]);
                            float n[3] = { uv[0], uv[1], w };
                            float e[3] = { ps_in[4 + 1][3], ps_in[4 + 2][3],
                                           ps_in[4 + 3][3] };
                            float rv[3];
                            gf_texture *tex = &ch->d3d_texture[t];
                            gf_reflection(n, e, rv);
                            gf_d3d_sample_texture(s, ch, tex, rv,
                                                  rc_regs[8 + t]);
                            break;
                        }
                        case 0x11: { /* DOTPRODUCT */
                            float *input_tex =
                                rc_regs[8 + ch->d3d_tex_shader_previous[t &
                                                                        3]];
                            uv[t == 1 ? 0 : 1] =
                                gf_dot3_map(ps_in[4 + t], input_tex,
                                    ch->d3d_tex_shader_dotmapping[t & 3]);
                            break;
                        }
                        default: {   /* not implemented */
                            float *color = rc_regs[8 + t];
                            color[0] = 0.0f;
                            color[1] = 0.5f;
                            color[2] = 0.5f;
                            color[3] = 1.0f;
                            break;
                        }
                        }
                    }
                }
                gf_d3d_register_combiners(s, ch, rc_regs, ps_tmp_regs_exp[0]);
            }
            aa = MIN(MAX(ps_tmp_regs_exp[0][3], 0.0f), 1.0f);
            if (ch->d3d_alpha_test_enable) {
                if (!gf_compare(ch->d3d_alpha_func, (uint32_t)(aa * 255.0f),
                                ch->d3d_alpha_ref)) {
                    continue;
                }
            }
            rr = MIN(MAX(ps_tmp_regs_exp[0][0], 0.0f), 1.0f);
            gg = MIN(MAX(ps_tmp_regs_exp[0][1], 0.0f), 1.0f);
            bb = MIN(MAX(ps_tmp_regs_exp[0][2], 0.0f), 1.0f);
            if (ch->d3d_blend_enable) {
                float sr = rr;
                float sg = gg;
                float sb = bb;
                float sa = aa;
                float dr, dg, db, da;
                if (ch->d3d_color_bytes == 2) {
                    uint16_t color = gf_dma_read16(s, ch->d3d_color_obj,
                                                   draw_offset + x * 2);
                    dr = ((color >> 11) & 0x1f) / 31.0f;
                    dg = ((color >> 5) & 0x3f) / 63.0f;
                    db = ((color >> 0) & 0x1f) / 31.0f;
                    da = 1.0f;
                } else if (ch->d3d_color_bytes == 4) {
                    uint32_t color = gf_dma_read32(s, ch->d3d_color_obj,
                                                   draw_offset + x * 4);
                    dr = ((color >> 16) & 0xff) / 255.0f;
                    dg = ((color >> 8) & 0xff) / 255.0f;
                    db = ((color >> 0) & 0xff) / 255.0f;
                    da = ((color >> 24) & 0xff) / 255.0f;
                } else {
                    uint8_t color = gf_dma_read8(s, ch->d3d_color_obj,
                                                 draw_offset + x);
                    dr = 0.0f;
                    dg = 0.0f;
                    db = color / 255.0f;
                    da = 1.0f;
                }
                rr = gf_blend_equation(ch->d3d_blend_equation_rgb,
                    sr, gf_blend_factor(ch->d3d_blend_sfactor_rgb, sr, sa,
                                        dr, da, ch->d3d_blend_color[0],
                                        ch->d3d_blend_color[3]),
                    dr, gf_blend_factor(ch->d3d_blend_dfactor_rgb, sr, sa,
                                        dr, da, ch->d3d_blend_color[0],
                                        ch->d3d_blend_color[3]));
                gg = gf_blend_equation(ch->d3d_blend_equation_rgb,
                    sg, gf_blend_factor(ch->d3d_blend_sfactor_rgb, sg, sa,
                                        dg, da, ch->d3d_blend_color[1],
                                        ch->d3d_blend_color[3]),
                    dg, gf_blend_factor(ch->d3d_blend_dfactor_rgb, sg, sa,
                                        dg, da, ch->d3d_blend_color[1],
                                        ch->d3d_blend_color[3]));
                bb = gf_blend_equation(ch->d3d_blend_equation_rgb,
                    sb, gf_blend_factor(ch->d3d_blend_sfactor_rgb, sb, sa,
                                        db, da, ch->d3d_blend_color[2],
                                        ch->d3d_blend_color[3]),
                    db, gf_blend_factor(ch->d3d_blend_dfactor_rgb, sb, sa,
                                        db, da, ch->d3d_blend_color[2],
                                        ch->d3d_blend_color[3]));
                aa = gf_blend_equation(ch->d3d_blend_equation_alpha,
                    sa, gf_blend_factor(ch->d3d_blend_sfactor_alpha, sa, sa,
                                        da, da, ch->d3d_blend_color[3],
                                        ch->d3d_blend_color[3]),
                    da, gf_blend_factor(ch->d3d_blend_dfactor_alpha, sa, sa,
                                        da, da, ch->d3d_blend_color[3],
                                        ch->d3d_blend_color[3]));
                rr = MIN(MAX(rr, 0.0f), 1.0f);
                gg = MIN(MAX(gg, 0.0f), 1.0f);
                bb = MIN(MAX(bb, 0.0f), 1.0f);
                aa = MIN(MAX(aa, 0.0f), 1.0f);
            }
            if (ch->d3d_color_mask != 0) {
                if (ch->d3d_color_bytes == 2) {
                    uint8_t r5 = rr * 31.0f + 0.5f;
                    uint8_t g6 = gg * 63.0f + 0.5f;
                    uint8_t b5 = bb * 31.0f + 0.5f;
                    uint16_t color = b5 << 0 | g6 << 5 | r5 << 11;
                    if (ch->d3d_color_mask == 0x01010101) {
                        gf_dma_write16(s, ch->d3d_color_obj,
                                       draw_offset + x * 2, color);
                    } else {
                        uint16_t dstcolor = gf_dma_read16(s,
                            ch->d3d_color_obj, draw_offset + x * 2);
                        dstcolor &= ~ch->d3d_color_mask_565;
                        dstcolor |= color & ch->d3d_color_mask_565;
                        gf_dma_write16(s, ch->d3d_color_obj,
                                       draw_offset + x * 2, dstcolor);
                    }
                } else if (ch->d3d_color_bytes == 4) {
                    uint8_t r8 = rr * 255.0f + 0.5f;
                    uint8_t g8 = gg * 255.0f + 0.5f;
                    uint8_t b8 = bb * 255.0f + 0.5f;
                    uint8_t a8 = aa * 255.0f + 0.5f;
                    uint32_t color = b8 << 0 | g8 << 8 | r8 << 16 | a8 << 24;
                    if (ch->d3d_color_mask == 0x01010101) {
                        gf_dma_write32(s, ch->d3d_color_obj,
                                       draw_offset + x * 4, color);
                    } else {
                        uint32_t dstcolor = gf_dma_read32(s,
                            ch->d3d_color_obj, draw_offset + x * 4);
                        dstcolor &= ~ch->d3d_color_mask_8888;
                        dstcolor |= color & ch->d3d_color_mask_8888;
                        gf_dma_write32(s, ch->d3d_color_obj,
                                       draw_offset + x * 4, dstcolor);
                    }
                } else {
                    uint8_t color = bb * 255.0f + 0.5f;
                    gf_dma_write8(s, ch->d3d_color_obj, draw_offset + x,
                                  color);
                }
            }
            if (ch->d3d_depth_test_enable && ch->d3d_depth_write_enable) {
                if (ch->d3d_depth_bytes == 2) {
                    gf_dma_write16(s, ch->d3d_zeta_obj,
                                   draw_offset_zeta + x * 2, z_new);
                } else {
                    gf_dma_write32(s, ch->d3d_zeta_obj,
                                   draw_offset_zeta + x * 4,
                                   (z_new << 8) | stencil);
                }
            }
        }
        draw_offset += pitch;
        draw_offset_zeta += pitch_zeta;
    }
    gf_redraw_area(s, redraw_offset, draw_width, draw_height);
}

static void gf_d3d_triangle(GeForceState *s, gf_channel *ch, uint32_t base)
{
    float vs_out[3][16][4];
    bool clipped[3];
    uint32_t clip_count = 0;
    float clip_thresh;
    uint32_t vi, ci, i;
    int v;

    if (ch->d3d_shade_mode == 0x00001d00) { /* FLAT */
        float (*v_pr)[4] = ch->d3d_vertex_data[(ch->d3d_vertex_index - 1) & 3];
        for (vi = 0; vi < 3; vi++) {
            float (*v_in)[4] = ch->d3d_vertex_data[(vi + base) & 3];
            for (ci = 0; ci < 4; ci++) {
                v_in[ch->d3d_attrib_in_color[0]][ci] =
                    v_pr[ch->d3d_attrib_in_color[0]][ci];
                v_in[ch->d3d_attrib_in_normal][ci] =
                    v_pr[ch->d3d_attrib_in_normal][ci];
            }
        }
    }
    for (vi = 0; vi < 3; vi++) {
        float (*v_out)[4] = vs_out[vi];
        float (*v_in)[4] = ch->d3d_vertex_data[(vi + base) & 3];
        if ((ch->d3d_transform_execution_mode & 3) != 0) {
            gf_d3d_vertex_shader(s, ch, v_in, v_out);
        } else {
            float *p = v_out[0];
            float *color_out[2];
            float *color_in[2];
            for (ci = 0; ci < 4; ci++) {
                p[ci] = v_in[0][ci];
            }
            color_out[0] = v_out[ch->d3d_attrib_out_color[0]];
            color_out[1] = v_out[ch->d3d_attrib_out_color[1]];
            color_in[0] = v_in[ch->d3d_attrib_in_color[0]];
            color_in[1] = v_in[ch->d3d_attrib_in_color[1]];
            if (ch->d3d_lighting_enable) {
                float nt[3];
                float *n = v_in[ch->d3d_attrib_in_normal];
                float pt[3];
                uint32_t light_index;
                color_out[0][3] = ch->d3d_material_factor[3];
                for (ci = 0; ci < 3; ci++) {
                    switch (ch->d3d_color_material_ambient) {
                    case 0:
                    default:
                        color_out[0][ci] = ch->d3d_scene_ambient_color[ci];
                        break;
                    case 1:
                        color_out[0][ci] = color_in[0][ci] *
                                           ch->d3d_material_factor[ci];
                        break;
                    case 2:
                        color_out[0][ci] = color_in[1][ci] *
                                           ch->d3d_material_factor[ci];
                        break;
                    }
                    color_out[1][ci] = 0.0f;
                }
                gf_normal_to_view(ch, n, nt);
                gf_position_to_view3(ch, p, pt);
                for (light_index = 0; light_index < 8; light_index++) {
                    uint32_t light_type =
                        (ch->d3d_light_enable_mask >> (light_index * 2)) & 3;
                    float n_dot_ld;
                    float n_dot_hv;
                    float att;
                    gf_light *light;
                    if (light_type == 0) {
                        continue;
                    }
                    light = &ch->d3d_light[light_index];
                    if (light_type == 1) {
                        n_dot_ld = gf_dot3(nt, light->inf_direction);
                        if (ch->d3d_local_viewer) {
                            float ed[3];
                            float hv[3];
                            for (ci = 0; ci < 3; ci++) {
                                ed[ci] = ch->d3d_eye_position[ci] - pt[ci];
                            }
                            gf_normalize_inplace(ed);
                            hv[0] = light->inf_direction[0] + ed[0];
                            hv[1] = light->inf_direction[1] + ed[1];
                            hv[2] = light->inf_direction[2] + ed[2];
                            gf_normalize_inplace(hv);
                            n_dot_hv = gf_dot3(nt, hv);
                        } else {
                            n_dot_hv = gf_dot3(nt, light->inf_half_vector);
                        }
                        att = 1.0f;
                    } else {
                        float ld[3];
                        float d;
                        float hv[3];
                        for (ci = 0; ci < 3; ci++) {
                            ld[ci] = light->local_position[ci] - pt[ci];
                        }
                        d = gf_normalize_inplace(ld);
                        n_dot_ld = gf_dot3(nt, ld);
                        if (ch->d3d_local_viewer) {
                            float ed[3];
                            for (ci = 0; ci < 3; ci++) {
                                ed[ci] = ch->d3d_eye_position[ci] - pt[ci];
                            }
                            gf_normalize_inplace(ed);
                            hv[0] = ld[0] + ed[0];
                            hv[1] = ld[1] + ed[1];
                            hv[2] = ld[2] + ed[2];
                        } else {
                            hv[0] = ld[0];
                            hv[1] = ld[1];
                            hv[2] = ld[2] + 1.0f;
                        }
                        gf_normalize_inplace(hv);
                        n_dot_hv = gf_dot3(nt, hv);
                        att = 1.0f / (
                            light->local_attenuation[0] +
                            light->local_attenuation[1] * d +
                            light->local_attenuation[2] * d * d);
                        if (light_type == 3) {
                            float rho = -gf_dot3(light->spot_direction, ld);
                            if (rho > light->spot_direction[3]) {
                                continue;
                            }
                        }
                    }
                    if (n_dot_ld < 0.0f) {
                        n_dot_ld = 0.0f;
                    }
                    for (ci = 0; ci < 3; ci++) {
                        float ambient = light->ambient_color[ci];
                        float diffuse = att * light->diffuse_color[ci] *
                                        n_dot_ld;
                        if (ch->d3d_color_material_ambient == 1) {
                            ambient *= color_in[0][ci];
                        } else if (ch->d3d_color_material_ambient == 2) {
                            ambient *= color_in[1][ci];
                        }
                        if (ch->d3d_color_material_diffuse == 1) {
                            diffuse *= color_in[0][ci];
                        } else if (ch->d3d_color_material_diffuse == 2) {
                            diffuse *= color_in[1][ci];
                        }
                        color_out[0][ci] += ambient + diffuse;
                    }
                    if (n_dot_hv < 0.0f) {
                        n_dot_hv = 0.0f;
                    }
                    if (n_dot_hv != 0.0f) {
                        float pf = pow(n_dot_hv, ch->d3d_specular_power);
                        for (ci = 0; ci < 3; ci++) {
                            color_out[ch->d3d_separate_specular & 1][ci] +=
                                att * light->specular_color[ci] * pf;
                        }
                    }
                }
            } else {
                for (ci = 0; ci < 4; ci++) {
                    color_out[0][ci] = color_in[0][ci];
                    color_out[1][ci] = 0.0f;
                }
            }
            for (i = 0; i < ch->d3d_tex_coord_count; i++) {
                float *tc = v_out[ch->d3d_attrib_out_tex_coord[i]];
                int comp_index;
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    uint32_t texgen = ch->d3d_texgen[i & 7][comp_index];
                    switch (texgen) {
                    case 0x0000:   /* disabled */
                        tc[comp_index] =
                            v_in[ch->d3d_attrib_in_tex_coord[i]][comp_index];
                        break;
                    case 0x2400: { /* EYE_LINEAR */
                        float pt[4];
                        gf_position_to_view4(ch, p, pt);
                        tc[comp_index] = gf_dot4(
                            ch->d3d_texgen_plane[i & 7][comp_index], pt);
                        break;
                    }
                    case 0x2401:   /* OBJECT_LINEAR */
                        tc[comp_index] = gf_dot4(
                            ch->d3d_texgen_plane[i & 7][comp_index], p);
                        break;
                    case 0x2402:   /* SPHERE_MAP */
                    case 0x8512: { /* REFLECTION_MAP */
                        float nt[3];
                        float *n = v_in[ch->d3d_attrib_in_normal];
                        float pt[3];
                        float u[3];
                        float r[3];
                        float ntu;
                        gf_normal_to_view(ch, n, nt);
                        gf_position_to_view3(ch, p, pt);
                        gf_normalize_out(pt, u);
                        ntu = gf_dot3(nt, u);
                        r[0] = u[0] - 2 * nt[0] * ntu;
                        r[1] = u[1] - 2 * nt[1] * ntu;
                        r[2] = u[2] - 2 * nt[2] * ntu;
                        if (texgen == 0x2402) { /* SPHERE_MAP */
                            float m = 2 * sqrt(r[0] * r[0] + r[1] * r[1] +
                                               (r[2] + 1.0f) * (r[2] + 1.0f));
                            if (comp_index < 2) {
                                tc[comp_index] = r[comp_index] / m + 0.5f;
                            } else {
                                tc[comp_index] = 0.0f;
                            }
                        } else {
                            if (comp_index < 3) {
                                tc[comp_index] = r[comp_index];
                            } else {
                                tc[comp_index] = 0.0f;
                            }
                        }
                        break;
                    }
                    case 0x8511: { /* NORMAL_MAP */
                        if (comp_index < 3) {
                            float *n = v_in[ch->d3d_attrib_in_normal];
                            float *r = &ch->d3d_inverse_model_view_matrix[
                                comp_index * 4];
                            tc[comp_index] = gf_dot3(n, r);
                        } else {
                            tc[comp_index] = 0.0f;
                        }
                        break;
                    }
                    default:       /* not implemented */
                        tc[comp_index] = 0.5f;
                        break;
                    }
                }
                if (ch->d3d_texture_matrix_enable[i]) {
                    float ttc[4];
                    float *m = ch->d3d_texture_matrix[i & 7];
                    ttc[0] = tc[0] * m[0]  + tc[1] * m[1]  + tc[2] * m[2] +
                             tc[3] * m[3];
                    ttc[1] = tc[0] * m[4]  + tc[1] * m[5]  + tc[2] * m[6] +
                             tc[3] * m[7];
                    ttc[2] = tc[0] * m[8]  + tc[1] * m[9]  + tc[2] * m[10] +
                             tc[3] * m[11];
                    ttc[3] = tc[0] * m[12] + tc[1] * m[13] + tc[2] * m[14] +
                             tc[3] * m[15];
                    for (comp_index = 0; comp_index < 4; comp_index++) {
                        tc[comp_index] = ttc[comp_index];
                    }
                }
            }
            if (ch->d3d_fog_enable) {
                float fog_dist;
                switch (ch->d3d_fog_gen_mode) {
                case 0:   /* SPEC_ALPHA */
                    fog_dist = v_in[ch->d3d_attrib_in_color[1]][3];
                    break;
                case 1: { /* RADIAL */
                    float pt[3];
                    gf_position_to_view3(ch, p, pt);
                    fog_dist = gf_length(pt);
                    break;
                }
                case 2:   /* PLANAR */
                case 3: { /* ABS_PLANAR */
                    float *m = ch->d3d_model_view_matrix[0];
                    fog_dist = p[0] * m[8] + p[1] * m[9] + p[2] * m[10] +
                               p[3] * m[11];
                    if (ch->d3d_fog_gen_mode == 3) { /* ABS_PLANAR */
                        fog_dist = fabs(fog_dist);
                    }
                    break;
                }
                default:  /* not implemented */
                    fog_dist = 3.0f;
                    break;
                }
                v_out[ch->d3d_attrib_out_fogc][0] = fog_dist;
            }
            if (ch->d3d_view_matrix_enable == 0 ||
                ch->d3d_view_matrix_enable == 2 ||
                ch->d3d_view_matrix_enable == 6) {
                float tp[4];
                float *m = ch->d3d_composite_matrix;
                int comp_index;
                tp[0] = p[0] * m[0]  + p[1] * m[1]  + p[2] * m[2] +
                        p[3] * m[3];
                tp[1] = p[0] * m[4]  + p[1] * m[5]  + p[2] * m[6] +
                        p[3] * m[7];
                tp[2] = p[0] * m[8]  + p[1] * m[9]  + p[2] * m[10] +
                        p[3] * m[11];
                tp[3] = p[0] * m[12] + p[1] * m[13] + p[2] * m[14] +
                        p[3] * m[15];
                for (comp_index = 0; comp_index < 4; comp_index++) {
                    p[comp_index] = tp[comp_index];
                }
            }
        }
        for (i = 0; i < 2; i++) {
            if (ch->d3d_attrib_out_enable[i]) {
                float *color = v_out[ch->d3d_attrib_out_color[i]];
                for (ci = 0; ci < 4; ci++) {
                    color[ci] = MIN(MAX(color[ci], 0.0f), 1.0f);
                }
            }
        }
    }
    clip_thresh = s->card_type <= 0x20 ?
        ch->d3d_viewport_offset[2] - ch->d3d_clip_min : 1.0f;
    for (v = 0; v < 3; v++) {
        clipped[v] = vs_out[v][0][2] < -vs_out[v][0][3] * clip_thresh;
        if (clipped[v]) {
            clip_count++;
        }
    }
    if (clip_count == 0) {
        gf_d3d_triangle_clipped(s, ch, vs_out[0], vs_out[1], vs_out[2]);
    } else if (clip_count == 3) {
        return;
    } else {
        uint32_t intersection_index = 0;
        float intersections[2][16][4];
        int v0;
        for (v0 = 0; v0 < 3; v0++) {
            uint32_t v1 = (v0 + 1) % 3;
            if (clipped[v0] != clipped[v1]) {
                float k = vs_out[v1][0][2] + vs_out[v1][0][3] * clip_thresh;
                float t = k / (k - vs_out[v0][0][2] -
                               vs_out[v0][0][3] * clip_thresh);
                float omt = 1.0f - t;
                int a, comp_index;
                if (intersection_index >= 2) {
                    break;
                }
                for (a = 0; a < 16; a++) {
                    for (comp_index = 0; comp_index < 4; comp_index++) {
                        intersections[intersection_index][a][comp_index] =
                            t * vs_out[v0][a][comp_index] +
                            omt * vs_out[v1][a][comp_index];
                    }
                }
                intersection_index++;
            }
        }
        if (clip_count == 2) {
            if (!clipped[0]) {
                gf_d3d_triangle_clipped(s, ch, vs_out[0], intersections[0],
                                        intersections[1]);
            } else if (!clipped[1]) {
                gf_d3d_triangle_clipped(s, ch, intersections[0], vs_out[1],
                                        intersections[1]);
            } else {
                gf_d3d_triangle_clipped(s, ch, intersections[1],
                                        intersections[0], vs_out[2]);
            }
        } else {
            if (clipped[0]) {
                gf_d3d_triangle_clipped(s, ch, intersections[0], vs_out[1],
                                        vs_out[2]);
                gf_d3d_triangle_clipped(s, ch, intersections[1],
                                        intersections[0], vs_out[2]);
            } else if (clipped[1]) {
                gf_d3d_triangle_clipped(s, ch, vs_out[0], intersections[0],
                                        vs_out[2]);
                gf_d3d_triangle_clipped(s, ch, intersections[0],
                                        intersections[1], vs_out[2]);
            } else {
                gf_d3d_triangle_clipped(s, ch, vs_out[0], vs_out[1],
                                        intersections[0]);
                gf_d3d_triangle_clipped(s, ch, vs_out[0], intersections[0],
                                        intersections[1]);
            }
        }
    }
}

void gf_d3d_process_vertex(GeForceState *s, gf_channel *ch, bool immediate)
{
    uint32_t ai, ci;

    if (immediate) {
        for (ai = 0; ai < ch->d3d_attrib_count; ai++) {
            for (ci = 0; ci < 4; ci++) {
                ch->d3d_vertex_data[ch->d3d_vertex_index & 3][ai][ci] =
                    ch->d3d_vertex_data_imm[ai][ci];
            }
        }
    }
    if (ch->d3d_vertex_data_array_format_homogeneous[0]) {
        float *p = ch->d3d_vertex_data[ch->d3d_vertex_index & 3][0];
        p[3] = 1.0f / p[3];
        p[0] *= p[3];
        p[1] *= p[3];
        p[2] *= p[3];
    }
    ch->d3d_vertex_index++;
    switch (ch->d3d_begin_end) {
    case 5:      /* TRIANGLES */
    case 0x1012: /* TRIANGLELIST */
    case 0x101a:
        if (ch->d3d_vertex_index == 3) {
            gf_d3d_triangle(s, ch, 0);
            ch->d3d_vertex_index = 0;
        }
        break;
    case 6:      /* TRIANGLE_STRIP */
        if (ch->d3d_vertex_index == 3 || ch->d3d_primitive_done) {
            gf_d3d_triangle(s, ch, 0);
            ch->d3d_primitive_done = true;
            ch->d3d_triangle_flip = !ch->d3d_triangle_flip;
            if (ch->d3d_vertex_index == 3) {
                ch->d3d_vertex_index = 0;
            }
        }
        break;
    case 7:      /* TRIANGLE_FAN */
    case 0xa:    /* POLYGON */
    case 0x1015:
    case 0x1017:
        if (ch->d3d_vertex_index == 3 || ch->d3d_primitive_done) {
            gf_d3d_triangle(s, ch, 0);
            ch->d3d_primitive_done = true;
            ch->d3d_triangle_flip = !ch->d3d_triangle_flip;
            if (ch->d3d_vertex_index == 3) {
                ch->d3d_vertex_index = 1;
            }
        }
        break;
    case 8:      /* QUADS */
        if (ch->d3d_vertex_index == 4) {
            gf_d3d_triangle(s, ch, 0);
            gf_d3d_triangle(s, ch, 2);
            ch->d3d_vertex_index = 0;
        }
        break;
    case 9:      /* QUAD_STRIP */
        if (ch->d3d_vertex_index == 4 ||
            (ch->d3d_vertex_index == 2 && ch->d3d_primitive_done)) {
            if (ch->d3d_vertex_index == 4) {
                gf_d3d_triangle(s, ch, 0);
                ch->d3d_triangle_flip = true;
                gf_d3d_triangle(s, ch, 1);
                ch->d3d_triangle_flip = false;
                ch->d3d_primitive_done = true;
                ch->d3d_vertex_index = 0;
            } else {
                gf_d3d_triangle(s, ch, 2);
                ch->d3d_triangle_flip = true;
                gf_d3d_triangle(s, ch, 3);
                ch->d3d_triangle_flip = false;
            }
        }
        break;
    default:     /* not implemented */
        ch->d3d_vertex_index = 0;
        break;
    }
    if (ch->d3d_vertex_index >= 4) {
        ch->d3d_vertex_index = 0;
    }
}

void gf_unpack_attribute(uint32_t value, bool d3d, float comp[4])
{
    if (d3d) {
        comp[0] = ((value >> (2 * 8)) & 0xff) / 255.0f;
        comp[1] = ((value >> (1 * 8)) & 0xff) / 255.0f;
        comp[2] = ((value >> (0 * 8)) & 0xff) / 255.0f;
        comp[3] = ((value >> (3 * 8)) & 0xff) / 255.0f;
    } else {
        uint32_t i;
        for (i = 0; i < 4; i++) {
            comp[i] = ((value >> (i * 8)) & 0xff) / 255.0f;
        }
    }
}

void gf_d3d_load_vertex(GeForceState *s, gf_channel *ch, uint32_t index)
{
    uint32_t index_adj = ch->d3d_vertex_data_base_index + index;
    uint32_t ai;

    for (ai = 0; ai < ch->d3d_attrib_count; ai++) {
        uint32_t comp_count = ch->d3d_vertex_data_array_format_size[ai];
        if (comp_count != 0) {
            uint32_t array_offset = ch->d3d_vertex_data_array_offset[ai];
            uint32_t array_obj = array_offset & 0x80000000 ?
                ch->d3d_vertex_b_obj : ch->d3d_vertex_a_obj;
            uint32_t attrib_stride;
            uint32_t format_type;
            array_offset &= 0x7fffffff;
            array_offset -= gf_ramin_read32(s, array_obj) >> 20; /* why? */
            attrib_stride = ch->d3d_vertex_data_array_format_stride[ai];
            array_offset += index_adj * attrib_stride;
            ch->d3d_vertex_data[ch->d3d_vertex_index & 3][ai][2] = 0.0f;
            ch->d3d_vertex_data[ch->d3d_vertex_index & 3][ai][3] = 1.0f;
            format_type = ch->d3d_vertex_data_array_format_type[ai];
            if ((format_type == 0 || format_type == 4) && comp_count == 4) {
                uint32_t value = gf_dma_read32(s, array_obj, array_offset);
                gf_unpack_attribute(value, format_type == 0,
                    ch->d3d_vertex_data[ch->d3d_vertex_index & 3][ai]);
            } else if (format_type == 5 && comp_count == 2) {
                uint32_t value = gf_dma_read32(s, array_obj, array_offset);
                ch->d3d_vertex_data[ch->d3d_vertex_index & 3][ai][0] =
                    (float)(int16_t)value;
                ch->d3d_vertex_data[ch->d3d_vertex_index & 3][ai][1] =
                    (float)(int16_t)(value >> 16);
            } else {
                uint32_t ci;
                for (ci = 0; ci < comp_count && ci < 4; ci++) {
                    uint32_t ui32 = gf_dma_read32(s, array_obj,
                                                  array_offset + ci * 4);
                    ch->d3d_vertex_data[ch->d3d_vertex_index & 3][ai][ci] =
                        gf_uint32_as_float(ui32);
                }
            }
        } else {
            uint32_t ci;
            for (ci = 0; ci < 4; ci++) {
                ch->d3d_vertex_data[ch->d3d_vertex_index & 3][ai][ci] =
                    ch->d3d_vertex_data_imm[ai][ci];
            }
        }
    }
    gf_d3d_process_vertex(s, ch, false);
}
