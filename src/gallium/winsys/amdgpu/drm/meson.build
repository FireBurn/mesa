# Copyright © 2017 Dylan Baker

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

libamdgpuwinsys = static_library(
  'amdgpuwinsys',
  files(
    'amdgpu_bo.c', 'amdgpu_bo.h', 'amdgpu_cs.cpp', 'amdgpu_cs.h',
    'amdgpu_surface.c', 'amdgpu_winsys.c',
    'amdgpu_winsys.h',
  ),
  include_directories : [
    inc_amd, inc_gallium, inc_gallium_aux, inc_include, inc_src,
    inc_amd_common, inc_amd_common_llvm,
  ],
  gnu_symbol_visibility : 'hidden',
  link_with : libamdgpu_addrlib,
  dependencies : [dep_llvm, dep_libdrm_amdgpu, idep_amdgfxregs_h, idep_mesautil],
)
