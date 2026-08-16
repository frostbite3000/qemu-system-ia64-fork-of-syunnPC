/*
 * QEMU NVIDIA Quadro2 Pro (NV15GL) emulation -- FIFO and 2D acceleration
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
#include "geforce_int.h"

static uint32_t gf_color_565_to_888(uint16_t value)
{
    uint8_t r, g, b;
    EXTRACT_565_TO_888(value, r, g, b);
    return r << 16 | g << 8 | b;
}

static uint16_t gf_color_888_to_565(uint32_t value)
{
    return (((value >> 19) & 0x1F) << 11) | (((value >> 10) & 0x3F) << 5) |
           ((value >> 3) & 0x1F);
}

static uint32_t gf_get_pixel(GeForceState *s, uint32_t obj, uint32_t ofs,
                             uint32_t x, uint32_t cb)
{
    uint32_t result;

    if (cb == 1) {
        result = gf_dma_read8(s, obj, ofs + x);
    } else if (cb == 2) {
        result = gf_dma_read16(s, obj, ofs + x * 2);
    } else {
        result = gf_dma_read32(s, obj, ofs + x * 4);
    }
    return result;
}

static void gf_put_pixel(GeForceState *s, gf_channel *ch, uint32_t ofs,
                         uint32_t x, uint32_t value)
{
    if (ch->s2d_color_bytes == 1) {
        gf_dma_write8(s, ch->s2d_img_dst, ofs + x, value);
    } else if (ch->s2d_color_bytes == 2) {
        gf_dma_write16(s, ch->s2d_img_dst, ofs + x * 2, value);
    } else if (ch->s2d_color_fmt == 6) {
        gf_dma_write32(s, ch->s2d_img_dst, ofs + x * 4, value & 0x00FFFFFF);
    } else {
        gf_dma_write32(s, ch->s2d_img_dst, ofs + x * 4, value);
    }
}

static void gf_put_pixel_swzs(GeForceState *s, gf_channel *ch, uint32_t ofs,
                              uint32_t value)
{
    if (ch->swzs_color_bytes == 1) {
        gf_dma_write8(s, ch->swzs_img_obj, ofs, value);
    } else if (ch->swzs_color_bytes == 2) {
        gf_dma_write16(s, ch->swzs_img_obj, ofs, value);
    } else {
        gf_dma_write32(s, ch->swzs_img_obj, ofs, value);
    }
}

/* Evaluate an arbitrary GDI ternary raster operation. */
static uint32_t gf_rop3(uint8_t rop, uint32_t d, uint32_t src, uint32_t p)
{
    uint32_t r = 0;
    int idx;

    for (idx = 0; idx < 8; idx++) {
        if ((rop >> idx) & 1) {
            uint32_t m = 0xFFFFFFFF;
            m &= (idx & 1) ? d : ~d;
            m &= (idx & 2) ? src : ~src;
            m &= (idx & 4) ? p : ~p;
            r |= m;
        }
    }
    return r;
}

static void gf_pixel_operation(GeForceState *s, gf_channel *ch, uint32_t op,
                               uint32_t *dstcolor, const uint32_t *srccolor,
                               uint32_t cb, uint32_t px, uint32_t py)
{
    if (op == 1) {
        uint8_t rop = ch->rop;
        uint32_t patt_color = 0;

        /* The raster op depends on the pattern iff high and low nibbles
         * of the ROP3 code differ. */
        if ((rop >> 4) != (rop & 0xF)) {
            uint32_t i = py % 8 * 8 + px % 8;
            if (ch->patt_type_color) {
                patt_color = ch->patt_data_color[i];
            } else {
                patt_color = ch->patt_data_mono[i] ? ch->patt_fg_color
                                                   : ch->patt_bg_color;
            }
        }
        *dstcolor = gf_rop3(rop, *dstcolor, *srccolor, patt_color);
    } else if (op == 5) {
        if (cb == 4) {
            if (*srccolor) {
                uint8_t sb = *srccolor;
                uint8_t sg = *srccolor >> 8;
                uint8_t sr = *srccolor >> 16;
                uint8_t sa = *srccolor >> 24;
                uint32_t beta = ch->beta;
                uint8_t db, dg, dr, da, isa, b, g, r, a;
                if (beta != 0xFFFFFFFF) {
                    uint8_t bb = beta;
                    uint8_t bg = beta >> 8;
                    uint8_t br = beta >> 16;
                    uint8_t ba = beta >> 24;
                    sb = sb * bb / 0xFF;
                    sg = sg * bg / 0xFF;
                    sr = sr * br / 0xFF;
                    sa = sa * ba / 0xFF;
                }
                db = *dstcolor;
                dg = *dstcolor >> 8;
                dr = *dstcolor >> 16;
                da = *dstcolor >> 24;
                isa = 0xFF - sa;
                b = gf_alpha_wrap(db * isa / 0xFF + sb);
                g = gf_alpha_wrap(dg * isa / 0xFF + sg);
                r = gf_alpha_wrap(dr * isa / 0xFF + sr);
                a = gf_alpha_wrap(da * isa / 0xFF + sa);
                *dstcolor = b << 0 | g << 8 | r << 16 | a << 24;
            }
        } else {
            uint32_t beta = ch->beta;
            uint8_t bb = beta;
            uint8_t bg = beta >> 8;
            uint8_t br = beta >> 16;
            uint8_t iba = 0xFF - (beta >> 24);
            uint8_t sb = *srccolor & 0x1F;
            uint8_t sg = (*srccolor >> 5) & 0x3F;
            uint8_t sr = (*srccolor >> 11) & 0x1F;
            uint8_t db = *dstcolor & 0x1F;
            uint8_t dg = (*dstcolor >> 5) & 0x3F;
            uint8_t dr = (*dstcolor >> 11) & 0x1F;
            uint8_t b = (db * iba + sb * bb) / 0xFF;
            uint8_t g = (dg * iba + sg * bg) / 0xFF;
            uint8_t r = (dr * iba + sr * br) / 0xFF;
            *dstcolor = b << 0 | g << 5 | r << 11;
        }
    } else {
        *dstcolor = *srccolor;
    }
}

static void gf_gdi_fillrect(GeForceState *s, gf_channel *ch, bool clipped)
{
    int16_t clipx0 = 0;
    int16_t clipy0 = 0;
    int16_t clipx1 = 0;
    int16_t clipy1 = 0;
    int16_t dx;
    int16_t dy;
    uint16_t width;
    uint16_t height;
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t srccolor = ch->gdi_rect_color;
    uint32_t draw_offset;
    uint32_t redraw_offset;
    uint16_t x, y;

    if (clipped) {
        clipx0 = ch->gdi_clip_yx0 & 0xFFFF;
        clipy0 = ch->gdi_clip_yx0 >> 16;
        clipx1 = ch->gdi_clip_yx1 & 0xFFFF;
        clipy1 = ch->gdi_clip_yx1 >> 16;
        dx = ch->gdi_rect_yx0 & 0xFFFF;
        dy = ch->gdi_rect_yx0 >> 16;
        clipx0 -= dx;
        clipy0 -= dy;
        clipx1 -= dx;
        clipy1 -= dy;
        width = (ch->gdi_rect_yx1 & 0xFFFF) - dx;
        height = (ch->gdi_rect_yx1 >> 16) - dy;
    } else {
        dx = ch->gdi_rect_xy >> 16;
        dy = ch->gdi_rect_xy & 0xFFFF;
        width = ch->gdi_rect_wh >> 16;
        height = ch->gdi_rect_wh & 0xFFFF;
    }
    draw_offset = ch->s2d_ofs_dst + dy * pitch + dx * ch->s2d_color_bytes;
    redraw_offset = gf_dma_lin_lookup(s, ch->s2d_img_dst, draw_offset);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            if (!clipped ||
                (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1)) {
                uint32_t dstcolor = gf_get_pixel(s, ch->s2d_img_dst,
                                                 draw_offset, x,
                                                 ch->s2d_color_bytes);
                gf_pixel_operation(s, ch, ch->gdi_operation, &dstcolor,
                                   &srccolor, ch->s2d_color_bytes,
                                   dx + x, dy + y);
                gf_put_pixel(s, ch, draw_offset, x, dstcolor);
            }
        }
        draw_offset += pitch;
    }
    gf_redraw_area(s, redraw_offset, width, height);
}

