/*
 * QEMU NVIDIA Quadro2 Pro (NV15GL) emulation
 *
 * PCI/AGP display adapter:
 *   BAR0: 16MB non-prefetchable MMIO register aperture
 *   BAR1: 128MB prefetchable linear framebuffer
 *   Capabilities: Power Management (0x60), AGP 2.0 (0x44)
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
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/display/edid.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "vga_regs.h"
#include "geforce_int.h"

/*
 * ------------------------------------------------------------------------
 * VRAM / RAMIN / physical / DMA object accessors
 * ------------------------------------------------------------------------
 */

uint8_t gf_vram_read8(GeForceState *s, uint32_t addr)
{
    return s->vga.vram_ptr[addr & s->memsize_mask];
}

uint16_t gf_vram_read16(GeForceState *s, uint32_t addr)
{
    addr &= s->memsize_mask;
    if (likely(addr + 2 <= s->memsize)) {
        return lduw_le_p(s->vga.vram_ptr + addr);
    }
    return gf_vram_read8(s, addr) | gf_vram_read8(s, addr + 1) << 8;
}

uint32_t gf_vram_read32(GeForceState *s, uint32_t addr)
{
    addr &= s->memsize_mask;
    if (likely(addr + 4 <= s->memsize)) {
        return ldl_le_p(s->vga.vram_ptr + addr);
    }
    return gf_vram_read16(s, addr) | gf_vram_read16(s, addr + 2) << 16;
}

uint64_t gf_vram_read64(GeForceState *s, uint32_t addr)
{
    addr &= s->memsize_mask;
    if (likely(addr + 8 <= s->memsize)) {
        return ldq_le_p(s->vga.vram_ptr + addr);
    }
    return (uint64_t)gf_vram_read32(s, addr) |
           (uint64_t)gf_vram_read32(s, addr + 4) << 32;
}

void gf_vram_write8(GeForceState *s, uint32_t addr, uint8_t val)
{
    s->vga.vram_ptr[addr & s->memsize_mask] = val;
}

void gf_vram_write16(GeForceState *s, uint32_t addr, uint16_t val)
{
    addr &= s->memsize_mask;
    if (likely(addr + 2 <= s->memsize)) {
        stw_le_p(s->vga.vram_ptr + addr, val);
        return;
    }
    gf_vram_write8(s, addr, val);
    gf_vram_write8(s, addr + 1, val >> 8);
}

void gf_vram_write32(GeForceState *s, uint32_t addr, uint32_t val)
{
    addr &= s->memsize_mask;
    if (likely(addr + 4 <= s->memsize)) {
        stl_le_p(s->vga.vram_ptr + addr, val);
        return;
    }
    gf_vram_write16(s, addr, val);
    gf_vram_write16(s, addr + 2, val >> 16);
}

void gf_vram_write64(GeForceState *s, uint32_t addr, uint64_t val)
{
    addr &= s->memsize_mask;
    if (likely(addr + 8 <= s->memsize)) {
        stq_le_p(s->vga.vram_ptr + addr, val);
        return;
    }
    gf_vram_write32(s, addr, val);
    gf_vram_write32(s, addr + 4, val >> 32);
}

uint8_t gf_ramin_read8(GeForceState *s, uint32_t addr)
{
    return gf_vram_read8(s, addr ^ s->ramin_flip);
}

uint16_t gf_ramin_read16(GeForceState *s, uint32_t addr)
{
    return gf_vram_read16(s, addr ^ s->ramin_flip);
}

uint32_t gf_ramin_read32(GeForceState *s, uint32_t addr)
{
    return gf_vram_read32(s, addr ^ s->ramin_flip);
}

void gf_ramin_write8(GeForceState *s, uint32_t addr, uint8_t val)
{
    gf_vram_write8(s, addr ^ s->ramin_flip, val);
}

void gf_ramin_write32(GeForceState *s, uint32_t addr, uint32_t val)
{
    gf_vram_write32(s, addr ^ s->ramin_flip, val);
}

static uint8_t gf_physical_read8(GeForceState *s, uint32_t addr)
{
    uint8_t data;
    pci_dma_read(&s->parent_obj, addr, &data, 1);
    return data;
}

static uint16_t gf_physical_read16(GeForceState *s, uint32_t addr)
{
    uint8_t data[2];
    pci_dma_read(&s->parent_obj, addr, data, 2);
    return lduw_le_p(data);
}

static uint32_t gf_physical_read32(GeForceState *s, uint32_t addr)
{
    uint8_t data[4];
    pci_dma_read(&s->parent_obj, addr, data, 4);
    return ldl_le_p(data);
}

static uint64_t gf_physical_read64(GeForceState *s, uint32_t addr)
{
    uint8_t data[8];
    pci_dma_read(&s->parent_obj, addr, data, 8);
    return ldq_le_p(data);
}

static void gf_physical_write8(GeForceState *s, uint32_t addr, uint8_t val)
{
    pci_dma_write(&s->parent_obj, addr, &val, 1);
}

static void gf_physical_write16(GeForceState *s, uint32_t addr, uint16_t val)
{
    uint8_t data[2];
    stw_le_p(data, val);
    pci_dma_write(&s->parent_obj, addr, data, 2);
}

static void gf_physical_write32(GeForceState *s, uint32_t addr, uint32_t val)
{
    uint8_t data[4];
    stl_le_p(data, val);
    pci_dma_write(&s->parent_obj, addr, data, 4);
}

static void gf_physical_write64(GeForceState *s, uint32_t addr, uint64_t val)
{
    uint8_t data[8];
    stq_le_p(data, val);
    pci_dma_write(&s->parent_obj, addr, data, 8);
}

uint32_t gf_dma_pt_lookup(GeForceState *s, uint32_t object, uint32_t addr)
{
    uint32_t addr_adj = addr + (gf_ramin_read32(s, object) >> 20);
    uint32_t page_offset = addr_adj & 0xFFF;
    uint32_t page_index = addr_adj >> 12;
    uint32_t page = gf_ramin_read32(s, object + 8 + page_index * 4) &
                    0xFFFFF000;
    return page | page_offset;
}

uint32_t gf_dma_lin_lookup(GeForceState *s, uint32_t object, uint32_t addr)
{
    uint32_t adjust = gf_ramin_read32(s, object) >> 20;
    uint32_t base = gf_ramin_read32(s, object + 8) & 0xFFFFF000;
    return base + adjust + addr;
}

#define GF_DMA_ACCESSOR(bits)                                               \
uint##bits##_t gf_dma_read##bits(GeForceState *s, uint32_t object,          \
                                 uint32_t addr)                             \
{                                                                           \
    uint32_t flags = gf_ramin_read32(s, object);                            \
    uint32_t addr_abs;                                                      \
    if (flags & 0x00002000) {                                               \
        addr_abs = gf_dma_lin_lookup(s, object, addr);                      \
    } else {                                                                \
        addr_abs = gf_dma_pt_lookup(s, object, addr);                       \
    }                                                                       \
    if (flags & 0x00020000) {                                               \
        return gf_physical_read##bits(s, addr_abs);                         \
    } else {                                                                \
        return gf_vram_read##bits(s, addr_abs);                             \
    }                                                                       \
}                                                                           \
                                                                            \
void gf_dma_write##bits(GeForceState *s, uint32_t object, uint32_t addr,    \
                        uint##bits##_t val)                                 \
{                                                                           \
    uint32_t flags = gf_ramin_read32(s, object);                            \
    uint32_t addr_abs;                                                      \
    if (flags & 0x00002000) {                                               \
        addr_abs = gf_dma_lin_lookup(s, object, addr);                      \
    } else {                                                                \
        addr_abs = gf_dma_pt_lookup(s, object, addr);                       \
    }                                                                       \
    if (flags & 0x00020000) {                                               \
        gf_physical_write##bits(s, addr_abs, val);                          \
    } else {                                                                \
        gf_vram_write##bits(s, addr_abs, val);                              \
    }                                                                       \
}

GF_DMA_ACCESSOR(8)
GF_DMA_ACCESSOR(16)
GF_DMA_ACCESSOR(32)
GF_DMA_ACCESSOR(64)

void gf_dma_copy(GeForceState *s, uint32_t dst_obj, uint32_t dst_addr,
                 uint32_t src_obj, uint32_t src_addr, uint32_t byte_count)
{
    uint32_t dst_flags = gf_ramin_read32(s, dst_obj);
    uint32_t src_flags = gf_ramin_read32(s, src_obj);
    uint8_t buffer[0x1000];
    uint32_t bytes_left = byte_count;

    while (bytes_left) {
        uint32_t dst_addr_abs;
        uint32_t src_addr_abs;
        uint32_t chunk_bytes;

        if (dst_flags & 0x00002000) {
            dst_addr_abs = gf_dma_lin_lookup(s, dst_obj, dst_addr);
        } else {
            dst_addr_abs = gf_dma_pt_lookup(s, dst_obj, dst_addr);
        }
        if (src_flags & 0x00002000) {
            src_addr_abs = gf_dma_lin_lookup(s, src_obj, src_addr);
        } else {
            src_addr_abs = gf_dma_pt_lookup(s, src_obj, src_addr);
        }
        chunk_bytes = MIN(bytes_left, MIN(0x1000 - (dst_addr_abs & 0xFFF),
                                          0x1000 - (src_addr_abs & 0xFFF)));
        if (src_flags & 0x00020000) {
            pci_dma_read(&s->parent_obj, src_addr_abs, buffer, chunk_bytes);
        } else {
            uint32_t off = src_addr_abs & s->memsize_mask;
            chunk_bytes = MIN(chunk_bytes, s->memsize - off);
            memcpy(buffer, s->vga.vram_ptr + off, chunk_bytes);
        }
        if (chunk_bytes == 0) {
            break;
        }
        if (dst_flags & 0x00020000) {
            pci_dma_write(&s->parent_obj, dst_addr_abs, buffer, chunk_bytes);
        } else {
            uint32_t off = dst_addr_abs & s->memsize_mask;
            chunk_bytes = MIN(chunk_bytes, s->memsize - off);
            if (chunk_bytes == 0) {
                break;
            }
            memcpy(s->vga.vram_ptr + off, buffer, chunk_bytes);
        }
        dst_addr += chunk_bytes;
        src_addr += chunk_bytes;
        bytes_left -= chunk_bytes;
    }
}

