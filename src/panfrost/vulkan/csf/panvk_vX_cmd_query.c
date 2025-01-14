/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "util/os_time.h"

#include "vk_log.h"
#include "vk_synchronization.h"

#include "genxml/gen_macros.h"

#include "panvk_buffer.h"
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_meta.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_macros.h"
#include "panvk_query_pool.h"

/* At the API level, a query consists of a status and a result.  Both are
 * uninitialized initially.  There are these query operations:
 *
 *  - Reset op sets the status to unavailable and leaves the result undefined.
 *  - Begin/End pair or Write op sets the status to available and the result
 *    to the final query value.  Because of VK_QUERY_RESULT_PARTIAL_BIT, the
 *    result must hold valid intermediate query values while the query is
 *    active.
 *  - Copy op copies the result and optionally the status to a buffer.
 *
 * All query operations define execution dependencies among themselves when
 * they reference the same queries.  The only exception is the Copy op when
 * VK_QUERY_RESULT_WAIT_BIT is not set.
 *
 * We use a panvk_cs_sync32 to store the status of a query:
 *
 *  - Reset op waits on all prior query operations affecting the query before
 *    setting the seqno to 0 synchronously.
 *  - Begin op does not access the seqno.
 *  - End or Write op sets the seqno to 1 asynchronously.
 *  - Copy op waits on the seqno only when VK_QUERY_RESULT_WAIT_BIT is set.
 *
 * Because Reset op acts as a full barrier, End or Write op knows the seqno is
 * 0 and does not need to wait.
 */

static void
panvk_cmd_reset_occlusion_queries(struct panvk_cmd_buffer *cmd,
                                  struct panvk_query_pool *pool,
                                  uint32_t first_query, uint32_t query_count)
{
   struct cs_builder *b = panvk_get_cs_builder(cmd, PANVK_SUBQUEUE_FRAGMENT);

   /* Wait on deferred sync to ensure all prior query operations have
    * completed
    */
   cs_wait_slots(b, SB_MASK(DEFERRED_SYNC), false);

   struct cs_index addr = cs_scratch_reg64(b, 0);
   struct cs_index zero32 = cs_scratch_reg32(b, 2);
   cs_move32_to(b, zero32, 0);

   /* Mark all query syncobj as not available */
   for (uint32_t query = first_query; query < first_query + query_count;
        query++) {
      cs_move64_to(b, addr, panvk_query_available_dev_addr(pool, query));
      cs_sync32_set(b, true, MALI_CS_SYNC_SCOPE_SYSTEM, zero32, addr,
                    cs_defer(SB_IMM_MASK, SB_ID(DEFERRED_SYNC)));
   }
}

static void
panvk_cmd_begin_occlusion_query(struct panvk_cmd_buffer *cmd,
                                struct panvk_query_pool *pool, uint32_t query,
                                VkQueryControlFlags flags)
{
   uint64_t report_addr = panvk_query_report_dev_addr(pool, query);

   cmd->state.gfx.occlusion_query.ptr = report_addr;
   cmd->state.gfx.occlusion_query.mode = flags & VK_QUERY_CONTROL_PRECISE_BIT
                                            ? MALI_OCCLUSION_MODE_COUNTER
                                            : MALI_OCCLUSION_MODE_PREDICATE;
   gfx_state_set_dirty(cmd, OQ);

   /* From the Vulkan spec:
    *
    *   "When an occlusion query begins, the count of passing samples
    *    always starts at zero."
    *
    */
   struct cs_builder *b = panvk_get_cs_builder(cmd, PANVK_SUBQUEUE_FRAGMENT);

   struct cs_index report_addr_gpu = cs_scratch_reg64(b, 0);
   struct cs_index clear_value = cs_scratch_reg64(b, 2);
   cs_move64_to(b, report_addr_gpu, report_addr);
   cs_move64_to(b, clear_value, 0);
   cs_store64(b, clear_value, report_addr_gpu, 0);
   cs_wait_slot(b, SB_ID(LS), false);
}

static void
panvk_cmd_end_occlusion_query(struct panvk_cmd_buffer *cmd,
                              struct panvk_query_pool *pool, uint32_t query)
{
   /* Ensure all RUN_FRAGMENT are encoded before this */
   panvk_per_arch(cmd_flush_draws)(cmd);
   cmd->state.gfx.occlusion_query.ptr = 0;
   cmd->state.gfx.occlusion_query.mode = MALI_OCCLUSION_MODE_DISABLED;
   gfx_state_set_dirty(cmd, OQ);

   struct cs_builder *b = panvk_get_cs_builder(cmd, PANVK_SUBQUEUE_FRAGMENT);

   struct cs_index flush_id = cs_scratch_reg32(b, 0);
   cs_move32_to(b, flush_id, 0);

   /* OQ accumulates sample counts to the report which is on a cached memory.
    * Wait for the accumulation and flush the caches.
    */
   cs_flush_caches(b, MALI_CS_FLUSH_MODE_CLEAN, MALI_CS_FLUSH_MODE_CLEAN, false,
                   flush_id,
                   cs_defer(SB_ALL_ITERS_MASK, SB_ID(DEFERRED_FLUSH)));

   struct cs_index sync_addr = cs_scratch_reg64(b, 0);
   struct cs_index seqno = cs_scratch_reg32(b, 2);
   cs_move32_to(b, seqno, 1);

   /* We wait on any previous flush, and defer on sync */
   cs_move64_to(b, sync_addr, panvk_query_available_dev_addr(pool, query));
   cs_sync32_set(b, true, MALI_CS_SYNC_SCOPE_SYSTEM, seqno, sync_addr,
                 cs_defer(SB_MASK(DEFERRED_FLUSH), SB_ID(DEFERRED_SYNC)));
}