static void gf_gdi_blit(GeForceState *s, gf_channel *ch, uint32_t type)
{
    int16_t dx = ch->gdi_image_xy & 0xFFFF;
    int16_t dy = ch->gdi_image_xy >> 16;
    int16_t clipx0 = (ch->gdi_clip_yx0 & 0xFFFF) - dx;
    int16_t clipy0 = (ch->gdi_clip_yx0 >> 16) - dy;
    int16_t clipx1 = (ch->gdi_clip_yx1 & 0xFFFF) - dx;
    int16_t clipy1 = (ch->gdi_clip_yx1 >> 16) - dy;
    uint32_t swidth = ch->gdi_image_swh & 0xFFFF;
    uint32_t dwidth = type ? ch->gdi_image_dwh & 0xFFFF : swidth;
    uint32_t height = ch->gdi_image_swh >> 16;
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t bg_color = ch->gdi_bg_color;
    uint32_t fg_color = ch->gdi_fg_color;
    uint32_t draw_offset;
    uint32_t redraw_offset;
    uint32_t bit_index = 0;
    uint16_t x, y;

    if (ch->s2d_color_bytes == 4 && ch->gdi_color_fmt != 3) {
        bg_color = gf_color_565_to_888(bg_color);
        fg_color = gf_color_565_to_888(fg_color);
    }
    draw_offset = ch->s2d_ofs_dst + dy * pitch + dx * ch->s2d_color_bytes;
    redraw_offset = gf_dma_lin_lookup(s, ch->s2d_img_dst, draw_offset);
    for (y = 0; y < height; y++) {
        for (x = 0; x < dwidth; x++) {
            if (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1) {
                uint32_t word_offset = bit_index / 32;
                uint32_t bit_offset = bit_index % 32;
                bool pixel;
                if (ch->gdi_mono_fmt == 1) {
                    bit_offset ^= 7;
                }
                pixel = (ch->gdi_words[word_offset] >> bit_offset) & 1;
                if (type || (!type && pixel)) {
                    uint32_t dstcolor = gf_get_pixel(s, ch->s2d_img_dst,
                                                     draw_offset, x,
                                                     ch->s2d_color_bytes);
                    uint32_t srccolor = pixel ? fg_color : bg_color;
                    gf_pixel_operation(s, ch, ch->gdi_operation, &dstcolor,
                                       &srccolor, ch->s2d_color_bytes,
                                       dx + x, dy + y);
                    gf_put_pixel(s, ch, draw_offset, x, dstcolor);
                }
            }
            bit_index++;
        }
        bit_index += swidth - dwidth;
        draw_offset += pitch;
    }
    gf_redraw_area(s, redraw_offset, dwidth, height);
}

static void gf_rect(GeForceState *s, gf_channel *ch)
{
    int16_t dx = ch->rect_yx & 0xFFFF;
    int16_t dy = ch->rect_yx >> 16;
    uint16_t width = ch->rect_hw & 0xFFFF;
    uint16_t height = ch->rect_hw >> 16;
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t srccolor = ch->rect_color;
    uint32_t draw_offset = ch->s2d_ofs_dst +
        dy * pitch + dx * ch->s2d_color_bytes;
    uint32_t redraw_offset = gf_dma_lin_lookup(s, ch->s2d_img_dst,
                                               draw_offset);
    uint16_t x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint32_t dstcolor = gf_get_pixel(s, ch->s2d_img_dst,
                                             draw_offset, x,
                                             ch->s2d_color_bytes);
            gf_pixel_operation(s, ch, ch->rect_operation, &dstcolor,
                               &srccolor, ch->s2d_color_bytes,
                               dx + x, dy + y);
            gf_put_pixel(s, ch, draw_offset, x, dstcolor);
        }
        draw_offset += pitch;
    }
    gf_redraw_area(s, redraw_offset, width, height);
}

static void gf_ifc(GeForceState *s, gf_channel *ch, uint32_t word)
{
    uint32_t chromacolor = 0;
    bool chroma_enabled = false;
    uint32_t i;

    if (ch->ifc_color_key_enable) {
        if (ch->ifc_color_bytes == 4) {
            chromacolor = ch->chroma_color & 0x00FFFFFF;
            chroma_enabled = ch->chroma_color & 0xFF000000;
        } else if (ch->ifc_color_bytes == 2) {
            chromacolor = ch->chroma_color & 0x0000FFFF;
            chroma_enabled = ch->chroma_color & 0xFFFF0000;
        } else {
            chromacolor = ch->chroma_color & 0x000000FF;
            chroma_enabled = ch->chroma_color & 0xFFFFFF00;
        }
    }
    for (i = 0; i < ch->ifc_pixels_per_word; i++) {
        if (ch->ifc_x >= ch->ifc_clip_x0 && ch->ifc_x < ch->ifc_clip_x1 &&
            ch->ifc_y >= ch->ifc_clip_y0 && ch->ifc_y < ch->ifc_clip_y1) {
            uint32_t srccolor;
            if (ch->ifc_color_bytes == 4) {
                srccolor = word;
            } else if (ch->ifc_color_bytes == 2) {
                srccolor = i == 0 ? word & 0xffff : word >> 16;
            } else {
                srccolor = (word >> (i * 8)) & 0xff;
            }
            if (!chroma_enabled || srccolor != chromacolor) {
                uint32_t dstcolor = gf_get_pixel(s, ch->s2d_img_dst,
                                                 ch->ifc_draw_offset,
                                                 ch->ifc_x,
                                                 ch->s2d_color_bytes);
                if (ch->ifc_color_bytes == 4 && ch->s2d_color_bytes == 2) {
                    dstcolor = gf_color_565_to_888(dstcolor);
                }
                gf_pixel_operation(s, ch, ch->ifc_operation, &dstcolor,
                                   &srccolor, ch->ifc_color_bytes,
                                   ch->ifc_ofs_x + ch->ifc_x,
                                   ch->ifc_ofs_y + ch->ifc_y);
                if (ch->ifc_color_bytes == 4 && ch->s2d_color_bytes == 2) {
                    dstcolor = gf_color_888_to_565(dstcolor);
                }
                gf_put_pixel(s, ch, ch->ifc_draw_offset, ch->ifc_x, dstcolor);
            }
        }
        ch->ifc_x++;
        if (ch->ifc_x >= ch->ifc_src_width) {
            gf_redraw_area(s, ch->ifc_redraw_offset, ch->ifc_dst_width, 1);
            ch->ifc_draw_offset += ch->s2d_pitch_dst;
            ch->ifc_redraw_offset += ch->s2d_pitch_dst;
            ch->ifc_x = 0;
            ch->ifc_y++;
        }
    }
}

static void gf_iifc(GeForceState *s, gf_channel *ch)
{
    int16_t dx = ch->iifc_yx & 0xFFFF;
    int16_t dy = ch->iifc_yx >> 16;
    int16_t clipx0 = ch->clip_x - dx;
    int16_t clipy0 = ch->clip_y - dy;
    int16_t clipx1 = clipx0 + ch->clip_width;
    int16_t clipy1 = clipy0 + ch->clip_height;
    uint32_t swidth = ch->iifc_shw & 0xFFFF;
    uint32_t dwidth = ch->iifc_dhw & 0xFFFF;
    uint32_t height = ch->iifc_dhw >> 16;
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t draw_offset = ch->s2d_ofs_dst +
        dy * pitch + dx * ch->s2d_color_bytes;
    uint32_t redraw_offset = gf_dma_lin_lookup(s, ch->s2d_img_dst,
                                               draw_offset);
    uint32_t symbol_index = 0;
    uint16_t x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < dwidth; x++) {
            if (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1) {
                uint8_t symbol;
                uint32_t dstcolor;
                if (ch->iifc_bpp4) {
                    uint32_t word_offset = symbol_index / 8;
                    uint32_t symbol_offset = (symbol_index % 8 ^ 1) * 4;
                    symbol = ch->iifc_words[word_offset] >> symbol_offset &
                             0xF;
                } else {
                    uint32_t word_offset = symbol_index / 4;
                    uint32_t symbol_offset = symbol_index % 4 * 8;
                    symbol = ch->iifc_words[word_offset] >> symbol_offset &
                             0xFF;
                }
                dstcolor = gf_get_pixel(s, ch->s2d_img_dst, draw_offset, x,
                                        ch->s2d_color_bytes);
                if (ch->iifc_color_bytes == 4) {
                    uint32_t srccolor = gf_dma_read32(s, ch->iifc_palette,
                        ch->iifc_palette_ofs + symbol * 4);
                    if (ch->s2d_color_bytes == 2) {
                        dstcolor = gf_color_565_to_888(dstcolor);
                    }
                    gf_pixel_operation(s, ch, ch->iifc_operation, &dstcolor,
                                       &srccolor, 4, dx + x, dy + y);
                    if (ch->s2d_color_bytes == 2) {
                        dstcolor = gf_color_888_to_565(dstcolor);
                    }
                } else if (ch->iifc_color_bytes == 2) {
                    uint32_t srccolor = gf_dma_read16(s, ch->iifc_palette,
                        ch->iifc_palette_ofs + symbol * 2);
                    gf_pixel_operation(s, ch, ch->iifc_operation, &dstcolor,
                                       &srccolor, 2, dx + x, dy + y);
                }
                gf_put_pixel(s, ch, draw_offset, x, dstcolor);
            }
            symbol_index++;
        }
        symbol_index += swidth - dwidth;
        draw_offset += pitch;
    }
    gf_redraw_area(s, redraw_offset, dwidth, height);
}

