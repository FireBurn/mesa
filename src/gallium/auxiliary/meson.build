# Copyright © 2017 Dylan Baker
# SPDX-License-Identifier: MIT

files_libgallium = files(
  'cso_cache/cso_cache.c',
  'cso_cache/cso_cache.h',
  'cso_cache/cso_context.c',
  'cso_cache/cso_context.h',
  'cso_cache/cso_hash.c',
  'cso_cache/cso_hash.h',
  'draw/draw_cliptest_tmp.h',
  'draw/draw_context.c',
  'draw/draw_context.h',
  'draw/draw_decompose_tmp.h',
  'draw/draw_fs.c',
  'draw/draw_fs.h',
  'draw/draw_gs.c',
  'draw/draw_gs.h',
  'draw/draw_gs_tmp.h',
  'draw/draw_mesh.c',
  'draw/draw_mesh.h',
  'draw/draw_mesh_prim.c',
  'draw/draw_mesh_prim.h',
  'draw/draw_mesh_prim_tmp.h',
  'draw/draw_pipe_aaline.c',
  'draw/draw_pipe_aapoint.c',
  'draw/draw_pipe.c',
  'draw/draw_pipe.h',
  'draw/draw_pipe_clip.c',
  'draw/draw_pipe_cull.c',
  'draw/draw_pipe_flatshade.c',
  'draw/draw_pipe_offset.c',
  'draw/draw_pipe_pstipple.c',
  'draw/draw_pipe_stipple.c',
  'draw/draw_pipe_twoside.c',
  'draw/draw_pipe_unfilled.c',
  'draw/draw_pipe_user_cull.c',
  'draw/draw_pipe_util.c',
  'draw/draw_pipe_validate.c',
  'draw/draw_pipe_vbuf.c',
  'draw/draw_pipe_wide_line.c',
  'draw/draw_pipe_wide_point.c',
  'draw/draw_prim_assembler.c',
  'draw/draw_prim_assembler.h',
  'draw/draw_prim_assembler_tmp.h',
  'draw/draw_private.h',
  'draw/draw_pt.c',
  'draw/draw_pt_decompose.h',
  'draw/draw_pt_emit.c',
  'draw/draw_pt_fetch.c',
  'draw/draw_pt_fetch_shade_emit.c',
  'draw/draw_pt_fetch_shade_pipeline.c',
  'draw/draw_pt_mesh_pipeline.c',
  'draw/draw_pt.h',
  'draw/draw_pt_post_vs.c',
  'draw/draw_pt_so_emit.c',
  'draw/draw_pt_util.c',
  'draw/draw_pt_vsplit.c',
  'draw/draw_pt_vsplit_tmp.h',
  'draw/draw_so_emit_tmp.h',
  'draw/draw_split_tmp.h',
  'draw/draw_tess.c',
  'draw/draw_tess.h',
  'draw/draw_vbuf.h',
  'draw/draw_vertex.c',
  'draw/draw_vertex.h',
  'draw/draw_vertex_header.h',
  'draw/draw_vs.c',
  'draw/draw_vs_exec.c',
  'draw/draw_vs.h',
  'draw/draw_vs_variant.c',
  'driver_ddebug/dd_context.c',
  'driver_ddebug/dd_draw.c',
  'driver_ddebug/dd_pipe.h',
  'driver_ddebug/dd_public.h',
  'driver_ddebug/dd_screen.c',
  'driver_ddebug/dd_util.h',
  'driver_noop/noop_pipe.c',
  'driver_noop/noop_public.h',
  'driver_noop/noop_state.c',
  'driver_trace/tr_context.c',
  'driver_trace/tr_context.h',
  'driver_trace/tr_dump.c',
  'driver_trace/tr_dump_defines.h',
  'driver_trace/tr_dump.h',
  'driver_trace/tr_dump_state.c',
  'driver_trace/tr_dump_state.h',
  'driver_trace/tr_public.h',
  'driver_trace/tr_screen.c',
  'driver_trace/tr_screen.h',
  'driver_trace/tr_texture.c',
  'driver_trace/tr_texture.h',
  'driver_trace/tr_video.c',
  'driver_trace/tr_video.h',
  'hud/font.c',
  'hud/font.h',
  'hud/hud_context.c',
  'hud/hud_context.h',
  'hud/hud_cpu.c',
  'hud/hud_nic.c',
  'hud/hud_cpufreq.c',
  'hud/hud_diskstat.c',
  'hud/hud_sensors_temp.c',
  'hud/hud_driver_query.c',
  'hud/hud_fps.c',
  'hud/hud_private.h',
  'indices/u_indices.h',
  'indices/u_indices_priv.h',
  'indices/u_primconvert.c',
  'indices/u_primconvert.h',
  'pipebuffer/pb_buffer_fenced.c',
  'pipebuffer/pb_buffer_fenced.h',
  'pipebuffer/pb_buffer.h',
  'pipebuffer/pb_bufmgr_cache.c',
  'pipebuffer/pb_bufmgr_debug.c',
  'pipebuffer/pb_bufmgr.h',
  'pipebuffer/pb_bufmgr_mm.c',
  'pipebuffer/pb_bufmgr_slab.c',
  'pipebuffer/pb_cache.c',
  'pipebuffer/pb_cache.h',
  'pipebuffer/pb_validate.c',
  'pipebuffer/pb_validate.h',
  'postprocess/filters.h',
  'postprocess/postprocess.h',
  'postprocess/pp_celshade.c',
  'postprocess/pp_celshade.h',
  'postprocess/pp_colors.c',
  'postprocess/pp_colors.h',
  'postprocess/pp_filters.h',
  'postprocess/pp_init.c',
  'postprocess/pp_mlaa_areamap.h',
  'postprocess/pp_mlaa.c',
  'postprocess/pp_mlaa.h',
  'postprocess/pp_private.h',
  'postprocess/pp_program.c',
  'postprocess/pp_run.c',
  'rtasm/rtasm_execmem.c',
  'rtasm/rtasm_execmem.h',
  'rtasm/rtasm_x86sse.c',
  'rtasm/rtasm_x86sse.h',
  'tgsi/tgsi_aa_point.c',
  'tgsi/tgsi_aa_point.h',
  'tgsi/tgsi_build.c',
  'tgsi/tgsi_build.h',
  'tgsi/tgsi_dump.c',
  'tgsi/tgsi_dump.h',
  'tgsi/tgsi_dynamic_indexing.c',
  'tgsi/tgsi_exec.c',
  'tgsi/tgsi_exec.h',
  'tgsi/tgsi_from_mesa.c',
  'tgsi/tgsi_from_mesa.h',
  'tgsi/tgsi_info.c',
  'tgsi/tgsi_info.h',
  'tgsi/tgsi_info_opcodes.h',
  'tgsi/tgsi_iterate.c',
  'tgsi/tgsi_iterate.h',
  'tgsi/tgsi_lowering.c',
  'tgsi/tgsi_lowering.h',
  'tgsi/tgsi_opcode_tmp.h',
  'tgsi/tgsi_parse.c',
  'tgsi/tgsi_parse.h',
  'tgsi/tgsi_point_sprite.c',
  'tgsi/tgsi_point_sprite.h',
  'tgsi/tgsi_sanity.c',
  'tgsi/tgsi_sanity.h',
  'tgsi/tgsi_scan.c',
  'tgsi/tgsi_scan.h',
  'tgsi/tgsi_strings.c',
  'tgsi/tgsi_strings.h',
  'tgsi/tgsi_text.c',
  'tgsi/tgsi_text.h',
  'tgsi/tgsi_transform.c',
  'tgsi/tgsi_transform.h',
  'tgsi/tgsi_two_side.c',
  'tgsi/tgsi_two_side.h',
  'tgsi/tgsi_ureg.c',
  'tgsi/tgsi_ureg.h',
  'tgsi/tgsi_util.c',
  'tgsi/tgsi_util.h',
  'tgsi/tgsi_vpos.c',
  'translate/translate.c',
  'translate/translate.h',
  'translate/translate_cache.c',
  'translate/translate_cache.h',
  'translate/translate_generic.c',
  'translate/translate_sse.c',
  'util/u_async_debug.h',
  'util/u_async_debug.c',
  'util/u_bitcast.h',
  'util/u_bitmask.c',
  'util/u_bitmask.h',
  'util/u_blend.h',
  'util/u_blitter.c',
  'util/u_blitter.h',
  'util/u_debug_cb.h',
  'util/u_debug_describe.c',
  'util/u_debug_describe.h',
  'util/u_debug_flush.c',
  'util/u_debug_flush.h',
  'util/u_debug_image.c',
  'util/u_debug_image.h',
  'util/u_debug_refcnt.c',
  'util/u_debug_refcnt.h',
  'util/u_draw.c',
  'util/u_draw.h',
  'util/u_draw_quad.c',
  'util/u_draw_quad.h',
  'util/u_driconf.c',
  'util/u_driconf.h',
  'util/u_dual_blend.h',
  'util/u_dump_defines.c',
  'util/u_dump.h',
  'util/u_dump_state.c',
  'util/u_framebuffer.c',
  'util/u_framebuffer.h',
  'util/u_gen_mipmap.c',
  'util/u_gen_mipmap.h',
  'util/u_handle_table.c',
  'util/u_handle_table.h',
  'util/u_helpers.c',
  'util/u_helpers.h',
  'util/u_index_modify.c',
  'util/u_index_modify.h',
  'util/u_inlines.h',
  'util/u_live_shader_cache.c',
  'util/u_live_shader_cache.h',
  'util/u_log.c',
  'util/u_log.h',
  'util/u_prim.h',
  'util/u_prim.c',
  'util/u_prim_restart.c',
  'util/u_prim_restart.h',
  'util/u_pstipple.c',
  'util/u_pstipple.h',
  'util/u_pwr8.h',
  'util/u_range.h',
  'util/u_rect.h',
  'util/u_resource.c',
  'util/u_resource.h',
  'util/u_sample_positions.c',
  'util/u_sample_positions.h',
  'util/u_sampler.c',
  'util/u_sampler.h',
  'util/u_screen.c',
  'util/u_screen.h',
  'util/u_simple_shaders.c',
  'util/u_simple_shaders.h',
  'util/u_split_draw.c',
  'util/u_split_draw.h',
  'util/u_split_prim.h',
  'util/u_sse.h',
  'util/u_suballoc.c',
  'util/u_suballoc.h',
  'util/u_surface.c',
  'util/u_surface.h',
  'util/u_tests.c',
  'util/u_tests.h',
  'util/u_texture.c',
  'util/u_texture.h',
  'util/u_tile.c',
  'util/u_tile.h',
  'util/u_transfer.c',
  'util/u_transfer.h',
  'util/u_transfer_helper.c',
  'util/u_transfer_helper.h',
  'util/u_threaded_context.c',
  'util/u_threaded_context.h',
  'util/u_threaded_context_calls.h',
  'util/u_trace_gallium.c',
  'util/u_trace_gallium.h',
  'util/u_upload_mgr.c',
  'util/u_upload_mgr.h',
  'util/u_vbuf.c',
  'util/u_vbuf.h',
  'util/u_vertex_state_cache.c',
  'util/u_vertex_state_cache.h',
  'util/u_video.h',
  'util/u_viewport.h',
  'nir/tgsi_to_nir.c',
  'nir/tgsi_to_nir.h',
  'nir/nir_to_tgsi.c',
  'nir/nir_to_tgsi.h',
  'nir/nir_draw_helpers.c',
  'nir/nir_draw_helpers.h',
)

