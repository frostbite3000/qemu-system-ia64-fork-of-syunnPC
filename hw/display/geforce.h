/*
 * QEMU NVIDIA Quadro2 Pro (NV15GL) emulation
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

#ifndef HW_DISPLAY_GEFORCE_H
#define HW_DISPLAY_GEFORCE_H

#include "hw/pci/pci_device.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/i2c-ddc.h"
#include "qemu/timer.h"
#include "vga_int.h"
#include "qom/object.h"
#include "qemu/units.h"

#define PCI_VENDOR_ID_NVIDIA_GF          0x10DE
#define PCI_DEVICE_ID_NV15GL_QUADRO2PRO  0x0153
#define GEFORCE_SUBSYSTEM_VENDOR_ID      0x10DE
#define GEFORCE_SUBSYSTEM_ID             0x006D

/* 0x3b4,0x3d4 */
#define GEFORCE_VGA_CRTC_MAX 0x18
#define GEFORCE_CRTC_MAX 0xF0

#define GEFORCE_CHANNEL_COUNT 32
#define GEFORCE_SUBCHANNEL_COUNT 8
#define GEFORCE_CACHE1_SIZE 64

#define GEFORCE_CLASS_COUNT 0x10000
#define GEFORCE_METHOD_COUNT 0x800

#define GEFORCE_PNPMMIO_SIZE 0x1000000
#define GEFORCE_LFB_BAR_SIZE (128 * MiB)

typedef struct gf_texture {
    uint32_t offset;
    uint32_t dma_obj;
    uint32_t format;
    bool cubemap;
    bool linear;
    bool unnormalized;
    bool compressed;
    bool dxt_alpha_data;
    bool dxt_alpha_explicit;
    uint32_t color_bytes;
    uint32_t levels;
    uint32_t base_size[3];
    uint32_t size[3];
    uint32_t face_bytes;
    uint32_t wrap[3];
    uint32_t control0;
    bool enabled;
    uint32_t control1;
    bool signed_any;
    bool signed_comp[4];
    uint32_t image_rect;
    uint32_t pal_dma_obj;
    uint32_t pal_ofs;
    uint32_t control3;
    uint32_t key_color;
    float offset_matrix[4];
} gf_texture;

typedef struct gf_light {
    float ambient_color[3];
    float diffuse_color[3];
    float specular_color[3];
    float inf_half_vector[3];
    float inf_direction[3];
    float spot_direction[4];
    float local_position[3];
    float local_attenuation[3];
} gf_light;