static void gf_sifc(GeForceState *s, gf_channel *ch)
{
    uint16_t dx = ch->sifc_clip_yx & 0xFFFF;
    uint16_t dy = ch->sifc_clip_yx >> 16;
    uint32_t dsdx;
    uint32_t dtdy;
    uint32_t swidth = ch->sifc_shw & 0xFFFF;
    uint32_t dwidth = ch->sifc_clip_hw & 0xFFFF;
    uint32_t height = ch->sifc_clip_hw >> 16;
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch +
                           dx * ch->s2d_color_bytes;
    uint32_t redraw_offset = gf_dma_lin_lookup(s, ch->s2d_img_dst,
                                               draw_offset);
    int32_t sx0;
    int32_t sy;
    uint32_t symbol_offset_y = 0;
    uint16_t x, y;

    if (ch->sifc_dxds == 0 || ch->sifc_dydt == 0) {
        return;
    }
    dsdx = (uint32_t)(UINT64_C(1099511627776) / ch->sifc_dxds);
    dtdy = (uint32_t)(UINT64_C(1099511627776) / ch->sifc_dydt);
    sx0 = ((ch->sifc_syx & 0xFFFF) << 16) - (dx << 20) - 0x80000;
    sy = (ch->sifc_syx & 0xFFFF0000) - (dy << 20) - 0x80000;
    if (sx0 < 0) {
        sx0 = 0;
    }
    if (sy < 0) {
        sy = 0;
    }
    for (y = 0; y < height; y++) {
        uint32_t sx = sx0;
        for (x = 0; x < dwidth; x++) {
            uint32_t dstcolor = gf_get_pixel(s, ch->s2d_img_dst, draw_offset,
                                             x, ch->s2d_color_bytes);
            uint32_t srccolor;
            uint32_t symbol_offset = symbol_offset_y + (sx >> 20);
            if (ch->sifc_color_bytes == 4) {
                srccolor = ch->sifc_words[symbol_offset];
            } else if (ch->sifc_color_bytes == 2) {
                uint16_t *sifc_words16 = (uint16_t *)ch->sifc_words;
                srccolor = sifc_words16[symbol_offset];
            } else {
                uint8_t *sifc_words8 = (uint8_t *)ch->sifc_words;
                srccolor = sifc_words8[symbol_offset];
            }
            if (ch->sifc_color_bytes == 4 && ch->s2d_color_bytes == 2) {
                dstcolor = gf_color_565_to_888(dstcolor);
            }
            gf_pixel_operation(s, ch, ch->sifc_operation, &dstcolor,
                               &srccolor, ch->sifc_color_bytes,
                               dx + x, dy + y);
            if (ch->sifc_color_bytes == 4 && ch->s2d_color_bytes == 2) {
                dstcolor = gf_color_888_to_565(dstcolor);
            }
            gf_put_pixel(s, ch, draw_offset, x, dstcolor);
            sx += dsdx;
        }
        sy += dtdy;
        symbol_offset_y = (sy >> 20) * swidth;
        draw_offset += pitch;
    }
    gf_redraw_area(s, redraw_offset, dwidth, height);
}

static void gf_copyarea(GeForceState *s, gf_channel *ch)
{
    uint16_t sx = ch->blit_syx & 0xFFFF;
    uint16_t sy = ch->blit_syx >> 16;
    uint16_t dx = ch->blit_dyx & 0xFFFF;
    uint16_t dy = ch->blit_dyx >> 16;
    uint16_t width = ch->blit_hw & 0xFFFF;
    uint16_t height = ch->blit_hw >> 16;
    uint32_t spitch = ch->s2d_pitch_src;
    uint32_t dpitch = ch->s2d_pitch_dst;
    uint32_t src_offset = ch->s2d_ofs_src;
    uint32_t draw_offset = ch->s2d_ofs_dst;
    bool xdir = dx > sx;
    bool ydir = dy > sy;
    uint32_t redraw_offset;
    uint32_t chromacolor = 0;
    bool chroma_enabled = false;
    uint16_t x, y;

    src_offset += (sy + ydir * (height - 1)) * spitch +
                  sx * ch->s2d_color_bytes;
    redraw_offset = gf_dma_lin_lookup(s, ch->s2d_img_dst, draw_offset) +
                    dy * dpitch + dx * ch->s2d_color_bytes;
    draw_offset += (dy + ydir * (height - 1)) * dpitch +
                   dx * ch->s2d_color_bytes;
    if (ch->blit_color_key_enable) {
        if (ch->s2d_color_bytes == 4) {
            chromacolor = ch->chroma_color & 0x00FFFFFF;
            chroma_enabled = ch->chroma_color & 0xFF000000;
        } else if (ch->s2d_color_bytes == 2) {
            chromacolor = ch->chroma_color & 0x0000FFFF;
            chroma_enabled = ch->chroma_color & 0xFFFF0000;
        } else {
            chromacolor = ch->chroma_color & 0x000000FF;
            chroma_enabled = ch->chroma_color & 0xFFFFFF00;
        }
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint16_t xa = xdir ? width - x - 1 : x;
            uint32_t srccolor = gf_get_pixel(s, ch->s2d_img_src, src_offset,
                                             xa, ch->s2d_color_bytes);
            if (!chroma_enabled || srccolor != chromacolor) {
                uint32_t dstcolor = gf_get_pixel(s, ch->s2d_img_dst,
                                                 draw_offset, xa,
                                                 ch->s2d_color_bytes);
                gf_pixel_operation(s, ch, ch->blit_operation, &dstcolor,
                                   &srccolor, ch->s2d_color_bytes,
                                   dx + x, dy + y);
                gf_put_pixel(s, ch, draw_offset, xa, dstcolor);
            }
        }
        src_offset += spitch * (1 - 2 * ydir);
        draw_offset += dpitch * (1 - 2 * ydir);
    }
    gf_redraw_area(s, redraw_offset, width, height);
}

static void gf_m2mf(GeForceState *s, gf_channel *ch)
{
    uint32_t src_offset = ch->m2mf_src_offset;
    uint32_t dst_offset = ch->m2mf_dst_offset;
    uint32_t dma_target;
    uint16_t y;

    for (y = 0; y < ch->m2mf_line_count; y++) {
        gf_dma_copy(s, ch->m2mf_dst, dst_offset, ch->m2mf_src, src_offset,
                    ch->m2mf_line_length);
        src_offset += ch->m2mf_src_pitch;
        dst_offset += ch->m2mf_dst_pitch;
    }
    dma_target = gf_ramin_read32(s, ch->m2mf_dst) >> 12 & 0xFF;
    if (dma_target == 0x03 || dma_target == 0x0b) {
        uint32_t redraw_offset = gf_dma_lin_lookup(s, ch->m2mf_dst,
                                                   ch->m2mf_dst_offset);
        uint32_t width = ch->m2mf_line_length / (gf_disp_bpp(s) >> 3);
        gf_redraw_area(s, redraw_offset, width, ch->m2mf_line_count);
    }
}

uint32_t gf_swizzle(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    bool xleft = true;
    bool yleft = height != 1;
    uint32_t xbit = 1;
    uint32_t ybit = 1;
    uint32_t rbit = 1;
    uint32_t r = 0;

    do {
        if (xleft) {
            if ((x & xbit) != 0) {
                r |= rbit;
            }
            rbit <<= 1;
            xbit <<= 1;
            xleft = xbit < width;
        }
        if (yleft) {
            if ((y & ybit) != 0) {
                r |= rbit;
            }
            rbit <<= 1;
            ybit <<= 1;
            yleft = ybit < height;
        }
    } while (xleft || yleft);
    return r;
}

static void gf_tfc(GeForceState *s, gf_channel *ch)
{
    uint16_t dx = ch->tfc_yx & 0xFFFF;
    uint16_t dy = ch->tfc_yx >> 16;
    int16_t clipx0 = (ch->tfc_clip_wx & 0xFFFF) - dx;
    int16_t clipy0 = (ch->tfc_clip_hy & 0xFFFF) - dy;
    int16_t clipx1 = clipx0 + (ch->tfc_clip_wx >> 16);
    int16_t clipy1 = clipy0 + (ch->tfc_clip_hy >> 16);
    uint32_t width = ch->tfc_hw & 0xFFFF;
    uint32_t height = ch->tfc_hw >> 16;
    uint32_t word_offset = 0;
    uint16_t x, y;

    if (ch->tfc_swizzled) {
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                if (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1) {
                    uint32_t srccolor;
                    if (ch->tfc_color_bytes == 4) {
                        srccolor = ch->tfc_words[word_offset];
                    } else if (ch->tfc_color_bytes == 2) {
                        uint16_t *tfc_words16 = (uint16_t *)ch->tfc_words;
                        srccolor = tfc_words16[word_offset];
                    } else {
                        uint8_t *tfc_words8 = (uint8_t *)ch->tfc_words;
                        srccolor = tfc_words8[word_offset];
                    }
                    gf_put_pixel_swzs(s, ch, ch->swzs_ofs +
                        gf_swizzle(x + dx, y + dy, ch->swzs_width,
                                   ch->swzs_height) * ch->swzs_color_bytes,
                        srccolor);
                }
                word_offset++;
            }
        }
    } else {
        uint32_t pitch = ch->s2d_pitch_dst;
        uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch +
                               dx * ch->s2d_color_bytes;
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                if (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1) {
                    uint32_t srccolor;
                    if (ch->tfc_color_bytes == 4) {
                        srccolor = ch->tfc_words[word_offset];
                    } else if (ch->tfc_color_bytes == 2) {
                        uint16_t *tfc_words16 = (uint16_t *)ch->tfc_words;
                        srccolor = tfc_words16[word_offset];
                    } else {
                        uint8_t *tfc_words8 = (uint8_t *)ch->tfc_words;
                        srccolor = tfc_words8[word_offset];
                    }
                    gf_put_pixel(s, ch, draw_offset, x, srccolor);
                }
                word_offset++;
            }
            draw_offset += pitch;
        }
    }
}

