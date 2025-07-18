/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 */

#include "tu_pass.h"

#include "vk_util.h"
#include "vk_render_pass.h"

#include "tu_cmd_buffer.h"
#include "tu_device.h"
#include "tu_image.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"

static void
tu_render_pass_add_subpass_dep(struct tu_render_pass *pass,
                               const VkSubpassDependency2 *dep)
{
   uint32_t src = dep->srcSubpass;
   uint32_t dst = dep->dstSubpass;

   /* Ignore subpass self-dependencies as they allow the app to call
    * vkCmdPipelineBarrier() inside the render pass and the driver should only
    * do the barrier when called, not when starting the render pass.
    *
    * We cannot decide whether to allow gmem rendering before a barrier
    * is actually emitted, so we delay the decision until then.
    */
   if (src == dst)
      return;

   /* From the Vulkan 1.2.195 spec:
    *
    * "If an instance of VkMemoryBarrier2 is included in the pNext chain, srcStageMask,
    *  dstStageMask, srcAccessMask, and dstAccessMask parameters are ignored. The synchronization
    *  and access scopes instead are defined by the parameters of VkMemoryBarrier2."
    */
   const VkMemoryBarrier2 *barrier =
      vk_find_struct_const(dep->pNext, MEMORY_BARRIER_2);
   VkPipelineStageFlags2 src_stage_mask = barrier ? barrier->srcStageMask : dep->srcStageMask;
   VkAccessFlags2 src_access_mask = barrier ? barrier->srcAccessMask : dep->srcAccessMask;
   VkPipelineStageFlags2 dst_stage_mask = barrier ? barrier->dstStageMask : dep->dstStageMask;
   VkAccessFlags2 dst_access_mask = barrier ? barrier->dstAccessMask : dep->dstAccessMask;
   VkAccessFlags3KHR src_access_mask2 = 0, dst_access_mask2 = 0;
   if (barrier) {
      const VkMemoryBarrierAccessFlags3KHR *access3 =
         vk_find_struct_const(dep->pNext, MEMORY_BARRIER_ACCESS_FLAGS_3_KHR);
      if (access3) {
         src_access_mask2 = access3->srcAccessMask3;
         dst_access_mask2 = access3->dstAccessMask3;
      }
   }

   /* We can conceptually break down the process of rewriting a sysmem
    * renderpass into a gmem one into two parts:
    *
    * 1. Split each draw and multisample resolve into N copies, one for each
    * bin. (If hardware binning, add one more copy where the FS is disabled
    * for the binning pass). This is always allowed because the vertex stage
    * is allowed to run an arbitrary number of times and there are no extra
    * ordering constraints within a draw.
    * 2. Take the last copy of the second-to-last draw and slide it down to
    * before the last copy of the last draw. Repeat for each earlier draw
    * until the draw pass for the last bin is complete, then repeat for each
    * earlier bin until we finish with the first bin.
    *
    * During this rearranging process, we can't slide draws past each other in
    * a way that breaks the subpass dependencies. For each draw, we must slide
    * it past (copies of) the rest of the draws in the renderpass. We can
    * slide a draw past another if there isn't a dependency between them, or
    * if the dependenc(ies) are dependencies between framebuffer-space stages
    * only with the BY_REGION bit set. Note that this includes
    * self-dependencies, since these may result in pipeline barriers that also
    * break the rearranging process.
    */

   if (!vk_subpass_dependency_is_fb_local(dep, src_stage_mask, dst_stage_mask)) {
      perf_debug((struct tu_device *)pass->base.device, "Disabling gmem rendering due to invalid subpass dependency");
      for (int i = 0; i < ARRAY_SIZE(pass->gmem_pixels); i++)
         pass->gmem_pixels[i] = 0;
   }

   struct tu_subpass_barrier *dst_barrier;
   if (dst == VK_SUBPASS_EXTERNAL) {
      dst_barrier = &pass->end_barrier;
   } else {
      dst_barrier = &pass->subpasses[dst].start_barrier;
   }

   dst_barrier->src_stage_mask |= src_stage_mask;
   dst_barrier->dst_stage_mask |= dst_stage_mask;
   dst_barrier->src_access_mask |= src_access_mask;
   dst_barrier->dst_access_mask |= dst_access_mask;
   dst_barrier->src_access_mask2 |= src_access_mask2;
   dst_barrier->dst_access_mask2 |= dst_access_mask2;
}

/* We currently only care about undefined layouts, because we have to
 * flush/invalidate CCU for those. PREINITIALIZED is the same thing as
 * UNDEFINED for anything not linear tiled, but we don't know yet whether the
 * images used are tiled, so just assume they are.
 */

static bool
layout_undefined(VkImageLayout layout)
{
   return layout == VK_IMAGE_LAYOUT_UNDEFINED ||
          layout == VK_IMAGE_LAYOUT_PREINITIALIZED;
}

/* This implements the following bit of spec text:
 *
 *    If there is no subpass dependency from VK_SUBPASS_EXTERNAL to the
 *    first subpass that uses an attachment, then an implicit subpass
 *    dependency exists from VK_SUBPASS_EXTERNAL to the first subpass it is
 *    used in. The implicit subpass dependency only exists if there
 *    exists an automatic layout transition away from initialLayout.
 *    The subpass dependency operates as if defined with the
 *    following parameters:
 *
 *    VkSubpassDependency implicitDependency = {
 *        .srcSubpass = VK_SUBPASS_EXTERNAL;
 *        .dstSubpass = firstSubpass; // First subpass attachment is used in
 *        .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
 *        .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
 *        .srcAccessMask = 0;
 *        .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
 *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
 *        .dependencyFlags = 0;
 *    };
 *
 *    Similarly, if there is no subpass dependency from the last subpass
 *    that uses an attachment to VK_SUBPASS_EXTERNAL, then an implicit
 *    subpass dependency exists from the last subpass it is used in to
 *    VK_SUBPASS_EXTERNAL. The implicit subpass dependency only exists
 *    if there exists an automatic layout transition into finalLayout.
 *    The subpass dependency operates as if defined with the following
 *    parameters:
 *
 *    VkSubpassDependency implicitDependency = {
 *        .srcSubpass = lastSubpass; // Last subpass attachment is used in
 *        .dstSubpass = VK_SUBPASS_EXTERNAL;
 *        .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
 *        .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
 *        .srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
 *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
 *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
 *        .dstAccessMask = 0;
 *        .dependencyFlags = 0;
 *    };
 *
 * Note: currently this is the only use we have for layout transitions,
 * besides needing to invalidate CCU at the beginning, so we also flag
 * transitions from UNDEFINED here.
 */