static uint32_t gf_ramfc_address(GeForceState *s, uint32_t chid,
                                 uint32_t offset)
{
    uint32_t ramfc;
    uint32_t ramfc_ch_size;

    if (s->card_type < 0x40) {
        ramfc = (s->fifo_ramfc & 0xFFF) << 8;
    } else {
        ramfc = (s->fifo_ramfc & 0xFFF) << 16;
    }
    if (s->card_type < 0x20) {
        ramfc_ch_size = 0x20;
    } else if (s->card_type < 0x40) {
        ramfc_ch_size = 0x40;
    } else {
        ramfc_ch_size = 0x80;
    }
    return ramfc + chid * ramfc_ch_size + offset;
}

void gf_ramfc_write32(GeForceState *s, uint32_t chid, uint32_t offset,
                      uint32_t val)
{
    gf_ramin_write32(s, gf_ramfc_address(s, chid, offset), val);
}

uint32_t gf_ramfc_read32(GeForceState *s, uint32_t chid, uint32_t offset)
{
    return gf_ramin_read32(s, gf_ramfc_address(s, chid, offset));
}

void gf_ramht_lookup(GeForceState *s, uint32_t handle, uint32_t chid,
                     uint32_t *object, uint8_t *engine)
{
    uint32_t ramht_addr = (s->fifo_ramht & 0xFFF) << 8;
    uint32_t ramht_bits = ((s->fifo_ramht >> 16) & 0xFF) + 9;
    uint32_t ramht_size = 1 << ramht_bits << 3;
    uint32_t hash = 0;
    uint32_t x = handle;
    uint32_t it;

    while (x) {
        hash ^= (x & ((1 << ramht_bits) - 1));
        x >>= ramht_bits;
    }
    hash ^= (chid & 0xF) << (ramht_bits - 4);
    hash = hash << 3;

    it = hash;
    do {
        if (gf_ramin_read32(s, ramht_addr + it) == handle) {
            uint32_t context = gf_ramin_read32(s, ramht_addr + it + 4);
            uint32_t ctx_chid;

            if (s->card_type < 0x40) {
                ctx_chid = (context >> 24) & 0x1F;
            } else {
                ctx_chid = (context >> 23) & 0x1F;
            }
            if (chid == ctx_chid) {
                if (object) {
                    if (s->card_type < 0x40) {
                        *object = (context & 0xFFFF) << 4;
                    } else {
                        *object = (context & 0xFFFFF) << 4;
                    }
                }
                if (engine) {
                    if (s->card_type < 0x40) {
                        *engine = (context >> 16) & 0xFF;
                    } else {
                        *engine = (context >> 20) & 0x7;
                    }
                }
                return;
            }
        }
        it += 8;
        if (it >= ramht_size) {
            it = 0;
        }
    } while (it != hash);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "geforce: ramht_lookup failed for 0x%08x\n", handle);
    if (object) {
        *object = 0;
    }
    if (engine) {
        *engine = 0xFF;
    }
}

/*
 * ------------------------------------------------------------------------
 * Timers and interrupts
 * ------------------------------------------------------------------------
 */

uint64_t gf_get_current_time(GeForceState *s)
{
    return (s->timer_inittime1 +
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->timer_inittime2) &
           ~UINT64_C(0x1F);
}

static uint32_t gf_get_mc_intr(GeForceState *s)
{
    uint32_t value = 0x00000000;

    if (s->bus_intr & s->bus_intr_en) {
        value |= 0x10000000;
    }
    if (s->fifo_intr & s->fifo_intr_en) {
        value |= 0x00000100;
    }
    if (s->graph_intr & s->graph_intr_en) {
        value |= 0x00001000;
    }
    if (s->crtc_intr & s->crtc_intr_en) {
        value |= 0x01000000;
    }
    return value;
}

void gf_update_irq_level(GeForceState *s)
{
    pci_set_irq(&s->parent_obj,
                (gf_get_mc_intr(s) && (s->mc_intr_en & 1)) ||
                (s->mc_soft_intr && (s->mc_intr_en & 2)));
}

#define GEFORCE_VBLANK_PERIOD_NS 16666667

static void gf_vblank_timer_cb(void *opaque)
{
    GeForceState *s = opaque;

    s->crtc_intr |= 0x00000001;
    gf_update_irq_level(s);
    if (s->fifo_wait_acquire) {
        s->fifo_wait_acquire = false;
        gf_update_fifo_wait(s);
        gf_fifo_process_all(s);
    }
    timer_mod(s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                               GEFORCE_VBLANK_PERIOD_NS);
}

/*
 * ------------------------------------------------------------------------
 * Display parameters and dirty area management
 * ------------------------------------------------------------------------
 */

static uint8_t gf_crtc28(GeForceState *s)
{
    /* Bit 7 selects slaved mode; a single display head is emulated. */
    return s->vga.cr[0x28] & 0x7F;
}

uint32_t gf_disp_pitch(GeForceState *s)
{
    if (gf_crtc28(s)) {
        return (s->vga.cr[0x13] |
                (s->vga.cr[0x19] >> 5 << 8) |
                ((s->vga.cr[0x42] >> 6 & 1) << 11)) << 3;
    }
    return s->vga.cr[VGA_CRTC_OFFSET] << 3;
}

uint32_t gf_disp_bpp(GeForceState *s)
{
    switch (gf_crtc28(s)) {
    case 0x01:
        return 8;
    case 0x02:
        return 16;
    case 0x03:
        return 32;
    default:
        return 8;
    }
}

void gf_redraw_area(GeForceState *s, uint32_t offset,
                    uint32_t width, uint32_t height)
{
    uint32_t pitch = gf_disp_pitch(s);
    uint32_t bypp = gf_disp_bpp(s) >> 3;
    uint64_t len;

    if (offset >= s->memsize || width == 0 || height == 0) {
        return;
    }
    if (pitch == 0) {
        pitch = width * bypp;
    }
    len = (uint64_t)(height - 1) * pitch + (uint64_t)width * bypp;
    len = MIN(len, (uint64_t)(s->memsize - offset));
    if (len) {
        memory_region_set_dirty(&s->vga.vram, offset, len);
    }
}

static bool gf_vbe_enabled(VGACommonState *vga)
{
    return vga->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_ENABLED;
}

static int gf_get_bpp(VGACommonState *vga)
{
    GeForceState *s = container_of(vga, GeForceState, vga);

    switch (gf_crtc28(s)) {
    case 0x01:
        return 8;
    case 0x02:
        return 16;
    case 0x03:
        return 32;
    case 0x00:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "geforce: unknown bpp setting 0x%02x\n",
                      gf_crtc28(s));
        break;
    }
    if (gf_vbe_enabled(vga)) {
        return vga->vbe_regs[VBE_DISPI_INDEX_BPP];
    }
    return 0;
}

static void gf_get_params(VGACommonState *vga, VGADisplayParams *params)
{
    GeForceState *s = container_of(vga, GeForceState, vga);

    if (gf_crtc28(s)) {
        uint32_t top_offset =
            (vga->cr[0x0d] |
             (vga->cr[0x0c] << 8) |
             (vga->cr[0x19] & 0x1F) << 16) << 2;
        top_offset += s->crtc_start;

        params->line_offset = gf_disp_pitch(s);
        params->start_addr = top_offset >> 2;
        params->line_compare = 65535;
        params->hpel = 8;
        params->hpel_split = false;
        return;
    }

    if (gf_vbe_enabled(vga)) {
        params->line_offset = vga->vbe_line_offset;
        params->start_addr = vga->vbe_start_addr;
        params->line_compare = 65535;
        params->hpel = 8;
        params->hpel_split = false;
        return;
    }

    params->line_offset = vga->cr[VGA_CRTC_OFFSET] << 3;
    params->start_addr = vga->cr[VGA_CRTC_START_LO] |
        (vga->cr[VGA_CRTC_START_HI] << 8);
    params->line_compare = vga->cr[VGA_CRTC_LINE_COMPARE] |
        ((vga->cr[VGA_CRTC_OVERFLOW] & 0x10) << 4) |
        ((vga->cr[VGA_CRTC_MAX_SCAN] & 0x40) << 3);
    params->hpel = vga->ar[VGA_ATC_PEL];
    params->hpel_split = vga->ar[VGA_ATC_MODE] & 0x20;
}

