/*
 * QEMU NVIDIA Quadro2 Pro (NV15GL) emulation -- D3D method handlers
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

#define MH(name) \
    static void gf_d3d_mh_##name(GeForceState *s, gf_channel *ch, \
                                 uint32_t cls, uint32_t method, \
                                 uint32_t param)

MH(object)
{
    uint32_t j, ci;

    /* There may be a better place for initialization */
    if (cls == 0x0096) {
        ch->d3d_window_offset_x = 2048;
        ch->d3d_window_offset_y = 2048;
        ch->d3d_attrib_count = 8;
    } else {
        ch->d3d_window_offset_x = 0;
        ch->d3d_window_offset_y = 0;
        ch->d3d_attrib_count = 16;
    }
    for (j = 0; j < ch->d3d_attrib_count; j++) {
        ch->d3d_vertex_data_array_format_type[j] = 0;
        ch->d3d_vertex_data_array_format_size[j] = 0;
        ch->d3d_vertex_data_array_format_stride[j] = 0;
        ch->d3d_vertex_data_array_format_dx[j] = false;
        ch->d3d_vertex_data_array_format_homogeneous[j] = false;
    }
    if (cls == 0x0096) {
        ch->d3d_vs_temp_regs_count = 0;
    } else if (cls == 0x0097) {
        ch->d3d_vs_temp_regs_count = 12;
    } else if (cls <= 0x0497) {
        ch->d3d_vs_temp_regs_count = 16;
    } else {
        ch->d3d_vs_temp_regs_count = 32;
    }
    if (cls == 0x0096) {
        ch->d3d_combiner_control_num_stages = 2;
        ch->d3d_tex_coord_count = 2;
    } else if (cls == 0x0097) {
        ch->d3d_tex_coord_count = 4;
    } else {
        ch->d3d_tex_coord_count = 8;
    }
    if (cls == 0x0096) {
        ch->d3d_attrib_in_color[0] = 1;
        ch->d3d_attrib_in_color[1] = 2;
        ch->d3d_attrib_in_normal = 5;
    } else {
        ch->d3d_attrib_in_color[0] = 3;
        ch->d3d_attrib_in_color[1] = 4;
        ch->d3d_attrib_in_normal = 2;
    }
    ch->d3d_attrib_out_color[0] = 3;
    ch->d3d_attrib_out_color[1] = 4;
    ch->d3d_attrib_out_fogc = 5;
    for (j = 0; j < 32; j++) {
        ch->d3d_attrib_out_enable[j] = true;
    }
    for (j = 0; j < 16; j++) {
        ch->d3d_attrib_in_tex_coord[j] = 0xf;
        ch->d3d_attrib_out_tex_coord[j] = 0xf;
    }
    for (j = 0; j < ch->d3d_tex_coord_count; j++) {
        if (cls == 0x0096) {
            ch->d3d_attrib_in_tex_coord[j] = j + 3;
        } else if (cls == 0x0097) {
            ch->d3d_attrib_in_tex_coord[j] = j + 9;
        } else {
            ch->d3d_attrib_in_tex_coord[j] = j + 8;
        }
        if (cls <= 0x0097) {
            ch->d3d_attrib_out_tex_coord[j] = j + 9;
        } else if (cls <= 0x0497) {
            ch->d3d_attrib_out_tex_coord[j] = j + 8;
        } else {
            ch->d3d_attrib_out_tex_coord[j] = j + 7;
        }
    }
    for (ci = 0; ci < 4; ci++) {
        ch->d3d_vertex_data_imm[ch->d3d_attrib_in_color[0]][ci] = 1.0f;
    }
}

MH(flip_read)
{
    s->graph_flip_read = param;
}

MH(flip_write)
{
    s->graph_flip_write = param;
}

MH(flip_modulo)
{
    s->graph_flip_modulo = param;
}

MH(flip_incr)
{
    if (s->graph_flip_modulo != 0) {
        s->graph_flip_write++;
        s->graph_flip_write %= s->graph_flip_modulo;
    }
}

MH(fifo_wait)
{
    if (s->graph_flip_read == s->graph_flip_write) {
        s->fifo_wait_flip = true;
        s->fifo_wait = true;
    }
}

MH(a_obj)
{
    ch->d3d_a_obj = param;
}

MH(b_obj)
{
    ch->d3d_b_obj = param;
}

MH(vertex_obj)
{
    ch->d3d_vertex_a_obj = param;
    ch->d3d_vertex_b_obj = param;
}

MH(color_obj)
{
    ch->d3d_color_obj = param;
}

MH(zeta_obj)
{
    ch->d3d_zeta_obj = param;
}

MH(vertex_a_obj)
{
    ch->d3d_vertex_a_obj = param;
}

MH(vertex_b_obj)
{
    ch->d3d_vertex_b_obj = param;
}

MH(semaphore_obj)
{
    ch->d3d_semaphore_obj = param;
}

MH(report_obj)
{
    ch->d3d_report_obj = param;
}

MH(clip_horizontal)
{
    ch->d3d_clip_horizontal = param;
}

MH(clip_vertical)
{
    ch->d3d_clip_vertical = param;
}

MH(surface_format)
{
    uint32_t format_color;
    uint32_t format_depth;

    ch->d3d_surface_format = param;
    if (cls <= 0x0097) {
        format_color = param & 0x0000000F;
        format_depth = (param >> 4) & 0x0000000F;
    } else {
        format_color = param & 0x0000001F;
        format_depth = (param >> 5) & 0x00000007;
    }
    if (format_color == 0x9) {        /* B8 */
        ch->d3d_color_bytes = 1;
    } else if (format_color == 0x3) { /* R5G6B5 */
        ch->d3d_color_bytes = 2;
    } else if (format_color == 0x4 || /* X8R8G8B8_Z8R8G8B8 */
               format_color == 0x5 || /* X8R8G8B8_O8R8G8B8 */
               format_color == 0x8) { /* A8R8G8B8 */
        ch->d3d_color_bytes = 4;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "geforce: unknown D3D color format: 0x%01x\n",
                      format_color);
    }
    if (format_depth == 0) {
        ch->d3d_depth_bytes = ch->d3d_color_bytes;
    } else if (format_depth == 1) { /* Z16 */
        ch->d3d_depth_bytes = 2;
    } else if (format_depth == 2) { /* Z24S8 */
        ch->d3d_depth_bytes = 4;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "geforce: unknown D3D depth format: 0x%01x\n",
                      format_depth);
    }
    if (cls == 0x0096) {
        ch->d3d_viewport_scale[2] =
            ch->d3d_depth_bytes == 2 ? 32767.0f : 8388607.0f;
    }
}

MH(surface_pitch_a)
{
    ch->d3d_surface_pitch_a = param;
}

MH(surface_color_offset)
{
    ch->d3d_surface_color_offset = param;
}

MH(surface_zeta_offset)
{
    ch->d3d_surface_zeta_offset = param;
}

MH(surface_pitch_z)
{
    ch->d3d_surface_pitch_z = param;
}

MH(combiner_alpha_icw)
{
    uint32_t i = method - 0x098;
    ch->d3d_combiner_alpha_icw[i & 7] = param;
}

MH(combiner_final)
{
    uint32_t i = method - (cls <= 0x0097 ? 0x0a2 : 0x23d);
    ch->d3d_combiner_final[i & 1] = param;
}

MH(0096_0a5)
{
    ch->d3d_local_viewer = (param & 0x00010000) != 0;
}

MH(0096_0a6)
{
    if (cls == 0x0096) {
        ch->d3d_color_material_emission = (param >> 0) & 1;
        ch->d3d_color_material_ambient = (param >> 1) & 1;
        ch->d3d_color_material_diffuse = (param >> 2) & 1;
        ch->d3d_color_material_specular = (param >> 3) & 1;
    } else {
        ch->d3d_color_material_emission = (param >> 0) & 3;
        ch->d3d_color_material_ambient = (param >> 2) & 3;
        ch->d3d_color_material_diffuse = (param >> 4) & 3;
        ch->d3d_color_material_specular = (param >> 6) & 3;
    }
}

MH(fog_mode)
{
    ch->d3d_fog_mode = param;
}

MH(fog_gen_mode)
{
    ch->d3d_fog_gen_mode = param;
}

MH(fog_params)
{
    uint32_t i = method & 3;
    if (i < 3) {
        ch->d3d_fog_params[i] = gf_uint32_as_float(param);
    }
}

MH(fog_enable)
{
    ch->d3d_fog_enable = param;
}

