/*
 * Copyright © 2017 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#ifndef FREEDRENO_BLIT_H_
#define FREEDRENO_BLIT_H_

#include "pipe/p_state.h"
#include "util/u_dump.h"

#include "freedreno_context.h"

BEGINC;

bool fd_blitter_blit(struct fd_context *ctx,
                     const struct pipe_blit_info *info) assert_dt;

void fd_blitter_clear(struct pipe_context *pctx, unsigned buffers,
                      const union pipe_color_union *color, double depth,
                      unsigned stencil) assert_dt;

void fd_blitter_clear_render_target(struct pipe_context *pctx, struct pipe_surface *ps,
                                    const union pipe_color_union *color, unsigned x,
                                    unsigned y, unsigned w, unsigned h,
                                    bool render_condition_enabled) assert_dt;

void fd_blitter_clear_depth_stencil(struct pipe_context *pctx, struct pipe_surface *ps,
                                    unsigned buffers, double depth, unsigned stencil,
                                    unsigned x, unsigned y, unsigned w, unsigned h,
                                    bool render_condition_enabled) assert_dt;

void fd_resource_copy_region(struct pipe_context *pctx,
                             struct pipe_resource *dst, unsigned dst_level,
                             unsigned dstx, unsigned dsty, unsigned dstz,
                             struct pipe_resource *src, unsigned src_level,
                             const struct pipe_box *src_box) assert_dt;

bool fd_blit(struct pipe_context *pctx,
             const struct pipe_blit_info *blit_info) assert_dt;

#define DBG_BLIT(blit, batch) do { \
   if (FD_DBG(MSGS)) { \
      const struct fd_resource *src = fd_resource((blit)->src.resource); \
      const struct fd_resource *dst = fd_resource((blit)->dst.resource); \
      const char *src_target = util_str_tex_target((blit)->src.resource->target, true); \
      const char *dst_target = util_str_tex_target((blit)->dst.resource->target, true); \
      const char *src_format = util_format_short_name((blit)->src.format); \
      const char *dst_format = util_format_short_name((blit)->dst.format); \
      const char *src_tiling = fd_resource_tile_mode_desc(src, (blit)->src.level); \
      const char *dst_tiling = fd_resource_tile_mode_desc(dst, (blit)->dst.level); \
      if (batch) { \
         DBG("%p: %s %s %s (%p) -> %s %s %s (%p)", (batch), \
            src_target, src_format, src_tiling, src, \
            dst_target, dst_format, dst_tiling, dst); \
      } else { \
         DBG("%s %s %s (%p) -> %s %s %s (%p)", \
            src_target, src_format, src_tiling, src, \
            dst_target, dst_format, dst_tiling, dst); \
      } \
   } \
} while (0)

ENDC;

#endif /* FREEDRENO_BLIT_H_ */
