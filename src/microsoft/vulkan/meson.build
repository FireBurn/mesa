# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT

dzn_entrypoints = custom_target(
  'dzn_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['dzn_entrypoints.h', 'dzn_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'dzn',
    '--beta', with_vulkan_beta.to_string()
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

libdzn_files = files(
  'dzn_cmd_buffer.c',
  'dzn_descriptor_set.c',
  'dzn_device.c',
  'dzn_dxcore.cpp',
  'dzn_image.c',
  'dzn_meta.c',
  'dzn_nir.c',
  'dzn_pipeline.c',
  'dzn_query.c',
  'dzn_sync.c',
  'dzn_util.c',
  'dzn_wsi.c',
)

dzn_deps = [
  idep_libdxil_compiler,
  idep_libspirv_to_dxil,
  idep_nir,
  idep_nir_headers,
  idep_vulkan_util,
  idep_vulkan_runtime,
  idep_vulkan_wsi,
  idep_mesautil,
  dep_dxheaders,
]

dzn_flags = [ ]
dzn_cpp_flags = [ ]

if with_platform_windows
  dzn_flags += '-DVK_USE_PLATFORM_WIN32_KHR'
  libdzn_files += files('dzn_dxgi.c')
endif

if cc.get_argument_syntax() != 'msvc'
  dzn_flags_to_try = [
    '-Werror=unused-variable',
    '-Werror=unused-but-set-variable',
    '-Werror=unused-value',
    '-Werror=format',
    '-Werror=switch',
  ]
  dzn_flags += cc.get_supported_arguments(dzn_flags_to_try)
  dzn_cpp_flags = cpp.get_supported_arguments(['-Wno-error=unused-value'])
endif

libvulkan_dzn = shared_library(
  'vulkan_dzn',
  [libdzn_files, dzn_entrypoints, sha1_h],
  vs_module_defs : vulkan_api_def,
  include_directories : [inc_include, inc_src, inc_util],
  dependencies : [dzn_deps, idep_vulkan_wsi, idep_xmlconfig],
  c_args : dzn_flags,
  cpp_args : dzn_cpp_flags,
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_bsymbolic, ld_args_gc_sections],
  name_prefix : host_machine.system() == 'windows' ? '' : [],
  install : true,
)

icd_file_name = 'libvulkan_dzn.so'
icd_lib_path = join_paths(get_option('prefix'), get_option('libdir'))
if with_platform_windows
  icd_lib_path = import('fs').relative_to(get_option('bindir'), with_vulkan_icd_dir)
  icd_file_name = 'vulkan_dzn.dll'
endif


icd_command = [
   prog_python, '@INPUT0@',
   '--api-version', '1.1', '--xml', '@INPUT1@',
   '--sizeof-pointer', sizeof_pointer,
   '--lib-path', join_paths(icd_lib_path, icd_file_name),
   '--out', '@OUTPUT@',
]

icd_dev_command = [
   prog_python, '@INPUT0@',
   '--api-version', '1.1', '--xml', '@INPUT1@',
   '--sizeof-pointer', sizeof_pointer,
   '--lib-path', join_paths(meson.current_build_dir(), icd_file_name),
   '--out', '@OUTPUT@',
]

if with_platform_windows
  icd_command += '--use-backslash'
  icd_dev_command += '--use-backslash'
endif

dzn_icd = custom_target(
  'dzn_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'dzn_icd.@0@.json'.format(host_machine.cpu()),
  command : icd_command,
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

_dev_icdname = 'dzn_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  'dzn_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : icd_dev_command,
  build_by_default : true,
)

devenv.append('VK_DRIVER_FILES', _dev_icd.full_path())
# Deprecated: replaced by VK_DRIVER_FILES above
devenv.append('VK_ICD_FILENAMES', _dev_icd.full_path())