if dep_libdrm.found()
  files_libgallium += files(
    'renderonly/renderonly.c',
    'renderonly/renderonly.h',
  )
endif

if draw_with_llvm
  files_libgallium += files(
    'gallivm/lp_bld_arit.c',
    'gallivm/lp_bld_arit.h',
    'gallivm/lp_bld_arit_overflow.c',
    'gallivm/lp_bld_arit_overflow.h',
    'gallivm/lp_bld_assert.c',
    'gallivm/lp_bld_assert.h',
    'gallivm/lp_bld_bitarit.c',
    'gallivm/lp_bld_bitarit.h',
    'gallivm/lp_bld_const.c',
    'gallivm/lp_bld_const.h',
    'gallivm/lp_bld_conv.c',
    'gallivm/lp_bld_conv.h',
    'gallivm/lp_bld_coro.c',
    'gallivm/lp_bld_coro.h',
    'gallivm/lp_bld_debug.cpp',
    'gallivm/lp_bld_debug.h',
    'gallivm/lp_bld_flow.c',
    'gallivm/lp_bld_flow.h',
    'gallivm/lp_bld_format_aos_array.c',
    'gallivm/lp_bld_format_aos.c',
    'gallivm/lp_bld_format_float.c',
    'gallivm/lp_bld_format_s3tc.c',
    'gallivm/lp_bld_format.c',
    'gallivm/lp_bld_format.h',
    'gallivm/lp_bld_format_soa.c',
    'gallivm/lp_bld_format_srgb.c',
    'gallivm/lp_bld_format_yuv.c',
    'gallivm/lp_bld_gather.c',
    'gallivm/lp_bld_gather.h',
    'gallivm/lp_bld.h',
    'gallivm/lp_bld_init_common.c',
    'gallivm/lp_bld_init.h',
    'gallivm/lp_bld_intr.c',
    'gallivm/lp_bld_intr.h',
    'gallivm/lp_bld_ir_common.c',
    'gallivm/lp_bld_ir_common.h',
    'gallivm/lp_bld_jit_sample.c',
    'gallivm/lp_bld_jit_types.h',
    'gallivm/lp_bld_jit_types.c',
    'gallivm/lp_bld_limits.h',
    'gallivm/lp_bld_logic.c',
    'gallivm/lp_bld_logic.h',
    'gallivm/lp_bld_misc.cpp',
    'gallivm/lp_bld_misc.h',
    'gallivm/lp_bld_nir.h',
    'gallivm/lp_bld_nir.c',
    'gallivm/lp_bld_nir_aos.c',
    'gallivm/lp_bld_nir_soa.c',
    'gallivm/lp_bld_pack.c',
    'gallivm/lp_bld_pack.h',
    'gallivm/lp_bld_passmgr.h',
    'gallivm/lp_bld_passmgr.c',
    'gallivm/lp_bld_printf.c',
    'gallivm/lp_bld_printf.h',
    'gallivm/lp_bld_quad.c',
    'gallivm/lp_bld_quad.h',
    'gallivm/lp_bld_sample_aos.c',
    'gallivm/lp_bld_sample_aos.h',
    'gallivm/lp_bld_sample.c',
    'gallivm/lp_bld_sample.h',
    'gallivm/lp_bld_sample_soa.c',
    'gallivm/lp_bld_struct.c',
    'gallivm/lp_bld_struct.h',
    'gallivm/lp_bld_swizzle.c',
    'gallivm/lp_bld_swizzle.h',
    'gallivm/lp_bld_tgsi_action.c',
    'gallivm/lp_bld_tgsi_action.h',
    'gallivm/lp_bld_tgsi.c',
    'gallivm/lp_bld_tgsi.h',
    'gallivm/lp_bld_tgsi_info.c',
    'gallivm/lp_bld_tgsi_soa.c',
    'gallivm/lp_bld_type.c',
    'gallivm/lp_bld_type.h',
    'draw/draw_llvm.c',
    'draw/draw_llvm.h',
    'draw/draw_pt_fetch_shade_pipeline_llvm.c',
    'draw/draw_vs_llvm.c',
    'tessellator/tessellator.cpp',
    'tessellator/tessellator.hpp',
    'tessellator/p_tessellator.cpp',
    'tessellator/p_tessellator.h',
    'nir/nir_to_tgsi_info.c',
    'nir/nir_to_tgsi_info.h',
  )
  if llvm_with_orcjit
    files_libgallium += files('gallivm/lp_bld_init_orc.cpp',)
  else
    files_libgallium += files('gallivm/lp_bld_init.c',)
  endif