MH(fog_color)
{
    uint32_t ci;
    for (ci = 0; ci < 4; ci++) {
        ch->d3d_fog_color[ci] = ((param >> (ci * 8)) & 0xff) / 255.0f;
    }
}

MH(window_offset)
{
    ch->d3d_window_offset_x = (int16_t)param;
    ch->d3d_window_offset_y = (int16_t)(param >> 16);
}

MH(window_clip)
{
    uint32_t index = (method >> 1) & 7;
    if ((method & 1) == 0) {
        ch->d3d_window_clip_x1[index] = param & 0x0000ffff;
        ch->d3d_window_clip_x2[index] = param >> 16;
    } else {
        ch->d3d_window_clip_y1[index] = param & 0x0000ffff;
        ch->d3d_window_clip_y2[index] = param >> 16;
    }
}

MH(alpha_test_enable)
{
    ch->d3d_alpha_test_enable = param;
}

MH(alpha_func)
{
    ch->d3d_alpha_func = param;
}

MH(alpha_ref)
{
    ch->d3d_alpha_ref = param;
}

MH(blend_enable)
{
    ch->d3d_blend_enable = param;
}

MH(cull_face_enable)
{
    ch->d3d_cull_face_enable = param;
}

MH(depth_test_enable)
{
    ch->d3d_depth_test_enable = param;
}

MH(lighting_enable)
{
    ch->d3d_lighting_enable = param;
}

MH(stencil_test_enable)
{
    ch->d3d_stencil_test_enable = param;
}

MH(blend_sfactor_0096)
{
    ch->d3d_blend_sfactor_rgb = (uint16_t)param;
    ch->d3d_blend_sfactor_alpha = (uint16_t)param;
}

MH(blend_dfactor_0096)
{
    ch->d3d_blend_dfactor_rgb = (uint16_t)param;
    ch->d3d_blend_dfactor_alpha = (uint16_t)param;
}

MH(blend_equation_0096)
{
    ch->d3d_blend_equation_rgb = (uint16_t)param;
    ch->d3d_blend_equation_alpha = (uint16_t)param;
}

MH(blend_sfactor_0497)
{
    ch->d3d_blend_sfactor_rgb = (uint16_t)param;
    ch->d3d_blend_sfactor_alpha = param >> 16;
}

MH(blend_dfactor_0497)
{
    ch->d3d_blend_dfactor_rgb = (uint16_t)param;
    ch->d3d_blend_dfactor_alpha = param >> 16;
}

MH(blend_equation_0497)
{
    ch->d3d_blend_equation_rgb = (uint16_t)param;
    ch->d3d_blend_equation_alpha = param >> 16;
}

MH(blend_color)
{
    ch->d3d_blend_color[0] = ((param >> 16) & 0xff) / 255.0f;
    ch->d3d_blend_color[1] = ((param >> 8) & 0xff) / 255.0f;
    ch->d3d_blend_color[2] = ((param >> 0) & 0xff) / 255.0f;
    ch->d3d_blend_color[3] = ((param >> 24) & 0xff) / 255.0f;
}

MH(depth_func)
{
    ch->d3d_depth_func = param;
}

MH(color_mask)
{
    ch->d3d_color_mask = param;
    ch->d3d_color_mask_565 = 0;
    ch->d3d_color_mask_8888 = 0;
    if (((param >> 0) & 1) != 0) {
        ch->d3d_color_mask_565 |= 0x001f;
        ch->d3d_color_mask_8888 |= 0x000000ff;
    }
    if (((param >> 8) & 1) != 0) {
        ch->d3d_color_mask_565 |= 0x07e0;
        ch->d3d_color_mask_8888 |= 0x0000ff00;
    }
    if (((param >> 16) & 1) != 0) {
        ch->d3d_color_mask_565 |= 0xf800;
        ch->d3d_color_mask_8888 |= 0x00ff0000;
    }
    if (((param >> 24) & 1) != 0) {
        ch->d3d_color_mask_8888 |= 0xff000000;
    }
}

MH(depth_write_enable)
{
    ch->d3d_depth_write_enable = param;
}

MH(stencil_mask)
{
    ch->d3d_stencil_mask = param;
}

MH(stencil_func)
{
    ch->d3d_stencil_func = param;
}

MH(stencil_func_ref)
{
    ch->d3d_stencil_func_ref = param;
}

MH(stencil_func_mask)
{
    ch->d3d_stencil_func_mask = param;
}

MH(0096_0dc)
{
    ch->d3d_stencil_op_sfail = param;
}

MH(stencil_op_dpfail)
{
    ch->d3d_stencil_op_dpfail = param;
}

MH(stencil_op_dppass)
{
    ch->d3d_stencil_op_dppass = param;
}

MH(shade_mode)
{
    ch->d3d_shade_mode = param;
}

MH(clip_min)
{
    ch->d3d_clip_min = gf_uint32_as_float(param);
}

MH(clip_max)
{
    ch->d3d_clip_max = gf_uint32_as_float(param);
}

MH(cull_face)
{
    ch->d3d_cull_face = param;
}

MH(front_face)
{
    ch->d3d_front_face = param;
}

MH(normalize_enable)
{
    ch->d3d_normalize_enable = param;
}

MH(material_factor)
{
    uint32_t i = method - 0x0ea;
    ch->d3d_material_factor[i & 3] = gf_uint32_as_float(param);
}

MH(separate_specular)
{
    ch->d3d_separate_specular = param & 1;
}

MH(light_enable_mask)
{
    ch->d3d_light_enable_mask = param;
}

MH(texgen)
{
    uint32_t method_offset = method - (cls <= 0x0097 ? 0x0f0 : 0x100);
    uint32_t tex_index = method_offset >> 2;
    uint32_t i = method_offset & 0x003;
    ch->d3d_texgen[tex_index & 7][i] = param;
}

MH(texture_matrix_enable)
{
    uint32_t i = method - (cls == 0x0096 ? 0x0f8 :
                           (cls == 0x0097 ? 0x108 : 0x090));
    ch->d3d_texture_matrix_enable[i & 0xf] = param;
}

MH(view_matrix_enable)
{
    ch->d3d_view_matrix_enable = param;
}

MH(model_view_matrix)
{
    uint32_t i = method & 0x00f;
    uint32_t m = (method >> 4) & 1;
    ch->d3d_model_view_matrix[m][i] = gf_uint32_as_float(param);
    if (s->card_type == 0x35) {
        ch->d3d_transform_constant[0x44 + (i >> 2)][i & 3] =
            gf_uint32_as_float(param);
    }
}

MH(inverse_model_view_matrix)
{
    uint32_t i = method & 0x00f;
    if (i < 12) {
        ch->d3d_inverse_model_view_matrix[i] = gf_uint32_as_float(param);
    }
    if (s->card_type == 0x35) {
        ch->d3d_transform_constant[0x48 + (i >> 2)][i & 3] =
            gf_uint32_as_float(param);
    }
}

MH(composite_matrix)
{
    uint32_t i = method & 0x00f;
    ch->d3d_composite_matrix[i] = gf_uint32_as_float(param);
    if (s->card_type == 0x35) {
        ch->d3d_transform_constant[0x3C + (i >> 2)][i & 3] =
            gf_uint32_as_float(param);
    }
}

MH(texture_matrix)
{
    uint32_t method_offset = method - (cls == 0x0096 ? 0x150 : 0x1b0);
    uint32_t tex_index = method_offset >> 4;
    uint32_t i = method_offset & 0x00f;
    ch->d3d_texture_matrix[tex_index & 7][i] = gf_uint32_as_float(param);
    if (s->card_type == 0x35) {
        uint32_t const_ofs = i >> 2;
        if (tex_index <= 3) {
            const_ofs += 0x80 + tex_index * 8;
        } else {
            const_ofs += 0x04 + (tex_index - 4) * 8;
        }
        ch->d3d_transform_constant[const_ofs & 0x1ff][i & 3] =
            gf_uint32_as_float(param);
    }
}

MH(texgen_plane)
{
    uint32_t method_offset = method - (cls == 0x0096 ? 0x180 :
                                       (cls == 0x0097 ? 0x210 : 0x380));
    uint32_t tex_index = method_offset >> 4;
    uint32_t tex_coord = (method_offset >> 2) & 3;
    uint32_t i = method_offset & 0x003;
    ch->d3d_texgen_plane[tex_index & 7][tex_coord][i] =
        gf_uint32_as_float(param);
}

MH(scissor_x_width)
{
    ch->d3d_scissor_x = param & 0x0000ffff;
    ch->d3d_scissor_width = param >> 16;
}

MH(scissor_y_height)
{
    ch->d3d_scissor_y = param & 0x0000ffff;
    ch->d3d_scissor_height = param >> 16;
}