static void gf_get_resolution(VGACommonState *vga, int *pwidth, int *pheight)
{
    GeForceState *s = container_of(vga, GeForceState, vga);
    uint32_t width, height;

    if (gf_crtc28(s)) {
        width = (vga->cr[1] + ((vga->cr[0x2D] & 0x02) << 7) + 1) * 8;
        height = (vga->cr[18] |
                  ((vga->cr[7] & 0x02) << 7) |
                  ((vga->cr[7] & 0x40) << 3) |
                  ((vga->cr[0x25] & 0x02) << 9) |
                  ((vga->cr[0x41] & 0x04) << 9)) + 1;
        *pwidth = width;
        *pheight = height;
        return;
    }

    if (gf_vbe_enabled(vga)) {
        *pwidth = vga->vbe_regs[VBE_DISPI_INDEX_XRES];
        *pheight = vga->vbe_regs[VBE_DISPI_INDEX_YRES];
        return;
    }

    width = (vga->cr[VGA_CRTC_H_DISP] + 1) * 8;
    height = (vga->cr[VGA_CRTC_V_DISP_END] |
              ((vga->cr[VGA_CRTC_OVERFLOW] & 0x02) << 7) |
              ((vga->cr[VGA_CRTC_OVERFLOW] & 0x40) << 3)) + 1;
    *pwidth = width;
    *pheight = height;
}

static uint16_t gf_get_crtc_vtotal(GeForceState *s)
{
    return s->vga.cr[6] +
           ((s->vga.cr[7] & 0x01) << 8) +
           ((s->vga.cr[7] & 0x20) << 4) +
           ((s->vga.cr[0x25] & 1) << 10) +
           ((s->vga.cr[0x41] & 1) << 11) + 2;
}

/*
 * ------------------------------------------------------------------------
 * Hardware cursor (drawn as an overlay while scanning out)
 * ------------------------------------------------------------------------
 */

uint8_t gf_alpha_wrap(int value)
{
    return -(value >> 8) ^ value;
}

static uint16_t gf_cursor_read16(GeForceState *s, uint32_t addr)
{
    if (s->hw_cursor.vram) {
        return gf_vram_read16(s, addr);
    } else {
        return gf_ramin_read16(s, addr);
    }
}

static uint32_t gf_cursor_read32(GeForceState *s, uint32_t addr)
{
    if (s->hw_cursor.vram) {
        return gf_vram_read32(s, addr);
    } else {
        return gf_ramin_read32(s, addr);
    }
}

static void gf_cursor_invalidate(VGACommonState *vga)
{
    GeForceState *s = container_of(vga, GeForceState, vga);

    if (s->hw_cursor.prev_enabled != s->hw_cursor.enabled ||
        s->hw_cursor.prev_x != s->hw_cursor.x ||
        s->hw_cursor.prev_y != s->hw_cursor.y ||
        s->hw_cursor.prev_size != s->hw_cursor.size) {
        if (s->hw_cursor.prev_enabled && s->hw_cursor.prev_y >= 0) {
            vga_invalidate_scanlines(vga, s->hw_cursor.prev_y,
                                     s->hw_cursor.prev_y +
                                     s->hw_cursor.prev_size);
        }
        if (s->hw_cursor.enabled && s->hw_cursor.y >= 0) {
            vga_invalidate_scanlines(vga, s->hw_cursor.y,
                                     s->hw_cursor.y + s->hw_cursor.size);
        }
        s->hw_cursor.prev_enabled = s->hw_cursor.enabled;
        s->hw_cursor.prev_x = s->hw_cursor.x;
        s->hw_cursor.prev_y = s->hw_cursor.y;
        s->hw_cursor.prev_size = s->hw_cursor.size;
    } else if (s->hw_cursor.enabled && s->hw_cursor.y >= 0) {
        /* Redraw in case the cursor image data was rewritten */
        vga_invalidate_scanlines(vga, s->hw_cursor.y,
                                 s->hw_cursor.y + s->hw_cursor.size);
    }
}

static void gf_cursor_draw_line(VGACommonState *vga, uint8_t *d, int scr_y)
{
    GeForceState *s = container_of(vga, GeForceState, vga);
    int size = s->hw_cursor.size;
    int hwcx = s->hw_cursor.x;
    int hwcy = s->hw_cursor.y;
    int scr_w, scr_h;
    int x0, x1, x;
    uint32_t cursor_color_bytes = s->hw_cursor.bpp32 ? 4 : 2;
    uint32_t pitch = (uint32_t)s->hw_cursor.size * cursor_color_bytes;
    uint32_t line_ofs;
    uint32_t *dp = (uint32_t *)d;

    if (!s->hw_cursor.enabled || scr_y < hwcy || scr_y >= hwcy + size) {
        return;
    }
    gf_get_resolution(vga, &scr_w, &scr_h);
    x0 = MAX(hwcx, 0);
    x1 = MIN(hwcx + size, scr_w);
    line_ofs = s->hw_cursor.offset + pitch * (scr_y - hwcy);
    for (x = x0; x < x1; x++) {
        uint32_t cursor_ofs = line_ofs + (x - hwcx) * cursor_color_bytes;
        uint32_t dst = dp[x];
        uint8_t dr = dst >> 16;
        uint8_t dg = dst >> 8;
        uint8_t db = dst;

        if (s->hw_cursor.bpp32) {
            uint32_t cursor_color = gf_cursor_read32(s, cursor_ofs);
            if (cursor_color != 0) {
                uint8_t alpha, cr, cg, cb;
                uint8_t ica;
                EXTRACT_8888_TO_8888(cursor_color, alpha, cr, cg, cb);
                ica = 0xFF - alpha;
                db = gf_alpha_wrap(db * ica / 0xFF + cb);
                dg = gf_alpha_wrap(dg * ica / 0xFF + cg);
                dr = gf_alpha_wrap(dr * ica / 0xFF + cr);
                dp[x] = db | dg << 8 | dr << 16 | 0xFF000000;
            }
        } else {
            uint8_t alpha, cr, cg, cb;
            EXTRACT_1555_TO_8888(gf_cursor_read16(s, cursor_ofs),
                                 alpha, cr, cg, cb);
            if (alpha) {
                dp[x] = cb | cg << 8 | cr << 16 | 0xFF000000;
            } else {
                dp[x] = (db ^ cb) | (dg ^ cg) << 8 | (dr ^ cr) << 16 |
                        0xFF000000;
            }
        }
    }
}

/*
 * ------------------------------------------------------------------------
 * Banked legacy window at 0xA0000 (active while an SVGA mode is set)
 * ------------------------------------------------------------------------
 */

static uint64_t gf_bank_window_read(void *opaque, hwaddr addr, unsigned size)
{
    GeForceState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint32_t offset = ((uint32_t)addr + i) & 0xffff;
        offset += s->bank_base[0];
        offset &= s->memsize_mask;
        val |= (uint64_t)s->vga.vram_ptr[offset] << (i * 8);
    }
    return val;
}

static void gf_bank_window_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    GeForceState *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint32_t offset = ((uint32_t)addr + i) & 0xffff;
        uint8_t value = val >> (i * 8);

        if (s->vga.cr[0x1c] & 0x80) {
            gf_vram_write8(s, offset ^ s->ramin_flip, value);
            continue;
        }
        offset += s->bank_base[0];
        offset &= s->memsize_mask;
        s->vga.vram_ptr[offset] = value;
        memory_region_set_dirty(&s->vga.vram, offset, 1);
    }
}

static const MemoryRegionOps gf_bank_window_ops = {
    .read = gf_bank_window_read,
    .write = gf_bank_window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void gf_bank_window_update(GeForceState *s)
{
    memory_region_set_enabled(&s->bank_window, gf_crtc28(s) != 0);
}

/*
 * ------------------------------------------------------------------------
 * Extended CRTC registers and DDC
 * ------------------------------------------------------------------------
 */

static void gf_ddc_write(GeForceState *s, bool scl, bool sda, uint8_t *readback)
{
    bool sda_in;

    bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SCL, scl);
    sda_in = bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SDA, sda);
    *readback = (sda_in ? 0x08 : 0x00) | (scl ? 0x04 : 0x00);
}

static void gf_update_hw_cursor_addr(GeForceState *s)
{
    s->hw_cursor.enabled =
        (s->vga.cr[0x31] & 0x01) || (s->crtc_cursor_config & 0x00000001);
    s->hw_cursor.vram =
        (s->vga.cr[0x30] & 0x80) || (s->crtc_cursor_config & 0x00000100) ||
        (s->card_type >= 0x40);
    s->hw_cursor.offset =
        (s->vga.cr[0x31] >> 2 << 11) |
        ((s->vga.cr[0x30] & 0x7F) << 17) |
        (s->vga.cr[0x2f] << 24);
    s->hw_cursor.offset += s->crtc_cursor_offset;
}

static uint8_t gf_svga_read_crtc(GeForceState *s, unsigned index)
{
    if (index <= GEFORCE_CRTC_MAX) {
        return s->vga.cr[index];
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "geforce: crtc: unknown index 0x%02x read\n", index);
    return 0xff;
}