static void
tu_render_pass_add_implicit_deps(struct tu_render_pass *pass,
                                 const VkRenderPassCreateInfo2 *info)
{
   const VkAttachmentDescription2* att = info->pAttachments;
   bool has_external_src[info->subpassCount];
   bool has_external_dst[info->subpassCount];
   bool att_used[pass->attachment_count];

   memset(has_external_src, 0, sizeof(has_external_src));
   memset(has_external_dst, 0, sizeof(has_external_dst));

   for (uint32_t i = 0; i < info->dependencyCount; i++) {
      uint32_t src = info->pDependencies[i].srcSubpass;
      uint32_t dst = info->pDependencies[i].dstSubpass;

      if (src == dst)
         continue;

      if (src == VK_SUBPASS_EXTERNAL)
         has_external_src[dst] = true;
      if (dst == VK_SUBPASS_EXTERNAL)
         has_external_dst[src] = true;
   }

   memset(att_used, 0, sizeof(att_used));

   for (unsigned i = 0; i < info->subpassCount; i++) {
      const VkSubpassDescription2 *subpass = &info->pSubpasses[i];
      bool src_implicit_dep = false;

      for (unsigned j = 0; j < subpass->inputAttachmentCount; j++) {
         uint32_t a = subpass->pInputAttachments[j].attachment;

         if (a == VK_ATTACHMENT_UNUSED)
            continue;

         uint32_t stencil_layout = vk_format_has_stencil(att[a].format) ?
               vk_att_ref_stencil_layout(&subpass->pInputAttachments[j], att) :
               VK_IMAGE_LAYOUT_UNDEFINED;
         uint32_t stencil_initial_layout = vk_att_desc_stencil_layout(&att[a], false);

         if ((att[a].initialLayout != subpass->pInputAttachments[j].layout ||
             stencil_initial_layout != stencil_layout) &&
             !att_used[a] && !has_external_src[i])
            src_implicit_dep = true;
         att_used[a] = true;
      }

      for (unsigned j = 0; j < subpass->colorAttachmentCount; j++) {
         uint32_t a = subpass->pColorAttachments[j].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;
         if (att[a].initialLayout != subpass->pColorAttachments[j].layout &&
             !att_used[a] && !has_external_src[i])
            src_implicit_dep = true;
         att_used[a] = true;
      }

      if (subpass->pDepthStencilAttachment &&
          subpass->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) {
         uint32_t a = subpass->pDepthStencilAttachment->attachment;
         uint32_t stencil_layout = vk_att_ref_stencil_layout(subpass->pDepthStencilAttachment, att);
         uint32_t stencil_initial_layout = vk_att_desc_stencil_layout(&att[a], false);

         if ((att[a].initialLayout != subpass->pDepthStencilAttachment->layout ||
             stencil_initial_layout != stencil_layout) &&
             !att_used[a] && !has_external_src[i]) {
            src_implicit_dep = true;
         }
         att_used[a] = true;
      }

      if (subpass->pResolveAttachments) {
         for (unsigned j = 0; j < subpass->colorAttachmentCount; j++) {
            uint32_t a = subpass->pResolveAttachments[j].attachment;
            if (a == VK_ATTACHMENT_UNUSED)
               continue;
            if (att[a].initialLayout != subpass->pResolveAttachments[j].layout &&
               !att_used[a] && !has_external_src[i])
               src_implicit_dep = true;
            att_used[a] = true;
         }
      }

      const VkSubpassDescriptionDepthStencilResolve *ds_resolve =
         vk_find_struct_const(subpass->pNext, SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);

      if (ds_resolve && ds_resolve->pDepthStencilResolveAttachment &&
          ds_resolve->pDepthStencilResolveAttachment->attachment != VK_ATTACHMENT_UNUSED) {
            uint32_t a = ds_resolve->pDepthStencilResolveAttachment->attachment;
            uint32_t stencil_layout = vk_att_ref_stencil_layout(ds_resolve->pDepthStencilResolveAttachment, att);
            uint32_t stencil_initial_layout = vk_att_desc_stencil_layout(&att[a], false);

            if ((att[a].initialLayout != subpass->pDepthStencilAttachment->layout ||
                stencil_initial_layout != stencil_layout) &&
                !att_used[a] && !has_external_src[i])
               src_implicit_dep = true;
            att_used[a] = true;
      }

      if (src_implicit_dep) {
         const VkSubpassDependency2 dep = {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = i,
            .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dependencyFlags = 0,
         };

         tu_render_pass_add_subpass_dep(pass, &dep);
      }
   }

   memset(att_used, 0, sizeof(att_used));

   for (int i = info->subpassCount - 1; i >= 0; i--) {
      const VkSubpassDescription2 *subpass = &info->pSubpasses[i];
      bool dst_implicit_dep = false;

      for (unsigned j = 0; j < subpass->inputAttachmentCount; j++) {
         uint32_t a = subpass->pInputAttachments[j].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;

         uint32_t stencil_layout = vk_format_has_stencil(att[a].format) ?
               vk_att_ref_stencil_layout(&subpass->pInputAttachments[j], att) :
               VK_IMAGE_LAYOUT_UNDEFINED;
         uint32_t stencil_final_layout = vk_att_desc_stencil_layout(&att[a], true);

         if ((att[a].finalLayout != subpass->pInputAttachments[j].layout ||
             stencil_final_layout != stencil_layout) &&
             !att_used[a] && !has_external_dst[i])
            dst_implicit_dep = true;
         att_used[a] = true;
      }

      for (unsigned j = 0; j < subpass->colorAttachmentCount; j++) {
         uint32_t a = subpass->pColorAttachments[j].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;
         if (att[a].finalLayout != subpass->pColorAttachments[j].layout &&
             !att_used[a] && !has_external_dst[i])
            dst_implicit_dep = true;
         att_used[a] = true;
      }

      if (subpass->pDepthStencilAttachment &&
          subpass->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) {
         uint32_t a = subpass->pDepthStencilAttachment->attachment;
         uint32_t stencil_layout = vk_att_ref_stencil_layout(subpass->pDepthStencilAttachment, att);
         uint32_t stencil_final_layout = vk_att_desc_stencil_layout(&att[a], true);

         if ((att[a].finalLayout != subpass->pDepthStencilAttachment->layout ||
             stencil_final_layout != stencil_layout) &&
             !att_used[a] && !has_external_dst[i]) {
            dst_implicit_dep = true;
         }
         att_used[a] = true;
      }

      if (subpass->pResolveAttachments) {
         for (unsigned j = 0; j < subpass->colorAttachmentCount; j++) {
            uint32_t a = subpass->pResolveAttachments[j].attachment;
            if (a == VK_ATTACHMENT_UNUSED)
               continue;
            if (att[a].finalLayout != subpass->pResolveAttachments[j].layout &&
                !att_used[a] && !has_external_dst[i])
               dst_implicit_dep = true;
            att_used[a] = true;
         }
      }

      const VkSubpassDescriptionDepthStencilResolve *ds_resolve =
         vk_find_struct_const(subpass->pNext, SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);

      if (ds_resolve && ds_resolve->pDepthStencilResolveAttachment &&
          ds_resolve->pDepthStencilResolveAttachment->attachment != VK_ATTACHMENT_UNUSED) {
            uint32_t a = ds_resolve->pDepthStencilResolveAttachment->attachment;
            uint32_t stencil_layout = vk_att_ref_stencil_layout(ds_resolve->pDepthStencilResolveAttachment, att);
            uint32_t stencil_final_layout = vk_att_desc_stencil_layout(&att[a], true);

            if ((att[a].finalLayout != subpass->pDepthStencilAttachment->layout ||
                stencil_final_layout != stencil_layout) &&
                !att_used[a] && !has_external_src[i])
               dst_implicit_dep = true;
            att_used[a] = true;
      }

      if (dst_implicit_dep) {
         VkSubpassDependency2 dep = {
            .srcSubpass = i,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            .srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = 0,
            .dependencyFlags = 0,
         };
         tu_render_pass_add_subpass_dep(pass, &dep);
      }
   }

   /* Handle UNDEFINED transitions, similar to the handling in tu_barrier().
    * Assume that if an attachment has an initial layout of UNDEFINED, it gets
    * transitioned eventually.
    */
   for (unsigned i = 0; i < info->attachmentCount; i++) {
      if (layout_undefined(att[i].initialLayout)) {
         if (vk_format_is_depth_or_stencil(att[i].format)) {
            pass->subpasses[0].start_barrier.incoherent_ccu_depth = true;
         } else {
            pass->subpasses[0].start_barrier.incoherent_ccu_color = true;
         }
      }
   }
}