MH(shader_program)
{
    uint32_t location;
    ch->d3d_shader_program = param;
    ch->d3d_shader_offset = ch->d3d_shader_program & ~3;
    location = ch->d3d_shader_program & 3;
    if (location == 1) {
        ch->d3d_shader_obj = ch->d3d_a_obj;
    } else if (location == 2) {
        ch->d3d_shader_obj = ch->d3d_b_obj;
    } else {
        ch->d3d_shader_obj = 0;
    }
}

MH(0497_240)
{
    uint32_t stage = (method >> 3) & 7;
    uint32_t rc_method = method & 7;
    if (rc_method == 0) {
        ch->d3d_combiner_alpha_icw[stage] = param;
    } else if (rc_method == 1) {
        ch->d3d_combiner_color_icw[stage] = param;
    } else if (rc_method == 2 || rc_method == 3) {
        uint32_t i = rc_method - 2;
        ch->d3d_combiner_const_color[stage][i][0] =
            ((param >> 16) & 0xff) / 255.0f;
        ch->d3d_combiner_const_color[stage][i][1] =
            ((param >> 8) & 0xff) / 255.0f;
        ch->d3d_combiner_const_color[stage][i][2] =
            ((param >> 0) & 0xff) / 255.0f;
        ch->d3d_combiner_const_color[stage][i][3] =
            ((param >> 24) & 0xff) / 255.0f;
    } else if (rc_method == 4) {
        ch->d3d_combiner_alpha_ocw[stage] = param;
    } else if (rc_method == 5) {
        ch->d3d_combiner_color_ocw[stage] = param;
    }
}

MH(viewport_x_width)
{
    ch->d3d_viewport_x = param & 0x0000ffff;
    ch->d3d_viewport_width = param >> 16;
}

MH(viewport_y_height)
{
    ch->d3d_viewport_y = param & 0x0000ffff;
    ch->d3d_viewport_height = param >> 16;
}

MH(specular_params)
{
    uint32_t i = method & 7;
    if (i < 6) {
        ch->d3d_specular_params[i] = gf_uint32_as_float(param);
    }
    if (i == 5) {
        /* Very rough approximation */
        if (ch->d3d_specular_params[0] > -0.2f) {
            ch->d3d_specular_power = ch->d3d_specular_params[2];
        } else {
            ch->d3d_specular_power = 1.0f /
                (1.0f + ch->d3d_specular_params[0]);
            ch->d3d_specular_power = ch->d3d_specular_power *
                (2.7f + 0.25f * log(ch->d3d_specular_power)) - 1.0f;
        }
    }
}

MH(scene_ambient_color)
{
    uint32_t i = method - (cls == 0x0096 ? 0x1b1 : 0x284);
    ch->d3d_scene_ambient_color[i & 3] = gf_uint32_as_float(param);
}

MH(viewport_offset)
{
    uint32_t i = (method - (cls == 0x0096 ? 0x1ba : 0x288)) & 3;
    ch->d3d_viewport_offset[i] = gf_uint32_as_float(param);
    if (s->card_type == 0x20) {
        ch->d3d_transform_constant[0x3b][i] = gf_uint32_as_float(param);
    } else if (s->card_type == 0x35) {
        ch->d3d_transform_constant[0x77][i] = gf_uint32_as_float(param);
    }
}

MH(eye_position)
{
    uint32_t i = (method - 0x294) & 3;
    ch->d3d_eye_position[i] = gf_uint32_as_float(param);
}

MH(0096_09c)
{
    uint32_t i = method & 1;
    uint32_t st;
    for (st = 0; st < 2; st++) {
        ch->d3d_combiner_const_color[st][i][0] =
            ((param >> 16) & 0xff) / 255.0f;
        ch->d3d_combiner_const_color[st][i][1] =
            ((param >> 8) & 0xff) / 255.0f;
        ch->d3d_combiner_const_color[st][i][2] =
            ((param >> 0) & 0xff) / 255.0f;
        ch->d3d_combiner_const_color[st][i][3] =
            ((param >> 24) & 0xff) / 255.0f;
    }
}

MH(0097_298)
{
    uint32_t method_offset = method - 0x298;
    uint32_t st = method_offset & 7;
    uint32_t i = (method_offset >> 3) & 1;
    ch->d3d_combiner_const_color[st][i][0] = ((param >> 16) & 0xff) / 255.0f;
    ch->d3d_combiner_const_color[st][i][1] = ((param >> 8) & 0xff) / 255.0f;
    ch->d3d_combiner_const_color[st][i][2] = ((param >> 0) & 0xff) / 255.0f;
    ch->d3d_combiner_const_color[st][i][3] = ((param >> 24) & 0xff) / 255.0f;
}

MH(combiner_alpha_ocw)
{
    uint32_t i = method - (cls == 0x0096 ? 0x09e : 0x2a8);
    ch->d3d_combiner_alpha_ocw[i & 7] = param;
}

MH(combiner_color_icw)
{
    uint32_t i = method - (cls == 0x0096 ? 0x09a : 0x2b0);
    ch->d3d_combiner_color_icw[i & 7] = param;
}

MH(texture_key_color)
{
    uint32_t texture_index = method - (cls == 0x0097 ? 0x2b8 : 0x740);
    ch->d3d_texture[texture_index & 0xf].key_color = param;
}

MH(viewport_scale)
{
    uint32_t i = method & 0x003;
    ch->d3d_viewport_scale[i] = gf_uint32_as_float(param);
    if (s->card_type == 0x20) {
        ch->d3d_transform_constant[0x3a][i] = gf_uint32_as_float(param);
    } else if (s->card_type == 0x35) {
        ch->d3d_transform_constant[0x76][i] = gf_uint32_as_float(param);
    }
}

MH(transform_program)
{
    uint32_t i = method & 0x003;
    if (ch->d3d_transform_program_load < 544) {
        ch->d3d_transform_program[ch->d3d_transform_program_load][i] = param;
    }
    if (i == 3) {
        ch->d3d_transform_program_load++;
    }
}

MH(transform_constant)
{
    uint32_t i = method & 0x003;
    if (ch->d3d_transform_constant_load < 512) {
        ch->d3d_transform_constant[ch->d3d_transform_constant_load][i] =
            gf_uint32_as_float(param);
    }
    if (i == 3) {
        ch->d3d_transform_constant_load++;
    }
}

MH(light)
{
    uint32_t light_index;
    uint32_t light_method;
    gf_light *light;

    if (cls <= 0x0097) {
        light_index = (method >> 5) & 7;
        light_method = method & 0x01f;
    } else {
        light_index = (method >> 4) & 7;
        light_method = (method & 0x00f) | ((method & 0x080) >> 3);
    }
    light = &ch->d3d_light[light_index];
    if (light_method <= 0x02) {
        light->ambient_color[light_method] = gf_uint32_as_float(param);
    } else if (light_method >= 0x03 && light_method <= 0x05) {
        uint32_t i = light_method - 0x03;
        light->diffuse_color[i] = gf_uint32_as_float(param);
    } else if (light_method >= 0x06 && light_method <= 0x08) {
        uint32_t i = light_method - 0x06;
        light->specular_color[i] = gf_uint32_as_float(param);
    } else if (light_method >= 0x0a && light_method <= 0x0c) {
        uint32_t i = light_method - 0x0a;
        light->inf_half_vector[i] = gf_uint32_as_float(param);
        if (s->card_type == 0x35) {
            ch->d3d_transform_constant[0x30 + light_index][i] =
                gf_uint32_as_float(param);
        }
    } else if (light_method >= 0x0d && light_method <= 0x0f) {
        uint32_t i = light_method - 0x0d;
        light->inf_direction[i] = gf_uint32_as_float(param);
    } else if (light_method >= 0x13 && light_method <= 0x16) {
        uint32_t i = light_method - 0x13;
        light->spot_direction[i] = gf_uint32_as_float(param);
    } else if (light_method >= 0x17 && light_method <= 0x19) {
        uint32_t i = light_method - 0x17;
        light->local_position[i] = gf_uint32_as_float(param);
        if (s->card_type == 0x35) {
            ch->d3d_transform_constant[0x64 + light_index][i] =
                gf_uint32_as_float(param);
        }
    } else if (light_method >= 0x1a && light_method <= 0x1c) {
        uint32_t i = light_method - 0x1a;
        light->local_attenuation[i] = gf_uint32_as_float(param);
        if (s->card_type == 0x35) {
            ch->d3d_transform_constant[0x30 + light_index][i] =
                gf_uint32_as_float(param);
        }
    }
}