endif

files_libgalliumvl = files(
  'vl/vl_bicubic_filter.c',
  'vl/vl_bicubic_filter.h',
  'vl/vl_codec.c',
  'vl/vl_codec.h',
  'vl/vl_compositor.c',
  'vl/vl_compositor.h',
  'vl/vl_compositor_gfx.c',
  'vl/vl_compositor_gfx.h',
  'vl/vl_compositor_cs.c',
  'vl/vl_compositor_cs.h',
  'vl/vl_csc.c',
  'vl/vl_csc.h',
  'vl/vl_decoder.c',
  'vl/vl_decoder.h',
  'vl/vl_defines.h',
  'vl/vl_deint_filter.c',
  'vl/vl_deint_filter.h',
  'vl/vl_deint_filter_cs.c',
  'vl/vl_deint_filter_cs.h',
  'vl/vl_idct.c',
  'vl/vl_idct.h',
  'vl/vl_matrix_filter.c',
  'vl/vl_matrix_filter.h',
  'vl/vl_mc.c',
  'vl/vl_mc.h',
  'vl/vl_median_filter.c',
  'vl/vl_median_filter.h',
  'vl/vl_mpeg12_bitstream.c',
  'vl/vl_mpeg12_bitstream.h',
  'vl/vl_mpeg12_decoder.c',
  'vl/vl_mpeg12_decoder.h',
  'vl/vl_types.h',
  'vl/vl_vertex_buffers.c',
  'vl/vl_vertex_buffers.h',
  'vl/vl_video_buffer.c',
  'vl/vl_video_buffer.h',
  'vl/vl_zscan.c',
  'vl/vl_zscan.h',
)