/* If an input attachment is used without an intervening write to the same
 * attachment, then we can just use the original image, even in GMEM mode.
 * This is an optimization, but it's also important because it allows us to
 * avoid having to invalidate UCHE at the beginning of each tile due to it
 * becoming invalid. The only reads of GMEM via UCHE should be after an
 * earlier subpass modified it, which only works if there's already an
 * appropriate dependency that will add the CACHE_INVALIDATE anyway. We
 * don't consider this in the dependency code, so this is also required for
 * correctness.
 */
static void
tu_render_pass_patch_input_gmem(struct tu_render_pass *pass)
{
   bool written[pass->attachment_count];

   memset(written, 0, sizeof(written));

   for (unsigned i = 0; i < pass->subpass_count; i++) {
      struct tu_subpass *subpass = &pass->subpasses[i];

      for (unsigned j = 0; j < subpass->input_count; j++) {
         uint32_t a = subpass->input_attachments[j].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;
         subpass->input_attachments[j].patch_input_gmem = written[a];
      }

      for (unsigned j = 0; j < subpass->color_count; j++) {
         uint32_t a = subpass->color_attachments[j].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;
         written[a] = true;

         for (unsigned k = 0; k < subpass->input_count; k++) {
            if (subpass->input_attachments[k].attachment == a &&
                !subpass->input_attachments[k].patch_input_gmem) {
               /* For render feedback loops, we have no idea whether the use
                * as a color attachment or input attachment will come first,
                * so we have to always use GMEM in case the color attachment
                * comes first and defensively invalidate UCHE in case the
                * input attachment comes first.
                */
               subpass->feedback_invalidate = true;
               subpass->input_attachments[k].patch_input_gmem = true;
            }
         }
      }

      for (unsigned j = 0; j < subpass->resolve_count; j++) {
         uint32_t a = subpass->resolve_attachments[j].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;
         written[a] = true;
      }

      if (subpass->depth_stencil_attachment.attachment != VK_ATTACHMENT_UNUSED) {
         written[subpass->depth_stencil_attachment.attachment] = true;
         for (unsigned k = 0; k < subpass->input_count; k++) {
            if (subpass->input_attachments[k].attachment ==
                subpass->depth_stencil_attachment.attachment &&
                !subpass->input_attachments[k].patch_input_gmem) {
               subpass->feedback_invalidate = true;
               subpass->input_attachments[k].patch_input_gmem = true;
            }
         }
      }
   }
}

static void
tu_render_pass_check_feedback_loop(struct tu_render_pass *pass)
{
   for (unsigned i = 0; i < pass->subpass_count; i++) {
      struct tu_subpass *subpass = &pass->subpasses[i];

      for (unsigned j = 0; j < subpass->color_count; j++) {
         uint32_t a = subpass->color_attachments[j].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;
         for (unsigned k = 0; k < subpass->input_count; k++) {
            if (subpass->input_attachments[k].attachment == a) {
               subpass->feedback_loop_color = true;
               break;
            }
         }
      }

      if (subpass->depth_stencil_attachment.attachment != VK_ATTACHMENT_UNUSED) {
         for (unsigned k = 0; k < subpass->input_count; k++) {
            if (subpass->input_attachments[k].attachment ==
                subpass->depth_stencil_attachment.attachment) {
               subpass->feedback_loop_ds = true;
               break;
            }
         }
      }
   }
}

static void update_samples(struct tu_subpass *subpass,
                           VkSampleCountFlagBits samples)
{
   assert(subpass->samples == 0 || subpass->samples == samples);
   subpass->samples = samples;
}

static void
tu_render_pass_calc_views(struct tu_render_pass *pass)
{
   uint32_t view_mask = 0;
   for (unsigned i = 0; i < pass->subpass_count; i++)
      view_mask |= pass->subpasses[i].multiview_mask;
   pass->num_views = util_last_bit(view_mask);
}

/* If there are any multisample attachments with a load op other than
 * clear/don't-care/none and store op other than don't-care/none, then we'd
 * have to load/store a scaled multisample image which doesn't make much
 * sense. Just disable fragment_density_map in this case.
 */
static bool
tu_render_pass_disable_fdm(struct tu_device *dev, struct tu_render_pass *pass)
{
   if (TU_DEBUG(NOFDM))
      return true;

   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];

      if (att->samples > 1 &&
          (att->load || att->load_stencil ||
           att->store || att->store_stencil)) {
         perf_debug(dev, "Disabling fragment density map due to %s of multisample attachment",
                    (att->load || att->load_stencil) ? "load" : "store");
         return true;
      }
   }

   return false;
}

static void
tu_render_pass_calc_hash(struct tu_render_pass *pass)
{
   #define HASH(hash, data) XXH64(&(data), sizeof(data), hash)

   uint64_t hash = HASH(0, pass->attachment_count);
   hash = XXH64(pass->attachments,
         pass->attachment_count * sizeof(pass->attachments[0]), hash);
   hash = HASH(hash, pass->subpass_count);
   for (unsigned i = 0; i < pass->subpass_count; i++) {
      hash = HASH(hash, pass->subpasses[i].samples);
      hash = HASH(hash, pass->subpasses[i].input_count);
      hash = HASH(hash, pass->subpasses[i].color_count);
      hash = HASH(hash, pass->subpasses[i].resolve_count);
   }

   pass->autotune_hash = hash;

   #undef HASH
}

static void
tu_render_pass_cond_config(struct tu_device *device,
                           struct tu_render_pass *pass)
{
   /* With generic clears CmdClearAttachments isn't a draw and doesn't
    * contribute to bin's geometry.
    */
   if (device->physical_device->info->a7xx.has_generic_clear)
      return;

   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];

      /* When there is no geometry in a tile, and there is no other operations to
       * read/write the tile, we can skip load/store.
       *
       * The only other operations are clear and resolve, which disable
       * conditional load/store.
       */
      att->cond_load_allowed =
         (att->load || att->load_stencil) && !att->clear_mask && !att->will_be_resolved;
      att->cond_store_allowed =
         (att->store || att->store_stencil) && !att->clear_mask;

      pass->has_cond_load_store |=
         att->cond_load_allowed | att->cond_store_allowed;
   }
}

