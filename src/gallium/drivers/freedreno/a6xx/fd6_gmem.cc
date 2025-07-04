/*
 * Copyright © 2016 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2018 Google, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#define FD_BO_NO_HARDPIN 1

#include <stdio.h>

#include "pipe/p_state.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_string.h"

#include "freedreno_draw.h"
#include "freedreno_resource.h"
#include "freedreno_state.h"
#include "freedreno_tracepoints.h"

#include "fd6_barrier.h"
#include "fd6_blitter.h"
#include "fd6_context.h"
#include "fd6_draw.h"
#include "fd6_emit.h"
#include "fd6_gmem.h"
#include "fd6_pack.h"
#include "fd6_program.h"
#include "fd6_resource.h"
#include "fd6_zsa.h"

/**
 * Emits the flags registers, suitable for RB_COLOR_FLAG_BUFFER,
 * RB_DEPTH_FLAG_BUFFER, TPL1_A2D_SRC_TEXTURE_FLAG_BASE, and RB_RESOLVE_SYSTEM_FLAG_BUFFER_BASE.
 */
void
fd6_emit_flag_reference(struct fd_ringbuffer *ring, struct fd_resource *rsc,
                        int level, int layer)
{
   if (fd_resource_ubwc_enabled(rsc, level)) {
      OUT_RELOC(ring, rsc->bo, fd_resource_ubwc_offset(rsc, level, layer), 0,
                0);
      OUT_RING(ring, A6XX_RB_COLOR_FLAG_BUFFER_PITCH_PITCH(
                        fdl_ubwc_pitch(&rsc->layout, level)) |
                        A6XX_RB_COLOR_FLAG_BUFFER_PITCH_ARRAY_PITCH(
                           rsc->layout.ubwc_layer_size >> 2));
   } else {
      OUT_RING(ring, 0x00000000); /* RB_COLOR_FLAG_BUFFER[i].ADDR_LO */
      OUT_RING(ring, 0x00000000); /* RB_COLOR_FLAG_BUFFER[i].ADDR_HI */
      OUT_RING(ring, 0x00000000);
   }
}

template <chip CHIP>
static void
emit_mrt(struct fd_ringbuffer *ring, struct pipe_framebuffer_state *pfb,
         const struct fd_gmem_stateobj *gmem)
{
   unsigned srgb_cntl = 0;
   unsigned i;

   /* Note, GLES 3.2 says "If the fragment’s layer number is negative, or
    * greater than or equal to the minimum number of layers of any attachment,
    * the effects of the fragment on the framebuffer contents are undefined."
    */
   unsigned max_layer_index = 0;
   enum a6xx_format mrt0_format = FMT6_NONE;

   for (i = 0; i < pfb->nr_cbufs; i++) {
      enum a3xx_color_swap swap = WZYX;
      bool sint = false, uint = false;
      struct fd_resource *rsc = NULL;
      ASSERTED struct fdl_slice *slice = NULL;
      uint32_t stride = 0;
      uint32_t array_stride = 0;
      uint32_t offset;

      if (!pfb->cbufs[i].texture)
         continue;

      struct pipe_surface *psurf = &pfb->cbufs[i];
      enum pipe_format pformat = psurf->format;
      rsc = fd_resource(psurf->texture);

      uint32_t base = gmem ? gmem->cbuf_base[i] : 0;
      slice = fd_resource_slice(rsc, psurf->level);
      enum a6xx_tile_mode tile_mode = (enum a6xx_tile_mode)
            fd_resource_tile_mode(psurf->texture, psurf->level);
      enum a6xx_format format = fd6_color_format(pformat, tile_mode);
      sint = util_format_is_pure_sint(pformat);
      uint = util_format_is_pure_uint(pformat);

      if (util_format_is_srgb(pformat))
         srgb_cntl |= (1 << i);

      offset =
         fd_resource_offset(rsc, psurf->level, psurf->first_layer);

      stride = fd_resource_pitch(rsc, psurf->level);
      array_stride = fd_resource_layer_stride(rsc, psurf->level);
      swap = fd6_color_swap(pformat, (enum a6xx_tile_mode)rsc->layout.tile_mode, false);

      max_layer_index = psurf->last_layer - psurf->first_layer;

      assert((offset + slice->size0) <= fd_bo_size(rsc->bo));

      /* Batch with no draws? */
      fd_ringbuffer_attach_bo(ring, rsc->bo);

      OUT_REG(ring,
         RB_MRT_BUF_INFO(CHIP, i,
            .color_format = format,
            .color_tile_mode = tile_mode,
            .color_swap = swap,
            .losslesscompen = fd_resource_ubwc_enabled(rsc, psurf->level),
         ),
         A6XX_RB_MRT_PITCH(i, stride),
         A6XX_RB_MRT_ARRAY_PITCH(i, array_stride),
         A6XX_RB_MRT_BASE(i, .bo = rsc->bo, .bo_offset = offset),
         A6XX_RB_MRT_BASE_GMEM(i, base));

      OUT_REG(ring, A6XX_SP_PS_MRT_REG(i, .color_format = format,
                                       .color_sint = sint, .color_uint = uint));

      OUT_PKT4(ring, REG_A6XX_RB_COLOR_FLAG_BUFFER(i), 3);
      fd6_emit_flag_reference(ring, rsc, psurf->level,
                              psurf->first_layer);

      if (i == 0)
         mrt0_format = format;
   }
   if (pfb->zsbuf.texture)
      max_layer_index = pfb->zsbuf.last_layer - pfb->zsbuf.first_layer;

   OUT_REG(ring, A6XX_GRAS_LRZ_MRT_BUFFER_INFO_0(.color_format = mrt0_format));

   OUT_REG(ring, A6XX_RB_SRGB_CNTL(.dword = srgb_cntl));
   OUT_REG(ring, A6XX_SP_SRGB_CNTL(.dword = srgb_cntl));

   OUT_REG(ring, A6XX_GRAS_CL_ARRAY_SIZE(max_layer_index));
}

template <chip CHIP>
static void
emit_zs(struct fd_context *ctx, struct fd_ringbuffer *ring,
        struct pipe_surface *zsbuf, const struct fd_gmem_stateobj *gmem)
{
   if (zsbuf->texture) {
      struct fd_resource *rsc = fd_resource(zsbuf->texture);
      struct fd_resource *stencil = rsc->stencil;
      uint32_t stride = fd_resource_pitch(rsc, zsbuf->level);
      uint32_t array_stride = fd_resource_layer_stride(rsc, zsbuf->level);
      uint32_t base = gmem ? gmem->zsbuf_base[0] : 0;
      uint32_t offset =
         fd_resource_offset(rsc, zsbuf->level, zsbuf->first_layer);

      /* We could have a depth buffer, but no draws with depth write/test
       * enabled, in which case it wouldn't have been part of the batch
       * resource tracking
       */
      fd_ringbuffer_attach_bo(ring, rsc->bo);

      if (zsbuf->format == PIPE_FORMAT_S8_UINT) {
         /* S8 is implemented as Z32_S8 minus the Z32 plane: */
         enum a6xx_depth_format fmt = DEPTH6_32;

         OUT_REG(ring,
            RB_DEPTH_BUFFER_INFO(CHIP,
               .depth_format = fmt,
               .tilemode = TILE6_3,
               .losslesscompen = fd_resource_ubwc_enabled(rsc, zsbuf->level),
            ),
            A6XX_RB_DEPTH_BUFFER_PITCH(0),
            A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH(0),
            A6XX_RB_DEPTH_BUFFER_BASE(.qword = 0),
            A6XX_RB_DEPTH_GMEM_BASE(base));

         OUT_REG(ring, A6XX_GRAS_SU_DEPTH_BUFFER_INFO(.depth_format = fmt));

         stencil = rsc;
      } else {
         enum a6xx_depth_format fmt = fd6_pipe2depth(zsbuf->format);

         OUT_REG(ring,
            RB_DEPTH_BUFFER_INFO(CHIP,
               .depth_format = fmt,
               .tilemode = TILE6_3,
               .losslesscompen = fd_resource_ubwc_enabled(rsc, zsbuf->level),
            ),
            A6XX_RB_DEPTH_BUFFER_PITCH(stride),
            A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH(array_stride),
            A6XX_RB_DEPTH_BUFFER_BASE(.bo = rsc->bo, .bo_offset = offset),
            A6XX_RB_DEPTH_GMEM_BASE(base));

         OUT_REG(ring, A6XX_GRAS_SU_DEPTH_BUFFER_INFO(.depth_format = fmt));

         OUT_PKT4(ring, REG_A6XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
         fd6_emit_flag_reference(ring, rsc, zsbuf->level,
                                 zsbuf->first_layer);
      }

      if (stencil) {
         stride = fd_resource_pitch(stencil, zsbuf->level);
         array_stride = fd_resource_layer_stride(stencil, zsbuf->level);
         uint32_t base = gmem ? gmem->zsbuf_base[1] : 0;
         uint32_t offset =
            fd_resource_offset(stencil, zsbuf->level, zsbuf->first_layer);

         fd_ringbuffer_attach_bo(ring, stencil->bo);

         OUT_REG(ring,
            RB_STENCIL_BUFFER_INFO(
               CHIP,
               .separate_stencil = true,
               .tilemode = TILE6_3,
            ),
            A6XX_RB_STENCIL_BUFFER_PITCH(stride),
            A6XX_RB_STENCIL_BUFFER_ARRAY_PITCH(array_stride),
            A6XX_RB_STENCIL_BUFFER_BASE(.bo = stencil->bo, .bo_offset = offset),
            A6XX_RB_STENCIL_GMEM_BASE(base)
         );
      } else {
         OUT_REG(ring, RB_STENCIL_BUFFER_INFO(CHIP, 0));
      }
   } else {
      OUT_REG(ring,
              RB_DEPTH_BUFFER_INFO(
                    CHIP,
                    .depth_format = DEPTH6_NONE,
              ),
              A6XX_RB_DEPTH_BUFFER_PITCH(),
              A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH(),
              A6XX_RB_DEPTH_BUFFER_BASE(),
              A6XX_RB_DEPTH_GMEM_BASE(),
      );

      OUT_REG(ring,
              A6XX_GRAS_SU_DEPTH_BUFFER_INFO(.depth_format = DEPTH6_NONE));

      OUT_REG(ring, RB_STENCIL_BUFFER_INFO(CHIP, 0));
   }
}

template <chip CHIP>
static void
emit_lrz(struct fd_batch *batch, struct fd_batch_subpass *subpass)
{
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   struct fd_ringbuffer *ring = batch->gmem;

   if (!subpass->lrz) {
      OUT_REG(ring, A6XX_GRAS_LRZ_BUFFER_BASE(),
              A6XX_GRAS_LRZ_BUFFER_PITCH(),
              A6XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE());
      if (CHIP >= A7XX)
         OUT_REG(ring, A7XX_GRAS_LRZ_DEPTH_BUFFER_INFO());
      return;
   }

   /* When swapping LRZ buffers we need to flush LRZ cache..
    * we possibly don't need this during the binning pass, it
    * appears that the corruption happens on the read-side, ie.
    * we change the LRZ buffer after a sub-pass, but get a
    * cache-hit on stale data from the previous LRZ buffer.
    */
   fd6_event_write<CHIP>(batch->ctx, ring, FD_LRZ_FLUSH);

   struct fd_resource *zsbuf = fd_resource(pfb->zsbuf.texture);
   OUT_REG(ring,
      A6XX_GRAS_LRZ_BUFFER_BASE(.bo = subpass->lrz),
      A6XX_GRAS_LRZ_BUFFER_PITCH(.pitch = zsbuf->lrz_layout.lrz_pitch),
      A6XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE(
         .bo = zsbuf->lrz_layout.lrz_fc_size ? subpass->lrz : NULL,
         .bo_offset = zsbuf->lrz_layout.lrz_fc_offset
      ),
   );
   fd_ringbuffer_attach_bo(ring, subpass->lrz);

   if (CHIP >= A7XX) {
      OUT_REG(ring,
         A7XX_GRAS_LRZ_DEPTH_BUFFER_INFO(
            .depth_format = fd6_pipe2depth(pfb->zsbuf.format),
         )
      );
   }
}

/* Emit any needed lrz clears to the prologue cmds
 */
template <chip CHIP>
static void
emit_lrz_clears(struct fd_batch *batch)
{
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   struct fd_context *ctx = batch->ctx;
   unsigned count = 0;

   if (!pfb->zsbuf.texture)
      return;

   struct fd_resource *zsbuf = fd_resource(pfb->zsbuf.texture);

   foreach_subpass (subpass, batch) {
      /* The lrz buffer isn't explicitly tracked by the batch resource
       * tracking (tracking the zsbuf is sufficient), but it still needs
       * to be attached to the ring
       */
      if (subpass->lrz)
         fd_ringbuffer_attach_bo(batch->gmem, subpass->lrz);

      if (!(subpass->fast_cleared & FD_BUFFER_LRZ))
         continue;

      subpass->fast_cleared &= ~FD_BUFFER_LRZ;

      /* prep before first clear: */
      if (count == 0) {
         struct fd_ringbuffer *ring = fd_batch_get_prologue(batch);

         fd6_emit_ccu_cntl<CHIP>(ring, ctx->screen, false);

         OUT_PKT7(ring, CP_SET_MARKER, 1);
         OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_BLIT2DSCALE));

         fd6_emit_flushes<CHIP>(ctx, ring, FD6_FLUSH_CACHE);

         if (ctx->screen->info->a6xx.magic.RB_DBG_ECO_CNTL_blit !=
             ctx->screen->info->a6xx.magic.RB_DBG_ECO_CNTL) {
            /* This a non-context register, so we have to WFI before changing. */
            OUT_WFI5(ring);
            OUT_PKT4(ring, REG_A6XX_RB_DBG_ECO_CNTL, 1);
            OUT_RING(ring, ctx->screen->info->a6xx.magic.RB_DBG_ECO_CNTL_blit);
         }
      }