vlwinsys_deps = []
files_libgalliumvlwinsys = files('vl/vl_winsys.h')
links_libgalliumvlwinsys = []
if host_machine.system() == 'windows'
  files_libgalliumvlwinsys += files('vl/vl_winsys_win32.c')
else
  files_libgalliumvlwinsys += files('vl/vl_winsys_drm.c')
endif
if with_platform_x11
  files_libgalliumvlwinsys += files('vl/vl_winsys_dri.c')
  links_libgalliumvlwinsys += libloader_x11
  if with_dri_platform == 'drm'
    vlwinsys_deps += [
      dep_xcb_sync, dep_xcb_present, dep_xshmfence, dep_xcb_xfixes,
      dep_xcb_dri3,
    ]
    files_libgalliumvlwinsys += files('vl/vl_winsys_dri3.c')
  endif
endif
if with_gallium_zink
  files_libgalliumvlwinsys += files('vl/vl_winsys_kopper.c')
endif

if with_glx == 'xlib'
  files_libgalliumvlwinsys += files('vl/vl_winsys_xlib_swrast.c')
elif with_gallium_drisw_kms
  files_libgalliumvlwinsys += files('vl/vl_winsys_dri_vgem.c')
endif

prog_enums2names = files('driver_trace/enums2names.py')
files_libgallium += custom_target(
  'tr_util.[ch]',
  input: ['../include/pipe/p_defines.h', '../include/pipe/p_video_enums.h', '../../util/blend.h'],
  output: ['tr_util.c', 'tr_util.h'],
  command: [prog_python, prog_enums2names, '@INPUT@', '-C', '@OUTPUT0@', '-H', '@OUTPUT1@', '-I', 'tr_util.h'],
  depend_files: prog_enums2names,
)