static void gf_sifm(GeForceState *s, gf_channel *ch, bool swizzled)
{
    uint16_t dx = ch->sifm_dyx & 0xFFFF;
    uint16_t dy = ch->sifm_dyx >> 16;
    uint16_t dwidth = ch->sifm_dhw & 0xFFFF;
    uint16_t dheight = ch->sifm_dhw >> 16;
    uint32_t spitch = ch->sifm_sfmt & 0xFFFF;
    uint16_t x, y;

    /* SIFM without scaling is used frequently in some operating systems */
    if (ch->sifm_dudx == 0x00100000 && ch->sifm_dvdy == 0x00100000) {
        uint16_t sx = (ch->sifm_syx & 0xFFFF) >> 4;
        uint16_t sy = (ch->sifm_syx >> 16) >> 4;
        uint32_t src_offset = ch->sifm_sofs + sy * spitch +
                              sx * ch->sifm_color_bytes;
        if (swizzled) {
            for (y = 0; y < dheight; y++) {
                for (x = 0; x < dwidth; x++) {
                    uint32_t srccolor = gf_get_pixel(s, ch->sifm_src,
                                                     src_offset, x,
                                                     ch->sifm_color_bytes);
                    if (ch->sifm_color_bytes == 2 &&
                        ch->swzs_color_bytes == 4) {
                        srccolor = gf_color_565_to_888(srccolor);
                    }
                    gf_put_pixel_swzs(s, ch, ch->swzs_ofs +
                        gf_swizzle(x + dx, y + dy, ch->swzs_width,
                                   ch->swzs_height) * ch->swzs_color_bytes,
                        srccolor);
                }
                src_offset += spitch;
            }
        } else {
            uint32_t dpitch = ch->s2d_pitch_dst;
            uint32_t draw_offset = ch->s2d_ofs_dst +
                dy * dpitch + dx * ch->s2d_color_bytes;
            uint32_t redraw_offset = gf_dma_lin_lookup(s, ch->s2d_img_dst,
                                                       draw_offset);
            for (y = 0; y < dheight; y++) {
                for (x = 0; x < dwidth; x++) {
                    uint32_t dstcolor = gf_get_pixel(s, ch->s2d_img_dst,
                                                     draw_offset, x,
                                                     ch->s2d_color_bytes);
                    uint32_t srccolor = gf_get_pixel(s, ch->sifm_src,
                                                     src_offset, x,
                                                     ch->sifm_color_bytes);
                    if (ch->sifm_color_fmt == 4) {
                        srccolor |= 0xFF000000;
                    }
                    gf_pixel_operation(s, ch, ch->sifm_operation, &dstcolor,
                                       &srccolor, ch->s2d_color_bytes,
                                       dx + x, dy + y);
                    gf_put_pixel(s, ch, draw_offset, x, dstcolor);
                }
                src_offset += spitch;
                draw_offset += dpitch;
            }
            gf_redraw_area(s, redraw_offset, dwidth, dheight);
        }
    } else {
        int32_t sx0 = ((ch->sifm_syx & 0xFFFF) << 16) - 0x80000;
        int32_t sy = (ch->sifm_syx & 0xFFFF0000) +
                     ((int32_t)ch->sifm_dvdy < 0 ? 0x80000 : -0x80000);
        if (sx0 < 0) {
            sx0 = 0;
        }
        if (sy < 0) {
            sy = 0;
        }
        if (swizzled) {
            for (y = 0; y < dheight; y++) {
                uint32_t sx = sx0;
                uint32_t src_offset = ch->sifm_sofs + (sy >> 20) * spitch;
                for (x = 0; x < dwidth; x++) {
                    uint32_t srccolor = gf_get_pixel(s, ch->sifm_src,
                                                     src_offset, sx >> 20,
                                                     ch->sifm_color_bytes);
                    if (ch->sifm_color_bytes == 2 &&
                        ch->swzs_color_bytes == 4) {
                        srccolor = gf_color_565_to_888(srccolor);
                    }
                    gf_put_pixel_swzs(s, ch, ch->swzs_ofs +
                        gf_swizzle(x + dx, y + dy, ch->swzs_width,
                                   ch->swzs_height) * ch->swzs_color_bytes,
                        srccolor);
                    sx += ch->sifm_dudx;
                }
                sy += ch->sifm_dvdy;
            }
        } else {
            uint32_t dpitch = ch->s2d_pitch_dst;
            uint32_t draw_offset = ch->s2d_ofs_dst +
                dy * dpitch + dx * ch->s2d_color_bytes;
            uint32_t redraw_offset = gf_dma_lin_lookup(s, ch->s2d_img_dst,
                                                       draw_offset);
            for (y = 0; y < dheight; y++) {
                uint32_t sx = sx0;
                uint32_t src_offset = ch->sifm_sofs + (sy >> 20) * spitch;
                for (x = 0; x < dwidth; x++) {
                    uint32_t dstcolor = gf_get_pixel(s, ch->s2d_img_dst,
                                                     draw_offset, x,
                                                     ch->s2d_color_bytes);
                    uint32_t srccolor = gf_get_pixel(s, ch->sifm_src,
                                                     src_offset, sx >> 20,
                                                     ch->sifm_color_bytes);
                    if (ch->sifm_color_fmt == 4) {
                        srccolor |= 0xFF000000;
                    }
                    gf_pixel_operation(s, ch, ch->sifm_operation, &dstcolor,
                                       &srccolor, ch->s2d_color_bytes,
                                       dx + x, dy + y);
                    gf_put_pixel(s, ch, draw_offset, x, dstcolor);
                    sx += ch->sifm_dudx;
                }
                sy += ch->sifm_dvdy;
                draw_offset += dpitch;
            }
            gf_redraw_area(s, redraw_offset, dwidth, dheight);
        }
    }
}

/*
 * ------------------------------------------------------------------------
 * Object method execution
 * ------------------------------------------------------------------------
 */

static void gf_execute_clip(GeForceState *s, gf_channel *ch, uint32_t method,
                            uint32_t param)
{
    if (method == 0x0c0) {
        ch->clip_x = (uint16_t)param;
        ch->clip_y = param >> 16;
    } else if (method == 0x0c1) {
        ch->clip_width = (uint16_t)param;
        ch->clip_height = param >> 16;
    }
}

static void gf_execute_m2mf(GeForceState *s, gf_channel *ch, uint32_t subc,
                            uint32_t method, uint32_t param)
{
    if (method == 0x061) {
        ch->m2mf_src = param;
    } else if (method == 0x062) {
        ch->m2mf_dst = param;
    } else if (method == 0x0c3) {
        ch->m2mf_src_offset = param;
    } else if (method == 0x0c4) {
        ch->m2mf_dst_offset = param;
    } else if (method == 0x0c5) {
        ch->m2mf_src_pitch = param;
    } else if (method == 0x0c6) {
        ch->m2mf_dst_pitch = param;
    } else if (method == 0x0c7) {
        ch->m2mf_line_length = param;
    } else if (method == 0x0c8) {
        ch->m2mf_line_count = param;
    } else if (method == 0x0c9) {
        ch->m2mf_format = param;
    } else if (method == 0x0ca) {
        ch->m2mf_buffer_notify = param;
        gf_m2mf(s, ch);
        if ((gf_ramin_read32(s, ch->schs[subc].notifier) & 0xFF) != 0x30) {
            gf_dma_write64(s, ch->schs[subc].notifier, 0x10 + 0x0,
                           gf_get_current_time(s));
            gf_dma_write32(s, ch->schs[subc].notifier, 0x10 + 0x8, 0);
            gf_dma_write32(s, ch->schs[subc].notifier, 0x10 + 0xC, 0);
        }
    }
}

static void gf_execute_rop(GeForceState *s, gf_channel *ch, uint32_t method,
                           uint32_t param)
{
    if (method == 0x0c0) {
        ch->rop = param;
    }
}

static void gf_execute_patt(GeForceState *s, gf_channel *ch, uint32_t method,
                            uint32_t param)
{
    if (method == 0x0c2) {
        ch->patt_shape = param;
    } else if (method == 0x0c3) {
        ch->patt_type_color = param == 2;
    } else if (method == 0x0c4) {
        ch->patt_bg_color = param;
    } else if (method == 0x0c5) {
        ch->patt_fg_color = param;
    } else if (method == 0x0c6 || method == 0x0c7) {
        uint32_t i;
        for (i = 0; i < 32; i++) {
            ch->patt_data_mono[i + (method & 1) * 32] =
                1 << (i ^ 7) & param;
        }
    } else if (method >= 0x100 && method < 0x110) {
        uint32_t i = (method - 0x100) * 4;
        ch->patt_data_color[i] = param & 0xFF;
        ch->patt_data_color[i + 1] = (param >> 8) & 0xFF;
        ch->patt_data_color[i + 2] = (param >> 16) & 0xFF;
        ch->patt_data_color[i + 3] = param >> 24;
    } else if (method >= 0x140 && method < 0x160) {
        uint32_t i = (method - 0x140) * 2;
        ch->patt_data_color[i] = param & 0xFFFF;
        ch->patt_data_color[i + 1] = param >> 16;
    } else if (method >= 0x1c0 && method < 0x200) {
        ch->patt_data_color[method - 0x1c0] = param;
    }
}

