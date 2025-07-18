/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_IMAGE_VIEW_H
#define NVK_IMAGE_VIEW_H 1

#include "nvk_private.h"

#include "vk_image.h"

#include "nil.h"

struct nvk_device;

struct nvk_image_view {
   struct vk_image_view vk;

   bool separate_zs;

   uint8_t plane_count;
   struct {
      uint8_t image_plane;

      enum nil_sample_layout sample_layout;

      /** Index in the image descriptor table for the sampled image descriptor */
      uint32_t sampled_desc_index;

      /** Index in the image descriptor table for the storage image descriptor */
      uint32_t storage_desc_index;
   } planes[NVK_MAX_IMAGE_PLANES];

   /* Surface info for Kepler storage images */
   struct nil_su_info su_info;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_image_view, vk.base, VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW)

struct nvk_image_view_capture {
   union {
      struct {
         uint32_t sampled_desc_index;
         uint32_t storage_desc_index;
      } single_plane;

      struct {
         struct {
            uint32_t desc_index;
         } planes[NVK_MAX_IMAGE_PLANES];
      } ycbcr;
   };
};

VkResult nvk_image_view_init(struct nvk_device *dev,
                             struct nvk_image_view *view,
                             bool driver_internal,
                             const VkImageViewCreateInfo *pCreateInfo);

void nvk_image_view_finish(struct nvk_device *dev,
                           struct nvk_image_view *view);

#endif