files_libgallium += custom_target(
  'u_tracepoints.c',
  input: 'util/u_tracepoints.py',
  output: 'u_tracepoints.c',
  command: [
    prog_python, '@INPUT@',
    '-p', join_paths(dir_source_root, 'src/util/perf/'),
    '-C', '@OUTPUT@',
  ],
  depend_files: u_trace_py,
)

files_u_tracepoints = custom_target(
  'u_tracepoints.h',
  input: 'util/u_tracepoints.py',
  output: 'u_tracepoints.h',
  command: [
    prog_python, '@INPUT@',
    '-p', join_paths(dir_source_root, 'src/util/perf/'),
    '-H', '@OUTPUT@',
  ],
  depend_files: u_trace_py,
)

files_libgallium += files_u_tracepoints

idep_u_tracepoints = declare_dependency(
  sources: files_u_tracepoints,
)

u_indices_gen_c = custom_target(
  'u_indices_gen.c',
  input : 'indices/u_indices_gen.py',
  output : 'u_indices_gen.c',
  command : [prog_python, '@INPUT@', '@OUTPUT@'],
)

u_unfilled_gen_c = custom_target(
  'u_unfilled_gen.c',
  input : 'indices/u_unfilled_gen.py',
  output : 'u_unfilled_gen.c',
  command : [prog_python, '@INPUT@', '@OUTPUT@'],
)