typedef struct gf_channel {
    uint32_t subr_return;
    bool subr_active;
    struct {
        uint32_t mthd;
        uint32_t subc;
        uint32_t mcnt;
        bool ni;
    } dma_state;
    struct {
        uint32_t object;
        uint8_t engine;
        uint32_t notifier;
    } schs[GEFORCE_SUBCHANNEL_COUNT];

    bool notify_pending;
    uint32_t notify_type;

    bool s2d_locked;
    uint32_t s2d_img_src;
    uint32_t s2d_img_dst;
    uint32_t s2d_color_fmt;
    uint32_t s2d_color_bytes;
    uint32_t s2d_pitch_src;
    uint32_t s2d_pitch_dst;
    uint32_t s2d_ofs_src;
    uint32_t s2d_ofs_dst;

    uint32_t swzs_img_obj;
    uint32_t swzs_fmt;
    uint32_t swzs_color_bytes;
    uint32_t swzs_width;
    uint32_t swzs_height;
    uint32_t swzs_ofs;

    bool ifc_color_key_enable;
    bool ifc_clip_enable;
    uint32_t ifc_operation;
    uint32_t ifc_color_fmt;
    uint32_t ifc_color_bytes;
    uint32_t ifc_pixels_per_word;
    uint32_t ifc_x;
    uint32_t ifc_y;
    uint32_t ifc_ofs_x;
    uint32_t ifc_ofs_y;
    uint32_t ifc_draw_offset;
    uint32_t ifc_redraw_offset;
    uint32_t ifc_dst_width;
    uint32_t ifc_dst_height;
    uint32_t ifc_src_width;
    uint32_t ifc_src_height;
    uint32_t ifc_clip_x0;
    uint32_t ifc_clip_y0;
    uint32_t ifc_clip_x1;
    uint32_t ifc_clip_y1;

    uint32_t iifc_palette;
    uint32_t iifc_palette_ofs;
    uint32_t iifc_operation;
    uint32_t iifc_color_fmt;
    uint32_t iifc_color_bytes;
    uint32_t iifc_bpp4;
    uint32_t iifc_yx;
    uint32_t iifc_dhw;
    uint32_t iifc_shw;
    uint32_t iifc_words_ptr;
    uint32_t iifc_words_left;
    uint32_t *iifc_words;

    uint32_t sifc_operation;
    uint32_t sifc_color_fmt;
    uint32_t sifc_color_bytes;
    uint32_t sifc_shw;
    uint32_t sifc_dxds;
    uint32_t sifc_dydt;
    uint32_t sifc_clip_yx;
    uint32_t sifc_clip_hw;
    uint32_t sifc_syx;
    uint32_t sifc_words_ptr;
    uint32_t sifc_words_left;
    uint32_t *sifc_words;

    bool blit_color_key_enable;
    uint32_t blit_operation;
    uint32_t blit_syx;
    uint32_t blit_dyx;
    uint32_t blit_hw;

    bool tfc_swizzled;
    uint32_t tfc_color_fmt;
    uint32_t tfc_color_bytes;
    uint32_t tfc_yx;
    uint32_t tfc_hw;
    uint32_t tfc_clip_wx;
    uint32_t tfc_clip_hy;
    uint32_t tfc_words_ptr;
    uint32_t tfc_words_left;
    uint32_t *tfc_words;
    bool tfc_upload;
    uint32_t tfc_upload_offset;

    uint32_t sifm_src;
    bool sifm_swizzled;
    bool sifm_swizzled_0389;
    uint32_t sifm_operation;
    uint32_t sifm_color_fmt;
    uint32_t sifm_color_bytes;
    uint32_t sifm_syx;
    uint32_t sifm_dyx;
    uint32_t sifm_shw;
    uint32_t sifm_dhw;
    int32_t sifm_dudx;
    int32_t sifm_dvdy;
    uint32_t sifm_sfmt;
    uint32_t sifm_sofs;

    uint32_t m2mf_src;
    uint32_t m2mf_dst;
    uint32_t m2mf_src_offset;
    uint32_t m2mf_dst_offset;
    uint32_t m2mf_src_pitch;
    uint32_t m2mf_dst_pitch;
    uint32_t m2mf_line_length;
    uint32_t m2mf_line_count;
    uint32_t m2mf_format;
    uint32_t m2mf_buffer_notify;

    uint32_t d3d_a_obj;
    uint32_t d3d_b_obj;
    uint32_t d3d_color_obj;
    uint32_t d3d_zeta_obj;
    uint32_t d3d_vertex_a_obj;
    uint32_t d3d_vertex_b_obj;
    uint32_t d3d_report_obj;
    uint32_t d3d_clip_horizontal;
    uint32_t d3d_clip_vertical;
    uint32_t d3d_surface_format;
    uint32_t d3d_color_bytes;
    uint32_t d3d_depth_bytes;
    uint32_t d3d_surface_pitch_a;
    uint32_t d3d_surface_pitch_z;
    bool d3d_local_viewer;
    uint32_t d3d_color_material_emission;
    uint32_t d3d_color_material_ambient;
    uint32_t d3d_color_material_diffuse;
    uint32_t d3d_color_material_specular;
    uint32_t d3d_fog_mode;
    uint32_t d3d_fog_gen_mode;
    float d3d_fog_params[3];
    uint32_t d3d_fog_enable;
    float d3d_fog_color[4];
    int16_t d3d_window_offset_x;
    int16_t d3d_window_offset_y;
    uint32_t d3d_window_clip_x1[8];
    uint32_t d3d_window_clip_x2[8];
    uint32_t d3d_window_clip_y1[8];
    uint32_t d3d_window_clip_y2[8];
    uint32_t d3d_surface_color_offset;
    uint32_t d3d_surface_zeta_offset;
    uint32_t d3d_combiner_alpha_icw[8];
    uint32_t d3d_combiner_final[2];
    uint32_t d3d_alpha_test_enable;
    uint32_t d3d_alpha_func;
    uint32_t d3d_alpha_ref;
    uint32_t d3d_blend_enable;
    uint16_t d3d_blend_sfactor_rgb;
    uint16_t d3d_blend_sfactor_alpha;
    uint16_t d3d_blend_dfactor_rgb;
    uint16_t d3d_blend_dfactor_alpha;
    uint16_t d3d_blend_equation_rgb;
    uint16_t d3d_blend_equation_alpha;
    float d3d_blend_color[4];
    uint32_t d3d_cull_face_enable;
    uint32_t d3d_depth_test_enable;
    uint32_t d3d_depth_write_enable;
    uint32_t d3d_stencil_mask;
    uint32_t d3d_stencil_func;
    uint32_t d3d_stencil_func_ref;
    uint32_t d3d_stencil_func_mask;
    uint32_t d3d_stencil_op_sfail;
    uint32_t d3d_stencil_op_dpfail;
    uint32_t d3d_stencil_op_dppass;
    uint32_t d3d_lighting_enable;
    uint32_t d3d_stencil_test_enable;
    uint32_t d3d_depth_func;
    uint32_t d3d_color_mask;
    uint32_t d3d_color_mask_565;
    uint32_t d3d_color_mask_8888;
    uint32_t d3d_shade_mode;
    float d3d_clip_min;
    float d3d_clip_max;
    uint32_t d3d_cull_face;
    uint32_t d3d_front_face;
    uint32_t d3d_normalize_enable;
    float d3d_material_factor[4];
    uint32_t d3d_separate_specular;
    uint32_t d3d_light_enable_mask;
    uint32_t d3d_texgen[8][4];
    uint32_t d3d_texture_matrix_enable[16];
    uint32_t d3d_view_matrix_enable;
    float d3d_model_view_matrix[2][16];
    float d3d_inverse_model_view_matrix[12];
    float d3d_composite_matrix[16];
    float d3d_texture_matrix[8][16];
    float d3d_texgen_plane[8][4][4];
    uint32_t d3d_scissor_x;
    uint32_t d3d_scissor_width;
    uint32_t d3d_scissor_y;
    uint32_t d3d_scissor_height;
    uint32_t d3d_shader_program;
    uint32_t d3d_shader_obj;
    uint32_t d3d_shader_offset;
    float d3d_specular_params[6];
    float d3d_specular_power;
    float d3d_scene_ambient_color[4];
    uint32_t d3d_viewport_x;
    uint32_t d3d_viewport_width;
    uint32_t d3d_viewport_y;
    uint32_t d3d_viewport_height;
    float d3d_viewport_offset[4];
    float d3d_eye_position[4];
    float d3d_combiner_const_color[8][2][4];
    uint32_t d3d_combiner_alpha_ocw[8];
    uint32_t d3d_combiner_color_icw[8];
    float d3d_viewport_scale[4];
    uint32_t d3d_transform_program[544][4];
    float d3d_transform_constant[512][4];
    gf_light d3d_light[8];
    uint32_t d3d_attrib_count;
    uint32_t d3d_vertex_data_base_index;
    uint32_t d3d_vertex_data_array_offset[16];
    uint32_t d3d_vertex_data_array_format_type[16];
    uint32_t d3d_vertex_data_array_format_size[16];
    uint32_t d3d_vertex_data_array_format_stride[16];
    bool d3d_vertex_data_array_format_dx[16];
    bool d3d_vertex_data_array_format_homogeneous[16];
    uint32_t d3d_begin_end;
    bool d3d_primitive_done;
    bool d3d_triangle_flip;
    uint32_t d3d_vertex_index;
    uint32_t d3d_attrib_index;
    uint32_t d3d_comp_index;
    float d3d_vertex_data[4][16][4];
    float d3d_vertex_data_imm[16][4];
    uint32_t d3d_index_array_offset;
    bool d3d_index_array_dma;
    bool d3d_index_array_type_16;
    gf_texture d3d_texture[16];
    uint32_t d3d_shader_control;
    uint32_t d3d_semaphore_obj;
    uint32_t d3d_semaphore_offset;
    uint32_t d3d_zstencil_clear_value;
    uint32_t d3d_color_clear_value;
    uint32_t d3d_clear_surface;
    uint32_t d3d_combiner_color_ocw[8];
    uint32_t d3d_combiner_control;
    uint32_t d3d_combiner_control_num_stages;
    uint32_t d3d_tex_shader_op[4];
    uint32_t d3d_tex_shader_dotmapping[4];
    uint32_t d3d_tex_shader_previous[4];
    uint32_t d3d_transform_execution_mode;
    uint32_t d3d_transform_program_load;
    uint32_t d3d_transform_program_start;
    uint32_t d3d_transform_constant_load;
    uint32_t d3d_attrib_in_normal;
    uint32_t d3d_attrib_in_color[2];
    uint32_t d3d_attrib_out_color[2];
    uint32_t d3d_attrib_out_fogc;
    uint32_t d3d_attrib_in_tex_coord[16];
    uint32_t d3d_attrib_out_tex_coord[16];
    bool d3d_attrib_out_enable[32];
    uint32_t d3d_vs_temp_regs_count;
    uint32_t d3d_tex_coord_count;

    uint8_t rop;

    uint32_t beta;

    uint16_t clip_x;
    uint16_t clip_y;
    uint16_t clip_width;
    uint16_t clip_height;

    uint32_t chroma_color_fmt;
    uint32_t chroma_color;

    uint32_t patt_shape;
    bool patt_type_color;
    uint32_t patt_bg_color;
    uint32_t patt_fg_color;
    bool patt_data_mono[64];
    uint32_t patt_data_color[64];

    uint32_t gdi_operation;
    uint32_t gdi_color_fmt;
    uint32_t gdi_mono_fmt;
    uint32_t gdi_clip_yx0;
    uint32_t gdi_clip_yx1;
    uint32_t gdi_rect_color;
    uint32_t gdi_rect_xy;
    uint32_t gdi_rect_yx0;
    uint32_t gdi_rect_yx1;
    uint32_t gdi_rect_wh;
    uint32_t gdi_bg_color;
    uint32_t gdi_fg_color;
    uint32_t gdi_image_swh;
    uint32_t gdi_image_dwh;
    uint32_t gdi_image_xy;
    uint32_t gdi_words_ptr;
    uint32_t gdi_words_left;
    uint32_t *gdi_words;

    uint32_t rect_operation;
    uint32_t rect_color_fmt;
    uint32_t rect_color;
    uint32_t rect_yx;
    uint32_t rect_hw;
} gf_channel;