/**
 * Checks if the pass should allow IB2 skipping.
 *
 * If any stores or resolves would be emitted in a non-final subpass, then we
 * need to turn off IB2 skipping to make sure that we don't early-return before
 * they happen.
 */
static void
tu_render_pass_check_ib2_skip(struct tu_render_pass *pass)
{
   pass->allow_ib2_skipping = true;
   for (int i = 0; i < pass->attachment_count; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];
      if ((att->store || att->store_stencil) &&
          att->last_subpass_idx != pass->subpass_count - 1) {
         pass->allow_ib2_skipping = false;
         return;
      }
   }

   for (int i = 0; i < pass->subpass_count - 1; i++) {
      struct tu_subpass *subpass = &pass->subpasses[i];
      for (int j = 0; j < subpass->resolve_count; j++) {
         if (subpass->resolve_attachments[j].attachment != VK_ATTACHMENT_UNUSED) {
            pass->allow_ib2_skipping = false;
            return;
         }
      }
   }
}

struct tu_gmem_alloc {
   uint32_t gmem_offset;
   uint32_t cpp;
   uint32_t first_subpass;
   uint32_t last_subpass;
};

static struct tu_gmem_alloc *
tu_gmem_alloc(struct tu_gmem_alloc *allocs,
              uint32_t *num_allocs,
              uint32_t cpp,
              uint32_t first_subpass,
              uint32_t last_subpass)
{
   struct tu_gmem_alloc *alloc = NULL;

   /* Find an allocation that is free during our subpass range, with the
    * closet usable cpp.
    */
   for (int i = 0; i < *num_allocs; i++) {
      if (!(allocs[i].first_subpass > last_subpass ||
            allocs[i].last_subpass < first_subpass)) {
         continue;
      }

      if (allocs[i].cpp == cpp) {
         alloc = &allocs[i];
         break;
      }
      if (allocs[i].cpp > cpp && (!alloc || alloc->cpp > allocs[i].cpp))
         alloc = &allocs[i];
   }
   if (alloc) {
      /* Expand the range of the existing allocation */
      alloc->first_subpass = MIN2(alloc->first_subpass, first_subpass);
      alloc->last_subpass = MAX2(alloc->last_subpass, last_subpass);
   } else {
      /* Make a new gmem allocation for this cpp. */
      alloc = &allocs[(*num_allocs)++];
      alloc->cpp = cpp;
      alloc->first_subpass = first_subpass;
      alloc->last_subpass = last_subpass;
   }

   return alloc;
}

static void
tu_render_pass_gmem_config(struct tu_render_pass *pass,
                           const struct tu_physical_device *phys_dev)
{
   for (enum tu_gmem_layout layout = (enum tu_gmem_layout) 0;
        layout < TU_GMEM_LAYOUT_COUNT;
        layout = (enum tu_gmem_layout)(layout + 1)) {
      /* log2(gmem_align/(tile_align_w*tile_align_h)) */
      uint32_t block_align_shift = 3;
      uint32_t tile_align_w = phys_dev->info->tile_align_w;
      uint32_t gmem_align = (1 << block_align_shift) * tile_align_w * phys_dev->info->tile_align_h;

      /* gmem allocations to make, possibly shared between attachments. Each
       * attachment may have 2 allocations, to handle separate stencil.
       */
      struct tu_gmem_alloc gmem_alloc[2 * pass->attachment_count];
      uint32_t num_gmem_alloc = 0;
      struct tu_gmem_alloc *att_gmem_alloc[2 * pass->attachment_count];
      for (int i = 0; i < ARRAY_SIZE(att_gmem_alloc); i++)
         att_gmem_alloc[i] = NULL;

      for (uint32_t i = 0; i < pass->attachment_count; i++) {
         struct tu_render_pass_attachment *att = &pass->attachments[i];
         bool cpp1 = (att->cpp == 1);
         if (att->gmem) {
            att_gmem_alloc[i * 2] =
               tu_gmem_alloc(gmem_alloc, &num_gmem_alloc, att->cpp,
                           att->first_subpass_idx, att->last_subpass_idx);

            /* take into account the separate stencil: */
            if (att->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
               cpp1 = (att->samples == 1);
               att_gmem_alloc[i * 2 + 1] =
                  tu_gmem_alloc(gmem_alloc, &num_gmem_alloc, att->samples,
                              att->first_subpass_idx, att->last_subpass_idx);
            }

            /* texture pitch must be aligned to 64, use a tile_align_w that is
             * a multiple of 64 for cpp==1 attachment to work as input
             * attachment
             */
            if (cpp1 && tile_align_w % 64 != 0) {
               tile_align_w *= 2;
               block_align_shift -= 1;
            }
         }
      }

      uint32_t cpp_total = 0;
      uint32_t min_cpp = UINT32_MAX;
      for (int i = 0; i < num_gmem_alloc; i++) {
         cpp_total += gmem_alloc[i].cpp;
         min_cpp = MIN2(min_cpp, gmem_alloc[i].cpp);
      }

      pass->tile_align_w = tile_align_w;
      pass->min_cpp = min_cpp;

      /* no gmem attachments */
      if (cpp_total == 0) {
         /* any non-zero value so tiling config works with no attachments */
         for (int i = 0; i < ARRAY_SIZE(pass->gmem_pixels); i++)
            pass->gmem_pixels[i] = 1024*1024;
         return;
      }

      /* TODO: this algorithm isn't optimal
       * for example, two attachments with cpp = {1, 4}
       * result:  nblocks = {12, 52}, pixels = 196608
       * optimal: nblocks = {13, 51}, pixels = 208896
       */
      uint32_t gmem_size = layout == TU_GMEM_LAYOUT_FULL
                              ? phys_dev->usable_gmem_size_gmem
                              : phys_dev->ccu_offset_gmem;
      uint32_t gmem_blocks = gmem_size / gmem_align;
      uint32_t offset = 0, pixels = ~0u, i;
      for (i = 0; i < num_gmem_alloc; i++) {
         struct tu_gmem_alloc *alloc = &gmem_alloc[i];

         uint32_t align = MAX2(1, alloc->cpp >> block_align_shift);
         uint32_t nblocks = MAX2((gmem_blocks * alloc->cpp / cpp_total) & ~(align - 1), align);

         if (nblocks > gmem_blocks) {
            /* gmem layout impossible */
            pass->gmem_pixels[layout] = 0;
            continue;
         }

         gmem_blocks -= nblocks;
         cpp_total -= alloc->cpp;
         alloc->gmem_offset = offset;
         offset += nblocks * gmem_align;
         pixels = MIN2(pixels, nblocks * gmem_align / alloc->cpp);
      }

      pass->gmem_pixels[layout] = pixels;

      for (i = 0; i < pass->attachment_count; i++) {
         struct tu_render_pass_attachment *att = &pass->attachments[i];
         if (!att->gmem)
            continue;

         att->gmem_offset[layout] = att_gmem_alloc[2 * i]->gmem_offset;
         if (att->format == VK_FORMAT_D32_SFLOAT_S8_UINT)
            att->gmem_offset_stencil[layout] = att_gmem_alloc[2 * i + 1]->gmem_offset;
      }
   }
}

