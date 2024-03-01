/*
 * Copyright © 2021 Bas Nieuwenhuizen
 * Copyright © 2023 Valve Corporation
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

#ifndef VK_ACCELERATION_STRUCTURE_H
#define VK_ACCELERATION_STRUCTURE_H

#include "vk_object.h"
#include "vk_meta.h"
#include "radix_sort/radix_sort_vk.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_acceleration_structure {
   struct vk_object_base base;

   VkBuffer buffer;
   uint64_t offset;
   uint64_t size;
};

VkDeviceAddress vk_acceleration_structure_get_va(struct vk_acceleration_structure *accel_struct);

VK_DEFINE_NONDISP_HANDLE_CASTS(vk_acceleration_structure, base, VkAccelerationStructureKHR,
                               VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR)

#define MAX_ENCODE_PASSES 2
#define MAX_UPDATE_PASSES 2

struct vk_acceleration_structure_build_ops {
   VkDeviceSize (*get_as_size)(VkDevice device,
                               VkGeometryTypeKHR geometry_type,
                               uint32_t leaf_count);
   VkDeviceSize (*get_update_scratch_size)(struct vk_device *device, uint32_t leaf_count);
   uint32_t (*get_encode_key[MAX_ENCODE_PASSES])(VkAccelerationStructureTypeKHR type,
                                                 VkBuildAccelerationStructureFlagBitsKHR flags);
   VkResult (*encode_bind_pipeline[MAX_ENCODE_PASSES])(VkCommandBuffer cmd_buffer,
                                                       uint32_t key);
   void (*encode_as[MAX_ENCODE_PASSES])(VkCommandBuffer cmd_buffer,
                                        struct vk_acceleration_structure *dst,
                                        VkDeviceAddress intermediate_as_addr,
                                        VkDeviceAddress intermediate_header_addr,
                                        uint32_t leaf_count,
                                        VkGeometryTypeKHR geometry_type,
                                        uint32_t key);
   void (*init_update_scratch)(VkCommandBuffer cmd_buffer,
                               VkDeviceAddress scratch,
                               uint32_t leaf_count,
                               struct vk_acceleration_structure *src_as,
                               struct vk_acceleration_structure *dst_as);
   void (*update_bind_pipeline[MAX_ENCODE_PASSES])(VkCommandBuffer cmd_buffer);
   void (*update_as[MAX_ENCODE_PASSES])(VkCommandBuffer cmd_buffer,
                                        VkDeviceAddress scratch,
                                        uint32_t leaf_count,
                                        VkGeometryTypeKHR geometry_type,
                                        struct vk_acceleration_structure *dst,
                                        struct vk_acceleration_structure *src);
};

struct vk_acceleration_structure_build_args {
   uint32_t subgroup_size;
   const radix_sort_vk_t *radix_sort;
};

void vk_cmd_build_acceleration_structures(VkCommandBuffer cmdbuf,
                                          struct vk_device *device,
                                          struct vk_meta_device *meta,
                                          uint32_t info_count,
                                          const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
                                          const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos,
                                          const struct vk_acceleration_structure_build_args *args);

void vk_get_as_build_sizes(VkDevice _device, VkAccelerationStructureBuildTypeKHR buildType,
                           const VkAccelerationStructureBuildGeometryInfoKHR *pBuildInfo,
                           const uint32_t *pMaxPrimitiveCounts,
                           VkAccelerationStructureBuildSizesInfoKHR *pSizeInfo,
                           const struct vk_acceleration_structure_build_args *args);

bool vk_acceleration_struct_vtx_format_supported(VkFormat format);

#ifdef __cplusplus
}
#endif

#endif