MH(0096_300)
{
    uint32_t comp_index = method & 0x003;
    if (comp_index < 3) {
        ch->d3d_vertex_data_imm[0][comp_index] = gf_uint32_as_float(param);
    }
    if (comp_index == 2) {
        ch->d3d_vertex_data_imm[0][3] = 1.0f;
        gf_d3d_process_vertex(s, ch, true);
    }
}

MH(0497_540)
{
    uint32_t comp_index = method & 0x003;
    if (comp_index != 3) {
        uint32_t attrib_index = (method >> 2) & 0xf;
        ch->d3d_vertex_data_imm[attrib_index][comp_index] =
            gf_uint32_as_float(param);
        if (comp_index == 2) {
            ch->d3d_vertex_data_imm[attrib_index][3] = 1.0f;
            if (attrib_index == 0) {
                gf_d3d_process_vertex(s, ch, true);
            }
        }
    }
}

MH(0096_306)
{
    uint32_t i = (method - (cls == 0x0096 ? 0x306 : 0x546)) & 3;
    ch->d3d_vertex_data_imm[0][i] = gf_uint32_as_float(param);
    if (i == 3) {
        gf_d3d_process_vertex(s, ch, true);
    }
}

MH(0096_30c)
{
    uint32_t i = method & 0x003;
    ch->d3d_vertex_data_imm[ch->d3d_attrib_in_normal][i] =
        gf_uint32_as_float(param);
}

MH(0096_314)
{
    uint32_t i = method & 0x003;
    ch->d3d_vertex_data_imm[ch->d3d_attrib_in_color[0]][i] =
        gf_uint32_as_float(param);
}

MH(0096_318)
{
    uint32_t i = method & 0x003;
    ch->d3d_vertex_data_imm[ch->d3d_attrib_in_color[0]][i] =
        gf_uint32_as_float(param);
    if (i == 2) {
        ch->d3d_vertex_data_imm[ch->d3d_attrib_in_color[0]][3] = 1.0f;
    }
}

MH(0096_31b)
{
    gf_unpack_attribute(param, false,
                        ch->d3d_vertex_data_imm[ch->d3d_attrib_in_color[0]]);
}

MH(texcoord)
{
    uint32_t method_offset = method - (cls == 0x0096 ? 0x324 : 0x564);
    uint32_t texcoord_index = method_offset / 10;
    uint32_t texcoord_method = method_offset % 10;
    float *texcoord = ch->d3d_vertex_data_imm[
        ch->d3d_attrib_in_tex_coord[texcoord_index & 0xf]];
    /* TEXCOORD3_4F/4S may require special handling */
    if (texcoord_method <= 1) {
        if (texcoord_method == 1) {
            texcoord[2] = 0.0f;
            texcoord[3] = 1.0f;
        }
        texcoord[texcoord_method] = gf_uint32_as_float(param);
    } else if (texcoord_method == 2) {
        texcoord[0] = (int16_t)(param & 0xffff);
        texcoord[1] = (int16_t)(param >> 16);
        texcoord[2] = 0.0f;
        texcoord[3] = 1.0f;
    } else if (texcoord_method >= 4 && texcoord_method <= 7) {
        texcoord[texcoord_method - 4] = gf_uint32_as_float(param);
    }
}

MH(0097_5c8)
{
    uint32_t i = method - (cls == 0x0097 ? 0x5c8 : 0x5a0);
    ch->d3d_vertex_data_array_offset[i & 0xf] = param;
}

MH(vertex_data_base_index)
{
    ch->d3d_vertex_data_base_index = param;
}

MH(vertex_data_array_format)
{
    uint32_t i;

    if (cls == 0x0096) {
        uint32_t method_offset = method - 0x340;
        i = method_offset >> 1;
        if ((method_offset & 1) == 0) {
            ch->d3d_vertex_data_array_offset[i & 0xf] = param;
            return;
        }
    } else {
        i = method - (cls == 0x0097 ? 0x5d8 : 0x5d0);
    }
    i &= 0xf;
    ch->d3d_vertex_data_array_format_stride[i] = (param >> 8) & 0xff;
    ch->d3d_vertex_data_array_format_dx[i] = (param & 0x00010000) != 0;
    ch->d3d_vertex_data_array_format_homogeneous[i] =
        (param & 0x01000000) != 0;
    if (!ch->d3d_vertex_data_array_format_dx[i]) {
        ch->d3d_vertex_data_array_format_type[i] = param & 0xf;
        ch->d3d_vertex_data_array_format_size[i] = (param >> 4) & 0xf;
    } else {
        uint32_t dxtype = param & 0xff;
        if (dxtype == 0x44) {
            ch->d3d_vertex_data_array_format_type[i] = 4;
            ch->d3d_vertex_data_array_format_size[i] = 4;
        } else if (dxtype == 0x88) {
            ch->d3d_vertex_data_array_format_type[i] = 2;
            ch->d3d_vertex_data_array_format_size[i] = 1;
        } else if (dxtype == 0x99) {
            ch->d3d_vertex_data_array_format_type[i] = 2;
            ch->d3d_vertex_data_array_format_size[i] = 2;
        } else if (dxtype == 0xaa) {
            ch->d3d_vertex_data_array_format_type[i] = 2;
            ch->d3d_vertex_data_array_format_size[i] = 3;
        } else if (dxtype == 0xbb) {
            ch->d3d_vertex_data_array_format_type[i] = 2;
            ch->d3d_vertex_data_array_format_size[i] = 4;
        } else if (dxtype == 0xcc) {
            ch->d3d_vertex_data_array_format_type[i] = 0;
            ch->d3d_vertex_data_array_format_size[i] = 4;
        } else if (dxtype == 0xee) {
            ch->d3d_vertex_data_array_format_type[i] = 5;
            ch->d3d_vertex_data_array_format_size[i] = 2;
        }
    }
}

MH(get_report)
{
    uint32_t offset = param & 0x00ffffff;
    gf_dma_write64(s, ch->d3d_report_obj, offset + 0x0,
                   gf_get_current_time(s));
    gf_dma_write32(s, ch->d3d_report_obj, offset + 0x8, 0);
    gf_dma_write32(s, ch->d3d_report_obj, offset + 0xC, 0);
}

MH(begin_end)
{
    if (param != 0) {
        ch->d3d_primitive_done = false;
        ch->d3d_triangle_flip = false;
        ch->d3d_vertex_index = 0;
        ch->d3d_attrib_index = cls == 0x0096 ? 7 : 0;
        ch->d3d_comp_index = 0;
    }
    ch->d3d_begin_end = param;
}

MH(array_element16)
{
    gf_d3d_load_vertex(s, ch, param & 0x0000ffff);
    gf_d3d_load_vertex(s, ch, param >> 16);
}

MH(array_element32)
{
    gf_d3d_load_vertex(s, ch, param);
}

MH(draw_arrays)
{
    uint32_t vertex_first = param & 0x00ffffff;
    uint32_t vertex_last = vertex_first + (param >> 24);
    uint32_t v;
    for (v = vertex_first; v <= vertex_last; v++) {
        gf_d3d_load_vertex(s, ch, v);
    }
}