static void
tu_render_pass_bandwidth_config(struct tu_render_pass *pass)
{
   pass->gmem_bandwidth_per_pixel = 0;
   pass->sysmem_bandwidth_per_pixel = 0;

   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      const struct tu_render_pass_attachment *att = &pass->attachments[i];

      /* approximate tu_load_gmem_attachment */
      if (att->load)
         pass->gmem_bandwidth_per_pixel += att->cpp;

      /* approximate tu_store_gmem_attachment */
      if (att->store)
         pass->gmem_bandwidth_per_pixel += att->cpp;

      /* approximate tu_clear_sysmem_attachment */
      if (att->clear_mask)
         pass->sysmem_bandwidth_per_pixel += att->cpp;

      /* approximate tu6_emit_sysmem_resolves */
      if (att->will_be_resolved) {
         pass->sysmem_bandwidth_per_pixel +=
            att->cpp + att->cpp / att->samples;
      }
   }
}

static void
attachment_set_ops(struct tu_device *device,
                   struct tu_render_pass_attachment *att,
                   VkAttachmentLoadOp load_op,
                   VkAttachmentLoadOp stencil_load_op,
                   VkAttachmentStoreOp store_op,
                   VkAttachmentStoreOp stencil_store_op)
{
   if (unlikely(device->instance->dont_care_as_load)) {
      if (load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE)
         load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
      if (stencil_load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE)
         stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
   }

   /* load/store ops */
   att->clear_mask =
      (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) ? VK_IMAGE_ASPECT_COLOR_BIT : 0;
   att->load = (load_op == VK_ATTACHMENT_LOAD_OP_LOAD);
   att->store = (store_op == VK_ATTACHMENT_STORE_OP_STORE);

   bool stencil_clear = (stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR);
   bool stencil_load = (stencil_load_op == VK_ATTACHMENT_LOAD_OP_LOAD);
   bool stencil_store = (stencil_store_op == VK_ATTACHMENT_STORE_OP_STORE);

   switch (att->format) {
   case VK_FORMAT_D24_UNORM_S8_UINT: /* || stencil load/store */
      if (att->clear_mask)
         att->clear_mask = VK_IMAGE_ASPECT_DEPTH_BIT;
      if (stencil_clear)
         att->clear_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
      if (stencil_load)
         att->load = true;
      if (stencil_store)
         att->store = true;
      /* If depth or stencil is passthrough (STORE_OP_NONE), then we need to
       * preserve the contents when storing by loading even if neither
       * component needs to be loaded.
       */
      if ((store_op == VK_ATTACHMENT_STORE_OP_NONE_EXT ||
           stencil_store_op == VK_ATTACHMENT_STORE_OP_NONE_EXT) &&
          att->store) {
         att->load = true;
      }
      break;
   case VK_FORMAT_S8_UINT: /* replace load/store with stencil load/store */
      att->clear_mask = stencil_clear ? VK_IMAGE_ASPECT_COLOR_BIT : 0;
      att->load = stencil_load;
      att->store = stencil_store;
      break;
   case VK_FORMAT_D32_SFLOAT_S8_UINT: /* separate stencil */
      if (att->clear_mask)
         att->clear_mask = VK_IMAGE_ASPECT_DEPTH_BIT;
      if (stencil_clear)
         att->clear_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
      if (stencil_load)
         att->load_stencil = true;
      if (stencil_store)
         att->store_stencil = true;
      break;
   default:
      break;
   }
}

static bool
is_depth_stencil_resolve_enabled(const VkSubpassDescriptionDepthStencilResolve *depth_stencil_resolve)
{
   if (depth_stencil_resolve &&
       depth_stencil_resolve->pDepthStencilResolveAttachment &&
       depth_stencil_resolve->pDepthStencilResolveAttachment->attachment != VK_ATTACHMENT_UNUSED) {
      return true;
   }
   return false;
}

static void
tu_subpass_use_attachment(struct tu_render_pass *pass, int i, uint32_t a, const VkRenderPassCreateInfo2 *pCreateInfo)
{
   struct tu_subpass *subpass = &pass->subpasses[i];
   struct tu_render_pass_attachment *att = &pass->attachments[a];

   att->gmem = true;
   update_samples(subpass, pCreateInfo->pAttachments[a].samples);
   att->clear_views |= subpass->multiview_mask;

   /* Loads and clears are emitted at the start of the subpass that needs them. */
   att->first_subpass_idx = MIN2(i, att->first_subpass_idx);

   /* Stores are emitted after the last subpass using them. */
   att->last_subpass_idx = MAX2(i, att->last_subpass_idx);
}