static void gf_svga_write_crtc(GeForceState *s, unsigned index, uint8_t value)
{
    bool update_cursor_addr = false;

    if (index == 0x1c) {
        if (!(s->vga.cr[index] & 0x80) && (value & 0x80) != 0) {
            /* Without clearing this register, Windows 95 hangs after reboot */
            s->crtc_intr_en = 0x00000000;
            gf_update_irq_level(s);
        }
    } else if (index == 0x1d || index == 0x1e) {
        s->bank_base[index - 0x1d] = value * 0x8000;
    } else if (index == 0x2f || index == 0x30 || index == 0x31) {
        update_cursor_addr = true;
    } else if (index == 0x37 || index == 0x3f || index == 0x51) {
        bool scl = value & 0x20;
        bool sda = value & 0x10;
        if (index == 0x3f) {
            gf_ddc_write(s, scl, sda, &s->vga.cr[0x3e]);
        } else {
            s->vga.cr[index - 1] = sda << 3 | scl << 2;
        }
    } else if (index == 0x58) {
        /*
         * Combined with 0x57, this register makes a pair which allows
         * access to 16 bytes of head-specific variables.  Disable writes
         * so that reads return zeroes.
         */
        return;
    }

    if (index <= GEFORCE_CRTC_MAX) {
        s->vga.cr[index] = value;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "geforce: crtc: unknown index 0x%02x write\n", index);
        return;
    }

    if (index == 0x28) {
        gf_bank_window_update(s);
    }
    if (update_cursor_addr) {
        gf_update_hw_cursor_addr(s);
    }
}

/*
 * ------------------------------------------------------------------------
 * VGA/SVGA I/O ports
 * ------------------------------------------------------------------------
 */

static uint32_t gf_svga_read(GeForceState *s, uint32_t address)
{
    switch (address) {
    case 0x03b4:
    case 0x03d4:
        return s->vga.cr_index;
    case 0x03b5:
    case 0x03d5:
        if (s->vga.cr_index > GEFORCE_VGA_CRTC_MAX) {
            return gf_svga_read_crtc(s, s->vga.cr_index);
        }
        break;
    case 0x03c2:
        /* Input Status 0: monitor presence detection (DAC sensing) */
        return 0x10;
    default:
        break;
    }
    return vga_ioport_read(&s->vga, address);
}

static void gf_svga_write(GeForceState *s, uint32_t address, uint32_t value)
{
    switch (address) {
    case 0x03b4:
    case 0x03d4:
        s->vga.cr_index = value;
        return;
    case 0x03b5:
    case 0x03d5:
        if (s->vga.cr_index > GEFORCE_VGA_CRTC_MAX) {
            gf_svga_write_crtc(s, s->vga.cr_index, value);
            return;
        }
        break;
    default:
        break;
    }
    vga_ioport_write(&s->vga, address, value);
}

static uint32_t gf_portio_read(void *opaque, uint32_t addr)
{
    GeForceState *s = opaque;

    return gf_svga_read(s, addr);
}

static void gf_portio_write(void *opaque, uint32_t addr, uint32_t val)
{
    GeForceState *s = opaque;

    gf_svga_write(s, addr, val);
}

/* RMA_ACCESS via ports 0x3d0/0x3d2 */
static uint32_t gf_rma_read(GeForceState *s, uint32_t address, unsigned io_len)
{
    uint8_t crtc38 = s->vga.cr[0x38];
    bool rma_enable = crtc38 & 1;
    int rma_index = crtc38 >> 1;

    if (io_len == 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "geforce: port 0x3d0 access with io_len = 1\n");
        return 0;
    }
    if (!rma_enable) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "geforce: port 0x3d0 access is disabled\n");
        return 0;
    }
    if (rma_index == 1) {
        if (address == 0x03d0) {
            return s->rma_addr;
        } else {
            return s->rma_addr >> 16;
        }
    } else if (rma_index == 2) {
        bool vram = false;
        uint32_t offset = s->rma_addr;

        if (s->rma_addr & 0x80000000) {
            vram = true;
            offset &= ~0x80000000;
        }
        if ((!vram && offset < GEFORCE_PNPMMIO_SIZE) ||
            (vram && offset < s->memsize)) {
            uint32_t value = vram ? gf_vram_read32(s, offset)
                                  : gf_register_read32(s, offset);
            if (address == 0x03d0) {
                return value;
            } else {
                return value >> 16;
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: rma: oob read from 0x%08x ignored\n",
                          s->rma_addr);
            return 0xFFFFFFFF;
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "geforce: rma: read index %d\n",
                      rma_index);
    }
    return 0;
}

static void gf_rma_write(GeForceState *s, uint32_t address, uint32_t value,
                         unsigned io_len)
{
    uint8_t crtc38 = s->vga.cr[0x38];
    bool rma_enable = crtc38 & 1;
    int rma_index = crtc38 >> 1;

    if (io_len == 1) {
        return;
    }
    if (!rma_enable) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "geforce: port 0x3d0 access is disabled\n");
        return;
    }
    if (rma_index == 1) {
        if (address == 0x03d0) {
            if (io_len == 2) {
                s->rma_addr &= 0xFFFF0000;
                s->rma_addr |= value;
            } else {
                s->rma_addr = value;
            }
        } else {
            s->rma_addr &= 0x0000FFFF;
            s->rma_addr |= value << 16;
        }
    } else if (rma_index == 2) {
        qemu_log_mask(LOG_GUEST_ERROR, "geforce: rma: write index 2\n");
    } else if (rma_index == 3) {
        bool vram = false;
        uint32_t offset = s->rma_addr & ~3;

        if (s->rma_addr & 0x80000000) {
            vram = true;
            offset &= ~0x80000000;
        }
        if ((!vram && offset < GEFORCE_PNPMMIO_SIZE) ||
            (vram && offset < s->memsize)) {
            if (address == 0x03d0) {
                if (io_len == 2) {
                    uint32_t value32 = vram ? gf_vram_read32(s, offset)
                                            : gf_register_read32(s, offset);
                    value32 &= 0xFFFF0000;
                    value32 |= value;
                    if (vram) {
                        gf_vram_write32(s, offset, value32);
                    } else {
                        gf_register_write32(s, offset, value32);
                    }
                } else {
                    if (vram) {
                        gf_vram_write32(s, offset, value);
                    } else {
                        gf_register_write32(s, offset, value);
                    }
                }
            } else {
                uint32_t value32 = vram ? gf_vram_read32(s, offset)
                                        : gf_register_read32(s, offset);
                value32 &= 0x0000FFFF;
                value32 |= value << 16;
                if (vram) {
                    gf_vram_write32(s, offset, value32);
                } else {
                    gf_register_write32(s, offset, value32);
                }
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: rma: oob write to 0x%08x ignored\n",
                          s->rma_addr);
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "geforce: rma: write unknown index %d\n",
                      rma_index);
    }
}

static uint32_t gf_rma_portio_read1(void *opaque, uint32_t addr)
{
    GeForceState *s = opaque;
    return gf_rma_read(s, addr & ~1, 1);
}

static void gf_rma_portio_write1(void *opaque, uint32_t addr, uint32_t val)
{
    GeForceState *s = opaque;
    gf_rma_write(s, addr & ~1, val, 1);
}

static uint32_t gf_rma_portio_read2(void *opaque, uint32_t addr)
{
    GeForceState *s = opaque;
    return gf_rma_read(s, addr, 2) & 0xffff;
}

static void gf_rma_portio_write2(void *opaque, uint32_t addr, uint32_t val)
{
    GeForceState *s = opaque;
    gf_rma_write(s, addr, val, 2);
}

static uint32_t gf_rma_portio_read4(void *opaque, uint32_t addr)
{
    GeForceState *s = opaque;
    return gf_rma_read(s, addr, 4);
}

static void gf_rma_portio_write4(void *opaque, uint32_t addr, uint32_t val)
{
    GeForceState *s = opaque;
    gf_rma_write(s, addr, val, 4);
}

static const MemoryRegionPortio gf_vga_portio_list[] = {
    { 0x04, 2, 1, .read = gf_portio_read, .write = gf_portio_write },
    { 0x0a, 1, 1, .read = gf_portio_read, .write = gf_portio_write },
    { 0x10, 16, 1, .read = gf_portio_read, .write = gf_portio_write },
    { 0x20, 4, 1, .read = gf_rma_portio_read1, .write = gf_rma_portio_write1 },
    { 0x20, 4, 2, .read = gf_rma_portio_read2, .write = gf_rma_portio_write2 },
    { 0x20, 4, 4, .read = gf_rma_portio_read4, .write = gf_rma_portio_write4 },
    { 0x24, 2, 1, .read = gf_portio_read, .write = gf_portio_write },
    { 0x2a, 1, 1, .read = gf_portio_read, .write = gf_portio_write },
    PORTIO_END_OF_LIST(),
};

/*
 * ------------------------------------------------------------------------
 * MMIO register aperture (BAR0)
 * ------------------------------------------------------------------------
 */

static const uint8_t *gf_rom_ptr(GeForceState *s, uint32_t *size)
{
    PCIDevice *pdev = &s->parent_obj;

    if (!pdev->has_rom || !memory_region_size(&pdev->rom)) {
        return NULL;
    }
    *size = memory_region_size(&pdev->rom);
    return memory_region_get_ram_ptr(&pdev->rom);
}