      fd6_clear_lrz<CHIP>(batch, zsbuf, subpass->lrz, subpass->clear_depth);

      count++;
   }

   /* cleanup after last clear: */
   if (count > 0) {
      struct fd_ringbuffer *ring = fd_batch_get_prologue(batch);

      if (ctx->screen->info->a6xx.magic.RB_DBG_ECO_CNTL_blit !=
          ctx->screen->info->a6xx.magic.RB_DBG_ECO_CNTL) {
         OUT_WFI5(ring);
         OUT_PKT4(ring, REG_A6XX_RB_DBG_ECO_CNTL, 1);
         OUT_RING(ring, ctx->screen->info->a6xx.magic.RB_DBG_ECO_CNTL);
      }

      /* Clearing writes via CCU color in the PS stage, and LRZ is read via
       * UCHE in the earlier GRAS stage.
       *
       * Note tu also asks for WFI but maybe that is only needed if
       * has_ccu_flush_bug (and it is added by fd6_emit_flushes() already
       * in that case)
       */
      fd6_emit_flushes<CHIP>(batch->ctx, ring,
                             FD6_FLUSH_CCU_COLOR |
                             FD6_INVALIDATE_CACHE);
   }
}

static bool
use_hw_binning(struct fd_batch *batch)
{
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;

   if ((gmem->maxpw * gmem->maxph) > 32)
      return false;

   return fd_binning_enabled && ((gmem->nbins_x * gmem->nbins_y) >= 2) &&
          (batch->num_draws > 0);
}

static void
patch_fb_read_gmem(struct fd_batch *batch)
{
   struct fd_screen *screen = batch->ctx->screen;
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;

   unsigned num_patches = fd_patch_num_elements(&batch->fb_read_patches);
   if (!num_patches)
      return;

   for (unsigned i = 0; i < num_patches; i++) {
     struct fd_cs_patch *patch =
        fd_patch_element(&batch->fb_read_patches, i);
      int buf = patch->val;
      struct pipe_surface *psurf = &pfb->cbufs[buf];
      struct pipe_resource *prsc = psurf->texture;
      struct fd_resource *rsc = fd_resource(prsc);
      enum pipe_format format = psurf->format;

      uint8_t swiz[4];
      fdl6_format_swiz(psurf->format, false, swiz);

      uint64_t base = screen->gmem_base + gmem->cbuf_base[buf];
      /* always TILE6_2 mode in GMEM, which also means no swap: */
      uint32_t descriptor[FDL6_TEX_CONST_DWORDS] = {
            A6XX_TEX_CONST_0_FMT(fd6_texture_format(
                  format, (enum a6xx_tile_mode)rsc->layout.tile_mode, false)) |
            A6XX_TEX_CONST_0_SAMPLES(fd_msaa_samples(prsc->nr_samples)) |
            A6XX_TEX_CONST_0_SWAP(WZYX) |
            A6XX_TEX_CONST_0_TILE_MODE(TILE6_2) |
            COND(util_format_is_srgb(format), A6XX_TEX_CONST_0_SRGB) |
            A6XX_TEX_CONST_0_SWIZ_X(fdl6_swiz(swiz[0])) |
            A6XX_TEX_CONST_0_SWIZ_Y(fdl6_swiz(swiz[1])) |
            A6XX_TEX_CONST_0_SWIZ_Z(fdl6_swiz(swiz[2])) |
            A6XX_TEX_CONST_0_SWIZ_W(fdl6_swiz(swiz[3])),

         A6XX_TEX_CONST_1_WIDTH(pfb->width) |
            A6XX_TEX_CONST_1_HEIGHT(pfb->height),

         A6XX_TEX_CONST_2_PITCH(gmem->bin_w * gmem->cbuf_cpp[buf]) |
            A6XX_TEX_CONST_2_TYPE(A6XX_TEX_2D),

         A6XX_TEX_CONST_3_ARRAY_PITCH(rsc->layout.layer_size),
         A6XX_TEX_CONST_4_BASE_LO(base),

         A6XX_TEX_CONST_5_BASE_HI(base >> 32) |
            A6XX_TEX_CONST_5_DEPTH(prsc->array_size)
      };

      memcpy(patch->cs, descriptor, FDL6_TEX_CONST_DWORDS * 4);
   }

   util_dynarray_clear(&batch->fb_read_patches);
}

template <chip CHIP>
static void
patch_fb_read_sysmem(struct fd_batch *batch)
{
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;

   unsigned num_patches =
      fd_patch_num_elements(&batch->fb_read_patches);
   if (!num_patches)
      return;
   for (unsigned i = 0; i < num_patches; i++) {
     struct fd_cs_patch *patch =
        fd_patch_element(&batch->fb_read_patches, i);
      int buf = patch->val;

      struct pipe_surface *psurf = &pfb->cbufs[buf];
      if (!psurf->texture)
         return;

      struct pipe_resource *prsc = psurf->texture;
      struct fd_resource *rsc = fd_resource(prsc);

      struct fdl_view_args args = {
         .chip = CHIP,

         .iova = fd_bo_get_iova(rsc->bo),

         .base_miplevel = psurf->level,
         .level_count = 1,

         .base_array_layer = psurf->first_layer,
         .layer_count = psurf->last_layer - psurf->first_layer + 1,

         .swiz = {PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z,
                  PIPE_SWIZZLE_W},
         .format = psurf->format,

         .type = FDL_VIEW_TYPE_2D,
         .chroma_offsets = {FDL_CHROMA_LOCATION_COSITED_EVEN,
                            FDL_CHROMA_LOCATION_COSITED_EVEN},
      };
      const struct fdl_layout *layouts[3] = {&rsc->layout, NULL, NULL};
      struct fdl6_view view;
      fdl6_view_init(&view, layouts, &args,
                     batch->ctx->screen->info->a6xx.has_z24uint_s8uint);
      memcpy(patch->cs, view.descriptor, FDL6_TEX_CONST_DWORDS * 4);
   }

   util_dynarray_clear(&batch->fb_read_patches);
}

template <chip CHIP>
static void
update_render_cntl(struct fd_batch *batch, struct pipe_framebuffer_state *pfb,
                   bool binning)
{
   struct fd_ringbuffer *ring = batch->gmem;

   if (CHIP >= A7XX) {
      OUT_REG(ring,
         RB_RENDER_CNTL(
            CHIP,
            .fs_disable = binning,
            .raster_mode = TYPE_TILED,
            .raster_direction = LR_TB
         )
      );
      OUT_REG(ring,
         A7XX_GRAS_SU_RENDER_CNTL(
            .fs_disable = binning,
         )
      );
      return;
   }

   struct fd_screen *screen = batch->ctx->screen;
   bool depth_ubwc_enable = false;
   uint32_t mrts_ubwc_enable = 0;
   int i;

   if (pfb->zsbuf.texture) {
      struct fd_resource *rsc = fd_resource(pfb->zsbuf.texture);
      depth_ubwc_enable =
         fd_resource_ubwc_enabled(rsc, pfb->zsbuf.level);
   }

   for (i = 0; i < pfb->nr_cbufs; i++) {
      if (!pfb->cbufs[i].texture)
         continue;

      struct pipe_surface *psurf = &pfb->cbufs[i];
      struct fd_resource *rsc = fd_resource(psurf->texture);

      if (fd_resource_ubwc_enabled(rsc, psurf->level))
         mrts_ubwc_enable |= 1 << i;
   }

   struct fd_reg_pair rb_render_cntl = RB_RENDER_CNTL(
         CHIP,
         .ccusinglecachelinesize = 2,
         .fs_disable = binning,
         .flag_depth = depth_ubwc_enable,
         .flag_mrts = mrts_ubwc_enable,
   );

   if (screen->info->a6xx.has_cp_reg_write) {
      OUT_PKT(ring, CP_REG_WRITE,
              CP_REG_WRITE_0(TRACK_RENDER_CNTL),
              CP_REG_WRITE_1(rb_render_cntl.reg),
              CP_REG_WRITE_2(rb_render_cntl.value),
      );
   } else {
      OUT_REG(ring, rb_render_cntl);
   }
}