static void
tu_subpass_resolve_attachment(struct tu_render_pass *pass, int i, uint32_t dst_a, uint32_t src_a)
{
   if (src_a != VK_ATTACHMENT_UNUSED && dst_a != VK_ATTACHMENT_UNUSED) {
      struct tu_render_pass_attachment *src_att = &pass->attachments[src_a];
      struct tu_render_pass_attachment *dst_att = &pass->attachments[dst_a];
      src_att->will_be_resolved = true;

      src_att->first_subpass_idx = MIN2(i, src_att->first_subpass_idx);
      src_att->last_subpass_idx = MAX2(i, src_att->last_subpass_idx);
      dst_att->first_subpass_idx = MIN2(i, dst_att->first_subpass_idx);
      dst_att->last_subpass_idx = MAX2(i, dst_att->last_subpass_idx);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateRenderPass2(VkDevice _device,
                     const VkRenderPassCreateInfo2 *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(tu_device, device, _device);

   if (TU_DEBUG(DYNAMIC))
      return vk_common_CreateRenderPass2(_device, pCreateInfo, pAllocator,
                                         pRenderPass);

   struct tu_render_pass *pass;
   size_t size;
   size_t attachments_offset;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2);

   size = sizeof(*pass);
   size += pCreateInfo->subpassCount * sizeof(pass->subpasses[0]);
   attachments_offset = size;
   size += pCreateInfo->attachmentCount * sizeof(pass->attachments[0]);

   pass = (struct tu_render_pass *) vk_object_zalloc(
      &device->vk, pAllocator, size, VK_OBJECT_TYPE_RENDER_PASS);
   if (pass == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pass->attachment_count = pCreateInfo->attachmentCount;
   pass->subpass_count = pCreateInfo->subpassCount;
   pass->attachments =
      (struct tu_render_pass_attachment *) ((char *) pass +
                                            attachments_offset);

   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];

      att->format = pCreateInfo->pAttachments[i].format;
      att->samples = pCreateInfo->pAttachments[i].samples;
      /* for d32s8, cpp is for the depth image, and
       * att->samples will be used as the cpp for the stencil image
       */
      if (att->format == VK_FORMAT_D32_SFLOAT_S8_UINT)
         att->cpp = 4 * att->samples;
      else
         att->cpp = vk_format_get_blocksize(att->format) * att->samples;
      /* Initially not allocated into gmem, tu_subpass_use_attachment() will move it there. */
      att->gmem = false;

      VkAttachmentLoadOp loadOp = pCreateInfo->pAttachments[i].loadOp;
      VkAttachmentLoadOp stencilLoadOp = pCreateInfo->pAttachments[i].stencilLoadOp;

      attachment_set_ops(device, att, loadOp, stencilLoadOp,
                         pCreateInfo->pAttachments[i].storeOp,
                         pCreateInfo->pAttachments[i].stencilStoreOp);

      att->first_subpass_idx = VK_SUBPASS_EXTERNAL;
      att->last_subpass_idx = 0;
   }
   uint32_t subpass_attachment_count = 0;
   struct tu_subpass_attachment *p;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription2 *desc = &pCreateInfo->pSubpasses[i];
      const VkSubpassDescriptionDepthStencilResolve *ds_resolve =
         vk_find_struct_const(desc->pNext, SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);

      subpass_attachment_count +=
         desc->inputAttachmentCount + desc->colorAttachmentCount +
         (desc->pResolveAttachments ? desc->colorAttachmentCount : 0) +
         (is_depth_stencil_resolve_enabled(ds_resolve) ? 1 : 0);
   }

   if (subpass_attachment_count) {
      pass->subpass_attachments = (struct tu_subpass_attachment *) vk_alloc2(
         &device->vk.alloc, pAllocator,
         subpass_attachment_count * sizeof(struct tu_subpass_attachment), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (pass->subpass_attachments == NULL) {
         vk_object_free(&device->vk, pAllocator, pass);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   } else
      pass->subpass_attachments = NULL;

   const VkRenderPassFragmentDensityMapCreateInfoEXT *fdm_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT);
   if (fdm_info && !tu_render_pass_disable_fdm(device, pass)) {
      pass->fragment_density_map.attachment =
         fdm_info->fragmentDensityMapAttachment.attachment;
      pass->has_fdm = true;
      if (pCreateInfo->flags & VK_RENDER_PASS_CREATE_PER_LAYER_FRAGMENT_DENSITY_BIT_VALVE)
         pass->has_layered_fdm = true;
   } else {
      pass->fragment_density_map.attachment = VK_ATTACHMENT_UNUSED;
   }

   if (TU_DEBUG(FDM) && !tu_render_pass_disable_fdm(device, pass))
      pass->has_fdm = true;

   p = pass->subpass_attachments;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription2 *desc = &pCreateInfo->pSubpasses[i];
      const VkSubpassDescriptionDepthStencilResolve *ds_resolve =
         vk_find_struct_const(desc->pNext, SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
      struct tu_subpass *subpass = &pass->subpasses[i];

      subpass->input_count = desc->inputAttachmentCount;
      subpass->color_count = desc->colorAttachmentCount;
      subpass->resolve_count = 0;
      subpass->resolve_depth_stencil = is_depth_stencil_resolve_enabled(ds_resolve);
      subpass->samples = (VkSampleCountFlagBits) 0;
      subpass->srgb_cntl = 0;
      subpass->legacy_dithering_enabled = desc->flags &
         VK_SUBPASS_DESCRIPTION_ENABLE_LEGACY_DITHERING_BIT_EXT;

      const BITMASK_ENUM(VkSubpassDescriptionFlagBits) raster_order_access_bits =
         VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT |
         VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_EXT |
         VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_STENCIL_ACCESS_BIT_EXT;

      subpass->raster_order_attachment_access = raster_order_access_bits & desc->flags;

      subpass->multiview_mask = desc->viewMask;

      if (desc->inputAttachmentCount > 0) {
         subpass->input_attachments = p;
         p += desc->inputAttachmentCount;

         for (uint32_t j = 0; j < desc->inputAttachmentCount; j++) {
            uint32_t a = desc->pInputAttachments[j].attachment;
            subpass->input_attachments[j].attachment = a;
            if (a != VK_ATTACHMENT_UNUSED) {
               struct tu_render_pass_attachment *att = &pass->attachments[a];
               /* Note: attachments only used as input attachments will be read
                * directly instead of through gmem, so we don't mark input
                * attachments as needing gmem.
                */
               att->first_subpass_idx = MIN2(i, att->first_subpass_idx);
               att->last_subpass_idx = MAX2(i, att->last_subpass_idx);
            }
         }
      }

      if (desc->colorAttachmentCount > 0) {
         subpass->color_attachments = p;
         p += desc->colorAttachmentCount;

         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            uint32_t a = desc->pColorAttachments[j].attachment;
            subpass->color_attachments[j].attachment = a;

            if (a != VK_ATTACHMENT_UNUSED) {
               tu_subpass_use_attachment(pass, i, a, pCreateInfo);

               if (vk_format_is_srgb(pass->attachments[a].format))
                  subpass->srgb_cntl |= 1 << j;
            }
         }
      }

      subpass->resolve_attachments = (desc->pResolveAttachments || subpass->resolve_depth_stencil) ? p : NULL;
      if (desc->pResolveAttachments) {
         p += desc->colorAttachmentCount;
         subpass->resolve_count += desc->colorAttachmentCount;
         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            uint32_t a = desc->pResolveAttachments[j].attachment;
            uint32_t src_a = desc->pColorAttachments[j].attachment;
            subpass->resolve_attachments[j].attachment = a;

            tu_subpass_resolve_attachment(pass, i, a, src_a);
         }
      }

      if (subpass->resolve_depth_stencil) {
         p++;
         subpass->resolve_count++;
         uint32_t a = ds_resolve->pDepthStencilResolveAttachment->attachment;
         uint32_t src_a = desc->pDepthStencilAttachment->attachment;
         subpass->resolve_attachments[subpass->resolve_count - 1].attachment = a;

         tu_subpass_resolve_attachment(pass, i, a, src_a);
      }

      uint32_t a = desc->pDepthStencilAttachment ?
         desc->pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED;
      subpass->depth_stencil_attachment.attachment = a;
      subpass->depth_used = a != VK_ATTACHMENT_UNUSED;
      subpass->stencil_used = a != VK_ATTACHMENT_UNUSED;
      if (a != VK_ATTACHMENT_UNUSED) {
         tu_subpass_use_attachment(pass, i, a, pCreateInfo);
      }

      const VkFragmentShadingRateAttachmentInfoKHR *fsr_att_info =
         vk_find_struct_const(desc->pNext,
                              FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
      if (fsr_att_info && fsr_att_info->pFragmentShadingRateAttachment &&
          fsr_att_info->pFragmentShadingRateAttachment->attachment !=
             VK_ATTACHMENT_UNUSED) {
         subpass->fsr_attachment =
            fsr_att_info->pFragmentShadingRateAttachment->attachment;
         subpass->fsr_attachment_texel_size =
            fsr_att_info->shadingRateAttachmentTexelSize;
      } else {
         subpass->fsr_attachment = VK_ATTACHMENT_UNUSED;
      }
   }

   tu_render_pass_patch_input_gmem(pass);

   tu_render_pass_check_feedback_loop(pass);

   /* disable unused attachments */
   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];
      if (!att->gmem) {
         att->clear_mask = 0;
         att->load = false;
      }
   }

   tu_render_pass_check_ib2_skip(pass);
   tu_render_pass_cond_config(device, pass);
   tu_render_pass_gmem_config(pass, device->physical_device);
   tu_render_pass_bandwidth_config(pass);
   tu_render_pass_calc_views(pass);
   tu_render_pass_calc_hash(pass);

   for (unsigned i = 0; i < pCreateInfo->dependencyCount; ++i) {
      tu_render_pass_add_subpass_dep(pass, &pCreateInfo->pDependencies[i]);
   }

   tu_render_pass_add_implicit_deps(pass, pCreateInfo);

   *pRenderPass = tu_render_pass_to_handle(pass);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroyRenderPass(VkDevice _device,
                     VkRenderPass _pass,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(tu_device, device, _device);

   if (TU_DEBUG(DYNAMIC)) {
      vk_common_DestroyRenderPass(_device, _pass, pAllocator);
      return;
   }

   VK_FROM_HANDLE(tu_render_pass, pass, _pass);

   if (!_pass)
      return;

   vk_free2(&device->vk.alloc, pAllocator, pass->subpass_attachments);
   vk_object_free(&device->vk, pAllocator, pass);
}