uint8_t gf_register_read8(GeForceState *s, uint32_t address)
{
    uint8_t value;

    if (address >= 0x1800 && address < 0x1900) {
        value = pci_default_read_config(&s->parent_obj, address - 0x1800, 1);
    } else if (address >= 0x300000 && address < 0x310000) {
        uint32_t romsize = 0;
        const uint8_t *rom = gf_rom_ptr(s, &romsize);
        if (s->parent_obj.config[0x50] == 0x00 && rom != NULL &&
            address - 0x300000 < romsize) {
            value = rom[address - 0x300000];
        } else {
            value = 0x00;
        }
    } else if ((address >= 0xc0300 && address < 0xc0400) ||
               (address >= 0xc2300 && address < 0xc2400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset == 0x3c3 ||
            offset == 0x3c4 || offset == 0x3c5 ||
            offset == 0x3cc || offset == 0x3cf) {
            if (!head) {
                value = gf_svga_read(s, offset);
            } else {
                value = 0x00;
            }
        } else {
            value = 0xFF;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: unknown register 0x%08x read\n", address);
        }
    } else if ((address >= 0x601300 && address < 0x601400) ||
               (address >= 0x603300 && address < 0x603400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset == 0x3b4 || offset == 0x3b5 ||
            offset == 0x3c0 || offset == 0x3c1 ||
            offset == 0x3c2 || offset == 0x3d4 ||
            offset == 0x3d5 || offset == 0x3d8 ||
            offset == 0x3da) {
            if (!head) {
                value = gf_svga_read(s, offset);
            } else {
                value = 0x00;
            }
        } else {
            value = 0xFF;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: unknown register 0x%08x read\n", address);
        }
    } else if ((address >= 0x681300 && address < 0x681400) ||
               (address >= 0x683300 && address < 0x683400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset >= 0x3c6 && offset <= 0x3c9) {
            if (!head) {
                value = gf_svga_read(s, offset);
            } else {
                value = 0x00;
            }
        } else {
            value = 0xFF;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: unknown register 0x%08x read\n", address);
        }
    } else if (address >= 0x700000 && address < 0x800000) {
        value = gf_vram_read8(s, (address - 0x700000) ^ s->ramin_flip);
    } else {
        value = gf_register_read32(s, address);
    }
    return value;
}

void gf_register_write8(GeForceState *s, uint32_t address, uint8_t value)
{
    if ((address >= 0xc0300 && address < 0xc0400) ||
        (address >= 0xc2300 && address < 0xc2400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset == 0x3c2 || offset == 0x3c3 ||
            offset == 0x3c4 || offset == 0x3c5 ||
            offset == 0x3ce || offset == 0x3cf) {
            if (!head) {
                gf_svga_write(s, offset, value);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: unknown register 0x%08x write\n", address);
        }
    } else if ((address >= 0x601300 && address < 0x601400) ||
               (address >= 0x603300 && address < 0x603400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset == 0x3b4 || offset == 0x3b5 ||
            offset == 0x3c0 || offset == 0x3c1 ||
            offset == 0x3c2 || offset == 0x3d4 ||
            offset == 0x3d5 || offset == 0x3da) {
            if (!head) {
                gf_svga_write(s, offset, value);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: unknown register 0x%08x write\n", address);
        }
    } else if ((address >= 0x681300 && address < 0x681400) ||
               (address >= 0x683300 && address < 0x683400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset >= 0x3c6 && offset <= 0x3c9) {
            if (!head) {
                gf_svga_write(s, offset, value);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: unknown register 0x%08x write\n", address);
        }
    } else if (address >= 0x700000 && address < 0x800000) {
        gf_vram_write8(s, (address - 0x700000) ^ s->ramin_flip, value);
    } else {
        gf_register_write32(s, address,
                            (gf_register_read32(s, address) & ~0xFF) | value);
    }
}

uint32_t gf_register_read32(GeForceState *s, uint32_t address)
{
    uint32_t value;

    if (address == 0x0) {
        if (s->card_type == 0x20) {
            value = 0x020200A5;
        } else {
            value = s->card_type << 20;
        }
    } else if (address == 0x100) {
        value = gf_get_mc_intr(s);
        if (s->mc_soft_intr) {
            value |= 0x80000000;
        }
    } else if (address == 0x140) {
        value = s->mc_intr_en;
    } else if (address == 0x200) {
        value = s->mc_enable;
    } else if (address == 0x1100) {
        value = s->bus_intr;
    } else if (address == 0x1140) {
        value = s->bus_intr_en;
    } else if (address >= 0x1800 && address < 0x1900) {
        value = pci_default_read_config(&s->parent_obj, address - 0x1800, 4);
    } else if (address == 0x2100) {
        value = s->fifo_intr;
    } else if (address == 0x2140) {
        value = s->fifo_intr_en;
    } else if (address == 0x2210) {
        value = s->fifo_ramht;
    } else if (address == 0x2214 && s->card_type < 0x40) {
        value = s->fifo_ramfc;
    } else if (address == 0x2218) {
        value = s->fifo_ramro;
    } else if (address == 0x2220 && s->card_type >= 0x40) {
        value = s->fifo_ramfc;
    } else if (address == 0x2400) { /* PFIFO_RUNOUT_STATUS */
        value = 0x00000010;
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            value = 0x00000000;
        }
    } else if (address == 0x2504) {
        value = s->fifo_mode;
    } else if (address == 0x3200) {
        value = s->fifo_cache1_push0;
    } else if (address == 0x3204) {
        value = s->fifo_cache1_push1;
    } else if (address == 0x3210) {
        value = s->fifo_cache1_put;
    } else if (address == 0x3214) { /* PFIFO_CACHE1_STATUS */
        value = 0x00000010;
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            value = 0x00000000;
        }
    } else if (address == 0x3220) {
        value = s->fifo_cache1_dma_push;
    } else if (address == 0x322c) {
        value = s->fifo_cache1_dma_instance;
    } else if (address == 0x3230) { /* PFIFO_CACHE1_DMA_CTL */
        value = 0x80000000;
    } else if (address == 0x3240) {
        value = s->fifo_cache1_dma_put;
    } else if (address == 0x3244) {
        value = s->fifo_cache1_dma_get;
    } else if (address == 0x3248) {
        value = s->fifo_cache1_ref_cnt;
    } else if (address == 0x3250) {
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            s->fifo_cache1_pull0 |= 0x00000100;
        }
        value = s->fifo_cache1_pull0;
    } else if (address == 0x3270) {
        value = s->fifo_cache1_get;
    } else if (address == 0x32e0) {
        value = s->fifo_grctx_instance;
    } else if (address == 0x3304) {
        value = 0x00000001;
    } else if ((address >= 0x3800 && address < 0x4000 &&
                s->card_type < 0x40) ||
               (address >= 0x90000 && address < 0x92000 &&
                s->card_type >= 0x40)) {
        uint32_t offset;
        uint32_t index;
        if (s->card_type < 0x40) {
            offset = address - 0x3800;
        } else {
            offset = address - 0x90000;
        }
        index = offset / 8;
        if (offset % 8 == 0) {
            value = s->fifo_cache1_method[index & (GEFORCE_CACHE1_SIZE - 1)];
        } else {
            value = s->fifo_cache1_data[index & (GEFORCE_CACHE1_SIZE - 1)];
        }
    } else if (address == 0x9100) {
        value = s->timer_intr;
    } else if (address == 0x9140) {
        value = s->timer_intr_en;
    } else if (address == 0x9200) {
        value = s->timer_num;
    } else if (address == 0x9210) {
        value = s->timer_den;
    } else if (address == 0x9400) {
        value = (uint32_t)gf_get_current_time(s);
    } else if (address == 0x9410) {
        value = gf_get_current_time(s) >> 32;
    } else if (address == 0x9420) {
        value = s->timer_alarm;
    } else if ((address >= 0xc0300 && address < 0xc0400) ||
               (address >= 0xc2300 && address < 0xc2400)) {
        value = gf_register_read8(s, address);
    } else if (address == 0x10020c) {
        value = s->memsize;
    } else if (address == 0x100320) { /* PFB_ZCOMP_SIZE */
        if (s->card_type == 0x20) {
            value = 0x00007fff;
        } else if (s->card_type == 0x35) {
            value = 0x0005c7ff;
        } else {
            value = 0x0002e3ff;
        }
    } else if (address == 0x101000) {
        value = s->straps0_primary;
    } else if (address >= 0x300000 && address < 0x310000) {
        uint32_t romsize = 0;
        const uint8_t *rom = gf_rom_ptr(s, &romsize);
        uint32_t offset = address - 0x300000;
        if (s->parent_obj.config[0x50] == 0x00 && rom != NULL &&
            offset + 4 <= romsize) {
            value = ldl_le_p(rom + offset);
        } else {
            value = 0x00000000;
        }
    } else if (address == 0x400100) {
        value = s->graph_intr;
    } else if (address == 0x400108) {
        value = s->graph_nsource;
    } else if ((address == 0x40013C && s->card_type >= 0x40) ||
               (address == 0x400140 && s->card_type < 0x40)) {
        value = s->graph_intr_en;
    } else if (address == 0x40014C) {
        value = s->graph_ctx_switch1;
    } else if (address == 0x400150) {
        value = s->graph_ctx_switch2;
    } else if (address == 0x400158) {
        value = s->graph_ctx_switch4;
    } else if (address == 0x40032c) {
        value = s->graph_ctxctl_cur;
    } else if (address == 0x400700) {
        value = s->graph_status;
    } else if (address == 0x400704) {
        value = s->graph_trapped_addr;
    } else if (address == 0x400708) {
        value = s->graph_trapped_data;
    } else if (address == 0x400718) {
        value = s->graph_notify;
    } else if (address == 0x400720) {
        value = s->graph_fifo;
    } else if (address == 0x400724) {
        value = s->graph_bpixel;
    } else if (address == 0x400780) {
        value = s->graph_channel_ctx_table;
    } else if ((address == 0x400640 && s->card_type == 0x15) ||
               (address == 0x400820 && s->card_type == 0x20)) {
        value = s->graph_offset0;
    } else if ((address == 0x400670 && s->card_type == 0x15) ||
               (address == 0x400850 && s->card_type == 0x20)) {
        value = s->graph_pitch0;
    } else if (address == 0x600100) {
        value = s->crtc_intr;
    } else if (address == 0x600140) {
        value = s->crtc_intr_en;
    } else if (address == 0x600800) {
        value = s->crtc_start;
    } else if (address == 0x600804) {
        value = s->crtc_config;
    } else if (address == 0x600808) {
        s->crtc_raster_pos ^= 1; /* fake */
        value = (vga_ioport_read(&s->vga, 0x03da) << 13) |
                s->crtc_raster_pos;
    } else if (address == 0x60080c) {
        value = s->crtc_cursor_offset;
    } else if (address == 0x600810) {
        value = s->crtc_cursor_config;
    } else if (address == 0x60081c) {
        value = s->crtc_gpio_ext;
        if (s->card_type == 0x35) {
            value |= 0x04000000;
        }
    } else if (address == 0x600868) {
        uint64_t frame_ns = GEFORCE_VBLANK_PERIOD_NS;
        uint64_t display_ns =
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) % frame_ns;
        value = (uint32_t)(gf_get_crtc_vtotal(s) * display_ns / frame_ns);
    } else if ((address >= 0x601300 && address < 0x601400) ||
               (address >= 0x603300 && address < 0x603400)) {
        value = gf_register_read8(s, address);
    } else if (address == 0x680300) {
        value = s->ramdac_cu_start_pos;
    } else if (address == 0x680404) { /* RAMDAC_NV10_CURSYNC */
        value = 0x00000000;
    } else if (address == 0x680508) {
        value = s->ramdac_vpll;
    } else if (address == 0x68050c) {
        value = s->ramdac_pll_select;
    } else if (address == 0x680578) {
        value = s->ramdac_vpll_b;
    } else if (address == 0x680600) {
        value = s->ramdac_general_control;
    } else if (address == 0x680828) { /* PRAMDAC_FP_HCRTC */
        value = 0x00000000; /* Second monitor is disconnected */
    } else if ((address >= 0x681300 && address < 0x681400) ||
               (address >= 0x683300 && address < 0x683400)) {
        value = gf_register_read8(s, address);
    } else if (address >= 0x700000 && address < 0x800000) {
        uint32_t offset = address & 0x000fffff;
        if (offset & 3) {
            value = gf_ramin_read8(s, offset + 0) << 0 |
                    gf_ramin_read8(s, offset + 1) << 8 |
                    gf_ramin_read8(s, offset + 2) << 16 |
                    gf_ramin_read8(s, offset + 3) << 24;
        } else {
            value = gf_ramin_read32(s, offset);
        }
    } else if ((address >= 0x800000 && address < 0xA00000) ||
               (address >= 0xC00000 && address < 0xE00000)) {
        uint32_t chid;
        uint32_t offset;
        uint32_t curchid;
        if (address >= 0x800000 && address < 0xA00000) {
            chid = (address >> 16) & 0x1F;
            offset = address & 0x1FFF;
        } else {
            chid = (address >> 12) & 0x1FF;
            if (chid >= GEFORCE_CHANNEL_COUNT) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "geforce: channel id >= 32\n");
                chid = 0;
            }
            offset = address & 0x1FF;
        }
        value = 0x00000000;
        curchid = s->fifo_cache1_push1 & 0x1F;
        if (offset == 0x54 && address >= 0xC00000 && address < 0xE00000) {
            if (s->chs[chid].subr_active) {
                value = s->chs[chid].subr_return;
            } else if (curchid == chid) {
                value = s->fifo_cache1_dma_get;
            } else {
                value = gf_ramfc_read32(s, chid, 0x4);
            }
        } else if (offset == 0x10) {
            value = 0xffff;
        } else if (offset >= 0x40 && offset <= 0x48) {
            if (curchid == chid) {
                if (offset == 0x40) {
                    value = s->fifo_cache1_dma_put;
                } else if (offset == 0x44) {
                    value = s->fifo_cache1_dma_get;
                } else if (offset == 0x48) {
                    value = s->fifo_cache1_ref_cnt;
                }
            } else {
                if (offset == 0x40) {
                    value = gf_ramfc_read32(s, chid, 0x0);
                } else if (offset == 0x44) {
                    value = gf_ramfc_read32(s, chid, 0x4);
                } else if (offset == 0x48) {
                    value = gf_ramfc_read32(s, chid, 0x8);
                }
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "geforce: unknown FIFO offset 0x%08x\n", offset);
        }
    } else {
        value = s->unk_regs[(address & (GEFORCE_PNPMMIO_SIZE - 1)) / 4];
    }
    return value;
}