static void
update_vsc_pipe(struct fd_batch *batch)
{
   struct fd_context *ctx = batch->ctx;
   struct fd6_context *fd6_ctx = fd6_context(ctx);
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct fd_ringbuffer *ring = batch->gmem;
   unsigned max_vsc_pipes = batch->ctx->screen->info->num_vsc_pipes;
   int i;

   if (batch->draw_strm_bits / 8 > fd6_ctx->vsc_draw_strm_pitch) {
      if (fd6_ctx->vsc_draw_strm)
         fd_bo_del(fd6_ctx->vsc_draw_strm);
      fd6_ctx->vsc_draw_strm = NULL;
      /* Note: probably only need to align to 0x40, but aligning stronger
       * reduces the odds that we will have to realloc again on the next
       * frame:
       */
      fd6_ctx->vsc_draw_strm_pitch = align(batch->draw_strm_bits / 8, 0x4000);
      mesa_logd("pre-resize  to: 0x%x",
                fd6_ctx->vsc_draw_strm_pitch);
   }

   if (batch->prim_strm_bits / 8 > fd6_ctx->vsc_prim_strm_pitch) {
      if (fd6_ctx->vsc_prim_strm)
         fd_bo_del(fd6_ctx->vsc_prim_strm);
      fd6_ctx->vsc_prim_strm = NULL;
      fd6_ctx->vsc_prim_strm_pitch = align(batch->prim_strm_bits / 8, 0x4000);
      mesa_logd("pre-resize VSC_PRIM_STRM_PITCH to: 0x%x",
                fd6_ctx->vsc_prim_strm_pitch);
   }

   if (!fd6_ctx->vsc_draw_strm) {
      /* We also use four bytes per vsc pipe at the end of the draw
       * stream buffer for VSC_PIPE_DATA_DRAW_SIZE written back by hw
       * (see VSC_SIZE_BASE)
       */
      unsigned sz = (max_vsc_pipes * fd6_ctx->vsc_draw_strm_pitch) +
                    (max_vsc_pipes * 4);
      fd6_ctx->vsc_draw_strm =
         fd_bo_new(ctx->screen->dev, sz, FD_BO_NOMAP, "vsc_draw_strm");
   }

   if (!fd6_ctx->vsc_prim_strm) {
      unsigned sz = max_vsc_pipes * fd6_ctx->vsc_prim_strm_pitch;
      fd6_ctx->vsc_prim_strm =
         fd_bo_new(ctx->screen->dev, sz, FD_BO_NOMAP, "vsc_prim_strm");
   }

   fd_ringbuffer_attach_bo(ring, fd6_ctx->vsc_draw_strm);
   fd_ringbuffer_attach_bo(ring, fd6_ctx->vsc_prim_strm);

   OUT_REG(ring, A6XX_VSC_BIN_SIZE(.width = gmem->bin_w, .height = gmem->bin_h),
           A6XX_VSC_SIZE_BASE(.bo = fd6_ctx->vsc_draw_strm,
                              .bo_offset = max_vsc_pipes *
                                 fd6_ctx->vsc_draw_strm_pitch));

   OUT_REG(ring, A6XX_VSC_EXPANDED_BIN_CNTL(.nx = gmem->nbins_x, .ny = gmem->nbins_y));

   OUT_PKT4(ring, REG_A6XX_VSC_PIPE_CONFIG_REG(0), max_vsc_pipes);
   for (i = 0; i < max_vsc_pipes; i++) {
      const struct fd_vsc_pipe *pipe = &gmem->vsc_pipe[i];
      OUT_RING(ring, A6XX_VSC_PIPE_CONFIG_REG_X(pipe->x) |
                        A6XX_VSC_PIPE_CONFIG_REG_Y(pipe->y) |
                        A6XX_VSC_PIPE_CONFIG_REG_W(pipe->w) |
                        A6XX_VSC_PIPE_CONFIG_REG_H(pipe->h));
   }

   OUT_REG(
      ring, A6XX_VSC_PIPE_DATA_PRIM_BASE(.bo = fd6_ctx->vsc_prim_strm),
      A6XX_VSC_PIPE_DATA_PRIM_STRIDE(.dword = fd6_ctx->vsc_prim_strm_pitch),
      A6XX_VSC_PIPE_DATA_PRIM_LENGTH(.dword = fd6_ctx->vsc_prim_strm_pitch - 64));

   OUT_REG(
      ring, A6XX_VSC_PIPE_DATA_DRAW_BASE(.bo = fd6_ctx->vsc_draw_strm),
      A6XX_VSC_PIPE_DATA_DRAW_STRIDE(.dword = fd6_ctx->vsc_draw_strm_pitch),
      A6XX_VSC_PIPE_DATA_DRAW_LENGTH(.dword = fd6_ctx->vsc_draw_strm_pitch - 64));
}

/*
 * If overflow is detected, either 0x1 (VSC_DRAW_STRM overflow) or 0x3
 * (VSC_PRIM_STRM overflow) plus the size of the overflowed buffer is
 * written to control->vsc_overflow.  This allows the CPU to
 * detect which buffer overflowed (and, since the current size is
 * encoded as well, this protects against already-submitted but
 * not executed batches from fooling the CPU into increasing the
 * size again unnecessarily).
 */
static void
emit_vsc_overflow_test(struct fd_batch *batch)
{
   struct fd_ringbuffer *ring = batch->gmem;
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct fd6_context *fd6_ctx = fd6_context(batch->ctx);

   assert((fd6_ctx->vsc_draw_strm_pitch & 0x3) == 0);
   assert((fd6_ctx->vsc_prim_strm_pitch & 0x3) == 0);

   /* Check for overflow, write vsc_scratch if detected: */
   for (int i = 0; i < gmem->num_vsc_pipes; i++) {
      OUT_PKT7(ring, CP_COND_WRITE5, 8);
      OUT_RING(ring, CP_COND_WRITE5_0_FUNCTION(WRITE_GE) |
                        CP_COND_WRITE5_0_WRITE_MEMORY);
      OUT_RING(ring, CP_COND_WRITE5_1_POLL_ADDR_LO(
                        REG_A6XX_VSC_PIPE_DATA_DRAW_SIZE(i)));
      OUT_RING(ring, CP_COND_WRITE5_2_POLL_ADDR_HI(0));
      OUT_RING(ring, CP_COND_WRITE5_3_REF(fd6_ctx->vsc_draw_strm_pitch - 64));
      OUT_RING(ring, CP_COND_WRITE5_4_MASK(~0));
      OUT_RELOC(ring,
                control_ptr(fd6_ctx, vsc_overflow)); /* WRITE_ADDR_LO/HI */
      OUT_RING(ring,
               CP_COND_WRITE5_7_WRITE_DATA(1 + fd6_ctx->vsc_draw_strm_pitch));

      OUT_PKT7(ring, CP_COND_WRITE5, 8);
      OUT_RING(ring, CP_COND_WRITE5_0_FUNCTION(WRITE_GE) |
                        CP_COND_WRITE5_0_WRITE_MEMORY);
      OUT_RING(ring, CP_COND_WRITE5_1_POLL_ADDR_LO(
                        REG_A6XX_VSC_PIPE_DATA_PRIM_SIZE(i)));
      OUT_RING(ring, CP_COND_WRITE5_2_POLL_ADDR_HI(0));
      OUT_RING(ring, CP_COND_WRITE5_3_REF(fd6_ctx->vsc_prim_strm_pitch - 64));
      OUT_RING(ring, CP_COND_WRITE5_4_MASK(~0));
      OUT_RELOC(ring,
                control_ptr(fd6_ctx, vsc_overflow)); /* WRITE_ADDR_LO/HI */
      OUT_RING(ring,
               CP_COND_WRITE5_7_WRITE_DATA(3 + fd6_ctx->vsc_prim_strm_pitch));
   }

   OUT_PKT7(ring, CP_WAIT_MEM_WRITES, 0);
}

static void
check_vsc_overflow(struct fd_context *ctx)
{
   struct fd6_context *fd6_ctx = fd6_context(ctx);
   struct fd6_control *control =
         (struct fd6_control *)fd_bo_map(fd6_ctx->control_mem);
   uint32_t vsc_overflow = control->vsc_overflow;

   if (!vsc_overflow)
      return;

   /* clear overflow flag: */
   control->vsc_overflow = 0;

   unsigned buffer = vsc_overflow & 0x3;
   unsigned size = vsc_overflow & ~0x3;

   if (buffer == 0x1) {
      /* VSC_DRAW_STRM overflow: */

      if (size < fd6_ctx->vsc_draw_strm_pitch) {
         /* we've already increased the size, this overflow is
          * from a batch submitted before resize, but executed
          * after
          */
         return;
      }

      fd_bo_del(fd6_ctx->vsc_draw_strm);
      fd6_ctx->vsc_draw_strm = NULL;
      fd6_ctx->vsc_draw_strm_pitch *= 2;

      mesa_logd("resized VSC_PIPE_DATA_DRAW_STRIDE to: 0x%x",
                fd6_ctx->vsc_draw_strm_pitch);

   } else if (buffer == 0x3) {
      /* VSC_PRIM_STRM overflow: */

      if (size < fd6_ctx->vsc_prim_strm_pitch) {
         /* we've already increased the size */
         return;
      }

      fd_bo_del(fd6_ctx->vsc_prim_strm);
      fd6_ctx->vsc_prim_strm = NULL;
      fd6_ctx->vsc_prim_strm_pitch *= 2;

      mesa_logd("resized VSC_PIPE_DATA_PRIM_STRIDE to: 0x%x",
                fd6_ctx->vsc_prim_strm_pitch);

   } else {
      /* NOTE: it's possible, for example, for overflow to corrupt the
       * control page.  I mostly just see this hit if I set initial VSC
       * buffer size extremely small.  Things still seem to recover,
       * but maybe we should pre-emptively realloc vsc_data/vsc_data2
       * and hope for different memory placement?
       */
      mesa_loge("invalid vsc_overflow value: 0x%08x", vsc_overflow);
   }
}

template <chip CHIP>
static void
emit_common_init(struct fd_batch *batch)
{
   struct fd_context *ctx = batch->ctx;
   struct fd_ringbuffer *ring = batch->gmem;
   struct fd_autotune *at = &batch->ctx->autotune;
   struct fd_batch_result *result = batch->autotune_result;

   if (!result)
      return;

   fd_ringbuffer_attach_bo(ring, at->results_mem);

   OUT_PKT4(ring, REG_A6XX_RB_SAMPLE_COUNTER_CNTL, 1);
   OUT_RING(ring, A6XX_RB_SAMPLE_COUNTER_CNTL_COPY);

   if (!ctx->screen->info->a7xx.has_event_write_sample_count) {
      OUT_PKT4(ring, REG_A6XX_RB_SAMPLE_COUNTER_BASE, 2);
      OUT_RELOC(ring, results_ptr(at, result[result->idx].samples_start));

      fd6_event_write<CHIP>(ctx, ring, FD_ZPASS_DONE);

      /* Copied from blob's cmdstream, not sure why it is done. */
      if (CHIP == A7XX) {
         fd6_event_write<CHIP>(ctx, ring, FD_CCU_CLEAN_DEPTH);
      }
   } else {
      OUT_PKT(ring, CP_EVENT_WRITE7,
         CP_EVENT_WRITE7_0(
            .event = ZPASS_DONE,
            .write_sample_count = true,
         ),
         EV_DST_RAM_CP_EVENT_WRITE7_1(
            results_ptr(at, result[result->idx].samples_start)
         ),
      );
   }
}