static void
tu_setup_dynamic_attachment(struct tu_render_pass_attachment *att,
                            struct tu_image_view *view)
{
   *att = {};
   att->format = view->vk.format;
   att->samples = (VkSampleCountFlagBits) view->image->layout->nr_samples;

   /* for d32s8, cpp is for the depth image, and
    * att->samples will be used as the cpp for the stencil image
    */
   if (att->format == VK_FORMAT_D32_SFLOAT_S8_UINT)
      att->cpp = 4 * att->samples;
   else
      att->cpp = vk_format_get_blocksize(att->format) * att->samples;
}

void
tu_setup_dynamic_render_pass(struct tu_cmd_buffer *cmd_buffer,
                             const VkRenderingInfo *info)
{
   struct tu_device *device = cmd_buffer->device;
   struct tu_render_pass *pass = &cmd_buffer->dynamic_pass;
   struct tu_subpass *subpass = &cmd_buffer->dynamic_subpass;

   *pass = {};
   *subpass = {};

   pass->subpass_count = 1;
   pass->attachments = cmd_buffer->dynamic_rp_attachments;

   subpass->color_count = subpass->resolve_count = info->colorAttachmentCount;
   subpass->input_count = info->colorAttachmentCount + 1;
   subpass->color_attachments = cmd_buffer->dynamic_color_attachments;
   subpass->input_attachments = cmd_buffer->dynamic_input_attachments;
   subpass->resolve_attachments = cmd_buffer->dynamic_resolve_attachments;
   subpass->multiview_mask = info->viewMask;
   subpass->legacy_dithering_enabled = info->flags &
      VK_RENDERING_ENABLE_LEGACY_DITHERING_BIT_EXT;

   /* Because we don't know with dynamic rendering when input attachments
    * are used relative to color attachments, we have to always assume
    * they may be written as a color or depth/stencil attachment first. This
    * means we can't apply the optimization in
    * tu_render_pass_patch_input_gmem(). Initialize this for all possible
    * attachments now so we don't have to update it later.
    */
   for (unsigned i = 0; i < ARRAY_SIZE(cmd_buffer->dynamic_input_attachments);
        i++) {
      subpass->input_attachments[i].patch_input_gmem = true;
   }

   uint32_t a = 0;
   for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[a];
      const VkRenderingAttachmentInfo *att_info = &info->pColorAttachments[i];

      if (att_info->imageView == VK_NULL_HANDLE) {
         subpass->color_attachments[i].attachment = VK_ATTACHMENT_UNUSED;
         subpass->input_attachments[i + 1].attachment = VK_ATTACHMENT_UNUSED;
         subpass->resolve_attachments[i].attachment = VK_ATTACHMENT_UNUSED;
         continue;
      }

      VK_FROM_HANDLE(tu_image_view, view, att_info->imageView);
      tu_setup_dynamic_attachment(att, view);
      att->gmem = true;
      att->clear_views = info->viewMask;
      attachment_set_ops(device, att, att_info->loadOp,
                         VK_ATTACHMENT_LOAD_OP_DONT_CARE, att_info->storeOp,
                         VK_ATTACHMENT_STORE_OP_DONT_CARE);
      subpass->color_attachments[i].attachment = a++;
      subpass->input_attachments[i + 1].attachment =
         subpass->color_attachments[i].attachment;
      subpass->input_attachments[i + 1].patch_input_gmem = true;

      subpass->samples = (VkSampleCountFlagBits) view->image->layout->nr_samples;

      if (vk_format_is_srgb(view->vk.format))
         subpass->srgb_cntl |= 1 << i;

      if (att_info->resolveMode != VK_RESOLVE_MODE_NONE) {
         struct tu_render_pass_attachment *resolve_att = &pass->attachments[a];
         VK_FROM_HANDLE(tu_image_view, resolve_view, att_info->resolveImageView);
         tu_setup_dynamic_attachment(resolve_att, resolve_view);
         resolve_att->gmem = false;
         attachment_set_ops(
            device, resolve_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE);
         subpass->resolve_attachments[i].attachment = a++;
         att->will_be_resolved = true;
      } else {
         subpass->resolve_attachments[i].attachment = VK_ATTACHMENT_UNUSED;
         att->will_be_resolved = false;
      }
   }

   if (info->pDepthAttachment || info->pStencilAttachment) {
      const struct VkRenderingAttachmentInfo *common_info =
         (info->pDepthAttachment &&
          info->pDepthAttachment->imageView != VK_NULL_HANDLE) ?
         info->pDepthAttachment :
         info->pStencilAttachment;

      if (common_info && common_info->imageView != VK_NULL_HANDLE) {
         VK_FROM_HANDLE(tu_image_view, view, common_info->imageView);

         struct tu_render_pass_attachment *att = &pass->attachments[a];
         tu_setup_dynamic_attachment(att, view);
         att->gmem = true;
         att->clear_views = info->viewMask;
         subpass->depth_stencil_attachment.attachment = a++;
         subpass->input_attachments[0].attachment =
            subpass->depth_stencil_attachment.attachment;
         subpass->input_attachments[0].patch_input_gmem = true;

         subpass->depth_used = (bool) info->pDepthAttachment;
         subpass->stencil_used = (bool) info->pStencilAttachment;

         attachment_set_ops(
            device, att,
            (info->pDepthAttachment && info->pDepthAttachment->imageView) ?
               info->pDepthAttachment->loadOp : VK_ATTACHMENT_LOAD_OP_NONE_EXT,
            (info->pStencilAttachment && info->pStencilAttachment->imageView) ?
               info->pStencilAttachment->loadOp : VK_ATTACHMENT_LOAD_OP_NONE_EXT,
            (info->pDepthAttachment && info->pDepthAttachment->imageView) ?
               info->pDepthAttachment->storeOp : VK_ATTACHMENT_STORE_OP_NONE_EXT,
            (info->pStencilAttachment && info->pStencilAttachment->imageView) ?
               info->pStencilAttachment->storeOp : VK_ATTACHMENT_STORE_OP_NONE_EXT);

         subpass->samples = (VkSampleCountFlagBits) view->image->layout->nr_samples;

         if (common_info->resolveMode != VK_RESOLVE_MODE_NONE) {
            unsigned i = subpass->resolve_count++;
            struct tu_render_pass_attachment *resolve_att = &pass->attachments[a];
            VK_FROM_HANDLE(tu_image_view, resolve_view,
                           common_info->resolveImageView);
            tu_setup_dynamic_attachment(resolve_att, resolve_view);
            resolve_att->gmem = false;
            attachment_set_ops(device, resolve_att,
                               VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                               VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                               VK_ATTACHMENT_STORE_OP_STORE,
                               VK_ATTACHMENT_STORE_OP_STORE);
            subpass->resolve_attachments[i].attachment = a++;
            att->will_be_resolved = true;
            subpass->resolve_depth_stencil = true;
         } else {
            att->will_be_resolved = false;
         }
      } else {
         subpass->depth_stencil_attachment.attachment = VK_ATTACHMENT_UNUSED;
         subpass->input_attachments[0].attachment = VK_ATTACHMENT_UNUSED;
      }
   } else {
      subpass->depth_stencil_attachment.attachment = VK_ATTACHMENT_UNUSED;
      subpass->input_attachments[0].attachment = VK_ATTACHMENT_UNUSED;
   }

   pass->attachment_count = a;

   const VkRenderingFragmentDensityMapAttachmentInfoEXT *fdm_info =
      vk_find_struct_const(info->pNext,
                           RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT);
   if (fdm_info && fdm_info->imageView != VK_NULL_HANDLE &&
       !tu_render_pass_disable_fdm(device, pass)) {
      VK_FROM_HANDLE(tu_image_view, view, fdm_info->imageView);

      struct tu_render_pass_attachment *att = &pass->attachments[a];
      tu_setup_dynamic_attachment(att, view);
      pass->fragment_density_map.attachment = a++;
      attachment_set_ops(device, att,
                         VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                         VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                         VK_ATTACHMENT_STORE_OP_DONT_CARE,
                         VK_ATTACHMENT_STORE_OP_DONT_CARE);
      pass->has_fdm = true;
      if (info->flags & VK_RENDERING_PER_LAYER_FRAGMENT_DENSITY_BIT_VALVE)
         pass->has_layered_fdm = true;
   } else {
      pass->fragment_density_map.attachment = VK_ATTACHMENT_UNUSED;
      pass->has_fdm = false;
   }

   const VkRenderingFragmentShadingRateAttachmentInfoKHR *fsr_info =
      vk_find_struct_const(info->pNext,
                           RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
   if (fsr_info && fsr_info->imageView != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(tu_image_view, view, fsr_info->imageView);

      struct tu_render_pass_attachment *att = &pass->attachments[a];
      tu_setup_dynamic_attachment(att, view);
      subpass->fsr_attachment = a++;
      attachment_set_ops(device, att,
                         VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                         VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                         VK_ATTACHMENT_STORE_OP_DONT_CARE,
                         VK_ATTACHMENT_STORE_OP_DONT_CARE);
      subpass->fsr_attachment_texel_size = fsr_info->shadingRateAttachmentTexelSize;
   } else {
      subpass->fsr_attachment = VK_ATTACHMENT_UNUSED;
   }

   if (TU_DEBUG(FDM) && !tu_render_pass_disable_fdm(device, pass))
      pass->has_fdm = true;

   pass->attachment_count = a;

   tu_render_pass_check_ib2_skip(pass);
   tu_render_pass_cond_config(device, pass);
   tu_render_pass_gmem_config(pass, device->physical_device);
   tu_render_pass_bandwidth_config(pass);
   tu_render_pass_calc_views(pass);
   tu_render_pass_calc_hash(pass);
}

