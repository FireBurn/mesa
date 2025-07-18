/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hk_private.h"

#include "hk_device.h"
#include "vk_descriptor_update_template.h"
#include "vk_object.h"

#include "util/list.h"
#include "util/vma.h"

/* Stride of the image heap, equal to the size of a texture/PBE descriptor */
#define HK_IMAGE_STRIDE (24)

struct hk_descriptor_set_layout;

struct hk_sampled_image_descriptor {
   struct agx_texture_packed tex;
   struct agx_sampler_packed sampler;
   uint16_t sampler_index;

   /* Negative if there is no border colour, else the clamp=0 sampler index used
    * for custom border colour emulation.
    */
   int16_t clamp_0_sampler_index_or_negative;

   /* We want LOD bias and min LOD together as fp16 in this order and 32-bit
    * aligned. This lets us keep everything vectorized on the shader side, since
    * this matches the AGX2 lod_bias + min_lod descriptor.
    */
   uint16_t lod_bias_fp16;
   uint16_t min_lod_fp16;

   /* This is only used for txf, it can be separate from the other stuff */
   uint16_t min_lod_uint16;
   uint16_t pad;
   /* TODO: This should probably be a heap! */
   uint32_t border[4];
   uint8_t pad2[4];
};
static_assert(sizeof(struct hk_sampled_image_descriptor) == 64,
              "hk_sampled_image_descriptor has no holes");

struct hk_storage_image_descriptor {
   struct agx_texture_packed tex;
   struct agx_pbe_packed pbe;
   uint8_t pad[16];
};
static_assert(sizeof(struct hk_storage_image_descriptor) == 64,
              "hk_storage_image_descriptor has no holes");

/* This has to match nir_address_format_64bit_bounded_global */
struct hk_buffer_address {
   uint64_t base_addr;
   uint32_t size;
   uint32_t zero; /* Must be zero! */
};

struct hk_descriptor_pool {
   struct vk_object_base base;

   struct list_head sets;

   struct agx_bo *bo;
   uint8_t *mapped_ptr;
   struct util_vma_heap heap;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(hk_descriptor_pool, base, VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

struct hk_descriptor_set {
   struct vk_object_base base;

   /* Link in hk_descriptor_pool::sets */
   struct list_head link;

   struct hk_descriptor_set_layout *layout;
   void *mapped_ptr;
   uint64_t addr;
   uint32_t size;

   struct hk_buffer_address dynamic_buffers[];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(hk_descriptor_set, base, VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)

static inline uint64_t
hk_descriptor_set_addr(const struct hk_descriptor_set *set)
{
   return set->addr;
}

struct hk_push_descriptor_set {
   uint8_t data[HK_PUSH_DESCRIPTOR_SET_SIZE];
};

void hk_push_descriptor_set_update(struct hk_push_descriptor_set *push_set,
                                   struct hk_descriptor_set_layout *layout,
                                   uint32_t write_count,
                                   const VkWriteDescriptorSet *writes);

void hk_push_descriptor_set_update_template(
   struct hk_push_descriptor_set *push_set,
   struct hk_descriptor_set_layout *layout,
   const struct vk_descriptor_update_template *template, const void *data);