#define TYPE_GEFORCE_DEVICE "geforce"
OBJECT_DECLARE_SIMPLE_TYPE(GeForceState, GEFORCE_DEVICE)

typedef void (*gf_method_handler)(GeForceState *s, gf_channel *ch,
                                  uint32_t cls, uint32_t method,
                                  uint32_t param);

struct GeForceState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    VGACommonState vga;

    /* BAR0: PNPMMIO register aperture */
    MemoryRegion mmio;
    /* BAR1: 128MB prefetchable linear framebuffer window */
    MemoryRegion lfb;
    MemoryRegion lfb_alias[8];
    /* Banked SVGA-mode window at 0xA0000 */
    MemoryRegion bank_window;

    QEMUTimer *vblank_timer;

    bitbang_i2c_interface bbi2c;
    I2CDDCState i2cddc;

    bool mc_soft_intr;
    uint32_t mc_intr_en;
    uint32_t mc_enable;
    uint32_t bus_intr;
    uint32_t bus_intr_en;
    bool fifo_wait;
    bool fifo_wait_soft;
    bool fifo_wait_notify;
    bool fifo_wait_flip;
    bool fifo_wait_acquire;
    uint32_t fifo_intr;
    uint32_t fifo_intr_en;
    uint32_t fifo_ramht;
    uint32_t fifo_ramfc;
    uint32_t fifo_ramro;
    uint32_t fifo_mode;
    uint32_t fifo_cache1_push0;
    uint32_t fifo_cache1_push1;
    uint32_t fifo_cache1_put;
    uint32_t fifo_cache1_dma_push;
    uint32_t fifo_cache1_dma_instance;
    uint32_t fifo_cache1_dma_put;
    uint32_t fifo_cache1_dma_get;
    uint32_t fifo_cache1_ref_cnt;
    uint32_t fifo_cache1_pull0;
    uint32_t fifo_cache1_semaphore;
    uint32_t fifo_cache1_get;
    uint32_t fifo_grctx_instance;
    uint32_t fifo_cache1_method[GEFORCE_CACHE1_SIZE];
    uint32_t fifo_cache1_data[GEFORCE_CACHE1_SIZE];
    uint32_t rma_addr;
    uint32_t timer_intr;
    uint32_t timer_intr_en;
    uint32_t timer_num;
    uint32_t timer_den;
    uint64_t timer_inittime1;
    uint64_t timer_inittime2;
    uint32_t timer_alarm;
    uint32_t straps0_primary;
    uint32_t straps0_primary_original;
    uint32_t graph_intr;
    uint32_t graph_nsource;
    uint32_t graph_intr_en;
    uint32_t graph_ctx_switch1;
    uint32_t graph_ctx_switch2;
    uint32_t graph_ctx_switch4;
    uint32_t graph_ctxctl_cur;
    uint32_t graph_status;
    uint32_t graph_trapped_addr;
    uint32_t graph_trapped_data;
    uint32_t graph_flip_read;
    uint32_t graph_flip_write;
    uint32_t graph_flip_modulo;
    uint32_t graph_notify;
    uint32_t graph_fifo;
    uint32_t graph_bpixel;
    uint32_t graph_channel_ctx_table;
    uint32_t graph_offset0;
    uint32_t graph_pitch0;
    uint32_t crtc_intr;
    uint32_t crtc_intr_en;
    uint32_t crtc_start;
    uint32_t crtc_config;
    uint32_t crtc_raster_pos;
    uint32_t crtc_cursor_offset;
    uint32_t crtc_cursor_config;
    uint32_t crtc_gpio_ext;
    uint32_t ramdac_cu_start_pos;
    uint32_t ramdac_vpll;
    uint32_t ramdac_vpll_b;
    uint32_t ramdac_pll_select;
    uint32_t ramdac_general_control;

    gf_method_handler empty_method_handlers[GEFORCE_METHOD_COUNT];
    gf_method_handler cl0096_method_handlers[GEFORCE_METHOD_COUNT];
    gf_method_handler cl0097_method_handlers[GEFORCE_METHOD_COUNT];
    gf_method_handler cl0497_method_handlers[GEFORCE_METHOD_COUNT];
    gf_method_handler cl4097_method_handlers[GEFORCE_METHOD_COUNT];
    gf_method_handler *class_method_handlers[GEFORCE_CLASS_COUNT];

    gf_channel chs[GEFORCE_CHANNEL_COUNT];

    /* Backing store for registers not modeled explicitly */
    uint32_t *unk_regs;

    uint32_t bank_base[2];

    uint32_t card_type;
    uint32_t memsize;
    uint32_t memsize_mask;
    uint32_t ramin_flip;
    uint32_t class_mask;

    struct {
        bool vram;
        uint32_t offset;
        int16_t x, y;
        uint8_t size;
        bool bpp32;
        bool enabled;
        /* State used for scanline invalidation between frames */
        int16_t prev_x, prev_y;
        uint8_t prev_size;
        bool prev_enabled;
    } hw_cursor;
};

#endif /* HW_DISPLAY_GEFORCE_H */