void gf_register_write32(GeForceState *s, uint32_t address, uint32_t value)
{
    if (address == 0x100) {
        s->mc_soft_intr = (bool)(value >> 31);
        gf_update_irq_level(s);
    } else if (address == 0x140) {
        s->mc_intr_en = value;
        gf_update_irq_level(s);
    } else if (address == 0x200) {
        s->mc_enable = value;
    } else if (address >= 0x1800 && address < 0x1900) {
        pci_default_write_config(&s->parent_obj, address - 0x1800, value, 4);
    } else if (address == 0x1100) {
        s->bus_intr &= ~value;
        gf_update_irq_level(s);
    } else if (address == 0x1140) {
        s->bus_intr_en = value;
        gf_update_irq_level(s);
    } else if (address == 0x2100) {
        s->fifo_intr &= ~value;
        gf_update_irq_level(s);
    } else if (address == 0x2140) {
        s->fifo_intr_en = value;
        gf_update_irq_level(s);
    } else if (address == 0x2210) {
        s->fifo_ramht = value;
    } else if (address == 0x2214 && s->card_type < 0x40) {
        s->fifo_ramfc = value;
    } else if (address == 0x2218) {
        s->fifo_ramro = value;
    } else if (address == 0x2220 && s->card_type >= 0x40) {
        s->fifo_ramfc = value;
    } else if (address == 0x2504) {
        bool process = (s->fifo_mode | value) != s->fifo_mode;
        s->fifo_mode = value;
        if (process) {
            gf_fifo_process_all(s);
        }
    } else if (address == 0x3200) {
        s->fifo_cache1_push0 = value;
        if ((s->fifo_cache1_push0 & 1) != 0) {
            gf_fifo_process_all(s);
        }
    } else if (address == 0x3204) {
        s->fifo_cache1_push1 = value;
    } else if (address == 0x3210) {
        s->fifo_cache1_put = value;
    } else if (address == 0x3220) {
        s->fifo_cache1_dma_push = value;
    } else if (address == 0x322c) {
        s->fifo_cache1_dma_instance = value;
    } else if (address == 0x3240) {
        s->fifo_cache1_dma_put = value;
    } else if (address == 0x3244) {
        s->fifo_cache1_dma_get = value;
    } else if (address == 0x3248) {
        s->fifo_cache1_ref_cnt = value;
    } else if (address == 0x3250) {
        s->fifo_cache1_pull0 = value;
        if ((s->fifo_cache1_pull0 & 1) != 0) {
            gf_fifo_process_all(s);
        }
    } else if (address == 0x3270) {
        s->fifo_cache1_get = value & (GEFORCE_CACHE1_SIZE * 4 - 1);
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            s->fifo_intr |= 0x00000001;
        } else {
            s->fifo_intr &= ~0x00000001;
            s->fifo_cache1_pull0 &= ~0x00000100;
            if (s->fifo_wait_soft) {
                s->fifo_wait_soft = false;
                gf_update_fifo_wait(s);
                gf_fifo_process_all(s);
            }
        }
        gf_update_irq_level(s);
    } else if (address == 0x32e0) {
        s->fifo_grctx_instance = value;
    } else if (address == 0x9100) {
        s->timer_intr &= ~value;
    } else if (address == 0x9140) {
        s->timer_intr_en = value;
    } else if (address == 0x9200) {
        s->timer_num = value;
    } else if (address == 0x9210) {
        s->timer_den = value;
    } else if (address == 0x9400 || address == 0x9410) {
        s->timer_inittime2 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (address == 0x9400) {
            s->timer_inittime1 =
                (s->timer_inittime1 & UINT64_C(0xFFFFFFFF00000000)) | value;
        } else {
            s->timer_inittime1 =
                (s->timer_inittime1 & UINT64_C(0x00000000FFFFFFFF)) |
                ((uint64_t)value << 32);
        }
    } else if (address == 0x9420) {
        s->timer_alarm = value;
    } else if ((address >= 0xc0300 && address < 0xc0400) ||
               (address >= 0xc2300 && address < 0xc2400)) {
        gf_register_write8(s, address, value);
    } else if (address == 0x101000) {
        if (value >> 31) {
            s->straps0_primary = value;
        } else {
            s->straps0_primary = s->straps0_primary_original;
        }
    } else if (address == 0x400100) {
        s->graph_intr &= ~value;
        gf_update_irq_level(s);
        if (s->fifo_wait_notify && s->graph_intr == 0) {
            s->fifo_wait_notify = false;
            gf_update_fifo_wait(s);
            gf_fifo_process_all(s);
        }
    } else if (address == 0x400108) {
        s->graph_nsource = value;
    } else if ((address == 0x40013C && s->card_type >= 0x40) ||
               (address == 0x400140 && s->card_type < 0x40)) {
        s->graph_intr_en = value;
        gf_update_irq_level(s);
    } else if (address == 0x40014C) {
        s->graph_ctx_switch1 = value;
    } else if (address == 0x400150) {
        s->graph_ctx_switch2 = value;
    } else if (address == 0x400158) {
        s->graph_ctx_switch4 = value;
    } else if (address == 0x40032c) {
        s->graph_ctxctl_cur = value;
    } else if (address == 0x400700) {
        s->graph_status = value;
    } else if (address == 0x400704) {
        s->graph_trapped_addr = value;
    } else if (address == 0x400708) {
        s->graph_trapped_data = value;
    } else if (address == 0x400718) {
        s->graph_notify = value;
    } else if (address == 0x40071c) {
        if ((value & 0x00000002) != 0 && s->graph_flip_modulo != 0) {
            s->graph_flip_read++;
            s->graph_flip_read %= s->graph_flip_modulo;
            if (s->fifo_wait_flip &&
                s->graph_flip_read != s->graph_flip_write) {
                s->fifo_wait_flip = false;
                gf_update_fifo_wait(s);
                gf_fifo_process_all(s);
            }
        }
    } else if (address == 0x400720) {
        s->graph_fifo = value;
    } else if (address == 0x400724) {
        s->graph_bpixel = value;
    } else if (address == 0x400780) {
        s->graph_channel_ctx_table = value;
    } else if ((address == 0x400640 && s->card_type == 0x15) ||
               (address == 0x400820 && s->card_type == 0x20)) {
        s->graph_offset0 = value;
    } else if ((address == 0x400670 && s->card_type == 0x15) ||
               (address == 0x400850 && s->card_type == 0x20)) {
        s->graph_pitch0 = value;
    } else if (address == 0x600100) {
        s->crtc_intr &= ~value;
        gf_update_irq_level(s);
    } else if (address == 0x600140) {
        s->crtc_intr_en = value;
        gf_update_irq_level(s);
    } else if (address == 0x600800) {
        s->crtc_start = value;
    } else if (address == 0x600804) {
        s->crtc_config = value;
    } else if (address == 0x60080c) {
        s->crtc_cursor_offset = value;
        s->hw_cursor.offset = s->crtc_cursor_offset;
    } else if (address == 0x600810) {
        s->crtc_cursor_config = value;
        s->hw_cursor.enabled =
            (s->vga.cr[0x31] & 0x01) || (value & 0x00000001);
        s->hw_cursor.vram =
            (s->vga.cr[0x30] & 0x80) || (value & 0x00000100) ||
            (s->card_type >= 0x40);
        s->hw_cursor.size = value & 0x00010000 ? 64 : 32;
        s->hw_cursor.bpp32 = value & 0x00001000;
    } else if (address == 0x60081c) {
        s->crtc_gpio_ext = value;
    } else if ((address >= 0x601300 && address < 0x601400) ||
               (address >= 0x603300 && address < 0x603400)) {
        gf_register_write8(s, address, value);
    } else if (address == 0x680300) {
        s->ramdac_cu_start_pos = value;
        s->hw_cursor.x = (int32_t)s->ramdac_cu_start_pos << 20 >> 20;
        s->hw_cursor.y = (int32_t)s->ramdac_cu_start_pos << 4 >> 20;
    } else if (address == 0x680508) {
        s->ramdac_vpll = value;
    } else if (address == 0x68050c) {
        s->ramdac_pll_select = value;
    } else if (address == 0x680578) {
        s->ramdac_vpll_b = value;
    } else if (address == 0x680600) {
        s->ramdac_general_control = value;
        s->vga.dac_8bit = (value >> 20) & 1;
    } else if ((address >= 0x681300 && address < 0x681400) ||
               (address >= 0x683300 && address < 0x683400)) {
        gf_register_write8(s, address, value);
    } else if (address >= 0x700000 && address < 0x800000) {
        gf_ramin_write32(s, address - 0x700000, value);
    } else if ((address >= 0x800000 && address < 0xA00000) ||
               (address >= 0xC00000 && address < 0xE00000)) {
        uint32_t chid;
        uint32_t offset;
        if (address >= 0x800000 && address < 0xA00000) {
            chid = (address >> 16) & 0x1F;
            offset = address & 0x1FFF;
        } else {
            chid = (address >> 12) & 0x1FF;
            if (chid >= GEFORCE_CHANNEL_COUNT) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "geforce: channel id >= 32\n");
                chid = 0;
            }
            offset = address & 0x1FF;
        }
        if ((s->fifo_mode & (1 << chid)) != 0) {
            if (offset == 0x40) {
                uint32_t curchid = s->fifo_cache1_push1 & 0x1F;
                if (curchid == chid) {
                    s->fifo_cache1_dma_put = value;
                } else {
                    gf_ramfc_write32(s, chid, 0x0, value);
                }
                gf_fifo_process(s, chid);
            }
        } else if (address >= 0x800000 && address < 0xA00000) {
            uint32_t subc = (address >> 13) & 7;
            gf_execute_command(s, chid, subc, offset / 4, value);
        }
    } else {
        s->unk_regs[(address & (GEFORCE_PNPMMIO_SIZE - 1)) / 4] = value;
    }
}