static void gf_execute_gdi(GeForceState *s, gf_channel *ch, uint32_t cls,
                           uint32_t method, uint32_t param)
{
    if (method == 0x0bf) {
        ch->gdi_operation = param;
    } else if (method == 0x0c0) {
        ch->gdi_color_fmt = param;
    } else if (method == 0x0c1) {
        ch->gdi_mono_fmt = param;
    } else if (method == 0x0ff) {
        ch->gdi_rect_color = param;
    } else if (method >= 0x100 && method < 0x140) {
        if (method & 1) {
            ch->gdi_rect_wh = param;
            gf_gdi_fillrect(s, ch, false);
        } else {
            ch->gdi_rect_xy = param;
        }
    } else if (method == 0x17d) {
        ch->gdi_clip_yx0 = param;
    } else if (method == 0x17e) {
        ch->gdi_clip_yx1 = param;
    } else if (method == 0x17f) {
        ch->gdi_rect_color = param;
    } else if (method >= 0x180 && method < 0x1c0) {
        if (method & 1) {
            ch->gdi_rect_yx1 = param;
            gf_gdi_fillrect(s, ch, true);
        } else {
            ch->gdi_rect_yx0 = param;
        }
    } else if ((method == 0x1fb && cls == 0x004a) ||
               (method == 0x2fb && cls == 0x004b)) {
        ch->gdi_clip_yx0 = param;
    } else if ((method == 0x1fc && cls == 0x004a) ||
               (method == 0x2fc && cls == 0x004b)) {
        ch->gdi_clip_yx1 = param;
    } else if ((method == 0x1fd && cls == 0x004a) ||
               (method == 0x2fd && cls == 0x004b)) {
        ch->gdi_fg_color = param;
    } else if ((method == 0x1fe && cls == 0x004a) ||
               (method == 0x2fe && cls == 0x004b)) {
        ch->gdi_image_swh = param;
    } else if ((method == 0x1ff && cls == 0x004a) ||
               (method == 0x2ff && cls == 0x004b)) {
        uint32_t width, height, word_count;
        ch->gdi_image_xy = param;
        width = ch->gdi_image_swh & 0xFFFF;
        height = ch->gdi_image_swh >> 16;
        word_count = GF_ALIGN(width * height, 32) >> 5;
        g_free(ch->gdi_words);
        ch->gdi_words_ptr = 0;
        ch->gdi_words_left = word_count;
        ch->gdi_words = g_new0(uint32_t, word_count);
    } else if ((method >= 0x200 && method < 0x280 && cls == 0x004a) ||
               (method >= 0x300 && method < 0x380 && cls == 0x004b)) {
        if (ch->gdi_words != NULL && ch->gdi_words_left != 0) {
            ch->gdi_words[ch->gdi_words_ptr++] = param;
            ch->gdi_words_left--;
            if (!ch->gdi_words_left) {
                gf_gdi_blit(s, ch, 0);
                g_free(ch->gdi_words);
                ch->gdi_words = NULL;
            }
        }
    } else if ((method == 0x2f9 && cls == 0x004a) ||
               (method == 0x4f9 && cls == 0x004b)) {
        ch->gdi_clip_yx0 = param;
    } else if ((method == 0x2fa && cls == 0x004a) ||
               (method == 0x4fa && cls == 0x004b)) {
        ch->gdi_clip_yx1 = param;
    } else if ((method == 0x2fb && cls == 0x004a) ||
               (method == 0x4fb && cls == 0x004b)) {
        ch->gdi_bg_color = param;
    } else if ((method == 0x2fc && cls == 0x004a) ||
               (method == 0x4fc && cls == 0x004b)) {
        ch->gdi_fg_color = param;
    } else if ((method == 0x2fd && cls == 0x004a) ||
               (method == 0x4fd && cls == 0x004b)) {
        ch->gdi_image_swh = param;
    } else if ((method == 0x2fe && cls == 0x004a) ||
               (method == 0x4fe && cls == 0x004b)) {
        ch->gdi_image_dwh = param;
    } else if ((method == 0x2ff && cls == 0x004a) ||
               (method == 0x4ff && cls == 0x004b)) {
        uint32_t width, height, word_count;
        ch->gdi_image_xy = param;
        width = ch->gdi_image_swh & 0xFFFF;
        height = ch->gdi_image_swh >> 16;
        word_count = GF_ALIGN(width * height, 32) >> 5;
        g_free(ch->gdi_words);
        ch->gdi_words_ptr = 0;
        ch->gdi_words_left = word_count;
        ch->gdi_words = g_new0(uint32_t, word_count);
    } else if ((method >= 0x300 && method < 0x380 && cls == 0x004a) ||
               (method >= 0x500 && method < 0x580 && cls == 0x004b)) {
        if (ch->gdi_words != NULL && ch->gdi_words_left != 0) {
            ch->gdi_words[ch->gdi_words_ptr++] = param;
            ch->gdi_words_left--;
            if (!ch->gdi_words_left) {
                gf_gdi_blit(s, ch, 1);
                g_free(ch->gdi_words);
                ch->gdi_words = NULL;
            }
        }
    } else if (method == 0x3fd) {
        ch->gdi_clip_yx0 = param;
    } else if (method == 0x3fe) {
        ch->gdi_clip_yx1 = param;
    } else if (method == 0x3ff) {
        ch->gdi_fg_color = param;
    }
}

static void gf_execute_swzsurf(GeForceState *s, gf_channel *ch,
                               uint32_t method, uint32_t param)
{
    if (method == 0x061) {
        ch->swzs_img_obj = param;
    } else if (method == 0x0c0) {
        uint32_t color_fmt;
        ch->swzs_fmt = param;
        ch->swzs_width = 1 << ((param >> 16) & 0xff);
        ch->swzs_height = 1 << (param >> 24);
        color_fmt = param & 0xffff;
        if (color_fmt == 1) {          /* Y8 */
            ch->swzs_color_bytes = 1;
        } else if (color_fmt == 2 ||   /* X1R5G5B5_Z1R5G5B5 */
                   color_fmt == 4) {   /* R5G6B5 */
            ch->swzs_color_bytes = 2;
        } else if (color_fmt == 0x6 || /* X8R8G8B8_Z8R8G8B8 */
                   color_fmt == 0xA || /* A8R8G8B8 */
                   color_fmt == 0xB) { /* Y32 */
            ch->swzs_color_bytes = 4;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "geforce: unknown swizzled surface color format: 0x%02x\n",
                color_fmt);
        }
    } else if (method == 0x0c1) {
        ch->swzs_ofs = param;
    }
}

static void gf_execute_chroma(GeForceState *s, gf_channel *ch,
                              uint32_t method, uint32_t param)
{
    if (method == 0x0c0) {
        ch->chroma_color_fmt = param;
    } else if (method == 0x0c1) {
        ch->chroma_color = param;
    }
}

static void gf_execute_rect(GeForceState *s, gf_channel *ch, uint32_t method,
                            uint32_t param)
{
    if (method == 0x0bf) {
        ch->rect_operation = param;
    } else if (method == 0x0c0) {
        ch->rect_color_fmt = param;
    } else if (method == 0x0c1) {
        ch->rect_color = param;
    } else if (method >= 0x100 && method < 0x120) {
        if (method & 1) {
            ch->rect_hw = param;
            gf_rect(s, ch);
        } else {
            ch->rect_yx = param;
        }
    }
}

static void gf_execute_imageblit(GeForceState *s, gf_channel *ch,
                                 uint32_t method, uint32_t param)
{
    if (method == 0x061) {
        ch->blit_color_key_enable = (gf_ramin_read32(s, param) & 0xFF) != 0x30;
    } else if (method == 0x0bf) {
        ch->blit_operation = param;
    } else if (method == 0x0c0) {
        ch->blit_syx = param;
    } else if (method == 0x0c1) {
        ch->blit_dyx = param;
    } else if (method == 0x0c2) {
        ch->blit_hw = param;
        gf_copyarea(s, ch);
    }
}

static void gf_update_color_bytes(uint32_t s2d_color_fmt, uint32_t color_fmt,
                                  uint32_t *color_bytes)
{
    if (s2d_color_fmt == 1) {      /* Y8 */
        *color_bytes = 1;          /* hack */
    } else if (color_fmt == 1 ||   /* R5G6B5 */
               color_fmt == 2 ||   /* A1R5G5B5 */
               color_fmt == 3) {   /* X1R5G5B5 */
        *color_bytes = 2;
    } else if (color_fmt == 4 ||   /* A8R8G8B8 */
               color_fmt == 5) {   /* X8R8G8B8 */
        *color_bytes = 4;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "geforce: unknown color format: 0x%02x\n", color_fmt);
    }
}

static void gf_update_color_bytes_ifc(GeForceState *s, gf_channel *ch)
{
    gf_update_color_bytes(ch->s2d_color_fmt, ch->ifc_color_fmt,
                          &ch->ifc_color_bytes);
}

static void gf_update_color_bytes_sifc(GeForceState *s, gf_channel *ch)
{
    gf_update_color_bytes(ch->s2d_color_fmt, ch->sifc_color_fmt,
                          &ch->sifc_color_bytes);
}

static void gf_update_color_bytes_tfc(GeForceState *s, gf_channel *ch)
{
    gf_update_color_bytes(ch->s2d_color_fmt, ch->tfc_color_fmt,
                          &ch->tfc_color_bytes);
}

void gf_update_color_bytes_iifc(GeForceState *s, gf_channel *ch)
{
    gf_update_color_bytes(0, ch->iifc_color_fmt, &ch->iifc_color_bytes);
}

void gf_update_color_bytes_s2d(GeForceState *s, gf_channel *ch)
{
    if (ch->s2d_color_fmt == 0x1) {        /* Y8 */
        ch->s2d_color_bytes = 1;
    } else if (ch->s2d_color_fmt == 0x2 || /* X1R5G5B5_Z1R5G5B5 */
               ch->s2d_color_fmt == 0x4 || /* R5G6B5 */
               ch->s2d_color_fmt == 0x5) { /* Y16 */
        ch->s2d_color_bytes = 2;
    } else if (ch->s2d_color_fmt == 0x6 || /* X8R8G8B8_Z8R8G8B8 */
               ch->s2d_color_fmt == 0x7 || /* X8R8G8B8_O8R8G8B8 */
               ch->s2d_color_fmt == 0xA || /* A8R8G8B8 */
               ch->s2d_color_fmt == 0xB) { /* Y32 */
        ch->s2d_color_bytes = 4;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "geforce: unknown 2d surface color format: 0x%02x\n",
                      ch->s2d_color_fmt);
    }
}