template <chip CHIP>
static void
emit_common_fini(struct fd_batch *batch)
{
   struct fd_context *ctx = batch->ctx;
   struct fd_ringbuffer *ring = batch->gmem;
   struct fd_autotune *at = &batch->ctx->autotune;
   struct fd_batch_result *result = batch->autotune_result;

   fd6_emit_flushes<CHIP>(batch->ctx, ring, batch->barrier);

   if (!result)
      return;

   fd_ringbuffer_attach_bo(ring, at->results_mem);

   OUT_PKT4(ring, REG_A6XX_RB_SAMPLE_COUNTER_CNTL, 1);
   OUT_RING(ring, A6XX_RB_SAMPLE_COUNTER_CNTL_COPY);

   if (!ctx->screen->info->a7xx.has_event_write_sample_count) {
      OUT_PKT4(ring, REG_A6XX_RB_SAMPLE_COUNTER_BASE, 2);
      OUT_RELOC(ring, results_ptr(at, result[result->idx].samples_end));

      fd6_event_write<CHIP>(batch->ctx, ring, FD_ZPASS_DONE);
   } else {
      OUT_PKT(ring, CP_EVENT_WRITE7,
         CP_EVENT_WRITE7_0(
            .event = ZPASS_DONE,
            .write_sample_count = true,
            .sample_count_end_offset = true,
            .write_accum_sample_count_diff = true,
         ),
         EV_DST_RAM_CP_EVENT_WRITE7_1(
            results_ptr(at, result[result->idx].samples_start)
         ),
      );
   }

   fd6_fence_write<CHIP>(ring, result->fence, results_ptr(at, fence));
}

/*
 * Emit conditional CP_INDIRECT_BRANCH based on VSC_CHANNEL_VISIBILITY[p],
 * ie. the IB is skipped for tiles that have no visible geometry.
 *
 * If we aren't using binning pass, this just emits a normal IB.
 */
static void
emit_conditional_ib(struct fd_batch *batch, const struct fd_tile *tile,
                    struct fd_ringbuffer *target)
{
   struct fd_ringbuffer *ring = batch->gmem;

   /* If we have fast clear, that won't count in the VSC state, so it
    * forces an unconditional IB (because we know there is something
    * to do for this tile)
    */
   if (batch->cleared || !use_hw_binning(batch)) {
      fd6_emit_ib(batch->gmem, target);
      return;
   }

   if (target->cur == target->start)
      return;

   emit_marker6(ring, 6);

   unsigned count = fd_ringbuffer_cmd_count(target);

   BEGIN_RING(ring, 5 + 4 * count); /* ensure conditional doesn't get split */

   OUT_PKT7(ring, CP_REG_TEST, 1);
   OUT_RING(ring, A6XX_CP_REG_TEST_0_REG(REG_A6XX_VSC_CHANNEL_VISIBILITY(tile->p)) |
                     A6XX_CP_REG_TEST_0_BIT(tile->n) |
                     A6XX_CP_REG_TEST_0_SKIP_WAIT_FOR_ME);

   OUT_PKT7(ring, CP_COND_REG_EXEC, 2);
   OUT_RING(ring, CP_COND_REG_EXEC_0_MODE(PRED_TEST));
   OUT_RING(ring, PRED_TEST_CP_COND_REG_EXEC_1_DWORDS(4 * count));

   for (unsigned i = 0; i < count; i++) {
      uint32_t dwords;
      OUT_PKT7(ring, CP_INDIRECT_BUFFER, 3);
      dwords = fd_ringbuffer_emit_reloc_ring_full(ring, target, i) / 4;
      assert(dwords > 0);
      OUT_RING(ring, dwords);
   }

   emit_marker6(ring, 6);
}

static void
set_scissor(struct fd_ringbuffer *ring, uint32_t x1, uint32_t y1, uint32_t x2,
            uint32_t y2)
{
   OUT_REG(ring, A6XX_GRAS_SC_WINDOW_SCISSOR_TL(.x = x1, .y = y1),
           A6XX_GRAS_SC_WINDOW_SCISSOR_BR(.x = x2, .y = y2));

   OUT_REG(ring, A6XX_GRAS_A2D_SCISSOR_TL(.x = x1, .y = y1),
           A6XX_GRAS_A2D_SCISSOR_BR(.x = x2, .y = y2));
}

template <chip CHIP>
static void
set_tessfactor_bo(struct fd_ringbuffer *ring, struct fd_batch *batch)
{
   /* This happens after all drawing has been emitted to the draw CS, so we know
    * whether we need the tess BO pointers.
    */
   if (!batch->tessellation)
      return;

   struct fd_screen *screen = batch->ctx->screen;

   assert(screen->tess_bo);
   fd_ringbuffer_attach_bo(ring, screen->tess_bo);
   OUT_REG(ring, PC_TESS_BASE(CHIP, screen->tess_bo));
   /* Updating PC_TESS_BASE could race with the next draw which uses it. */
   OUT_WFI5(ring);
}

struct bin_size_params {
   enum a6xx_render_mode render_mode;
   bool force_lrz_write_dis;
   enum a6xx_buffers_location buffers_location;
   enum a6xx_lrz_feedback_mask lrz_feedback_zmode_mask;
};

template <chip CHIP>
static void
set_bin_size(struct fd_ringbuffer *ring, const struct fd_gmem_stateobj *gmem,
             struct bin_size_params p)
{
   unsigned w = gmem ? gmem->bin_w : 0;
   unsigned h = gmem ? gmem->bin_h : 0;

   if (CHIP == A6XX) {
      OUT_REG(ring, A6XX_GRAS_SC_BIN_CNTL(
            .binw = w, .binh = h,
            .render_mode = p.render_mode,
            .force_lrz_write_dis = p.force_lrz_write_dis,
            .buffers_location = p.buffers_location,
            .lrz_feedback_zmode_mask = p.lrz_feedback_zmode_mask,
      ));
   } else {
      OUT_REG(ring, A6XX_GRAS_SC_BIN_CNTL(
            .binw = w, .binh = h,
            .render_mode = p.render_mode,
            .force_lrz_write_dis = p.force_lrz_write_dis,
            .lrz_feedback_zmode_mask = p.lrz_feedback_zmode_mask,
      ));
   }
   OUT_REG(ring, RB_CNTL(
         CHIP,
         .binw = w, .binh = h,
         .render_mode = p.render_mode,
         .force_lrz_write_dis = p.force_lrz_write_dis,
         .buffers_location = p.buffers_location,
         .lrz_feedback_zmode_mask = p.lrz_feedback_zmode_mask,
   ));
   /* no flag for RB_RESOLVE_CNTL_3... */
   OUT_REG(ring, A6XX_RB_RESOLVE_CNTL_3(.binw = w, .binh = h));
}

template <chip CHIP>
static void
emit_binning_pass(struct fd_batch *batch) assert_dt
{
   struct fd_ringbuffer *ring = batch->gmem;
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct fd_screen *screen = batch->ctx->screen;

   assert(!batch->tessellation);

   set_scissor(ring, 0, 0, gmem->width - 1, gmem->height - 1);

   emit_marker6(ring, 7);
   OUT_PKT7(ring, CP_SET_MARKER, 1);
   OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_BIN_VISIBILITY));
   emit_marker6(ring, 7);

   OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
   OUT_RING(ring, 0x1);

   OUT_PKT7(ring, CP_SET_MODE, 1);
   OUT_RING(ring, 0x1);

   OUT_WFI5(ring);

   OUT_REG(ring, A6XX_VFD_RENDER_MODE(.render_mode = BINNING_PASS));

   update_vsc_pipe(batch);

   if (CHIP == A6XX) {
      OUT_REG(ring, A6XX_PC_POWER_CNTL(screen->info->a6xx.magic.PC_POWER_CNTL));
      OUT_REG(ring, A6XX_VFD_POWER_CNTL(screen->info->a6xx.magic.PC_POWER_CNTL));
   }

   OUT_PKT7(ring, CP_EVENT_WRITE, 1);
   OUT_RING(ring, UNK_2C);

   OUT_PKT4(ring, REG_A6XX_RB_WINDOW_OFFSET, 1);
   OUT_RING(ring, A6XX_RB_WINDOW_OFFSET_X(0) | A6XX_RB_WINDOW_OFFSET_Y(0));

   OUT_PKT4(ring, REG_A6XX_TPL1_WINDOW_OFFSET, 1);
   OUT_RING(ring,
            A6XX_TPL1_WINDOW_OFFSET_X(0) | A6XX_TPL1_WINDOW_OFFSET_Y(0));

   /* emit IB to binning drawcmds: */
   trace_start_binning_ib(&batch->trace, ring);
   foreach_subpass (subpass, batch) {
      emit_lrz<CHIP>(batch, subpass);
      fd6_emit_ib(ring, subpass->draw);
   }
   trace_end_binning_ib(&batch->trace, ring);

   OUT_PKT7(ring, CP_SET_DRAW_STATE, 3);
   OUT_RING(ring, CP_SET_DRAW_STATE__0_COUNT(0) |
                     CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS |
                     CP_SET_DRAW_STATE__0_GROUP_ID(0));
   OUT_RING(ring, CP_SET_DRAW_STATE__1_ADDR_LO(0));
   OUT_RING(ring, CP_SET_DRAW_STATE__2_ADDR_HI(0));

   OUT_PKT7(ring, CP_EVENT_WRITE, 1);
   OUT_RING(ring, UNK_2D);

   /* This flush is probably required because the VSC, which produces the
    * visibility stream, is a client of UCHE, whereas the CP needs to read
    * the visibility stream (without caching) to do draw skipping. The
    * WFI+WAIT_FOR_ME combination guarantees that the binning commands
    * submitted are finished before reading the VSC regs (in
    * emit_vsc_overflow_test) or the VSC_DATA buffer directly (implicitly
    * as part of draws).
    */
   fd6_emit_flushes<CHIP>(batch->ctx, ring,
                          FD6_FLUSH_CACHE |
                          FD6_WAIT_FOR_IDLE |
                          FD6_WAIT_FOR_ME);

   trace_start_vsc_overflow_test(&batch->trace, batch->gmem);
   emit_vsc_overflow_test(batch);
   trace_end_vsc_overflow_test(&batch->trace, batch->gmem);

   OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
   OUT_RING(ring, 0x0);

   OUT_PKT7(ring, CP_SET_MODE, 1);
   OUT_RING(ring, 0x0);

   fd6_emit_ccu_cntl<CHIP>(ring, screen, true);
}