static uint64_t gf_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    GeForceState *s = opaque;
    uint32_t offset = addr & (GEFORCE_PNPMMIO_SIZE - 1);

    switch (size) {
    case 1:
        return gf_register_read8(s, offset);
    case 2:
        return gf_register_read32(s, offset) & 0xffff;
    default:
        return gf_register_read32(s, offset);
    }
}

static void gf_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    GeForceState *s = opaque;
    uint32_t offset = addr & (GEFORCE_PNPMMIO_SIZE - 1);

    switch (size) {
    case 1:
        gf_register_write8(s, offset, val);
        break;
    case 2:
        gf_register_write8(s, offset, val & 0xff);
        gf_register_write8(s, offset + 1, (val >> 8) & 0xff);
        break;
    default:
        gf_register_write32(s, offset, val);
        break;
    }
}

static const MemoryRegionOps gf_mmio_ops = {
    .read = gf_mmio_read,
    .write = gf_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * ------------------------------------------------------------------------
 * Device reset / realize
 * ------------------------------------------------------------------------
 */

static void gf_free_channel_buffers(GeForceState *s)
{
    int i;

    for (i = 0; i < GEFORCE_CHANNEL_COUNT; i++) {
        g_free(s->chs[i].iifc_words);
        g_free(s->chs[i].sifc_words);
        g_free(s->chs[i].tfc_words);
        g_free(s->chs[i].gdi_words);
    }
}

static void gf_init_members(GeForceState *s)
{
    int i;

    s->mc_soft_intr = false;
    s->mc_intr_en = 0;
    s->mc_enable = 0;
    s->bus_intr = 0;
    s->bus_intr_en = 0;
    s->fifo_wait = false;
    s->fifo_wait_soft = false;
    s->fifo_wait_notify = false;
    s->fifo_wait_flip = false;
    s->fifo_wait_acquire = false;
    s->fifo_intr = 0;
    s->fifo_intr_en = 0;
    s->fifo_ramht = 0;
    s->fifo_ramfc = 0;
    s->fifo_ramro = 0;
    s->fifo_mode = 0;
    s->fifo_cache1_push0 = 0;
    s->fifo_cache1_push1 = 0;
    s->fifo_cache1_put = 0;
    s->fifo_cache1_dma_push = 0;
    s->fifo_cache1_dma_instance = 0;
    s->fifo_cache1_dma_put = 0;
    s->fifo_cache1_dma_get = 0;
    s->fifo_cache1_ref_cnt = 0;
    s->fifo_cache1_pull0 = 0;
    s->fifo_cache1_semaphore = 0;
    s->fifo_cache1_get = 0;
    s->fifo_grctx_instance = 0;
    memset(s->fifo_cache1_method, 0, sizeof(s->fifo_cache1_method));
    memset(s->fifo_cache1_data, 0, sizeof(s->fifo_cache1_data));
    s->rma_addr = 0;
    s->timer_intr = 0;
    s->timer_intr_en = 0;
    s->timer_num = 0;
    s->timer_den = 0;
    s->timer_inittime1 = 0;
    s->timer_inittime2 = 0;
    s->timer_alarm = 0;
    s->graph_intr = 0;
    s->graph_nsource = 0;
    s->graph_intr_en = 0;
    s->graph_ctx_switch1 = 0;
    s->graph_ctx_switch2 = 0;
    s->graph_ctx_switch4 = 0;
    s->graph_ctxctl_cur = 0;
    s->graph_status = 0;
    s->graph_trapped_addr = 0;
    s->graph_trapped_data = 0;
    s->graph_flip_read = 0;
    s->graph_flip_write = 0;
    s->graph_flip_modulo = 0;
    s->graph_notify = 0;
    s->graph_fifo = 0;
    s->graph_bpixel = 0;
    s->graph_channel_ctx_table = 0;
    s->graph_offset0 = 0;
    s->graph_pitch0 = 0;
    s->crtc_intr = 0;
    s->crtc_intr_en = 0;
    s->crtc_start = 0;
    s->crtc_config = 0;
    s->crtc_raster_pos = 0;
    s->crtc_cursor_offset = 0;
    s->crtc_cursor_config = 0;
    s->crtc_gpio_ext = 0;
    s->ramdac_cu_start_pos = 0;
    s->ramdac_vpll = 0;
    s->ramdac_vpll_b = 0;
    s->ramdac_pll_select = 0;
    s->ramdac_general_control = 0;

    gf_free_channel_buffers(s);
    memset(s->chs, 0x00, sizeof(s->chs));
    for (i = 0; i < GEFORCE_CHANNEL_COUNT; i++) {
        s->chs[i].swzs_color_bytes = 1;
        s->chs[i].s2d_color_bytes = 1;
        s->chs[i].d3d_color_bytes = 1;
        s->chs[i].d3d_depth_bytes = 1;
    }

    if (s->unk_regs) {
        memset(s->unk_regs, 0, GEFORCE_PNPMMIO_SIZE);
    }

    s->bank_base[0] = 0;
    s->bank_base[1] = 0;

    s->hw_cursor.x = 0;
    s->hw_cursor.y = 0;
    s->hw_cursor.size = 32;
    s->hw_cursor.offset = 0;
    s->hw_cursor.bpp32 = false;
    s->hw_cursor.enabled = false;
    s->hw_cursor.vram = false;

    /* NV15: 64MB board default; straps value is a guess (matches Bochs) */
    s->straps0_primary_original = 0x7FF86C6B | 0x00000180;
    s->straps0_primary = s->straps0_primary_original;
    s->ramin_flip = s->memsize - 64;
    s->memsize_mask = s->memsize - 1;
    s->class_mask = s->card_type < 0x40 ? 0x00000FFF : 0x0000FFFF;
}

static void gf_reset(DeviceState *dev)
{
    GeForceState *s = GEFORCE_DEVICE(dev);

    vga_common_reset(&s->vga);
    gf_init_members(s);
    gf_bank_window_update(s);
    /* Disable ROM shadowing to allow clearing of VRAM */
    s->parent_obj.config[0x50] = 0x00;
}

static void gf_init_pci_caps(PCIDevice *dev, Error **errp)
{
    uint8_t *conf = dev->config;

    /* AGP 2.0 capability at 0x44 */
    if (pci_add_capability(dev, PCI_CAP_ID_AGP, 0x44, 0x10, errp) < 0) {
        return;
    }
    conf[0x46] = 0x20; /* AGP revision 2.0 */
    conf[0x47] = 0x00;
    conf[0x48] = 0x07; /* AGP status: 1x/2x/4x rates ... */
    conf[0x49] = 0x00;
    conf[0x4a] = 0x00;
    conf[0x4b] = 0x1F; /* ... RQ depth */
    conf[0x54] = 0x01;
    conf[0x55] = 0x00;
    conf[0x56] = 0x00;
    conf[0x57] = 0x00;
    /* AGP command register is writable */
    dev->wmask[0x4c] = 0xFF;
    dev->wmask[0x4d] = 0xFF;
    dev->wmask[0x4e] = 0xFF;
    dev->wmask[0x4f] = 0xFF;

    /* Power Management capability (version 1) at 0x60 */
    if (pci_add_capability(dev, PCI_CAP_ID_PM, 0x60, PCI_PM_SIZEOF,
                           errp) < 0) {
        return;
    }
    pci_set_word(conf + 0x62, 0x0001); /* PMC: PM version 1 */
    dev->wmask[0x64] = 0x03;           /* PMCSR: PowerState */

    pci_set_word(conf + PCI_STATUS,
                 pci_get_word(conf + PCI_STATUS) |
                 PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
                 PCI_STATUS_DEVSEL_MEDIUM);

    /* Subsystem ID mirror / scratch registers */
    conf[0x40] = conf[0x2c];
    conf[0x41] = conf[0x2d];
    conf[0x42] = conf[0x2e];
    conf[0x43] = conf[0x2f];

    /* ROM shadow control */
    dev->wmask[0x50] = 0xFF;
}

static void gf_realize(PCIDevice *dev, Error **errp)
{
    ERRP_GUARD();
    GeForceState *s = GEFORCE_DEVICE(dev);
    VGACommonState *vga = &s->vga;
    I2CBus *i2cbus;
    uint32_t vram_size_mb = vga->vram_size_mb;
    int i;

    if (vram_size_mb < 16 || vram_size_mb > 128 ||
        !is_power_of_2(vram_size_mb)) {
        error_setg(errp, "geforce: invalid vgamem_mb %u (must be a power of "
                   "two between 16 and 128)", vram_size_mb);
        return;
    }

    s->card_type = 0x15; /* NV15 (Celsius) */
    s->memsize = vram_size_mb * MiB;

    if (!vga_common_init(vga, OBJECT(dev), errp)) {
        return;
    }
    s->memsize = vga->vram_size;
    vga->get_bpp = gf_get_bpp;
    vga->get_params = gf_get_params;
    vga->get_resolution = gf_get_resolution;
    vga->cursor_invalidate = gf_cursor_invalidate;
    vga->cursor_draw_line = gf_cursor_draw_line;

    vga_init(vga, OBJECT(dev), pci_address_space(dev),
             pci_address_space_io(dev), false);
    vga->con = graphic_console_init(DEVICE(dev), 0, vga->hw_ops, vga);

    portio_list_init(&vga->vga_port_list, OBJECT(dev), gf_vga_portio_list,
                     s, "geforce-vga-io");
    portio_list_set_flush_coalesced(&vga->vga_port_list);
    portio_list_add(&vga->vga_port_list, pci_address_space_io(dev), 0x3b0);

    /* BAR0: register aperture */
    s->unk_regs = g_malloc0(GEFORCE_PNPMMIO_SIZE);
    memory_region_init_io(&s->mmio, OBJECT(dev), &gf_mmio_ops, s,
                          "geforce-mmio", GEFORCE_PNPMMIO_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /* BAR1: 128MB prefetchable window; VRAM mirrored to fill it */
    memory_region_init(&s->lfb, OBJECT(dev), "geforce-lfb",
                       GEFORCE_LFB_BAR_SIZE);
    for (i = 0; i < GEFORCE_LFB_BAR_SIZE / s->memsize &&
                i < ARRAY_SIZE(s->lfb_alias); i++) {
        g_autofree char *name = g_strdup_printf("geforce-lfb-alias%d", i);
        memory_region_init_alias(&s->lfb_alias[i], OBJECT(dev), name,
                                 &vga->vram, 0, s->memsize);
        memory_region_add_subregion(&s->lfb, (uint64_t)i * s->memsize,
                                    &s->lfb_alias[i]);
    }
    pci_register_bar(dev, 1,
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_32,
                     &s->lfb);

    /* Banked SVGA window at 0xA0000 */
    memory_region_init_io(&s->bank_window, OBJECT(dev), &gf_bank_window_ops,
                          s, "geforce-bank-window", 0x10000);
    memory_region_add_subregion_overlap(vga->legacy_address_space, 0xa0000,
                                        &s->bank_window, 3);
    memory_region_set_enabled(&s->bank_window, false);

    /* PCI configuration */
    pci_config_set_interrupt_pin(dev->config, 1);
    gf_init_pci_caps(dev, errp);
    if (*errp) {
        return;
    }

    /* DDC/EDID */
    i2cbus = i2c_init_bus(DEVICE(dev), "geforce.ddc");
    bitbang_i2c_init(&s->bbi2c, i2cbus);
    i2c_slave_set_address(I2C_SLAVE(&s->i2cddc), 0x50);
    qdev_realize(DEVICE(&s->i2cddc), BUS(i2cbus), &error_abort);

    /* Vertical retrace timer */
    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gf_vblank_timer_cb, s);
    timer_mod(s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                               GEFORCE_VBLANK_PERIOD_NS);

    gf_init_method_handlers(s);
    gf_reset(DEVICE(dev));
}

static void gf_exit(PCIDevice *dev)
{
    GeForceState *s = GEFORCE_DEVICE(dev);

    timer_free(s->vblank_timer);
    gf_free_channel_buffers(s);
    g_free(s->unk_regs);
    s->unk_regs = NULL;
}

static const VMStateDescription gf_vmstate = {
    .name = TYPE_GEFORCE_DEVICE,
    .unmigratable = 1,
};

static const Property gf_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", GeForceState, vga.vram_size_mb, 64),
    DEFINE_EDID_PROPERTIES(GeForceState, i2cddc.edid_info),
};