static void gf_execute_ifc(GeForceState *s, gf_channel *ch, uint32_t method,
                           uint32_t param)
{
    if (method == 0x061) {
        ch->ifc_color_key_enable = (gf_ramin_read32(s, param) & 0xFF) != 0x30;
    } else if (method == 0x062) {
        ch->ifc_clip_enable = (gf_ramin_read32(s, param) & 0xFF) != 0x30;
    } else if (method == 0x0bf) {
        ch->ifc_operation = param;
    } else if (method == 0x0c0) {
        ch->ifc_color_fmt = param;
        gf_update_color_bytes_ifc(s, ch);
        ch->ifc_pixels_per_word = 4 / ch->ifc_color_bytes;
    } else if (method == 0x0c1) {
        ch->ifc_x = 0;
        ch->ifc_y = 0;
        ch->ifc_ofs_x = param & 0xFFFF;
        ch->ifc_ofs_y = param >> 16;
        ch->ifc_draw_offset = ch->s2d_ofs_dst +
            ch->ifc_ofs_y * ch->s2d_pitch_dst +
            ch->ifc_ofs_x * ch->s2d_color_bytes;
        ch->ifc_redraw_offset = gf_dma_lin_lookup(s, ch->s2d_img_dst,
                                                  ch->ifc_draw_offset);
    } else if (method == 0x0c2) {
        ch->ifc_dst_width = param & 0xFFFF;
        ch->ifc_dst_height = param >> 16;
        ch->ifc_clip_x0 = 0;
        ch->ifc_clip_y0 = 0;
        ch->ifc_clip_x1 = ch->ifc_dst_width;
        ch->ifc_clip_y1 = ch->ifc_dst_height;
        if (ch->ifc_clip_enable) {
            int32_t clipx0 = ch->clip_x - ch->ifc_ofs_x;
            int32_t clipy0 = ch->clip_y - ch->ifc_ofs_y;
            int32_t clipx1 = clipx0 + ch->clip_width;
            int32_t clipy1 = clipy0 + ch->clip_height;
            ch->ifc_clip_x0 = MAX((int32_t)ch->ifc_clip_x0, clipx0);
            ch->ifc_clip_y0 = MAX((int32_t)ch->ifc_clip_y0, clipy0);
            ch->ifc_clip_x1 = MIN((int32_t)ch->ifc_clip_x1, clipx1);
            ch->ifc_clip_y1 = MIN((int32_t)ch->ifc_clip_y1, clipy1);
        }
    } else if (method == 0x0c3) {
        ch->ifc_src_width = param & 0xFFFF;
        ch->ifc_src_height = param >> 16;
    } else if (method >= 0x100 && method < 0x800) {
        gf_ifc(s, ch, param);
    }
}

static void gf_execute_surf2d(GeForceState *s, gf_channel *ch,
                              uint32_t method, uint32_t param)
{
    ch->s2d_locked = true;
    if (method == 0x061) {
        ch->s2d_img_src = param;
    } else if (method == 0x062) {
        ch->s2d_img_dst = param;
    } else if (method == 0x0c0) {
        uint32_t s2d_color_bytes_prev = ch->s2d_color_bytes;
        ch->s2d_color_fmt = param;
        gf_update_color_bytes_s2d(s, ch);
        if (ch->s2d_color_bytes != s2d_color_bytes_prev &&
            (ch->s2d_color_bytes == 1 || s2d_color_bytes_prev == 1)) {
            gf_update_color_bytes_ifc(s, ch);
            gf_update_color_bytes_sifc(s, ch);
            gf_update_color_bytes_tfc(s, ch);
        }
    } else if (method == 0x0c1) {
        ch->s2d_pitch_src = param & 0xFFFF;
        ch->s2d_pitch_dst = param >> 16;
    } else if (method == 0x0c2) {
        ch->s2d_ofs_src = param;
    } else if (method == 0x0c3) {
        ch->s2d_ofs_dst = param;
    }
}

static void gf_execute_iifc(GeForceState *s, gf_channel *ch, uint32_t method,
                            uint32_t param)
{
    if (method == 0x061) {
        ch->iifc_palette = param;
    } else if (method == 0x0f9) {
        ch->iifc_operation = param;
    } else if (method == 0x0fa) {
        ch->iifc_color_fmt = param;
        gf_update_color_bytes_iifc(s, ch);
    } else if (method == 0x0fb) {
        ch->iifc_bpp4 = param;
    } else if (method == 0x0fc) {
        ch->iifc_palette_ofs = param;
    } else if (method == 0x0fd) {
        ch->iifc_yx = param;
    } else if (method == 0x0fe) {
        ch->iifc_dhw = param;
    } else if (method == 0x0ff) {
        uint32_t width, height, word_count;
        ch->iifc_shw = param;
        width = ch->iifc_shw & 0xFFFF;
        height = ch->iifc_shw >> 16;
        word_count = GF_ALIGN(width * height * (ch->iifc_bpp4 ? 4 : 8),
                              32) >> 5;
        g_free(ch->iifc_words);
        ch->iifc_words_ptr = 0;
        ch->iifc_words_left = word_count;
        ch->iifc_words = g_new0(uint32_t, word_count);
    } else if (method >= 0x100 && method < 0x800) {
        if (ch->iifc_words != NULL && ch->iifc_words_left != 0) {
            ch->iifc_words[ch->iifc_words_ptr++] = param;
            ch->iifc_words_left--;
            if (!ch->iifc_words_left) {
                gf_iifc(s, ch);
                g_free(ch->iifc_words);
                ch->iifc_words = NULL;
            }
        }
    }
}

static void gf_execute_sifc(GeForceState *s, gf_channel *ch, uint32_t method,
                            uint32_t param)
{
    if (method == 0x0bf) {
        ch->sifc_operation = param;
    } else if (method == 0x0c0) {
        ch->sifc_color_fmt = param;
        gf_update_color_bytes_sifc(s, ch);
    } else if (method == 0x0c1) {
        ch->sifc_shw = param;
    } else if (method == 0x0c2) {
        ch->sifc_dxds = param;
    } else if (method == 0x0c3) {
        ch->sifc_dydt = param;
    } else if (method == 0x0c4) {
        ch->sifc_clip_yx = param;
    } else if (method == 0x0c5) {
        ch->sifc_clip_hw = param;
    } else if (method == 0x0c6) {
        uint32_t width, height, word_count;
        ch->sifc_syx = param;
        width = ch->sifc_shw & 0xFFFF;
        height = ch->sifc_shw >> 16;
        word_count = GF_ALIGN(width * height * ch->sifc_color_bytes, 4) >> 2;
        g_free(ch->sifc_words);
        ch->sifc_words_ptr = 0;
        ch->sifc_words_left = word_count;
        ch->sifc_words = g_new0(uint32_t, word_count);
    } else if (method >= 0x100 && method < 0x800) {
        if (ch->sifc_words != NULL && ch->sifc_words_left != 0) {
            ch->sifc_words[ch->sifc_words_ptr++] = param;
            ch->sifc_words_left--;
            if (!ch->sifc_words_left) {
                gf_sifc(s, ch);
                g_free(ch->sifc_words);
                ch->sifc_words = NULL;
            }
        }
    }
}

static void gf_execute_beta(GeForceState *s, gf_channel *ch, uint32_t method,
                            uint32_t param)
{
    if (method == 0x0c0) {
        ch->beta = param;
    }
}

static void gf_execute_tfc(GeForceState *s, gf_channel *ch, uint32_t method,
                           uint32_t param)
{
    if (method == 0x061) {
        uint8_t cls8 = gf_ramin_read32(s, param);
        ch->tfc_swizzled = cls8 == 0x52 || cls8 == 0x9e;
    } else if (method == 0x0c0) {
        ch->tfc_color_fmt = param;
        gf_update_color_bytes_tfc(s, ch);
    } else if (method == 0x0c1) {
        ch->tfc_yx = param;
    } else if (method == 0x0c2) {
        ch->tfc_hw = param;
        ch->tfc_upload = param == 0x01000100 && ch->tfc_yx == 0 &&
            ch->tfc_color_fmt == 4 && ch->s2d_color_fmt == 0xA &&
            ch->s2d_pitch_src == 0x0400 && ch->s2d_pitch_dst == 0x0400;
        if (ch->tfc_upload) {
            ch->tfc_upload_offset = ch->s2d_ofs_dst;
        } else {
            uint32_t width = ch->tfc_hw & 0xFFFF;
            uint32_t height = ch->tfc_hw >> 16;
            uint32_t word_count =
                GF_ALIGN(width * height * ch->tfc_color_bytes, 4) >> 2;
            g_free(ch->tfc_words);
            ch->tfc_words_ptr = 0;
            ch->tfc_words_left = word_count;
            ch->tfc_words = g_new0(uint32_t, word_count);
        }
    } else if (method == 0x0c3) {
        ch->tfc_clip_wx = param;
    } else if (method == 0x0c4) {
        ch->tfc_clip_hy = param;
    } else if (method >= 0x100 && method < 0x800) {
        if (ch->tfc_upload) {
            gf_dma_write32(s, ch->s2d_img_dst, ch->tfc_upload_offset, param);
            ch->tfc_upload_offset += 4;
        } else if (ch->tfc_words != NULL && ch->tfc_words_left != 0) {
            ch->tfc_words[ch->tfc_words_ptr++] = param;
            ch->tfc_words_left--;
            if (!ch->tfc_words_left) {
                gf_tfc(s, ch);
                g_free(ch->tfc_words);
                ch->tfc_words = NULL;
            }
        }
    }
}

