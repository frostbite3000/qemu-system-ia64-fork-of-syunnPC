/*
 * QEMU NVIDIA Quadro2 Pro (NV15GL) emulation -- internal interfaces
 *
 * Based on the Bochs GeForce emulation:
 *   Copyright (C) 2025-2026  The Bochs Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_DISPLAY_GEFORCE_INT_H
#define HW_DISPLAY_GEFORCE_INT_H

#include "geforce.h"
#include "geforce_pxextract.h"

#define GF_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* geforce.c: memory accessors and core services */
uint8_t gf_vram_read8(GeForceState *s, uint32_t addr);
uint16_t gf_vram_read16(GeForceState *s, uint32_t addr);
uint32_t gf_vram_read32(GeForceState *s, uint32_t addr);
uint64_t gf_vram_read64(GeForceState *s, uint32_t addr);
void gf_vram_write8(GeForceState *s, uint32_t addr, uint8_t val);
void gf_vram_write16(GeForceState *s, uint32_t addr, uint16_t val);
void gf_vram_write32(GeForceState *s, uint32_t addr, uint32_t val);
void gf_vram_write64(GeForceState *s, uint32_t addr, uint64_t val);
uint8_t gf_ramin_read8(GeForceState *s, uint32_t addr);
uint16_t gf_ramin_read16(GeForceState *s, uint32_t addr);
uint32_t gf_ramin_read32(GeForceState *s, uint32_t addr);
void gf_ramin_write8(GeForceState *s, uint32_t addr, uint8_t val);
void gf_ramin_write32(GeForceState *s, uint32_t addr, uint32_t val);
uint8_t gf_dma_read8(GeForceState *s, uint32_t object, uint32_t addr);
uint16_t gf_dma_read16(GeForceState *s, uint32_t object, uint32_t addr);
uint32_t gf_dma_read32(GeForceState *s, uint32_t object, uint32_t addr);
uint64_t gf_dma_read64(GeForceState *s, uint32_t object, uint32_t addr);
void gf_dma_write8(GeForceState *s, uint32_t object, uint32_t addr,
                   uint8_t val);
void gf_dma_write16(GeForceState *s, uint32_t object, uint32_t addr,
                    uint16_t val);
void gf_dma_write32(GeForceState *s, uint32_t object, uint32_t addr,
                    uint32_t val);
void gf_dma_write64(GeForceState *s, uint32_t object, uint32_t addr,
                    uint64_t val);
void gf_dma_copy(GeForceState *s, uint32_t dst_obj, uint32_t dst_addr,
                 uint32_t src_obj, uint32_t src_addr, uint32_t byte_count);
uint32_t gf_dma_pt_lookup(GeForceState *s, uint32_t object, uint32_t addr);
uint32_t gf_dma_lin_lookup(GeForceState *s, uint32_t object, uint32_t addr);
uint32_t gf_ramfc_read32(GeForceState *s, uint32_t chid, uint32_t offset);
void gf_ramfc_write32(GeForceState *s, uint32_t chid, uint32_t offset,
                      uint32_t val);
void gf_ramht_lookup(GeForceState *s, uint32_t handle, uint32_t chid,
                     uint32_t *object, uint8_t *engine);

uint8_t gf_register_read8(GeForceState *s, uint32_t address);
void gf_register_write8(GeForceState *s, uint32_t address, uint8_t value);
uint32_t gf_register_read32(GeForceState *s, uint32_t address);
void gf_register_write32(GeForceState *s, uint32_t address, uint32_t value);

uint64_t gf_get_current_time(GeForceState *s);
void gf_update_irq_level(GeForceState *s);

/*
 * Mark a rectangle of the current display surface dirty.  "offset" is an
 * absolute byte offset into VRAM of the upper left corner of the changed
 * area, "width" is in pixels of the current display depth.
 */
void gf_redraw_area(GeForceState *s, uint32_t offset,
                    uint32_t width, uint32_t height);
uint32_t gf_disp_pitch(GeForceState *s);
uint32_t gf_disp_bpp(GeForceState *s);

/* geforce_2d.c: FIFO and 2D acceleration */
void gf_update_fifo_wait(GeForceState *s);
void gf_fifo_process_all(GeForceState *s);
void gf_fifo_process(GeForceState *s, uint32_t chid);
int gf_execute_command(GeForceState *s, uint32_t chid, uint32_t subc,
                       uint32_t method, uint32_t param);
void gf_update_color_bytes_iifc(GeForceState *s, gf_channel *ch);
void gf_update_color_bytes_s2d(GeForceState *s, gf_channel *ch);
uint32_t gf_swizzle(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
uint8_t gf_alpha_wrap(int value);

/* geforce_3d.c: D3D engine core */
void gf_d3d_clear_surface_op(GeForceState *s, gf_channel *ch);
void gf_d3d_process_vertex(GeForceState *s, gf_channel *ch, bool immediate);
void gf_d3d_load_vertex(GeForceState *s, gf_channel *ch, uint32_t index);
void gf_d3d_texture_process_format(gf_texture *tex);
void gf_texture_update_size(gf_texture *tex, uint32_t cls);
float gf_uint32_as_float(uint32_t val);
void gf_unpack_attribute(uint32_t value, bool d3d, float comp[4]);

/* geforce_3d_mh.c: D3D method handlers */
void gf_init_method_handlers(GeForceState *s);
void gf_execute_d3d(GeForceState *s, gf_channel *ch, uint32_t cls,
                    uint32_t method, uint32_t param);

#endif /* HW_DISPLAY_GEFORCE_INT_H */