MH(inline_array)
{
    uint32_t format_type;
    bool process = false;

    if (cls == 0x0096) {
        while (ch->d3d_attrib_index < 16 &&
               ch->d3d_vertex_data_array_format_size[ch->d3d_attrib_index] ==
               0) {
            uint32_t ci;
            for (ci = 0; ci < 4; ci++) {
                ch->d3d_vertex_data[ch->d3d_vertex_index & 3]
                                   [ch->d3d_attrib_index][ci] =
                    ch->d3d_vertex_data_imm[ch->d3d_attrib_index][ci];
            }
            if (ch->d3d_attrib_index == 0) {
                break;
            }
            ch->d3d_attrib_index--;
        }
    }
    if (ch->d3d_attrib_index >= 16) {
        ch->d3d_attrib_index = 0;
    }
    if (ch->d3d_comp_index == 0) {
        ch->d3d_vertex_data[ch->d3d_vertex_index & 3]
                           [ch->d3d_attrib_index][2] = 0.0f;
        ch->d3d_vertex_data[ch->d3d_vertex_index & 3]
                           [ch->d3d_attrib_index][3] = 1.0f;
    }
    format_type = ch->d3d_vertex_data_array_format_type[ch->d3d_attrib_index];
    if ((format_type == 0 || format_type == 4) &&
        ch->d3d_vertex_data_array_format_size[ch->d3d_attrib_index] == 4) {
        gf_unpack_attribute(param, format_type == 0,
            ch->d3d_vertex_data[ch->d3d_vertex_index & 3]
                               [ch->d3d_attrib_index]);
        ch->d3d_comp_index = 4;
    } else if (format_type == 5 &&
               ch->d3d_vertex_data_array_format_size[ch->d3d_attrib_index] ==
               2) {
        ch->d3d_vertex_data[ch->d3d_vertex_index & 3]
                           [ch->d3d_attrib_index][0] = (float)(int16_t)param;
        ch->d3d_vertex_data[ch->d3d_vertex_index & 3]
                           [ch->d3d_attrib_index][1] =
            (float)(int16_t)(param >> 16);
        ch->d3d_comp_index = 2;
    } else {
        if (ch->d3d_comp_index < 4) {
            ch->d3d_vertex_data[ch->d3d_vertex_index & 3]
                               [ch->d3d_attrib_index][ch->d3d_comp_index++] =
                gf_uint32_as_float(param);
        }
    }
    while (ch->d3d_comp_index ==
           ch->d3d_vertex_data_array_format_size[ch->d3d_attrib_index]) {
        if (ch->d3d_comp_index == 0) {
            uint32_t ci;
            for (ci = 0; ci < 4; ci++) {
                ch->d3d_vertex_data[ch->d3d_vertex_index & 3]
                                   [ch->d3d_attrib_index][ci] =
                    ch->d3d_vertex_data_imm[ch->d3d_attrib_index][ci];
            }
        } else {
            ch->d3d_comp_index = 0;
        }
        if (cls == 0x0096) {
            if (ch->d3d_attrib_index == 0) {
                ch->d3d_attrib_index = 7;
                process = true;
                break;
            } else {
                ch->d3d_attrib_index--;
            }
        } else {
            if (ch->d3d_attrib_index == 15) {
                ch->d3d_attrib_index = 0;
                process = true;
                break;
            } else {
                ch->d3d_attrib_index++;
            }
        }
    }
    if (process) {
        gf_d3d_process_vertex(s, ch, false);
    }
}

MH(index_array_offset)
{
    ch->d3d_index_array_offset = param;
}

MH(index_array_dma)
{
    ch->d3d_index_array_dma = (param & 1) != 0;
    ch->d3d_index_array_type_16 = ((param >> 4) & 1) != 0;
}

MH(0497_609)
{
    uint32_t vertex_first = param & 0x00ffffff;
    uint32_t vertex_last = vertex_first + (param >> 24);
    uint32_t index_array_obj = ch->d3d_index_array_dma ?
        ch->d3d_vertex_b_obj : ch->d3d_vertex_a_obj;
    uint32_t v;
    for (v = vertex_first; v <= vertex_last; v++) {
        uint32_t vertex_array_index;
        if (ch->d3d_index_array_type_16) {
            vertex_array_index = gf_dma_read16(s, index_array_obj,
                ch->d3d_index_array_offset + v * 2);
        } else {
            vertex_array_index = gf_dma_read32(s, index_array_obj,
                ch->d3d_index_array_offset + v * 4);
        }
        gf_d3d_load_vertex(s, ch, vertex_array_index);
    }
}

MH(0097_60a)
{
    if (ch->d3d_vertex_index != 2) {
        uint32_t ai, ci;
        for (ai = 0; ai < ch->d3d_attrib_count; ai++) {
            for (ci = 0; ci < 4; ci++) {
                ch->d3d_vertex_data[ch->d3d_vertex_index & 3][ai][ci] =
                    ch->d3d_vertex_data[2 - (param & 1)][ai][ci];
            }
        }
    }
    gf_d3d_process_vertex(s, ch, false);
}

MH(0497_610)
{
    uint32_t texture_index = method & 0x00f;
    gf_texture *tex = &ch->d3d_texture[texture_index];
    if (cls == 0x0497) {
        tex->pal_dma_obj = (param & 1) == 1 ? ch->d3d_b_obj : ch->d3d_a_obj;
        tex->pal_ofs = param & 0xffffffc0;
    } else {
        tex->control3 = param;
    }
}

MH(0097_620)
{
    uint32_t comp_index = method & 1;
    uint32_t attrib_index = (method >> 1) & 0xf;
    ch->d3d_vertex_data_imm[attrib_index][comp_index] =
        gf_uint32_as_float(param);
    if (comp_index == 1) {
        ch->d3d_vertex_data_imm[attrib_index][2] = 0.0f;
        ch->d3d_vertex_data_imm[attrib_index][3] = 1.0f;
        if (attrib_index == 0) {
            gf_d3d_process_vertex(s, ch, true);
        }
    }
}

MH(0097_640)
{
    uint32_t attrib_index = method & 0xf;
    ch->d3d_vertex_data_imm[attrib_index][0] = (int16_t)(param & 0xffff);
    ch->d3d_vertex_data_imm[attrib_index][1] = (int16_t)(param >> 16);
    ch->d3d_vertex_data_imm[attrib_index][2] = 0.0f;
    ch->d3d_vertex_data_imm[attrib_index][3] = 1.0f;
    if (attrib_index == 0) {
        gf_d3d_process_vertex(s, ch, true);
    }
}

MH(0097_650)
{
    uint32_t attrib_index = method & 0xf;
    gf_unpack_attribute(param, false, ch->d3d_vertex_data_imm[attrib_index]);
    if (attrib_index == 0) {
        gf_d3d_process_vertex(s, ch, true);
    }
}

MH(0097_680)
{
    uint32_t comp_index = method & 3;
    uint32_t attrib_index = (method >> 2) & 0xf;
    ch->d3d_vertex_data_imm[attrib_index][comp_index] =
        gf_uint32_as_float(param);
    if (comp_index == 3 && attrib_index == 0) {
        gf_d3d_process_vertex(s, ch, true);
    }
}

MH(texture)
{
    uint32_t method_offset = method -
        (cls == 0x0096 ? 0x086 : (cls == 0x0097 ? 0x6c0 : 0x680));
    uint32_t texture_index;
    uint32_t texture_method;
    gf_texture *tex;

    if (cls == 0x0096) {
        texture_index = method_offset & 1;
        texture_method = method_offset >> 1;
    } else {
        texture_index = method_offset >> (cls == 0x0097 ? 4 : 3);
        texture_method = method_offset & (cls == 0x0097 ? 0xf : 7);
    }
    tex = &ch->d3d_texture[texture_index & 0xf];
    if (texture_method == 0) {
        tex->offset = param;
    } else if (texture_method == 1) {
        tex->dma_obj = (param & 3) == 1 ? ch->d3d_a_obj : ch->d3d_b_obj;
        tex->cubemap = (param & 4) != 0;
        if (cls == 0x0096) {
            tex->format = (param >> 7) & 0x1f;
            tex->levels = (param >> 12) & 0xf;
            tex->base_size[0] = (param >> 16) & 0xf;
            tex->base_size[1] = (param >> 20) & 0xf;
            tex->wrap[0] = (param >> 24) & 0xf;
            tex->wrap[1] = (param >> 28) & 0xf;
        } else {
            tex->format = (param >> 8) & 0xff;
            tex->levels = (param >> 16) & 0xf;
            tex->base_size[0] = (param >> 20) & 0xf;
            tex->base_size[1] = (param >> 24) & 0xf;
            tex->base_size[2] = (param >> 28) & 0xf;
        }
        gf_d3d_texture_process_format(tex);
        gf_texture_update_size(tex, cls);
    } else if (texture_method == 2 && cls != 0x0096) {
        tex->wrap[0] = (param >> 0) & 0xf;
        tex->wrap[1] = (param >> 8) & 0xf;
        tex->wrap[2] = (param >> 16) & 0xf;
    } else if ((texture_method == 2 && cls == 0x0096) ||
               (texture_method == 3 && cls != 0x0096)) {
        tex->control0 = param;
        if (cls == 0x4097) {
            tex->enabled = param >> 31;
        } else {
            tex->enabled = (param >> 30) & 1;
        }
        if (cls == 0x0096) {
            ch->d3d_tex_shader_op[texture_index & 3] =
                tex->enabled ? 0x01 : 0x00;
        }
    } else if ((texture_method == 3 && cls == 0x0096) ||
               (texture_method == 4 && cls != 0x0096)) {
        tex->control1 = param;
    } else if ((texture_method == 6 && cls == 0x0096) ||
               (texture_method == 5 && cls != 0x0096)) {
        /* filtering is not implemented */
        if (cls != 0x0096) {
            uint32_t signed_argb = param >> 28;
            uint32_t i;
            tex->signed_any = signed_argb != 0;
            for (i = 0; i < 4; i++) {
                tex->signed_comp[i] = (signed_argb & (1 << i)) != 0;
            }
        } else {
            tex->signed_any = false;
        }
    } else if ((texture_method == 5 && cls == 0x0096) ||
               (texture_method == 7 && cls == 0x0097) ||
               (texture_method == 6 && cls >= 0x0497)) {
        tex->image_rect = param;
        gf_texture_update_size(tex, cls);
    } else if ((texture_method == 7 && cls == 0x0096) ||
               (texture_method == 8 && cls == 0x0097)) {
        tex->pal_dma_obj = (param & 1) == 1 ? ch->d3d_b_obj : ch->d3d_a_obj;
        tex->pal_ofs = param & 0xffffffc0;
    } else if (texture_method >= 10 && texture_method <= 13 &&
               cls == 0x0097) {
        tex->offset_matrix[texture_method - 10] = gf_uint32_as_float(param);
    }
}

