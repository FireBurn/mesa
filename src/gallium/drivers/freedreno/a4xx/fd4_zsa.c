/*
 * Copyright © 2014 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "pipe/p_state.h"
#include "util/u_memory.h"
#include "util/u_string.h"

#include "fd4_context.h"
#include "fd4_format.h"
#include "fd4_zsa.h"

void *
fd4_zsa_state_create(struct pipe_context *pctx,
                     const struct pipe_depth_stencil_alpha_state *cso)
{
   struct fd4_zsa_stateobj *so;

   so = CALLOC_STRUCT(fd4_zsa_stateobj);
   if (!so)
      return NULL;

   so->base = *cso;

   so->rb_depth_control |=
      A4XX_RB_DEPTH_CONTROL_ZFUNC(cso->depth_func); /* maps 1:1 */

   if (cso->depth_enabled)
      so->rb_depth_control |=
         A4XX_RB_DEPTH_CONTROL_Z_TEST_ENABLE | A4XX_RB_DEPTH_CONTROL_Z_READ_ENABLE;

   if (cso->depth_writemask)
      so->rb_depth_control |= A4XX_RB_DEPTH_CONTROL_Z_WRITE_ENABLE;

   if (cso->stencil[0].enabled) {
      const struct pipe_stencil_state *s = &cso->stencil[0];

      so->rb_stencil_control |=
         A4XX_RB_STENCIL_CONTROL_STENCIL_READ |
         A4XX_RB_STENCIL_CONTROL_STENCIL_ENABLE |
         A4XX_RB_STENCIL_CONTROL_FUNC(s->func) | /* maps 1:1 */
         A4XX_RB_STENCIL_CONTROL_FAIL(fd_stencil_op(s->fail_op)) |
         A4XX_RB_STENCIL_CONTROL_ZPASS(fd_stencil_op(s->zpass_op)) |
         A4XX_RB_STENCIL_CONTROL_ZFAIL(fd_stencil_op(s->zfail_op));
      so->rb_stencil_control2 |= A4XX_RB_STENCIL_CONTROL2_STENCIL_BUFFER;
      so->rb_stencilrefmask |=
         0xff000000 | /* ??? */
         A4XX_RB_STENCILREFMASK_STENCILWRITEMASK(s->writemask) |
         A4XX_RB_STENCILREFMASK_STENCILMASK(s->valuemask);

      if (cso->stencil[1].enabled) {
         const struct pipe_stencil_state *bs = &cso->stencil[1];

         so->rb_stencil_control |=
            A4XX_RB_STENCIL_CONTROL_STENCIL_ENABLE_BF |
            A4XX_RB_STENCIL_CONTROL_FUNC_BF(bs->func) | /* maps 1:1 */
            A4XX_RB_STENCIL_CONTROL_FAIL_BF(fd_stencil_op(bs->fail_op)) |
            A4XX_RB_STENCIL_CONTROL_ZPASS_BF(fd_stencil_op(bs->zpass_op)) |
            A4XX_RB_STENCIL_CONTROL_ZFAIL_BF(fd_stencil_op(bs->zfail_op));
         so->rb_stencilrefmask_bf |=
            0xff000000 | /* ??? */
            A4XX_RB_STENCILREFMASK_BF_STENCILWRITEMASK(bs->writemask) |
            A4XX_RB_STENCILREFMASK_BF_STENCILMASK(bs->valuemask);
      }
   }

   if (cso->alpha_enabled) {
      uint32_t ref = cso->alpha_ref_value * 255.0f;
      so->gras_alpha_control = A4XX_GRAS_ALPHA_CONTROL_ALPHA_TEST_ENABLE;
      so->rb_alpha_control =
         A4XX_RB_ALPHA_CONTROL_ALPHA_TEST |
         A4XX_RB_ALPHA_CONTROL_ALPHA_REF(ref) |
         A4XX_RB_ALPHA_CONTROL_ALPHA_TEST_FUNC(cso->alpha_func);
      so->rb_depth_control |= A4XX_RB_DEPTH_CONTROL_EARLY_Z_DISABLE;
   }

   return so;
}