static void gf_execute_sifm(GeForceState *s, gf_channel *ch, uint32_t cls,
                            uint32_t method, uint32_t param)
{
    if (method == 0x061) {
        ch->sifm_src = param;
    } else if (method == 0x066) {
        uint8_t surf_cls8 = gf_ramin_read32(s, param);
        bool swizzled = surf_cls8 == 0x52 || surf_cls8 == 0x9e;
        if (cls == 0x0389) {
            ch->sifm_swizzled_0389 = swizzled;
        } else {
            ch->sifm_swizzled = swizzled;
        }
    } else if (method == 0x0c0) {
        ch->sifm_color_fmt = param;
        if (ch->sifm_color_fmt == 8) {          /* ??? */
            ch->sifm_color_bytes = 1;
        } else if (ch->sifm_color_fmt == 1 ||   /* A1R5G5B5 */
                   ch->sifm_color_fmt == 2 ||   /* X1R5G5B5 */
                   ch->sifm_color_fmt == 7) {   /* R5G6B5 */
            ch->sifm_color_bytes = 2;
        } else if (ch->sifm_color_fmt == 3 ||   /* A8R8G8B8 */
                   ch->sifm_color_fmt == 4) {   /* X8R8G8B8 */
            ch->sifm_color_bytes = 4;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: unknown sifm color format: 0x%02x\n",
                          ch->sifm_color_fmt);
        }
    } else if (method == 0x0c1) {
        ch->sifm_operation = param;
    } else if (method == 0x0c4) {
        ch->sifm_dyx = param;
    } else if (method == 0x0c5) {
        ch->sifm_dhw = param;
    } else if (method == 0x0c6) {
        ch->sifm_dudx = param;
    } else if (method == 0x0c7) {
        ch->sifm_dvdy = param;
    } else if (method == 0x100) {
        ch->sifm_shw = param;
    } else if (method == 0x101) {
        ch->sifm_sfmt = param;
    } else if (method == 0x102) {
        ch->sifm_sofs = param;
    } else if (method == 0x103) {
        ch->sifm_syx = param;
        gf_sifm(s, ch, cls == 0x0389 ? ch->sifm_swizzled_0389
                                     : ch->sifm_swizzled);
    }
}

/*
 * ------------------------------------------------------------------------
 * Command execution and FIFO processing
 * ------------------------------------------------------------------------
 */

int gf_execute_command(GeForceState *s, uint32_t chid, uint32_t subc,
                       uint32_t method, uint32_t param)
{
    int result = 0;
    bool software_method = false;
    gf_channel *ch = &s->chs[chid];

    if (method == 0x000) {
        if (ch->schs[subc].engine == 0x01) {
            uint32_t word1 = gf_ramin_read32(s, ch->schs[subc].object + 0x4);
            uint32_t word0;
            uint8_t cls8;
            if (s->card_type < 0x40) {
                word1 = (word1 & 0x0000FFFF) |
                        (ch->schs[subc].notifier >> 4 << 16);
            } else {
                word1 = (word1 & 0xFFF00000) | (ch->schs[subc].notifier >> 4);
            }
            word0 = gf_ramin_read32(s, ch->schs[subc].object);
            cls8 = word0;
            if (cls8 == 0x4a || cls8 == 0x4b) {
                if (s->card_type < 0x40) {
                    word0 = (word0 & 0xFFFC7FFF) | (ch->gdi_operation << 15);
                    word1 = (word1 & 0xFFFFFFFC) | ch->gdi_mono_fmt;
                } else {
                    word0 = (word0 & 0xFFC7FFFF) | (ch->gdi_operation << 19);
                    word1 = (word1 & 0xFCFFFFFF) | (ch->gdi_mono_fmt << 24);
                }
                gf_ramin_write32(s, ch->schs[subc].object, word0);
            } else if (cls8 == 0x62) {
                if (s->card_type < 0x40) {
                    gf_ramin_write32(s, ch->schs[subc].object + 0x8,
                                     (ch->s2d_img_src >> 4) |
                                     (ch->s2d_img_dst >> 4 << 16));
                } else {
                    gf_ramin_write32(s, ch->schs[subc].object + 0x8,
                                     ch->s2d_img_src >> 4);
                    gf_ramin_write32(s, ch->schs[subc].object + 0xC,
                                     ch->s2d_img_dst >> 4);
                }
            } else if (cls8 == 0x64) {
                gf_ramin_write32(s, ch->schs[subc].object + 0x8,
                                 ch->iifc_palette >> 4);
                if (s->card_type < 0x40) {
                    word0 = (word0 & 0xFFFC7FFF) | (ch->iifc_operation << 15);
                } else {
                    word0 = (word0 & 0xFFC7FFFF) | (ch->iifc_operation << 19);
                }
                gf_ramin_write32(s, ch->schs[subc].object, word0);
                if (s->card_type < 0x40) {
                    word1 = (word1 & 0xFFFF00FF) |
                            ((ch->iifc_color_fmt + 9) << 8);
                } else {
                    /* should be stored somewhere else */
                    gf_ramin_write32(s, ch->schs[subc].object + 0x10,
                                     ch->iifc_color_fmt);
                }
            }
            gf_ramin_write32(s, ch->schs[subc].object + 0x4, word1);
        }
        gf_ramht_lookup(s, param, chid, &ch->schs[subc].object,
                        &ch->schs[subc].engine);
        if (ch->schs[subc].engine == 0x01) {
            uint32_t word1 = gf_ramin_read32(s, ch->schs[subc].object + 0x4);
            uint32_t word0;
            uint8_t cls8;
            if (s->card_type < 0x40) {
                ch->schs[subc].notifier = word1 >> 16 << 4;
            } else {
                ch->schs[subc].notifier = (word1 & 0xFFFFF) << 4;
            }
            word0 = gf_ramin_read32(s, ch->schs[subc].object);
            cls8 = word0;
            if (cls8 == 0x48) {
                /* Hack for XFree86 4.1.0 - 4.3.0 */
                if (!ch->s2d_locked) {
                    uint32_t srcdst =
                        gf_ramin_read32(s, ch->schs[subc].object + 0x8);
                    ch->s2d_img_src = (srcdst & 0xFFFF) << 4;
                    ch->s2d_img_dst = srcdst >> 16 << 4;
                    ch->s2d_color_fmt = s->graph_bpixel & 0xf;
                    gf_update_color_bytes_s2d(s, ch);
                    ch->s2d_pitch_src = s->graph_pitch0 & 0xffff;
                    ch->s2d_pitch_dst = ch->s2d_pitch_src;
                    ch->s2d_ofs_src = s->graph_offset0;
                    ch->s2d_ofs_dst = s->graph_offset0;
                }
            } else if (cls8 == 0x4a || cls8 == 0x4b) {
                if (s->card_type < 0x40) {
                    ch->gdi_operation = (word0 >> 15) & 7;
                    ch->gdi_mono_fmt = word1 & 3;
                } else {
                    ch->gdi_operation = (word0 >> 19) & 7;
                    ch->gdi_mono_fmt = (word1 >> 24) & 3;
                }
            } else if (cls8 == 0x62) {
                if (s->card_type < 0x40) {
                    uint32_t srcdst =
                        gf_ramin_read32(s, ch->schs[subc].object + 0x8);
                    ch->s2d_img_src = (srcdst & 0xFFFF) << 4;
                    ch->s2d_img_dst = srcdst >> 16 << 4;
                } else {
                    ch->s2d_img_src =
                        gf_ramin_read32(s, ch->schs[subc].object + 0x8) << 4;
                    ch->s2d_img_dst =
                        gf_ramin_read32(s, ch->schs[subc].object + 0xC) << 4;
                }
            } else if (cls8 == 0x64) {
                uint32_t shift;
                ch->iifc_palette =
                    gf_ramin_read32(s, ch->schs[subc].object + 0x8) << 4;
                shift = s->card_type < 0x40 ? 15 : 19;
                ch->iifc_operation = (word0 >> shift) & 7;
                if (s->card_type < 0x40) {
                    ch->iifc_color_fmt = (word1 >> 8 & 0xFF) - 9;
                } else {
                    /* should be stored somewhere else */
                    ch->iifc_color_fmt =
                        gf_ramin_read32(s, ch->schs[subc].object + 0x10);
                    if (ch->iifc_color_fmt == 0) {
                        ch->iifc_color_fmt = 1;
                    }
                }
                gf_update_color_bytes_iifc(s, ch);
            } else if (cls8 == 0x96 || cls8 == 0x97) {
                gf_execute_d3d(s, ch, word0 & s->class_mask, 0, 0);
            }
        } else if (ch->schs[subc].engine == 0x00) {
            software_method = true;
        }
    } else if (method == 0x014) {
        s->fifo_cache1_ref_cnt = param;
    } else if (method == 0x018) {
        uint32_t semaphore_obj;
        gf_ramht_lookup(s, param, chid, &semaphore_obj, NULL);
        s->fifo_cache1_semaphore = semaphore_obj >> 4;
    } else if (method == 0x019) {
        s->fifo_cache1_semaphore &= 0x000FFFFF;
        s->fifo_cache1_semaphore |= param << 20;
    } else if (method == 0x01a || method == 0x01b) {
        uint32_t semaphore_obj = (s->fifo_cache1_semaphore & 0x000FFFFF) << 4;
        uint32_t semaphore_offset = s->fifo_cache1_semaphore >> 20;
        if (method == 0x01a) {
            if (gf_dma_read32(s, semaphore_obj, semaphore_offset) != param) {
                s->fifo_wait_acquire = true;
                s->fifo_wait = true;
                result = 2;
            }
        } else {
            gf_dma_write32(s, semaphore_obj, semaphore_offset, param);
        }
    } else if (method >= 0x040) {
        if (ch->schs[subc].engine == 0x01) {
            uint32_t cls;
            uint8_t cls8;
            if (method >= 0x060 && method < 0x080) {
                gf_ramht_lookup(s, param, chid, &param, NULL);
            }
            cls = gf_ramin_read32(s, ch->schs[subc].object) & s->class_mask;
            cls8 = cls;
            switch (cls8) {
            case 0x19:
                gf_execute_clip(s, ch, method, param);
                break;
            case 0x39:
                gf_execute_m2mf(s, ch, subc, method, param);
                break;
            case 0x43:
                gf_execute_rop(s, ch, method, param);
                break;
            case 0x44:
            case 0x18:
                gf_execute_patt(s, ch, method, param);
                break;
            case 0x4a:
            case 0x4b:
                gf_execute_gdi(s, ch, cls, method, param);
                break;
            case 0x52:
            case 0x9e:
                gf_execute_swzsurf(s, ch, method, param);
                break;
            case 0x57:
                gf_execute_chroma(s, ch, method, param);
                break;
            case 0x5e:
                gf_execute_rect(s, ch, method, param);
                break;
            case 0x5f:
            case 0x9f:
                gf_execute_imageblit(s, ch, method, param);
                break;
            case 0x61:
            case 0x65:
            case 0x8a:
            case 0x21:
                gf_execute_ifc(s, ch, method, param);
                break;
            case 0x62:
                gf_execute_surf2d(s, ch, method, param);
                break;
            case 0x64:
                gf_execute_iifc(s, ch, method, param);
                break;
            case 0x66:
            case 0x76:
                gf_execute_sifc(s, ch, method, param);
                break;
            case 0x72:
                gf_execute_beta(s, ch, method, param);
                break;
            case 0x7b:
                gf_execute_tfc(s, ch, method, param);
                break;
            case 0x89:
                gf_execute_sifm(s, ch, cls, method, param);
                break;
            case 0x96:
            case 0x97:
                gf_execute_d3d(s, ch, cls, method, param);
                if (s->fifo_wait_flip) {
                    result = 1;
                }
                break;
            }
            if (ch->notify_pending) {
                ch->notify_pending = false;
                if ((gf_ramin_read32(s, ch->schs[subc].notifier) & 0xFF) !=
                    0x30) {
                    gf_dma_write64(s, ch->schs[subc].notifier, 0x0,
                                   gf_get_current_time(s));
                    gf_dma_write32(s, ch->schs[subc].notifier, 0x8, 0);
                    gf_dma_write32(s, ch->schs[subc].notifier, 0xC, 0);
                }
                if (ch->notify_type) {
                    uint32_t notifier;
                    s->graph_intr |= 0x00000001;
                    gf_update_irq_level(s);
                    s->graph_nsource |= 0x00000001;
                    s->graph_notify = 0x00110000;
                    notifier = ch->schs[subc].notifier >> 4;
                    if (s->card_type < 0x40) {
                        s->graph_ctx_switch2 = notifier << 16;
                    } else {
                        s->graph_ctx_switch1 = notifier;
                    }
                    s->graph_ctx_switch4 = ch->schs[subc].object >> 4;
                    s->graph_trapped_addr = (method << 2) | (subc << 16) |
                                            (chid << 20);
                    s->graph_trapped_data = param;
                    s->fifo_wait_notify = true;
                    s->fifo_wait = true;
                }
            }
            if (method == 0x041) {
                ch->notify_pending = true;
                ch->notify_type = param;
            } else if (method == 0x060) {
                ch->schs[subc].notifier = param;
            }
        } else if (ch->schs[subc].engine == 0x00) {
            software_method = true;
        }
    }
    if (software_method) {
        s->fifo_wait_soft = true;
        s->fifo_wait = true;
        s->fifo_intr |= 0x00000001;
        gf_update_irq_level(s);
        s->fifo_cache1_pull0 |= 0x00000100;
        s->fifo_cache1_method[(s->fifo_cache1_put / 4) &
                              (GEFORCE_CACHE1_SIZE - 1)] =
            (method << 2) | (subc << 13);
        s->fifo_cache1_data[(s->fifo_cache1_put / 4) &
                            (GEFORCE_CACHE1_SIZE - 1)] = param;
        s->fifo_cache1_put += 4;
        if (s->fifo_cache1_put == GEFORCE_CACHE1_SIZE * 4) {
            s->fifo_cache1_put = 0;
        }
        result = 1;
    }
    return result;
}