MH(shader_control)
{
    ch->d3d_shader_control = param;
}

MH(semaphore_offset)
{
    ch->d3d_semaphore_offset = param;
}

MH(75c)
{
    gf_dma_write32(s, ch->d3d_semaphore_obj, ch->d3d_semaphore_offset, param);
}

MH(75d)
{
    /* Semaphore release mechanism should be used instead */
    s->crtc_start = param;
}

MH(zstencil_clear_value)
{
    ch->d3d_zstencil_clear_value = param;
}

MH(color_clear_value)
{
    ch->d3d_color_clear_value = param;
}

MH(clear_surface)
{
    ch->d3d_clear_surface = param;
    gf_d3d_clear_surface_op(s, ch);
}

MH(combiner_color_ocw)
{
    uint32_t i = method - (cls == 0x0096 ? 0x0a0 : 0x790);
    ch->d3d_combiner_color_ocw[i & 7] = param;
}

MH(combiner_control)
{
    ch->d3d_combiner_control = param;
    ch->d3d_combiner_control_num_stages = param & 0xf;
}

MH(tex_shader_op)
{
    uint32_t i;
    for (i = 0; i < 4; i++) {
        ch->d3d_tex_shader_op[i] = (param >> (i * 5)) & 0x1f;
    }
}

MH(tex_shader_dotmapping)
{
    ch->d3d_tex_shader_dotmapping[1] = (param >> 0) & 0xf;
    ch->d3d_tex_shader_dotmapping[2] = (param >> 4) & 0xf;
    ch->d3d_tex_shader_dotmapping[3] = (param >> 8) & 0xf;
}

MH(tex_shader_previous)
{
    ch->d3d_tex_shader_previous[2] = (param >> 16) & 3;
    ch->d3d_tex_shader_previous[3] = (param >> 20) & 3;
}

MH(transform_execution_mode)
{
    ch->d3d_transform_execution_mode = param;
}

MH(transform_program_load)
{
    ch->d3d_transform_program_load = param;
}

MH(transform_program_start)
{
    ch->d3d_transform_program_start = param;
}

MH(transform_constant_load)
{
    ch->d3d_transform_constant_load = param;
}

MH(4097_7f1)
{
    ch->d3d_attrib_out_color[0] = param & 0xf;
    ch->d3d_attrib_out_color[1] = (param >> 4) & 0xf;
    ch->d3d_attrib_out_fogc = (param >> 24) == 6 ? 5 : 1; /* hack */
}

MH(4097_7f2)
{
    uint32_t i;
    for (i = 0; i < 8; i++) {
        ch->d3d_attrib_out_tex_coord[i] = (param >> (i * 4)) & 0xf;
    }
}

MH(4097_7f3)
{
    uint32_t i;
    for (i = 0; i < 2; i++) {
        ch->d3d_attrib_out_tex_coord[i + 8] = (param >> (i * 4)) & 0xf;
    }
}

MH(4097_7fd)
{
    uint32_t i;
    for (i = 0; i < 32; i++) {
        ch->d3d_attrib_out_enable[i] = (bool)((param >> i) & 1);
    }
    ch->d3d_fog_enable = ch->d3d_attrib_out_enable[4];
}

static void gf_empty_method_handler(GeForceState *s, gf_channel *ch,
                                    uint32_t cls, uint32_t method,
                                    uint32_t param)
{
}

static void gf_set_method_handler(GeForceState *s, uint32_t cls,
                                  uint32_t method, gf_method_handler handler)
{
    if (cls >= GEFORCE_CLASS_COUNT || method >= GEFORCE_METHOD_COUNT) {
        return;
    }
    if (s->class_method_handlers[cls][method] == gf_empty_method_handler) {
        s->class_method_handlers[cls][method] = handler;
    }
}

static void gf_set_method_handler_range(GeForceState *s, uint32_t cls,
                                        uint32_t method_start,
                                        uint32_t method_end,
                                        gf_method_handler handler)
{
    uint32_t i;

    for (i = method_start; i <= method_end; i++) {
        gf_set_method_handler(s, cls, i, handler);
    }
}

static void gf_set_d3d_method_handler_range(GeForceState *s, int cl0096,
                                            int cl0097, int cl0497,
                                            int cl4097,
                                            uint32_t method_start,
                                            uint32_t method_end,
                                            gf_method_handler handler)
{
    if (cl0096) {
        gf_set_method_handler_range(s, 0x0096, method_start, method_end,
                                    handler);
    }
    if (cl0097) {
        gf_set_method_handler_range(s, 0x0097, method_start, method_end,
                                    handler);
    }
    if (cl0497) {
        gf_set_method_handler_range(s, 0x0497, method_start, method_end,
                                    handler);
    }
    if (cl4097) {
        gf_set_method_handler_range(s, 0x4097, method_start, method_end,
                                    handler);
    }
}

static void gf_set_d3d_method_handler(GeForceState *s, int cl0096,
                                      int cl0097, int cl0497, int cl4097,
                                      uint32_t method,
                                      gf_method_handler handler)
{
    gf_set_d3d_method_handler_range(s, cl0096, cl0097, cl0497, cl4097,
                                    method, method, handler);
}