static void
panvk_copy_occlusion_query_results(struct panvk_cmd_buffer *cmd,
                                   struct panvk_query_pool *pool,
                                   uint32_t first_query, uint32_t query_count,
                                   uint64_t dst_buffer_addr,
                                   VkDeviceSize stride,
                                   VkQueryResultFlags flags)
{
   unsigned result_stride =
      flags & VK_QUERY_RESULT_64_BIT ? sizeof(uint64_t) : sizeof(uint32_t);

   struct cs_builder *b = panvk_get_cs_builder(cmd, PANVK_SUBQUEUE_FRAGMENT);
   struct cs_index scratch_addr0 = cs_scratch_reg64(b, 0);
   struct cs_index scratch_addr1 = cs_scratch_reg64(b, 2);
   struct cs_index scratch_val0 = cs_scratch_reg32(b, 4);
   struct cs_index available = cs_scratch_reg32(b, 5);
   struct cs_index write_results = cs_scratch_reg32(b, 6);
   struct cs_index scratch_val2 = cs_scratch_reg64(b, 8);

   for (uint32_t query = first_query; query < first_query + query_count;
        query++) {
      cs_move64_to(b, scratch_addr0,
                   panvk_query_available_dev_addr(pool, query));

      if (flags & VK_QUERY_RESULT_WAIT_BIT) {
         cs_move32_to(b, scratch_val0, 0);

         /* Wait on the sync object of the current query */
         cs_sync32_wait(b, false, MALI_CS_CONDITION_GREATER, scratch_val0,
                        scratch_addr0);

         /* After the wait, all subqueues are available */
         cs_move32_to(b, available, 1);
      } else {
         cs_move64_to(b, scratch_addr0,
                      panvk_query_available_dev_addr(pool, query));
         cs_load32_to(b, available, scratch_addr0, 0);
         cs_wait_slot(b, SB_ID(LS), false);
      }

      cs_add32(b, write_results, available,
               (flags & VK_QUERY_RESULT_PARTIAL_BIT) != 0);

      assert(pool->vk.query_type == VK_QUERY_TYPE_OCCLUSION);

      cs_if(b, MALI_CS_CONDITION_GREATER, write_results) {
         cs_move64_to(b, scratch_addr0,
                      panvk_query_report_dev_addr(pool, query));
         cs_move64_to(b, scratch_addr1, dst_buffer_addr + query * stride);

         if (flags & VK_QUERY_RESULT_64_BIT) {
            cs_load64_to(b, scratch_val2, scratch_addr0, 0);
            cs_wait_slot(b, SB_ID(LS), false);
            cs_store64(b, scratch_val2, scratch_addr1, 0);
         } else {
            cs_load32_to(b, scratch_val0, scratch_addr0, 0);
            cs_wait_slot(b, SB_ID(LS), false);
            cs_store32(b, scratch_val0, scratch_addr1, 0);
         }

         if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
            cs_store32(b, available, scratch_addr1, result_stride);

         cs_wait_slot(b, SB_ID(LS), false);
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdResetQueryPool)(VkCommandBuffer commandBuffer,
                                  VkQueryPool queryPool, uint32_t firstQuery,
                                  uint32_t queryCount)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(panvk_query_pool, pool, queryPool);

   if (queryCount == 0)
      return;

   switch (pool->vk.query_type) {
   case VK_QUERY_TYPE_OCCLUSION: {
      panvk_cmd_reset_occlusion_queries(cmd, pool, firstQuery, queryCount);
      break;
   }
   default:
      unreachable("Unsupported query type");
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBeginQueryIndexedEXT)(VkCommandBuffer commandBuffer,
                                        VkQueryPool queryPool, uint32_t query,
                                        VkQueryControlFlags flags,
                                        uint32_t index)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(panvk_query_pool, pool, queryPool);

   /* TODO: transform feedback */
   assert(index == 0);

   switch (pool->vk.query_type) {
   case VK_QUERY_TYPE_OCCLUSION: {
      panvk_cmd_begin_occlusion_query(cmd, pool, query, flags);
      break;
   }
   default:
      unreachable("Unsupported query type");
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdEndQueryIndexedEXT)(VkCommandBuffer commandBuffer,
                                      VkQueryPool queryPool, uint32_t query,
                                      uint32_t index)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(panvk_query_pool, pool, queryPool);

   /* TODO: transform feedback */
   assert(index == 0);

   switch (pool->vk.query_type) {
   case VK_QUERY_TYPE_OCCLUSION: {
      panvk_cmd_end_occlusion_query(cmd, pool, query);
      break;
   }
   default:
      unreachable("Unsupported query type");
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdWriteTimestamp2)(VkCommandBuffer commandBuffer,
                                   VkPipelineStageFlags2 stage,
                                   VkQueryPool queryPool, uint32_t query)
{
   UNUSED VK_FROM_HANDLE(panvk_cmd_buffer, cmd, commandBuffer);
   UNUSED VK_FROM_HANDLE(panvk_query_pool, pool, queryPool);

   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdCopyQueryPoolResults)(
   VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery,
   uint32_t queryCount, VkBuffer dstBuffer, VkDeviceSize dstOffset,
   VkDeviceSize stride, VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(panvk_query_pool, pool, queryPool);
   VK_FROM_HANDLE(panvk_buffer, dst_buffer, dstBuffer);

   uint64_t dst_buffer_addr = panvk_buffer_gpu_ptr(dst_buffer, dstOffset);

   switch (pool->vk.query_type) {
   case VK_QUERY_TYPE_OCCLUSION: {
      panvk_copy_occlusion_query_results(cmd, pool, firstQuery, queryCount,
                                         dst_buffer_addr, stride, flags);
      break;
   }
   default:
      unreachable("Unsupported query type");
   }
}
