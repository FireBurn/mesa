/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef GL_NIR_LINKER_H
#define GL_NIR_LINKER_H

#include <stdbool.h>

#include "nir_defines.h"
#include "util/glheader.h"
#include "util/set.h"
#include "main/menums.h"

#ifdef __cplusplus
extern "C" {
#endif

struct gl_constants;
struct gl_context;
struct gl_extensions;
struct gl_linked_shader;
struct gl_shader;
struct gl_shader_program;
struct gl_shader;
struct gl_program;
struct gl_transform_feedback_info;
struct xfb_decl;
struct nir_xfb_info;
struct pipe_screen;

struct gl_nir_linker_options {
   bool fill_parameters;
};

#define nir_foreach_gl_uniform_variable(var, shader) \
   nir_foreach_variable_with_modes(var, shader, nir_var_uniform | \
                                                nir_var_mem_ubo | \
                                                nir_var_mem_ssbo | \
                                                nir_var_image)

void gl_nir_opts(nir_shader *nir);

void gl_nir_detect_recursion_linked(struct gl_shader_program *prog,
                                    nir_shader *shader);

bool gl_nir_link_spirv(const struct pipe_screen *screen,
                       const struct gl_constants *consts,
                       const struct gl_extensions *exts,
                       struct gl_shader_program *prog,
                       const struct gl_nir_linker_options *options);

bool gl_nir_link_glsl(struct gl_context *ctx,
                      struct gl_shader_program *prog);

bool gl_nir_link_function_calls(struct gl_shader_program *prog,
                                struct gl_shader *main,
                                struct gl_linked_shader *linked_sh,
                                struct gl_shader **shader_list,
                                unsigned num_shaders);

bool gl_nir_link_uniforms(const struct gl_constants *consts,
                          struct gl_shader_program *prog,
                          bool fill_parameters);

bool gl_nir_link_varyings(const struct gl_constants *consts,
                          const struct gl_extensions *exts,
                          gl_api api, struct gl_shader_program *prog);

const char * gl_nir_mode_string(const nir_variable *var);

bool gl_nir_validate_intrastage_arrays(struct gl_shader_program *prog,
                                       nir_variable *var,
                                       nir_variable *existing,
                                       nir_shader *existing_shader,
                                       bool match_precision);

void gl_nir_linker_size_arrays(nir_shader *shader);

struct nir_xfb_info *
gl_to_nir_xfb_info(struct gl_transform_feedback_info *info, void *mem_ctx);

nir_variable * gl_nir_lower_xfb_varying(nir_shader *shader,
                                        const char *old_var_name,
                                        nir_variable *toplevel_var);

void gl_nir_set_uniform_initializers(const struct gl_constants *consts,
                                     struct gl_shader_program *prog);

bool nir_add_packed_var_to_resource_list(const struct gl_constants *consts,
                                         struct gl_shader_program *shProg,
                                         struct set *resource_set,
                                         nir_variable *var,
                                         unsigned stage, GLenum type);

void
init_program_resource_list(struct gl_shader_program *prog);

void nir_build_program_resource_list(const struct gl_constants *consts,
                                     struct gl_shader_program *prog,
                                     bool rebuild_resourse_list);

void gl_nir_link_assign_atomic_counter_resources(const struct gl_constants *consts,
                                                 struct gl_shader_program *prog);

void gl_nir_link_check_atomic_counter_resources(const struct gl_constants *consts,
                                                struct gl_shader_program *prog);

void gl_nir_link_assign_xfb_resources(const struct gl_constants *consts,
                                      struct gl_shader_program *prog);

void gl_nir_validate_interstage_inout_blocks(struct gl_shader_program *prog,
                                             const struct gl_linked_shader *producer,
                                             const struct gl_linked_shader *consumer);

void gl_nir_validate_interstage_uniform_blocks(struct gl_shader_program *prog,
                                               struct gl_linked_shader **stages);

bool gl_nir_link_uniform_blocks(const struct gl_constants *consts,
                                struct gl_shader_program *prog);

void gl_nir_validate_intrastage_interface_blocks(struct gl_shader_program *prog,
                                                 const struct gl_shader **shader_list,
                                                 unsigned num_shaders);

bool
gl_nir_can_add_pointsize_to_program(const struct gl_constants *consts,
                                    struct gl_program *prog);

bool
gl_nir_add_point_size(struct nir_shader *nir);

bool lower_packed_varying_needs_lowering(nir_shader *shader, nir_variable *var,
                                         bool xfb_enabled,
                                         bool disable_xfb_packing,
                                         bool disable_varying_packing);

void
gl_nir_lower_optimize_varyings(const struct gl_constants *consts,
                               struct gl_shader_program *prog, bool spirv);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GL_NIR_LINKER_H */