static void
emit_msaa(struct fd_ringbuffer *ring, unsigned nr)
{
   enum a3xx_msaa_samples samples = fd_msaa_samples(nr);

   OUT_PKT4(ring, REG_A6XX_TPL1_RAS_MSAA_CNTL, 2);
   OUT_RING(ring, A6XX_TPL1_RAS_MSAA_CNTL_SAMPLES(samples));
   OUT_RING(ring, A6XX_TPL1_DEST_MSAA_CNTL_SAMPLES(samples) |
                     COND(samples == MSAA_ONE,
                          A6XX_TPL1_DEST_MSAA_CNTL_MSAA_DISABLE));

   OUT_PKT4(ring, REG_A6XX_GRAS_SC_RAS_MSAA_CNTL, 2);
   OUT_RING(ring, A6XX_GRAS_SC_RAS_MSAA_CNTL_SAMPLES(samples));
   OUT_RING(ring, A6XX_GRAS_SC_DEST_MSAA_CNTL_SAMPLES(samples) |
                     COND(samples == MSAA_ONE,
                          A6XX_GRAS_SC_DEST_MSAA_CNTL_MSAA_DISABLE));

   OUT_PKT4(ring, REG_A6XX_RB_RAS_MSAA_CNTL, 2);
   OUT_RING(ring, A6XX_RB_RAS_MSAA_CNTL_SAMPLES(samples));
   OUT_RING(ring,
            A6XX_RB_DEST_MSAA_CNTL_SAMPLES(samples) |
               COND(samples == MSAA_ONE, A6XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE));

   OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_GMEM_BUFFER_INFO, 1);
   OUT_RING(ring, A6XX_RB_RESOLVE_GMEM_BUFFER_INFO_SAMPLES(samples));
}

template <chip CHIP>
static void prepare_tile_setup(struct fd_batch *batch);
template <chip CHIP>
static void prepare_tile_fini(struct fd_batch *batch);

static void
fd7_emit_static_binning_regs(struct fd_ringbuffer *ring)
{
   OUT_REG(ring, A7XX_RB_UNKNOWN_8812(0x0));
   OUT_REG(ring, A7XX_RB_CCU_DBG_ECO_CNTL(0x0));
   OUT_REG(ring, A7XX_GRAS_UNKNOWN_8007(0x0));
   OUT_REG(ring, A6XX_GRAS_UNKNOWN_8110(0x2));
   OUT_REG(ring, A7XX_RB_UNKNOWN_8E09(0x4));
   OUT_REG(ring, A7XX_RB_CLEAR_TARGET(.clear_mode = CLEAR_MODE_GMEM));
}

template <chip CHIP>
struct fd_ringbuffer *
fd6_build_preemption_preamble(struct fd_context *ctx)
{
   struct fd_screen *screen = ctx->screen;
   struct fd_ringbuffer *ring;

   ring = fd_ringbuffer_new_object(ctx->pipe, 0x1000);
   fd6_emit_static_regs<CHIP>(ctx, ring);
   fd6_emit_ccu_cntl<CHIP>(ring, screen, false);

   if (CHIP == A6XX) {
      OUT_REG(ring, A6XX_PC_POWER_CNTL(screen->info->a6xx.magic.PC_POWER_CNTL));
      OUT_REG(ring, A6XX_VFD_POWER_CNTL(screen->info->a6xx.magic.PC_POWER_CNTL));
   } else if (CHIP >= A7XX) {
      fd7_emit_static_binning_regs(ring);
   }

   /* TODO use CP_MEM_TO_SCRATCH_MEM on a7xx. The VSC scratch mem should be
    * automatically saved, unlike GPU registers, so we wouldn't have to
    * manually restore this state.
    */
   OUT_PKT7(ring, CP_MEM_TO_REG, 3);
   OUT_RING(ring, CP_MEM_TO_REG_0_REG(REG_A6XX_VSC_CHANNEL_VISIBILITY(0)) |
                  CP_MEM_TO_REG_0_CNT(32));
   OUT_RELOC(ring, control_ptr(fd6_context(ctx), vsc_state));

   return ring;
}
FD_GENX(fd6_build_preemption_preamble);

/* before first tile */
template <chip CHIP>
static void
fd6_emit_tile_init(struct fd_batch *batch) assert_dt
{
   struct fd_ringbuffer *ring = batch->gmem;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct fd_screen *screen = batch->ctx->screen;

   emit_lrz_clears<CHIP>(batch);

   fd6_emit_restore<CHIP>(batch, ring);

   fd6_event_write<CHIP>(batch->ctx, ring, FD_LRZ_FLUSH);

   if (batch->prologue) {
      trace_start_prologue(&batch->trace, ring);
      fd6_emit_ib(ring, batch->prologue);
      trace_end_prologue(&batch->trace, ring);
   }

   fd6_cache_inv<CHIP>(batch->ctx, ring);

   prepare_tile_setup<CHIP>(batch);
   prepare_tile_fini<CHIP>(batch);

   OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
   OUT_RING(ring, 0x0);

   /* blob controls "local" in IB2, but I think that is not required */
   OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_LOCAL, 1);
   OUT_RING(ring, 0x1);

   fd6_emit_ccu_cntl<CHIP>(ring, screen, true);

   emit_zs<CHIP>(batch->ctx, ring, &pfb->zsbuf, batch->gmem_state);
   emit_mrt<CHIP>(ring, pfb, batch->gmem_state);
   emit_msaa(ring, pfb->samples);
   patch_fb_read_gmem(batch);

   if (CHIP >= A7XX)
      fd7_emit_static_binning_regs(ring);

   if (use_hw_binning(batch)) {
      /* enable stream-out during binning pass: */
      OUT_REG(ring, A6XX_VPC_SO_OVERRIDE(false));

      set_bin_size<CHIP>(ring, gmem, {
            .render_mode = BINNING_PASS,
            .buffers_location = BUFFERS_IN_GMEM,
            .lrz_feedback_zmode_mask = LRZ_FEEDBACK_NONE,
      });
      update_render_cntl<CHIP>(batch, pfb, true);
      emit_binning_pass<CHIP>(batch);

      /* and disable stream-out for draw pass: */
      OUT_REG(ring, A6XX_VPC_SO_OVERRIDE(true));

      /*
       * NOTE: even if we detect VSC overflow and disable use of
       * visibility stream in draw pass, it is still safe to execute
       * the reset of these cmds:
       */

      set_bin_size<CHIP>(ring, gmem, {
            .render_mode = RENDERING_PASS,
            .force_lrz_write_dis = !screen->info->a6xx.has_lrz_feedback,
            .buffers_location = BUFFERS_IN_GMEM,
            .lrz_feedback_zmode_mask = screen->info->a6xx.has_lrz_feedback
                                          ? LRZ_FEEDBACK_EARLY_Z_LATE_Z
                                          : LRZ_FEEDBACK_NONE,
      });

      OUT_REG(ring, A6XX_VFD_RENDER_MODE(RENDERING_PASS));

      if (CHIP == A6XX) {
         OUT_REG(ring, A6XX_PC_POWER_CNTL(screen->info->a6xx.magic.PC_POWER_CNTL));
         OUT_REG(ring, A6XX_VFD_POWER_CNTL(screen->info->a6xx.magic.PC_POWER_CNTL));
      }

      OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
      OUT_RING(ring, 0x1);

      /* Upload state regs to memory to be restored on skipsaverestore
       * preemption.
       */
      OUT_PKT7(ring, CP_REG_TO_MEM, 3);
      OUT_RING(ring, CP_REG_TO_MEM_0_REG(REG_A6XX_VSC_CHANNEL_VISIBILITY(0)) |
                     CP_REG_TO_MEM_0_CNT(32));
      OUT_RELOC(ring, control_ptr(fd6_context(batch->ctx), vsc_state));
   } else {
      /* no binning pass, so enable stream-out for draw pass:: */
      OUT_REG(ring, A6XX_VPC_SO_OVERRIDE(false));

      set_bin_size<CHIP>(ring, gmem, {
            .render_mode = RENDERING_PASS,
            .force_lrz_write_dis = !screen->info->a6xx.has_lrz_feedback,
            .buffers_location = BUFFERS_IN_GMEM,
            .lrz_feedback_zmode_mask =
               screen->info->a6xx.has_lrz_feedback
                  ? LRZ_FEEDBACK_EARLY_Z_OR_EARLY_Z_LATE_Z
                  : LRZ_FEEDBACK_NONE,
      });
   }

   update_render_cntl<CHIP>(batch, pfb, false);

   emit_common_init<CHIP>(batch);
}

template <chip CHIP>
static void
set_window_offset(struct fd_ringbuffer *ring, uint32_t x1, uint32_t y1)
{
   OUT_PKT4(ring, REG_A6XX_RB_WINDOW_OFFSET, 1);
   OUT_RING(ring, A6XX_RB_WINDOW_OFFSET_X(x1) | A6XX_RB_WINDOW_OFFSET_Y(y1));

   OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_WINDOW_OFFSET, 1);
   OUT_RING(ring, A6XX_RB_RESOLVE_WINDOW_OFFSET_X(x1) | A6XX_RB_RESOLVE_WINDOW_OFFSET_Y(y1));

   OUT_REG(ring, SP_WINDOW_OFFSET(CHIP, .x = x1, .y = y1));

   OUT_PKT4(ring, REG_A6XX_TPL1_WINDOW_OFFSET, 1);
   OUT_RING(ring,
            A6XX_TPL1_WINDOW_OFFSET_X(x1) | A6XX_TPL1_WINDOW_OFFSET_Y(y1));
}