void
tu_setup_dynamic_inheritance(struct tu_cmd_buffer *cmd_buffer,
                             const VkCommandBufferInheritanceRenderingInfo *info)
{
   struct tu_render_pass *pass = &cmd_buffer->dynamic_pass;
   struct tu_subpass *subpass = &cmd_buffer->dynamic_subpass;

   pass->subpass_count = 1;
   pass->attachments = cmd_buffer->dynamic_rp_attachments;
   pass->fragment_density_map.attachment = VK_ATTACHMENT_UNUSED;

   subpass->color_count = info->colorAttachmentCount;
   subpass->resolve_count = 0;
   subpass->resolve_depth_stencil = false;
   subpass->color_attachments = cmd_buffer->dynamic_color_attachments;
   subpass->resolve_attachments = NULL;
   subpass->feedback_invalidate = false;
   subpass->feedback_loop_ds = subpass->feedback_loop_color = false;
   subpass->input_count = 0;
   subpass->samples = (VkSampleCountFlagBits) 0;
   subpass->srgb_cntl = 0;
   subpass->raster_order_attachment_access = false;
   subpass->multiview_mask = info->viewMask;
   subpass->samples = info->rasterizationSamples;

   unsigned a = 0;
   for (unsigned i = 0; i < info->colorAttachmentCount; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[a];
      VkFormat format = info->pColorAttachmentFormats[i];

      if (format == VK_FORMAT_UNDEFINED) {
         subpass->color_attachments[i].attachment = VK_ATTACHMENT_UNUSED;
         continue;
      }

      att->format = format;
      att->samples = info->rasterizationSamples;
      subpass->samples = info->rasterizationSamples;
      subpass->color_attachments[i].attachment = a++;

      /* conservatively assume that the attachment may be conditionally
       * loaded/stored.
       */
      att->cond_load_allowed = att->cond_store_allowed = true;
   }

   if (info->depthAttachmentFormat != VK_FORMAT_UNDEFINED ||
       info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED) {
      struct tu_render_pass_attachment *att = &pass->attachments[a];
      att->format = info->depthAttachmentFormat != VK_FORMAT_UNDEFINED ?
         info->depthAttachmentFormat : info->stencilAttachmentFormat;
      att->samples = info->rasterizationSamples;
      subpass->depth_stencil_attachment.attachment = a++;
      subpass->depth_used =
         info->depthAttachmentFormat != VK_FORMAT_UNDEFINED;
      subpass->stencil_used =
         info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED;
      att->cond_load_allowed = att->cond_store_allowed = true;
   } else {
      subpass->depth_stencil_attachment.attachment = VK_ATTACHMENT_UNUSED;
      subpass->depth_used = false;
      subpass->stencil_used = false;
   }

   tu_render_pass_calc_views(pass);
}

VKAPI_ATTR void VKAPI_CALL
tu_GetRenderAreaGranularity(VkDevice _device,
                            VkRenderPass renderPass,
                            VkExtent2D *pGranularity)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   pGranularity->width = device->physical_device->info->gmem_align_w;
   pGranularity->height = device->physical_device->info->gmem_align_h;
}

VKAPI_ATTR void VKAPI_CALL
tu_GetRenderingAreaGranularityKHR(VkDevice _device,
                                  const VkRenderingAreaInfoKHR *pRenderingAreaInfo,
                                  VkExtent2D *pGranularity)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   pGranularity->width = device->physical_device->info->gmem_align_w;
   pGranularity->height = device->physical_device->info->gmem_align_h;
}

uint32_t
tu_subpass_get_attachment_to_resolve(const struct tu_subpass *subpass, uint32_t index)
{
   if (subpass->resolve_depth_stencil &&
       index == (subpass->resolve_count - 1))
      return subpass->depth_stencil_attachment.attachment;

   return subpass->color_attachments[index].attachment;
}