void gf_init_method_handlers(GeForceState *s)
{
    int i;
#define SET1(a, b, c, d, m, h) \
    gf_set_d3d_method_handler(s, a, b, c, d, m, gf_d3d_mh_##h)
#define SETR(a, b, c, d, m0, m1, h) \
    gf_set_d3d_method_handler_range(s, a, b, c, d, m0, m1, gf_d3d_mh_##h)

    for (i = 0; i < GEFORCE_METHOD_COUNT; i++) {
        s->empty_method_handlers[i] = gf_empty_method_handler;
        s->cl0096_method_handlers[i] = gf_empty_method_handler;
        s->cl0097_method_handlers[i] = gf_empty_method_handler;
        s->cl0497_method_handlers[i] = gf_empty_method_handler;
        s->cl4097_method_handlers[i] = gf_empty_method_handler;
    }
    for (i = 0; i < GEFORCE_CLASS_COUNT; i++) {
        s->class_method_handlers[i] = s->empty_method_handlers;
    }
    s->class_method_handlers[0x0096] = s->cl0096_method_handlers;
    s->class_method_handlers[0x0097] = s->cl0097_method_handlers;
    s->class_method_handlers[0x0497] = s->cl0497_method_handlers;
    s->class_method_handlers[0x4097] = s->cl4097_method_handlers;

    SET1(1, 1, 1, 1, 0x000, object);
    SET1(1, 1, 1, 1, 0x048, flip_read);
    SET1(1, 1, 1, 1, 0x049, flip_write);
    SET1(1, 1, 1, 1, 0x04a, flip_modulo);
    SET1(1, 1, 1, 1, 0x04b, flip_incr);
    SET1(1, 1, 1, 1, 0x04c, fifo_wait);
    SET1(1, 1, 1, 1, 0x061, a_obj);
    SET1(1, 1, 1, 1, 0x062, b_obj);
    SET1(1, 0, 0, 0, 0x063, vertex_obj);
    SET1(1, 1, 1, 1, 0x065, color_obj);
    SET1(1, 1, 1, 1, 0x066, zeta_obj);
    SET1(1, 1, 1, 1, 0x067, vertex_a_obj);
    SET1(1, 1, 1, 1, 0x068, vertex_b_obj);
    SET1(1, 1, 1, 1, 0x069, semaphore_obj);
    SET1(1, 1, 1, 1, 0x06a, report_obj);
    SET1(1, 1, 1, 1, 0x080, clip_horizontal);
    SET1(1, 1, 1, 1, 0x081, clip_vertical);
    SET1(1, 1, 1, 1, 0x082, surface_format);
    SET1(1, 1, 1, 1, 0x083, surface_pitch_a);
    SET1(1, 1, 1, 1, 0x084, surface_color_offset);
    SET1(1, 1, 1, 1, 0x085, surface_zeta_offset);
    SET1(0, 0, 0, 1, 0x08b, surface_pitch_z);
    SETR(1, 0, 0, 0, 0x098, 0x099, combiner_alpha_icw);
    SETR(0, 1, 0, 0, 0x098, 0x09f, combiner_alpha_icw);
    SETR(1, 1, 0, 0, 0x0a2, 0x0a3, combiner_final);
    SETR(0, 0, 1, 0, 0x23d, 0x23e, combiner_final);
    SET1(1, 1, 0, 0, 0x0a5, 0096_0a5);
    SET1(0, 0, 1, 0, 0x509, 0096_0a5);
    SET1(1, 1, 0, 0, 0x0a6, 0096_0a6);
    SET1(0, 0, 1, 1, 0x0e4, 0096_0a6);
    SET1(1, 1, 0, 0, 0x0a7, fog_mode);
    SET1(0, 0, 1, 1, 0x233, fog_mode);
    SET1(1, 1, 0, 0, 0x0a8, fog_gen_mode);
    SET1(0, 0, 1, 1, 0x232, fog_gen_mode);
    SETR(1, 0, 0, 0, 0x1a0, 0x1a2, fog_params);
    SETR(0, 1, 0, 0, 0x270, 0x272, fog_params);
    SETR(0, 0, 1, 1, 0x234, 0x236, fog_params);
    SET1(1, 1, 0, 0, 0x0a9, fog_enable);
    SET1(0, 0, 1, 0, 0x0db, fog_enable);
    SET1(1, 1, 0, 0, 0x0aa, fog_color);
    SET1(0, 0, 1, 0, 0x0dc, fog_color);
    SET1(0, 0, 1, 1, 0x0ae, window_offset);
    SETR(0, 0, 1, 1, 0x0b0, 0x0bf, window_clip);
    SET1(1, 1, 0, 0, 0x0c0, alpha_test_enable);
    SET1(0, 0, 1, 1, 0x0c1, alpha_test_enable);
    SET1(1, 1, 0, 0, 0x0cf, alpha_func);
    SET1(0, 0, 1, 1, 0x0c2, alpha_func);
    SET1(1, 1, 0, 0, 0x0d0, alpha_ref);
    SET1(0, 0, 1, 1, 0x0c3, alpha_ref);
    SET1(1, 1, 0, 0, 0x0c1, blend_enable);
    SET1(0, 0, 1, 1, 0x0c4, blend_enable);
    SET1(1, 1, 0, 0, 0x0c2, cull_face_enable);
    SET1(0, 0, 1, 1, 0x60f, cull_face_enable);
    SET1(1, 1, 0, 0, 0x0c3, depth_test_enable);
    SET1(0, 0, 1, 1, 0x29d, depth_test_enable);
    SET1(1, 1, 0, 0, 0x0c5, lighting_enable);
    SET1(0, 0, 1, 1, 0x516, lighting_enable);
    SET1(1, 1, 0, 0, 0x0cb, stencil_test_enable);
    SET1(0, 0, 1, 1, 0x0ca, stencil_test_enable);
    SET1(1, 1, 0, 0, 0x0d1, blend_sfactor_0096);
    SET1(1, 1, 0, 0, 0x0d2, blend_dfactor_0096);
    SET1(1, 1, 0, 0, 0x0d4, blend_equation_0096);
    SET1(0, 0, 1, 1, 0x0c5, blend_sfactor_0497);
    SET1(0, 0, 1, 1, 0x0c6, blend_dfactor_0497);
    SET1(0, 0, 1, 1, 0x0c8, blend_equation_0497);
    SET1(1, 1, 0, 0, 0x0d3, blend_color);
    SET1(0, 0, 1, 1, 0x0c7, blend_color);
    SET1(1, 1, 0, 0, 0x0d5, depth_func);
    SET1(0, 0, 1, 1, 0x29b, depth_func);
    SET1(1, 1, 0, 0, 0x0d6, color_mask);
    SET1(0, 0, 1, 1, 0x0c9, color_mask);
    SET1(1, 1, 0, 0, 0x0d7, depth_write_enable);
    SET1(0, 0, 1, 1, 0x29c, depth_write_enable);
    SET1(1, 1, 0, 0, 0x0d8, stencil_mask);
    SET1(0, 0, 1, 1, 0x0cb, stencil_mask);
    SET1(1, 1, 0, 0, 0x0d9, stencil_func);
    SET1(0, 0, 1, 1, 0x0cc, stencil_func);
    SET1(1, 1, 0, 0, 0x0da, stencil_func_ref);
    SET1(0, 0, 1, 1, 0x0cd, stencil_func_ref);
    SET1(1, 1, 0, 0, 0x0db, stencil_func_mask);
    SET1(0, 0, 1, 1, 0x0ce, stencil_func_mask);
    SET1(1, 1, 0, 0, 0x0dc, 0096_0dc);
    SET1(0, 0, 1, 1, 0x0cf, 0096_0dc);
    SET1(1, 1, 0, 0, 0x0dd, stencil_op_dpfail);
    SET1(0, 0, 1, 1, 0x0d0, stencil_op_dpfail);
    SET1(1, 1, 0, 0, 0x0de, stencil_op_dppass);
    SET1(0, 0, 1, 1, 0x0d1, stencil_op_dppass);
    SET1(1, 1, 0, 0, 0x0df, shade_mode);
    SET1(0, 0, 1, 1, 0x0da, shade_mode);
    SET1(1, 1, 1, 1, 0x0e5, clip_min);
    SET1(1, 1, 1, 1, 0x0e6, clip_max);
    SET1(1, 1, 0, 0, 0x0e7, cull_face);
    SET1(0, 0, 1, 1, 0x60c, cull_face);
    SET1(1, 1, 0, 0, 0x0e8, front_face);
    SET1(0, 0, 1, 1, 0x60d, front_face);
    SET1(1, 1, 0, 0, 0x0e9, normalize_enable);
    SET1(0, 0, 1, 1, 0x0df, normalize_enable);
    SETR(1, 1, 0, 0, 0x0ea, 0x0ed, material_factor);
    SET1(0, 0, 1, 0, 0x0ed, material_factor);
    SET1(1, 1, 0, 0, 0x0ee, separate_specular);
    SET1(0, 0, 1, 1, 0x50a, separate_specular);
    SET1(1, 1, 0, 0, 0x0ef, light_enable_mask);
    SET1(0, 0, 1, 1, 0x508, light_enable_mask);
    SETR(1, 0, 0, 0, 0x0f0, 0x0f7, texgen);
    SETR(0, 1, 0, 0, 0x0f0, 0x0ff, texgen);
    SETR(0, 0, 1, 0, 0x100, 0x11f, texgen);
    SETR(1, 0, 0, 0, 0x0f8, 0x0f9, texture_matrix_enable);
    SETR(0, 1, 0, 0, 0x108, 0x10b, texture_matrix_enable);
    SETR(0, 0, 1, 0, 0x090, 0x097, texture_matrix_enable);
    SET1(1, 0, 0, 0, 0x0fa, view_matrix_enable);
    SETR(1, 0, 0, 0, 0x100, 0x11f, model_view_matrix);
    SETR(0, 1, 1, 0, 0x120, 0x13f, model_view_matrix);
    SETR(1, 0, 0, 0, 0x120, 0x12b, inverse_model_view_matrix);
    SETR(0, 1, 1, 0, 0x160, 0x16b, inverse_model_view_matrix);
    SETR(1, 0, 0, 0, 0x140, 0x14f, composite_matrix);
    SETR(0, 1, 1, 0, 0x1a0, 0x1af, composite_matrix);
    SETR(1, 0, 0, 0, 0x150, 0x16f, texture_matrix);
    SETR(0, 1, 0, 0, 0x1b0, 0x1ef, texture_matrix);
    SETR(0, 0, 1, 0, 0x1b0, 0x22f, texture_matrix);
    SETR(1, 0, 0, 0, 0x180, 0x19f, texgen_plane);
    SETR(0, 1, 0, 0, 0x210, 0x24f, texgen_plane);
    SETR(0, 0, 1, 0, 0x380, 0x3ff, texgen_plane);
    SET1(0, 0, 1, 1, 0x230, scissor_x_width);
    SET1(0, 0, 1, 1, 0x231, scissor_y_height);
    SET1(0, 0, 1, 1, 0x239, shader_program);
    SETR(0, 0, 1, 0, 0x240, 0x27f, 0497_240);
    SET1(0, 0, 1, 1, 0x280, viewport_x_width);
    SET1(0, 0, 1, 1, 0x281, viewport_y_height);
    SETR(1, 0, 0, 0, 0x1a8, 0x1ad, specular_params);
    SETR(0, 1, 0, 0, 0x278, 0x27d, specular_params);
    SETR(0, 0, 1, 0, 0x500, 0x505, specular_params);
    SETR(1, 0, 0, 0, 0x1b1, 0x1b3, scene_ambient_color);
    SETR(0, 1, 1, 1, 0x284, 0x286, scene_ambient_color);
    SETR(1, 0, 0, 0, 0x1ba, 0x1bd, viewport_offset);
    SETR(0, 1, 1, 1, 0x288, 0x28b, viewport_offset);
    SETR(0, 1, 1, 1, 0x294, 0x297, eye_position);
    SETR(1, 0, 0, 0, 0x09c, 0x09d, 0096_09c);
    SETR(0, 1, 0, 0, 0x298, 0x2a7, 0097_298);
    SETR(1, 0, 0, 0, 0x09e, 0x09f, combiner_alpha_ocw);
    SETR(0, 1, 0, 0, 0x2a8, 0x2af, combiner_alpha_ocw);
    SETR(1, 0, 0, 0, 0x09a, 0x09b, combiner_color_icw);
    SETR(0, 1, 0, 0, 0x2b0, 0x2b7, combiner_color_icw);
    SETR(0, 1, 0, 0, 0x2b8, 0x2bb, texture_key_color);
    SETR(0, 0, 1, 1, 0x740, 0x74f, texture_key_color);
    SETR(0, 1, 0, 0, 0x2bc, 0x2bf, viewport_scale);
    SETR(0, 0, 1, 1, 0x28c, 0x28f, viewport_scale);
    SETR(0, 1, 0, 0, 0x2c0, 0x2c3, transform_program);
    SETR(0, 0, 1, 1, 0x2e0, 0x2e3, transform_program);
    SETR(0, 1, 0, 0, 0x2e0, 0x2e3, transform_constant);
    SETR(0, 0, 1, 1, 0x7c0, 0x7cf, transform_constant);
    SETR(1, 0, 0, 0, 0x200, 0x2ff, light);
    SETR(0, 1, 1, 1, 0x400, 0x4ff, light);
    SETR(1, 0, 0, 0, 0x300, 0x302, 0096_300);
    SETR(0, 1, 0, 0, 0x540, 0x542, 0096_300);
    SETR(0, 0, 1, 1, 0x540, 0x57f, 0497_540);
    SETR(1, 0, 0, 0, 0x306, 0x309, 0096_306);
    SETR(0, 1, 0, 0, 0x546, 0x549, 0096_306);
    SETR(1, 0, 0, 0, 0x30c, 0x30e, 0096_30c);
    SETR(0, 1, 0, 0, 0x54c, 0x54e, 0096_30c);
    SETR(1, 0, 0, 0, 0x314, 0x317, 0096_314);
    SETR(0, 1, 0, 0, 0x554, 0x557, 0096_314);
    SETR(1, 0, 0, 0, 0x318, 0x31a, 0096_318);
    SETR(0, 1, 0, 0, 0x558, 0x55a, 0096_318);
    SET1(1, 0, 0, 0, 0x31b, 0096_31b);
    SET1(0, 1, 0, 0, 0x55b, 0096_31b);
    SETR(1, 0, 0, 0, 0x324, 0x337, texcoord);
    SETR(0, 1, 0, 0, 0x564, 0x58b, texcoord);
    SETR(0, 1, 0, 0, 0x5c8, 0x5d7, 0097_5c8);
    SETR(0, 0, 1, 1, 0x5a0, 0x5af, 0097_5c8);
    SET1(0, 0, 0, 1, 0x5cf, vertex_data_base_index);
    SETR(1, 0, 0, 0, 0x340, 0x34f, vertex_data_array_format);
    SETR(0, 1, 0, 0, 0x5d8, 0x5e7, vertex_data_array_format);
    SETR(0, 0, 1, 1, 0x5d0, 0x5df, vertex_data_array_format);
    SET1(0, 1, 0, 0, 0x5f4, get_report);
    SET1(0, 0, 1, 1, 0x600, get_report);
    SET1(1, 0, 0, 0, 0x37f, begin_end);
    SET1(1, 0, 0, 0, 0x4ff, begin_end);
    SET1(1, 1, 0, 0, 0x5ff, begin_end);
    SET1(0, 0, 1, 1, 0x602, begin_end);
    SET1(1, 0, 0, 0, 0x380, array_element16);
    SET1(0, 1, 0, 0, 0x600, array_element16);
    SET1(0, 0, 1, 1, 0x603, array_element16);
    SET1(1, 0, 0, 0, 0x440, array_element32);
    SET1(0, 1, 0, 0, 0x602, array_element32);
    SET1(0, 0, 1, 1, 0x604, array_element32);
    SET1(1, 0, 0, 0, 0x500, draw_arrays);
    SET1(0, 1, 0, 0, 0x604, draw_arrays);
    SET1(0, 0, 1, 1, 0x605, draw_arrays);
    SETR(1, 0, 0, 0, 0x600, 0x6ff, inline_array);
    SET1(0, 1, 1, 1, 0x606, inline_array);
    SET1(0, 0, 1, 1, 0x607, index_array_offset);
    SET1(0, 0, 1, 1, 0x608, index_array_dma);
    SET1(0, 0, 1, 1, 0x609, 0497_609);
    SET1(0, 1, 0, 0, 0x60a, 0097_60a);
    SET1(0, 0, 1, 0, 0x0e7, 0097_60a);
    SETR(0, 0, 1, 1, 0x610, 0x61f, 0497_610);
    SETR(0, 1, 1, 1, 0x620, 0x63f, 0097_620);
    SETR(0, 1, 1, 1, 0x640, 0x64f, 0097_640);
    SETR(0, 1, 1, 1, 0x650, 0x65f, 0097_650);
    SETR(0, 1, 0, 0, 0x680, 0x6bf, 0097_680);
    SETR(0, 0, 1, 1, 0x700, 0x73f, 0097_680);
    SETR(1, 0, 0, 0, 0x086, 0x095, texture);
    SETR(0, 1, 0, 0, 0x6c0, 0x6ff, texture);
    SETR(0, 0, 1, 1, 0x680, 0x6ff, texture);
    SET1(1, 1, 1, 1, 0x758, shader_control);
    SET1(1, 1, 1, 1, 0x75b, semaphore_offset);
    SET1(1, 1, 1, 1, 0x75c, 75c);
    SET1(1, 1, 1, 1, 0x75d, 75d);
    SET1(1, 1, 1, 1, 0x763, zstencil_clear_value);
    SET1(1, 1, 1, 1, 0x764, color_clear_value);
    SET1(1, 1, 1, 1, 0x765, clear_surface);
    SETR(1, 0, 0, 0, 0x0a0, 0x0a1, combiner_color_ocw);
    SETR(0, 1, 0, 0, 0x790, 0x797, combiner_color_ocw);
    SET1(0, 1, 0, 0, 0x798, combiner_control);
    SET1(0, 0, 1, 0, 0x23f, combiner_control);
    SET1(0, 1, 0, 0, 0x79c, tex_shader_op);
    SET1(0, 1, 0, 0, 0x79d, tex_shader_dotmapping);
    SET1(0, 1, 0, 0, 0x79e, tex_shader_previous);
    SET1(1, 1, 1, 1, 0x7a5, transform_execution_mode);
    SET1(1, 1, 1, 1, 0x7a7, transform_program_load);
    SET1(1, 1, 1, 1, 0x7a8, transform_program_start);
    SET1(0, 1, 0, 0, 0x7a9, transform_constant_load);
    SET1(0, 0, 1, 1, 0x7bf, transform_constant_load);
    SET1(0, 0, 0, 1, 0x7f1, 4097_7f1);
    SET1(0, 0, 0, 1, 0x7f2, 4097_7f2);
    SET1(0, 0, 0, 1, 0x7f3, 4097_7f3);
    SET1(0, 0, 0, 1, 0x7fd, 4097_7fd);
#undef SET1
#undef SETR
}

void gf_execute_d3d(GeForceState *s, gf_channel *ch, uint32_t cls,
                    uint32_t method, uint32_t param)
{
    if (cls >= GEFORCE_CLASS_COUNT || method >= GEFORCE_METHOD_COUNT) {
        return;
    }
    s->class_method_handlers[cls][method](s, ch, cls, method, param);
}