/* before mem2gmem */
template <chip CHIP>
static void
fd6_emit_tile_prep(struct fd_batch *batch, const struct fd_tile *tile)
{
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   struct fd_screen *screen = batch->ctx->screen;
   struct fd_context *ctx = batch->ctx;
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct fd6_context *fd6_ctx = fd6_context(ctx);
   struct fd_ringbuffer *ring = batch->gmem;

   emit_marker6(ring, 7);
   OUT_PKT7(ring, CP_SET_MARKER, 1);
   OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_BIN_RENDER_START) |
                  A6XX_CP_SET_MARKER_0_USES_GMEM);
   emit_marker6(ring, 7);

   uint32_t x1 = tile->xoff;
   uint32_t y1 = tile->yoff;
   uint32_t x2 = tile->xoff + tile->bin_w - 1;
   uint32_t y2 = tile->yoff + tile->bin_h - 1;

   set_scissor(ring, x1, y1, x2, y2);
   set_tessfactor_bo<CHIP>(ring, batch);

   fd6_emit_ccu_cntl<CHIP>(ring, screen, true);

   emit_zs<CHIP>(batch->ctx, ring, &pfb->zsbuf, batch->gmem_state);
   emit_mrt<CHIP>(ring, pfb, batch->gmem_state);
   emit_msaa(ring, pfb->samples);

   if (use_hw_binning(batch)) {
      const struct fd_vsc_pipe *pipe = &gmem->vsc_pipe[tile->p];
      unsigned num_vsc_pipes = ctx->screen->info->num_vsc_pipes;

      OUT_PKT7(ring, CP_WAIT_FOR_ME, 0);

      OUT_PKT7(ring, CP_SET_MODE, 1);
      OUT_RING(ring, 0x0);

      OUT_PKT7(ring, CP_SET_BIN_DATA5, 7);
      /* A702 also sets BIT(0) but that hangchecks */
      OUT_RING(ring, CP_SET_BIN_DATA5_0_VSC_SIZE(pipe->w * pipe->h) |
                        CP_SET_BIN_DATA5_0_VSC_N(tile->n));
      OUT_RELOC(ring, fd6_ctx->vsc_draw_strm, /* per-pipe draw-stream address */
                (tile->p * fd6_ctx->vsc_draw_strm_pitch), 0, 0);
      OUT_RELOC(
         ring, fd6_ctx->vsc_draw_strm, /* VSC_PIPE_DATA_DRAW_BASE + (p * 4) */
         (tile->p * 4) + (num_vsc_pipes * fd6_ctx->vsc_draw_strm_pitch),
         0, 0);
      OUT_RELOC(ring, fd6_ctx->vsc_prim_strm,
                (tile->p * fd6_ctx->vsc_prim_strm_pitch), 0, 0);

      OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
      OUT_RING(ring, 0x0);

      /* and disable stream-out for draw pass: */
      OUT_REG(ring, A6XX_VPC_SO_OVERRIDE(true));

      /*
       * NOTE: even if we detect VSC overflow and disable use of
       * visibility stream in draw pass, it is still safe to execute
       * the reset of these cmds:
       */

      set_bin_size<CHIP>(ring, gmem, {
            .render_mode = RENDERING_PASS,
            .force_lrz_write_dis = !screen->info->a6xx.has_lrz_feedback,
            .buffers_location = BUFFERS_IN_GMEM,
            .lrz_feedback_zmode_mask = screen->info->a6xx.has_lrz_feedback
                                          ? LRZ_FEEDBACK_EARLY_Z_LATE_Z
                                          : LRZ_FEEDBACK_NONE,
      });

      OUT_REG(ring, A6XX_VFD_RENDER_MODE(RENDERING_PASS));

      if (CHIP == A6XX) {
         OUT_REG(ring, A6XX_PC_POWER_CNTL(screen->info->a6xx.magic.PC_POWER_CNTL));
         OUT_REG(ring, A6XX_VFD_POWER_CNTL(screen->info->a6xx.magic.PC_POWER_CNTL));
      }

      OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
      OUT_RING(ring, 0x1);

   } else {
      OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
      OUT_RING(ring, 0x1);

      /* no binning pass, so enable stream-out for draw pass:: */
      OUT_REG(ring, A6XX_VPC_SO_OVERRIDE(false));

      set_bin_size<CHIP>(ring, gmem, {
            .render_mode = RENDERING_PASS,
            .force_lrz_write_dis = !screen->info->a6xx.has_lrz_feedback,
            .buffers_location = BUFFERS_IN_GMEM,
            .lrz_feedback_zmode_mask =
               screen->info->a6xx.has_lrz_feedback
                  ? LRZ_FEEDBACK_EARLY_Z_OR_EARLY_Z_LATE_Z
                  : LRZ_FEEDBACK_NONE,
      });
   }

   set_window_offset<CHIP>(ring, x1, y1);

   set_bin_size<CHIP>(ring, gmem, {
         .render_mode = RENDERING_PASS,
         .force_lrz_write_dis = !ctx->screen->info->a6xx.has_lrz_feedback,
         .buffers_location = BUFFERS_IN_GMEM,
         .lrz_feedback_zmode_mask = ctx->screen->info->a6xx.has_lrz_feedback
                                       ? LRZ_FEEDBACK_EARLY_Z_LATE_Z
                                       : LRZ_FEEDBACK_NONE,
   });

   OUT_PKT7(ring, CP_SET_MODE, 1);
   OUT_RING(ring, 0x0);
}

static void
set_blit_scissor(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
   const struct pipe_framebuffer_state *pfb = &batch->framebuffer;

   struct pipe_scissor_state blit_scissor;

   blit_scissor.minx = 0;
   blit_scissor.miny = 0;
   blit_scissor.maxx = ALIGN(pfb->width, 16);
   blit_scissor.maxy = ALIGN(pfb->height, 4);

   OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_CNTL_1, 2);
   OUT_RING(ring, A6XX_RB_RESOLVE_CNTL_1_X(blit_scissor.minx) |
                     A6XX_RB_RESOLVE_CNTL_1_Y(blit_scissor.miny));
   OUT_RING(ring, A6XX_RB_RESOLVE_CNTL_2_X(blit_scissor.maxx - 1) |
                     A6XX_RB_RESOLVE_CNTL_2_Y(blit_scissor.maxy - 1));
}

template <chip CHIP>
static void
emit_blit(struct fd_batch *batch, struct fd_ringbuffer *ring, uint32_t base,
          struct pipe_surface *psurf, bool stencil)
{
   struct fd_resource *rsc = fd_resource(psurf->texture);
   enum pipe_format pfmt = psurf->format;
   uint32_t offset;
   bool ubwc_enabled;

   assert(psurf->first_layer == psurf->last_layer);

   /* separate stencil case: */
   if (stencil) {
      rsc = rsc->stencil;
      pfmt = rsc->b.b.format;
   }

   offset =
      fd_resource_offset(rsc, psurf->level, psurf->first_layer);
   ubwc_enabled = fd_resource_ubwc_enabled(rsc, psurf->level);

   assert(psurf->first_layer == psurf->last_layer);

   enum a6xx_tile_mode tile_mode = (enum a6xx_tile_mode)
         fd_resource_tile_mode(&rsc->b.b, psurf->level);
   enum a6xx_format format = fd6_color_format(pfmt, tile_mode);
   uint32_t stride = fd_resource_pitch(rsc, psurf->level);
   uint32_t array_stride = fd_resource_layer_stride(rsc, psurf->level);
   enum a3xx_color_swap swap =
         fd6_color_swap(pfmt, (enum a6xx_tile_mode)rsc->layout.tile_mode,
                        false);
   enum a3xx_msaa_samples samples = fd_msaa_samples(rsc->b.b.nr_samples);

   OUT_REG(ring,
           A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO(
                 .tile_mode = tile_mode,
                 .flags = ubwc_enabled,
                 .samples = samples,
                 .color_swap = swap,
                 .color_format = format,
           ),
           A6XX_RB_RESOLVE_SYSTEM_BUFFER_BASE(.bo = rsc->bo, .bo_offset = offset),
           A6XX_RB_RESOLVE_SYSTEM_BUFFER_PITCH(stride),
           A6XX_RB_RESOLVE_SYSTEM_BUFFER_ARRAY_PITCH(array_stride));

   OUT_REG(ring, A6XX_RB_RESOLVE_GMEM_BUFFER_BASE(.dword = base));

   if (ubwc_enabled) {
      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_SYSTEM_FLAG_BUFFER_BASE, 3);
      fd6_emit_flag_reference(ring, rsc, psurf->level,
                              psurf->first_layer);
   }

   if (CHIP >= A7XX)
      OUT_REG(ring, A7XX_RB_CLEAR_TARGET(.clear_mode = CLEAR_MODE_GMEM));

   fd6_emit_blit<CHIP>(batch->ctx, ring);
}

template <chip CHIP>
static void
emit_restore_blit(struct fd_batch *batch, struct fd_ringbuffer *ring,
                  uint32_t base, struct pipe_surface *psurf, unsigned buffer)
{
   bool stencil = (buffer == FD_BUFFER_STENCIL);

   OUT_REG(ring,
           A6XX_RB_RESOLVE_OPERATION(
                 .type = BLIT_EVENT_LOAD,
                 .sample_0 = util_format_is_pure_integer(psurf->format),
                 .depth = (buffer == FD_BUFFER_DEPTH),
           ),
   );

   emit_blit<CHIP>(batch, ring, base, psurf, stencil);
}

template <chip CHIP>
static void
emit_subpass_clears(struct fd_batch *batch, struct fd_batch_subpass *subpass)
{
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct fd_ringbuffer *ring = subpass->subpass_clears;
   enum a3xx_msaa_samples samples = fd_msaa_samples(pfb->samples);

   uint32_t buffers = subpass->fast_cleared;

   if (buffers & PIPE_CLEAR_COLOR) {

      for (int i = 0; i < pfb->nr_cbufs; i++) {
         union pipe_color_union *color = &subpass->clear_color[i];
         union util_color uc = {0};

         if (!pfb->cbufs[i].texture)
            continue;

         if (!(buffers & (PIPE_CLEAR_COLOR0 << i)))
            continue;

         enum pipe_format pfmt = pfb->cbufs[i].format;

         // XXX I think RB_CLEAR_COLOR_DWn wants to take into account SWAP??
         union pipe_color_union swapped;
         switch (fd6_color_swap(pfmt, TILE6_LINEAR, false)) {
         case WZYX:
            swapped.ui[0] = color->ui[0];
            swapped.ui[1] = color->ui[1];
            swapped.ui[2] = color->ui[2];
            swapped.ui[3] = color->ui[3];
            break;
         case WXYZ:
            swapped.ui[2] = color->ui[0];
            swapped.ui[1] = color->ui[1];
            swapped.ui[0] = color->ui[2];
            swapped.ui[3] = color->ui[3];
            break;
         case ZYXW:
            swapped.ui[3] = color->ui[0];
            swapped.ui[0] = color->ui[1];
            swapped.ui[1] = color->ui[2];
            swapped.ui[2] = color->ui[3];
            break;
         case XYZW:
            swapped.ui[3] = color->ui[0];
            swapped.ui[2] = color->ui[1];
            swapped.ui[1] = color->ui[2];
            swapped.ui[0] = color->ui[3];
            break;
         }

         util_pack_color_union(pfmt, &uc, &swapped);

         OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO, 1);
         OUT_RING(ring,
                  A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO_TILE_MODE(TILE6_LINEAR) |
                     A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO_SAMPLES(samples) |
                     A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO_COLOR_FORMAT(fd6_color_format(pfmt, TILE6_LINEAR)));

         OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_OPERATION, 1);
         OUT_RING(ring, A6XX_RB_RESOLVE_OPERATION_TYPE(BLIT_EVENT_CLEAR) |
                           A6XX_RB_RESOLVE_OPERATION_CLEAR_MASK(0xf));

         OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_GMEM_BUFFER_BASE, 1);
         OUT_RING(ring, gmem->cbuf_base[i]);

         OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_CNTL_0, 1);
         OUT_RING(ring, 0);

         OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_CLEAR_COLOR_DW0, 4);
         OUT_RING(ring, uc.ui[0]);
         OUT_RING(ring, uc.ui[1]);
         OUT_RING(ring, uc.ui[2]);
         OUT_RING(ring, uc.ui[3]);

         if (CHIP >= A7XX)
            OUT_REG(ring, A7XX_RB_CLEAR_TARGET(.clear_mode = CLEAR_MODE_GMEM));

         fd6_emit_blit<CHIP>(batch->ctx, ring);
      }
   }

   const bool has_depth = !!pfb->zsbuf.texture;
   const bool has_separate_stencil =
      has_depth && fd_resource(pfb->zsbuf.texture)->stencil;

   /* First clear depth or combined depth/stencil. */
   if ((has_depth && (buffers & PIPE_CLEAR_DEPTH)) ||
       (!has_separate_stencil && (buffers & PIPE_CLEAR_STENCIL))) {
      enum pipe_format pfmt = pfb->zsbuf.format;
      uint32_t clear_value;
      uint32_t mask = 0;

      if (has_separate_stencil) {
         pfmt = util_format_get_depth_only(pfb->zsbuf.format);
         clear_value = util_pack_z(pfmt, subpass->clear_depth);
      } else {
         pfmt = pfb->zsbuf.format;
         clear_value =
            util_pack_z_stencil(pfmt, subpass->clear_depth, subpass->clear_stencil);
      }

      if (buffers & PIPE_CLEAR_DEPTH)
         mask |= 0x1;

      if (!has_separate_stencil && (buffers & PIPE_CLEAR_STENCIL))
         mask |= 0x2;

      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO, 1);
      OUT_RING(ring,
               A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO_TILE_MODE(TILE6_LINEAR) |
                  A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO_SAMPLES(samples) |
                  A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO_COLOR_FORMAT(fd6_color_format(pfmt, TILE6_LINEAR)));

      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_OPERATION, 1);
      OUT_RING(ring, A6XX_RB_RESOLVE_OPERATION_TYPE(BLIT_EVENT_CLEAR) |
                        A6XX_RB_RESOLVE_OPERATION_DEPTH |
                        A6XX_RB_RESOLVE_OPERATION_CLEAR_MASK(mask));

      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_GMEM_BUFFER_BASE, 1);
      OUT_RING(ring, gmem->zsbuf_base[0]);

      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_CNTL_0, 1);
      OUT_RING(ring, 0);

      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_CLEAR_COLOR_DW0, 1);
      OUT_RING(ring, clear_value);

      fd6_emit_blit<CHIP>(batch->ctx, ring);
   }

   /* Then clear the separate stencil buffer in case of 32 bit depth
    * formats with separate stencil. */
   if (has_separate_stencil && (buffers & PIPE_CLEAR_STENCIL)) {
      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO, 1);
      OUT_RING(ring, A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO_TILE_MODE(TILE6_LINEAR) |
                        A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO_SAMPLES(samples) |
                        A6XX_RB_RESOLVE_SYSTEM_BUFFER_INFO_COLOR_FORMAT(FMT6_8_UINT));

      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_OPERATION, 1);
      OUT_RING(ring, A6XX_RB_RESOLVE_OPERATION_TYPE(BLIT_EVENT_CLEAR) |
                        A6XX_RB_RESOLVE_OPERATION_DEPTH |
                        A6XX_RB_RESOLVE_OPERATION_CLEAR_MASK(0x1));

      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_GMEM_BUFFER_BASE, 1);
      OUT_RING(ring, gmem->zsbuf_base[1]);

      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_CNTL_0, 1);
      OUT_RING(ring, 0);

      OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_CLEAR_COLOR_DW0, 1);
      OUT_RING(ring, subpass->clear_stencil & 0xff);

      fd6_emit_blit<CHIP>(batch->ctx, ring);
   }
}