void gf_update_fifo_wait(GeForceState *s)
{
    s->fifo_wait = s->fifo_wait_soft || s->fifo_wait_notify ||
                   s->fifo_wait_flip || s->fifo_wait_acquire;
}

void gf_fifo_process_all(GeForceState *s)
{
    uint32_t offset = (s->fifo_cache1_push1 & 0x1f) + 1;
    uint32_t i;

    for (i = 0; i < GEFORCE_CHANNEL_COUNT; i++) {
        gf_fifo_process(s, (i + offset) & 0x1f);
    }
}

void gf_fifo_process(GeForceState *s, uint32_t chid)
{
    uint32_t oldchid;
    uint32_t sro = 0;
    gf_channel *ch;

    if (s->fifo_wait) {
        return;
    }
    if ((s->fifo_mode & (1 << chid)) == 0) {
        return;
    }
    if ((s->fifo_cache1_push0 & 1) == 0) {
        return;
    }
    if ((s->fifo_cache1_pull0 & 1) == 0) {
        return;
    }
    oldchid = s->fifo_cache1_push1 & 0x1F;
    if (oldchid == chid) {
        if (s->fifo_cache1_dma_put == s->fifo_cache1_dma_get) {
            return;
        }
    } else {
        if (gf_ramfc_read32(s, chid, 0x0) == gf_ramfc_read32(s, chid, 0x4)) {
            return;
        }
    }
    if (oldchid != chid) {
        gf_ramfc_write32(s, oldchid, 0x0, s->fifo_cache1_dma_put);
        gf_ramfc_write32(s, oldchid, 0x4, s->fifo_cache1_dma_get);
        gf_ramfc_write32(s, oldchid, 0x8, s->fifo_cache1_ref_cnt);
        gf_ramfc_write32(s, oldchid, 0xC, s->fifo_cache1_dma_instance);
        if (s->card_type >= 0x20) {
            sro = s->card_type < 0x40 ? 0x2C : 0x30;
            gf_ramfc_write32(s, oldchid, sro, s->fifo_cache1_semaphore);
        }
        if (s->card_type >= 0x40) {
            gf_ramfc_write32(s, oldchid, 0x38, s->fifo_grctx_instance);
        }
        s->fifo_cache1_dma_put = gf_ramfc_read32(s, chid, 0x0);
        s->fifo_cache1_dma_get = gf_ramfc_read32(s, chid, 0x4);
        s->fifo_cache1_ref_cnt = gf_ramfc_read32(s, chid, 0x8);
        s->fifo_cache1_dma_instance = gf_ramfc_read32(s, chid, 0xC);
        if (s->card_type >= 0x20) {
            s->fifo_cache1_semaphore = gf_ramfc_read32(s, chid, sro);
        }
        if (s->card_type >= 0x40) {
            s->fifo_grctx_instance = gf_ramfc_read32(s, chid, 0x38);
            s->graph_ctxctl_cur = s->fifo_grctx_instance | 0x01000000;
        }
        s->fifo_cache1_push1 = (s->fifo_cache1_push1 & ~0x1F) | chid;
    }
    s->fifo_cache1_dma_push |= 0x100;
    if (s->fifo_cache1_dma_instance == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "geforce: fifo: DMA instance = 0\n");
        return;
    }
    ch = &s->chs[chid];
    while (s->fifo_cache1_dma_get != s->fifo_cache1_dma_put) {
        uint32_t word = gf_dma_read32(s, s->fifo_cache1_dma_instance << 4,
                                      s->fifo_cache1_dma_get);
        s->fifo_cache1_dma_get += 4;
        if (ch->dma_state.mcnt) {
            int cmd_result = gf_execute_command(s, chid, ch->dma_state.subc,
                                                ch->dma_state.mthd, word);
            if (cmd_result <= 1) {
                if (!ch->dma_state.ni) {
                    ch->dma_state.mthd++;
                }
                ch->dma_state.mcnt--;
            } else {
                s->fifo_cache1_dma_get -= 4;
            }
            if (cmd_result != 0) {
                break;
            }
        } else {
            if ((word & 0xe0000003) == 0x20000000) {
                /* old jump */
                s->fifo_cache1_dma_get = word & 0x1fffffff;
            } else if ((word & 3) == 1) {
                /* jump */
                s->fifo_cache1_dma_get = word & 0xfffffffc;
            } else if ((word & 3) == 2) {
                /* call */
                if (ch->subr_active) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                        "geforce: fifo: call with subroutine active\n");
                    break;
                }
                ch->subr_return = s->fifo_cache1_dma_get;
                ch->subr_active = true;
                s->fifo_cache1_dma_get = word & 0xfffffffc;
            } else if (word == 0x00020000) {
                /* return */
                if (!ch->subr_active) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                        "geforce: fifo: return with subroutine inactive\n");
                    break;
                }
                s->fifo_cache1_dma_get = ch->subr_return;
                ch->subr_active = false;
            } else if ((word & 0xa0030003) == 0) {
                ch->dma_state.mthd = (word >> 2) & 0x7ff;
                ch->dma_state.subc = (word >> 13) & 7;
                ch->dma_state.mcnt = (word >> 18) & 0x7ff;
                ch->dma_state.ni = word & 0x40000000;
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "geforce: fifo: unexpected word 0x%08x\n",
                              word);
                break;
            }
        }
    }
}
