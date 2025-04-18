/*
 * Copyright (C) 2022 Collabora, Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pan_earlyzs.h"
#include "panfrost/util/pan_ir.h"

/*
 * Return an "early" mode. If it is known that the depth/stencil tests always
 * pass (so the shader is always executed), weak early is usually faster than
 * force early.
 */
static enum pan_earlyzs
best_early_mode(bool zs_always_passes, bool force_early)
{
   if (zs_always_passes && !force_early)
      return PAN_EARLYZS_WEAK_EARLY;
   else
      return PAN_EARLYZS_FORCE_EARLY;
}

/*
 * Analyze a fragment shader and provided API state to determine the early-ZS
 * configuration. The order of arguments must match the order of states in the
 * lookup table, synchronized with pan_earlyzs_get.
 */
static struct pan_earlyzs_state
analyze(const struct pan_shader_info *s, bool writes_zs_or_oq,
        bool alpha_to_coverage, bool zs_always_passes,
        enum pan_earlyzs_zs_tilebuf_read zs_read,
        bool should_force_early_update)
{
   /* If the shader writes depth or stencil, all depth/stencil tests must
    * be deferred until the value is known after the ZS_EMIT instruction,
    * if present. ZS_EMIT must precede ATEST, so the value is known when
    * ATEST executes, justifying the late test/update.
    * Also, if alpha_to_coverage is set that also forces a late update.
    * NOTE: it's not at all clear why alpha_to_coverage always requires
    * a late update; the late update should only really be required if
    * we're writing z or stencil, or testing for occlusion queries.
    * The docs are somewhat contradictory on this point.
    * But empirically we observe this requirement on Valhall, and doing
    * the update later is never wrong (just potentially a bit slower).
    */
   bool shader_writes_zs = (s->fs.writes_depth || s->fs.writes_stencil);
   bool late_update = shader_writes_zs || alpha_to_coverage;
   bool late_kill = shader_writes_zs;
   bool force_early_update =
      s->fs.early_fragment_tests || should_force_early_update;
   bool force_early_kill = s->fs.early_fragment_tests;

   /* Late coverage updates are required if the coverage mask depends on
    * the results of the shader. Discards are implemented as coverage mask
    * updates and must be considered. Strictly, depth/stencil writes may
    * also update the coverage mask, but these already force late updates.
    */
   bool late_coverage =
      s->fs.writes_coverage || s->fs.can_discard || alpha_to_coverage;

   /* Late coverage mask updates may affect the value written to the
    * depth/stencil buffer (if a pixel is discarded entirely). However,
    * they do not affect depth/stencil testing. So they may only matter if
    * depth or stencil is written.
    *
    * That dependency does mean late coverage mask updates require late
    * depth/stencil updates.
    *
    * Similarly, occlusion queries count samples that pass the
    * depth/stencil tests, so occlusion queries with late coverage also
    * require a late update.
    */
   late_update |= (late_coverage && writes_zs_or_oq);

   /* Side effects require late depth/stencil tests to ensure the shader
    * isn't killed before the side effects execute.
    */
   late_kill |= s->writes_global;

   /* Shader reads require late depth/stencil tests to ensure the shader
    * isn't killed before the side effects execute, unless the HW supports
    * read-only ZS optimization, in which case it can be lowered to
    * force-early. */
   bool optimize_shader_read_only_zs = false;
   if (zs_read != PAN_EARLYZS_ZS_TILEBUF_NOT_READ) {
      if (!late_update && zs_read == PAN_EARLYZS_ZS_TILEBUF_READ_OPT) {
         optimize_shader_read_only_zs = true;
         force_early_update |= true;
      } else {
         late_update |= true;
      }

      if (!late_kill && zs_read == PAN_EARLYZS_ZS_TILEBUF_READ_OPT) {
         optimize_shader_read_only_zs = true;
         force_early_kill |= true;
      }
   }

   /* Finally, the shader may override and force early fragment tests */
   late_update &= !s->fs.early_fragment_tests;
   late_kill &= !s->fs.early_fragment_tests;

   /* Collect results */
   return (struct pan_earlyzs_state){
      .update = late_update
                   ? PAN_EARLYZS_FORCE_LATE
                   : best_early_mode(zs_always_passes, force_early_update),
      .kill = late_kill ? PAN_EARLYZS_FORCE_LATE
                        : best_early_mode(zs_always_passes, force_early_kill),
      .shader_readonly_zs = optimize_shader_read_only_zs,
   };
}

/*
 * Analyze a fragment shader to determine all possible early-ZS configurations.
 * Returns a lookup table of configurations indexed by the API state.
 */
struct pan_earlyzs_lut
pan_earlyzs_analyze(const struct pan_shader_info *s, unsigned arch)
{
   /* On v11+, update operation cannot be weak early */
   bool should_force_early_update = arch >= 11;

   struct pan_earlyzs_lut lut;

   for (unsigned v0 = 0; v0 < 2; ++v0) {
      for (unsigned v1 = 0; v1 < 2; ++v1) {
         for (unsigned v2 = 0; v2 < 2; ++v2) {
            for (unsigned v3 = 0; v3 < PAN_EARLYZS_ZS_TILEBUF_MODE_COUNT;
                 ++v3) {
               enum pan_earlyzs_zs_tilebuf_read zs_read = v3;

               /* Shader read-only ZS optimization only exists on v10. */
               if (arch != 10 && v3 == PAN_EARLYZS_ZS_TILEBUF_READ_OPT)
                  zs_read = PAN_EARLYZS_ZS_TILEBUF_READ_NO_OPT;

               lut.states[v0][v1][v2][v3] =
                  analyze(s, v0, v1, v2, zs_read, should_force_early_update);
            }
         }
      }
   }

   return lut;
}