/*
 * transfer from system memory to gmem
 */
template <chip CHIP>
static void
emit_restore_blits(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;

   if (batch->restore & FD_BUFFER_COLOR) {
      unsigned i;
      for (i = 0; i < pfb->nr_cbufs; i++) {
         if (!pfb->cbufs[i].texture)
            continue;
         if (!(batch->restore & (PIPE_CLEAR_COLOR0 << i)))
            continue;
         emit_restore_blit<CHIP>(batch, ring, gmem->cbuf_base[i], &pfb->cbufs[i],
                                 FD_BUFFER_COLOR);
      }
   }

   if (batch->restore & (FD_BUFFER_DEPTH | FD_BUFFER_STENCIL)) {
      struct fd_resource *rsc = fd_resource(pfb->zsbuf.texture);

      if (!rsc->stencil || (batch->restore & FD_BUFFER_DEPTH)) {
         emit_restore_blit<CHIP>(batch, ring, gmem->zsbuf_base[0], &pfb->zsbuf,
                                 FD_BUFFER_DEPTH);
      }
      if (rsc->stencil && (batch->restore & FD_BUFFER_STENCIL)) {
         emit_restore_blit<CHIP>(batch, ring, gmem->zsbuf_base[1], &pfb->zsbuf,
                                 FD_BUFFER_STENCIL);
      }
   }
}

template <chip CHIP>
static void
prepare_tile_setup(struct fd_batch *batch)
{
   if (batch->restore) {
      batch->tile_loads =
         fd_submit_new_ringbuffer(batch->submit, 0x1000, FD_RINGBUFFER_STREAMING);

      set_blit_scissor(batch, batch->tile_loads);
      emit_restore_blits<CHIP>(batch, batch->tile_loads);
   }

   foreach_subpass (subpass, batch) {
      if (!subpass->fast_cleared)
         continue;

      subpass->subpass_clears =
         fd_submit_new_ringbuffer(batch->submit, 0x1000, FD_RINGBUFFER_STREAMING);

      set_blit_scissor(batch, subpass->subpass_clears);
      emit_subpass_clears<CHIP>(batch, subpass);
   }
}

/*
 * transfer from system memory to gmem
 */
static void
fd6_emit_tile_mem2gmem(struct fd_batch *batch, const struct fd_tile *tile)
{
}

/* before IB to rendering cmds: */
static void
fd6_emit_tile_renderprep(struct fd_batch *batch, const struct fd_tile *tile)
{
   if (batch->tile_loads) {
      trace_start_tile_loads(&batch->trace, batch->gmem, batch->restore);
      emit_conditional_ib(batch, tile, batch->tile_loads);
      trace_end_tile_loads(&batch->trace, batch->gmem);
   }
}

static bool
blit_can_resolve(enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);

   /* blit event can only do resolve for simple cases:
    * averaging samples as unsigned integers or choosing only one sample
    */
   if (util_format_is_snorm(format) || util_format_is_srgb(format))
      return false;

   /* can't do formats with larger channel sizes
    * note: this includes all float formats
    * note2: single channel integer formats seem OK
    */
   if (desc->channel[0].size > 10)
      return false;

   switch (format) {
   /* for unknown reasons blit event can't msaa resolve these formats when tiled
    * likely related to these formats having different layout from other cpp=2
    * formats
    */
   case PIPE_FORMAT_R8G8_UNORM:
   case PIPE_FORMAT_R8G8_UINT:
   case PIPE_FORMAT_R8G8_SINT:
   case PIPE_FORMAT_R8G8_SRGB:
   /* TODO: this one should be able to work? */
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      return false;
   default:
      break;
   }

   return true;
}

static bool
needs_resolve(struct pipe_surface *psurf)
{
   return psurf->nr_samples &&
          (psurf->nr_samples != psurf->texture->nr_samples);
}

/**
 * Returns the UNKNOWN_8C01 value for handling partial depth/stencil
 * clear/stores to Z24S8.
 */
static uint32_t
fd6_unknown_8c01(enum pipe_format format, unsigned buffers)
{
   buffers &= FD_BUFFER_DEPTH | FD_BUFFER_STENCIL;
   if (format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
      if (buffers == FD_BUFFER_DEPTH)
         return 0x08000041;
      else if (buffers == FD_BUFFER_STENCIL)
         return 0x00084001;
   }
   return 0;
}

template <chip CHIP>
static void
emit_resolve_blit(struct fd_batch *batch, struct fd_ringbuffer *ring,
                  uint32_t base, struct pipe_surface *psurf,
                  unsigned buffer) assert_dt
{
   uint32_t info = 0;
   bool stencil = false;

   if (!fd_resource(psurf->texture)->valid)
      return;

   /* if we need to resolve, but cannot with BLIT event, we instead need
    * to generate per-tile CP_BLIT (r2d) commands:
    *
    * The separate-stencil is a special case, we might need to use CP_BLIT
    * for depth, but we can still resolve stencil with a BLIT event
    */
   if (needs_resolve(psurf) && !blit_can_resolve(psurf->format) &&
       (buffer != FD_BUFFER_STENCIL)) {
      /* We could potentially use fd6_unknown_8c01() to handle partial z/s
       * resolve to packed z/s, but we would need a corresponding ability in the
       * !resolve case below, so batch_draw_tracking_for_dirty_bits() has us
       * just do a restore of the other channel for partial packed z/s writes.
       */
      fd6_resolve_tile<CHIP>(batch, ring, base, psurf, 0);
      return;
   }

   switch (buffer) {
   case FD_BUFFER_COLOR:
      info = A6XX_RB_RESOLVE_OPERATION_TYPE(BLIT_EVENT_STORE);
      break;
   case FD_BUFFER_STENCIL:
      info = A6XX_RB_RESOLVE_OPERATION_TYPE(BLIT_EVENT_STORE_AND_CLEAR);
      stencil = true;
      break;
   case FD_BUFFER_DEPTH:
      info = A6XX_RB_RESOLVE_OPERATION_TYPE(BLIT_EVENT_STORE) | A6XX_RB_RESOLVE_OPERATION_DEPTH;
      break;
   }

   if (util_format_is_pure_integer(psurf->format) ||
       util_format_is_depth_or_stencil(psurf->format))
      info |= A6XX_RB_RESOLVE_OPERATION_SAMPLE_0;

   OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_OPERATION, 1);
   OUT_RING(ring, info);

   emit_blit<CHIP>(batch, ring, base, psurf, stencil);
}

/*
 * transfer from gmem to system memory (ie. normal RAM)
 */

template <chip CHIP>
static void
prepare_tile_fini(struct fd_batch *batch)
   assert_dt
{
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   struct fd_ringbuffer *ring;

   batch->tile_store =
      fd_submit_new_ringbuffer(batch->submit, 0x1000, FD_RINGBUFFER_STREAMING);
   ring = batch->tile_store;

   set_blit_scissor(batch, ring);

   if (batch->resolve & (FD_BUFFER_DEPTH | FD_BUFFER_STENCIL)) {
      struct fd_resource *rsc = fd_resource(pfb->zsbuf.texture);

      if (!rsc->stencil || (batch->resolve & FD_BUFFER_DEPTH)) {
         emit_resolve_blit<CHIP>(batch, ring, gmem->zsbuf_base[0],
                                 &pfb->zsbuf, FD_BUFFER_DEPTH);
      }
      if (rsc->stencil && (batch->resolve & FD_BUFFER_STENCIL)) {
         emit_resolve_blit<CHIP>(batch, ring, gmem->zsbuf_base[1],
                                 &pfb->zsbuf, FD_BUFFER_STENCIL);
      }
   }

   if (batch->resolve & FD_BUFFER_COLOR) {
      unsigned i;
      for (i = 0; i < pfb->nr_cbufs; i++) {
         if (!pfb->cbufs[i].texture)
            continue;
         if (!(batch->resolve & (PIPE_CLEAR_COLOR0 << i)))
            continue;
         emit_resolve_blit<CHIP>(batch, ring, gmem->cbuf_base[i],
                                 &pfb->cbufs[i], FD_BUFFER_COLOR);
      }
   }
}