static void gf_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = gf_realize;
    pc->exit = gf_exit;
    pc->vendor_id = PCI_VENDOR_ID_NVIDIA_GF;
    pc->device_id = PCI_DEVICE_ID_NV15GL_QUADRO2PRO;
    pc->subsystem_vendor_id = GEFORCE_SUBSYSTEM_VENDOR_ID;
    pc->subsystem_id = GEFORCE_SUBSYSTEM_ID;
    pc->class_id = PCI_CLASS_DISPLAY_VGA;
    pc->romfile = NULL;
    device_class_set_legacy_reset(dc, gf_reset);
    dc->vmsd = &gf_vmstate;
    device_class_set_props(dc, gf_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "NVIDIA Quadro2 Pro (NV15GL)";
    dc->user_creatable = true;
    dc->hotpluggable = false;
}

static void gf_instance_init(Object *o)
{
    GeForceState *s = GEFORCE_DEVICE(o);

    object_initialize_child(o, "edid", &s->i2cddc, TYPE_I2CDDC);
}

static const TypeInfo gf_info = {
    .name          = TYPE_GEFORCE_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GeForceState),
    .instance_init = gf_instance_init,
    .class_init    = gf_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void gf_register_types(void)
{
    type_register_static(&gf_info);
}

type_init(gf_register_types)