libgallium_extra_c_args = []
libgallium = static_library(
  'gallium',
  [files_libgallium, u_indices_gen_c, u_unfilled_gen_c],
  include_directories : [
    inc_loader, inc_gallium, inc_src, inc_include, include_directories('util')
  ],
  c_args : [c_msvc_compat_args, libgallium_extra_c_args],
  cpp_args : [cpp_msvc_compat_args],
  gnu_symbol_visibility : 'hidden',
  dependencies : [
    dep_libdrm, dep_llvm, dep_dl, dep_m, dep_thread, dep_lmsensors, dep_ws2_32,
    idep_nir, idep_nir_headers, idep_mesautil,
  ],
  build_by_default : false
)

if with_tests
  test('gallium-aux',
    executable(
      'gallium-aux',
      'util/u_surface_test.cpp',
      include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux],
      link_with: libgallium,
      dependencies : [idep_gtest, idep_mesautil],
    ),
    suite: 'gallium',
    protocol : 'gtest',
  )
endif

_libgalliumvl_stub = static_library(
  'galliumvl_stub',
  'vl/vl_stubs.c',
  c_args : [c_msvc_compat_args],
  cpp_args : [cpp_msvc_compat_args],
  gnu_symbol_visibility : 'hidden',
  include_directories: [inc_gallium, inc_include, inc_src],
  dependencies : idep_mesautil,
  build_by_default : false,
)

libgalliumvl = static_library(
  'galliumvl',
  files_libgalliumvl,
  c_args : [c_msvc_compat_args],
  cpp_args : [cpp_msvc_compat_args],
  gnu_symbol_visibility : 'hidden',
  include_directories : [inc_gallium, inc_include, inc_src],
  dependencies : [idep_nir, idep_mesautil],
  build_by_default : false,
)

# some drivers export their screen creation function globally, so all frontends have to contain the
# full libgalliumvl. So we'll handle this here globally for everybody.
if (with_gallium_mediafoundation or with_gallium_va or with_gallium_vdpau or with_dri or with_gallium_radeonsi)
  libgalliumvl_stub = libgalliumvl
else
  libgalliumvl_stub = _libgalliumvl_stub
endif

# XXX: The dependencies here may be off...
libgalliumvlwinsys = static_library(
  'galliumvlwinsys',
  files_libgalliumvlwinsys,
  include_directories : [inc_gallium, inc_include, inc_loader, inc_src, include_directories('../drivers/zink')],
  dependencies : [dep_libdrm, vlwinsys_deps, idep_mesautil],
  link_with : links_libgalliumvlwinsys,
  build_by_default : false,
)