template <chip CHIP>
static void
fd6_emit_tile(struct fd_batch *batch, const struct fd_tile *tile)
{
   foreach_subpass (subpass, batch) {
      if (subpass->subpass_clears) {
         trace_start_clears(&batch->trace, batch->gmem, subpass->fast_cleared);
         emit_conditional_ib(batch, tile, subpass->subpass_clears);
         trace_end_clears(&batch->trace, batch->gmem);
      }

      emit_lrz<CHIP>(batch, subpass);

      fd6_emit_ib(batch->gmem, subpass->draw);
   }

   if (batch->tile_epilogue)
      fd6_emit_ib(batch->gmem, batch->tile_epilogue);
}

static void
fd6_emit_tile_gmem2mem(struct fd_batch *batch, const struct fd_tile *tile)
{
   struct fd_ringbuffer *ring = batch->gmem;

   if (batch->epilogue)
      fd6_emit_ib(batch->gmem, batch->epilogue);

   if (use_hw_binning(batch)) {
      OUT_PKT7(ring, CP_SET_MARKER, 1);
      OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_BIN_END_OF_DRAWS) |
                     A6XX_CP_SET_MARKER_0_USES_GMEM);
   }

   OUT_PKT7(ring, CP_SET_DRAW_STATE, 3);
   OUT_RING(ring, CP_SET_DRAW_STATE__0_COUNT(0) |
                     CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS |
                     CP_SET_DRAW_STATE__0_GROUP_ID(0));
   OUT_RING(ring, CP_SET_DRAW_STATE__1_ADDR_LO(0));
   OUT_RING(ring, CP_SET_DRAW_STATE__2_ADDR_HI(0));

   OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_LOCAL, 1);
   OUT_RING(ring, 0x0);

   emit_marker6(ring, 7);
   OUT_PKT7(ring, CP_SET_MARKER, 1);
   OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_BIN_RESOLVE) |
                  A6XX_CP_SET_MARKER_0_USES_GMEM);
   emit_marker6(ring, 7);

   if (batch->tile_store) {
      trace_start_tile_stores(&batch->trace, batch->gmem, batch->resolve);
      emit_conditional_ib(batch, tile, batch->tile_store);
      trace_end_tile_stores(&batch->trace, batch->gmem);
   }

   OUT_PKT7(ring, CP_SET_MARKER, 1);
   OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_BIN_RENDER_END));
}

template <chip CHIP>
static void
fd6_emit_tile_fini(struct fd_batch *batch)
{
   struct fd_ringbuffer *ring = batch->gmem;

   emit_common_fini<CHIP>(batch);

   OUT_PKT4(ring, REG_A6XX_GRAS_LRZ_CNTL, 1);
   OUT_RING(ring, A6XX_GRAS_LRZ_CNTL_ENABLE);

   fd6_event_write<CHIP>(batch->ctx, ring, FD_LRZ_FLUSH);
   fd6_event_write<CHIP>(batch->ctx, ring, FD_CCU_CLEAN_BLIT_CACHE);

   if (use_hw_binning(batch)) {
      check_vsc_overflow(batch->ctx);
   }
}

template <chip CHIP>
static void
emit_sysmem_clears(struct fd_batch *batch, struct fd_batch_subpass *subpass)
   assert_dt
{
   struct fd_context *ctx = batch->ctx;
   struct fd_ringbuffer *ring = batch->gmem;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;

   uint32_t buffers = subpass->fast_cleared;

   if (!buffers)
      return;

   struct pipe_box box2d;
   u_box_2d(0, 0, pfb->width, pfb->height, &box2d);

   trace_start_clears(&batch->trace, ring, buffers);

   if (buffers & PIPE_CLEAR_COLOR) {
      for (int i = 0; i < pfb->nr_cbufs; i++) {
         union pipe_color_union color = subpass->clear_color[i];

         if (!pfb->cbufs[i].texture)
            continue;

         if (!(buffers & (PIPE_CLEAR_COLOR0 << i)))
            continue;

         fd6_clear_surface<CHIP>(ctx, ring, &pfb->cbufs[i], &box2d, &color, 0);
      }
   }
   if (buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL)) {
      union pipe_color_union value = {};

      const bool has_depth = !!pfb->zsbuf.texture;
      struct pipe_resource *separate_stencil =
         has_depth && fd_resource(pfb->zsbuf.texture)->stencil
            ? &fd_resource(pfb->zsbuf.texture)->stencil->b.b
            : NULL;

      if ((buffers & PIPE_CLEAR_DEPTH) || (!separate_stencil && (buffers & PIPE_CLEAR_STENCIL))) {
         value.f[0] = subpass->clear_depth;
         value.ui[1] = subpass->clear_stencil;
         fd6_clear_surface<CHIP>(ctx, ring, &pfb->zsbuf, &box2d,
                                 &value, fd6_unknown_8c01(pfb->zsbuf.format, buffers));
      }

      if (separate_stencil && (buffers & PIPE_CLEAR_STENCIL)) {
         value.ui[0] = subpass->clear_stencil;

         struct pipe_surface stencil_surf = pfb->zsbuf;
         stencil_surf.format = PIPE_FORMAT_S8_UINT;
         stencil_surf.texture = separate_stencil;

         fd6_clear_surface<CHIP>(ctx, ring, &stencil_surf, &box2d, &value, 0);
      }
   }

   fd6_emit_flushes<CHIP>(ctx, ring, FD6_FLUSH_CCU_COLOR | FD6_INVALIDATE_CCU_COLOR);

   trace_end_clears(&batch->trace, ring);
}

template <chip CHIP>
static void
fd6_emit_sysmem_prep(struct fd_batch *batch) assert_dt
{
   struct fd_ringbuffer *ring = batch->gmem;

   emit_lrz_clears<CHIP>(batch);

   fd6_emit_restore<CHIP>(batch, ring);
   fd6_event_write<CHIP>(batch->ctx, ring, FD_LRZ_FLUSH);

   if (batch->prologue) {
      if (!batch->nondraw) {
         trace_start_prologue(&batch->trace, ring);
      }
      fd6_emit_ib(ring, batch->prologue);
      if (!batch->nondraw) {
         trace_end_prologue(&batch->trace, ring);
      }
   }

   /* remaining setup below here does not apply to blit/compute: */
   if (batch->nondraw)
      return;

   struct pipe_framebuffer_state *pfb = &batch->framebuffer;

   if (pfb->width > 0 && pfb->height > 0)
      set_scissor(ring, 0, 0, pfb->width - 1, pfb->height - 1);
   else
      set_scissor(ring, 0, 0, 0, 0);

   set_tessfactor_bo<CHIP>(ring, batch);
   set_window_offset<CHIP>(ring, 0, 0);

   set_bin_size<CHIP>(ring, NULL, {
         .render_mode = RENDERING_PASS,
         .buffers_location = BUFFERS_IN_SYSMEM,
   });

   if (CHIP >= A7XX) {
      OUT_REG(ring, A7XX_RB_UNKNOWN_8812(0x3ff)); // all buffers in sysmem
      OUT_REG(ring, A7XX_RB_CCU_DBG_ECO_CNTL(batch->ctx->screen->info->a6xx.magic.RB_CCU_DBG_ECO_CNTL));
      OUT_REG(ring, A7XX_GRAS_UNKNOWN_8007(0x0));
      OUT_REG(ring, A6XX_GRAS_UNKNOWN_8110(0x2));
      OUT_REG(ring, A7XX_RB_UNKNOWN_8E09(0x4));
   }

   emit_marker6(ring, 7);
   OUT_PKT7(ring, CP_SET_MARKER, 1);
   OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_DIRECT_RENDER));
   emit_marker6(ring, 7);

   OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
   OUT_RING(ring, 0x0);

   /* blob controls "local" in IB2, but I think that is not required */
   OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_LOCAL, 1);
   OUT_RING(ring, 0x1);

   /* enable stream-out, with sysmem there is only one pass: */
   OUT_REG(ring, A6XX_VPC_SO_OVERRIDE(false));

   OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
   OUT_RING(ring, 0x1);

   emit_zs<CHIP>(batch->ctx, ring, &pfb->zsbuf, NULL);
   emit_mrt<CHIP>(ring, pfb, NULL);
   emit_msaa(ring, pfb->samples);
   patch_fb_read_sysmem<CHIP>(batch);

   emit_common_init<CHIP>(batch);
}

template <chip CHIP>
static void
fd6_emit_sysmem(struct fd_batch *batch)
   assert_dt
{
   struct fd_ringbuffer *ring = batch->gmem;
   struct fd_screen *screen = batch->ctx->screen;

   foreach_subpass (subpass, batch) {
      if (subpass->fast_cleared) {
         unsigned flushes = 0;
         if (subpass->fast_cleared & FD_BUFFER_COLOR)
            flushes |= FD6_INVALIDATE_CCU_COLOR;
         if (subpass->fast_cleared & (FD_BUFFER_DEPTH | FD_BUFFER_STENCIL))
            flushes |= FD6_INVALIDATE_CCU_DEPTH;

         fd6_emit_flushes<CHIP>(batch->ctx, ring, flushes);
         emit_sysmem_clears<CHIP>(batch, subpass);
      }

      fd6_emit_ccu_cntl<CHIP>(ring, screen, false);

      struct pipe_framebuffer_state *pfb = &batch->framebuffer;
      update_render_cntl<CHIP>(batch, pfb, false);

      emit_lrz<CHIP>(batch, subpass);

      fd6_emit_ib(ring, subpass->draw);
   }
}

template <chip CHIP>
static void
fd6_emit_sysmem_fini(struct fd_batch *batch) assert_dt
{
   struct fd_ringbuffer *ring = batch->gmem;

   emit_common_fini<CHIP>(batch);

   if (batch->tile_epilogue)
      fd6_emit_ib(batch->gmem, batch->tile_epilogue);

   if (batch->epilogue)
      fd6_emit_ib(batch->gmem, batch->epilogue);

   OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
   OUT_RING(ring, 0x0);

   fd6_event_write<CHIP>(batch->ctx, ring, FD_LRZ_FLUSH);

   fd6_emit_flushes<CHIP>(batch->ctx, ring,
                          FD6_FLUSH_CCU_COLOR |
                          FD6_FLUSH_CCU_DEPTH);
}

template <chip CHIP>
void
fd6_gmem_init(struct pipe_context *pctx)
   disable_thread_safety_analysis
{
   struct fd_context *ctx = fd_context(pctx);

   ctx->emit_tile_init = fd6_emit_tile_init<CHIP>;
   ctx->emit_tile_prep = fd6_emit_tile_prep<CHIP>;
   ctx->emit_tile_mem2gmem = fd6_emit_tile_mem2gmem;
   ctx->emit_tile_renderprep = fd6_emit_tile_renderprep;
   ctx->emit_tile = fd6_emit_tile<CHIP>;
   ctx->emit_tile_gmem2mem = fd6_emit_tile_gmem2mem;
   ctx->emit_tile_fini = fd6_emit_tile_fini<CHIP>;
   ctx->emit_sysmem_prep = fd6_emit_sysmem_prep<CHIP>;
   ctx->emit_sysmem = fd6_emit_sysmem<CHIP>;
   ctx->emit_sysmem_fini = fd6_emit_sysmem_fini<CHIP>;
}
FD_GENX(fd6_gmem_init);
