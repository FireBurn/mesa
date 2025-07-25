/*
 * Copyright © 2021 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>

#include "radv_buffer.h"
#include "radv_cs.h"
#include "radv_spm.h"
#include "sid.h"

#define SPM_RING_BASE_ALIGN 32

static bool
radv_spm_init_bo(struct radv_device *device)
{
   struct radeon_winsys *ws = device->ws;
   VkResult result;

   struct radeon_winsys_bo *bo = NULL;
   result = radv_bo_create(device, NULL, device->spm.buffer_size, 4096, RADEON_DOMAIN_GTT,
                           RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_ZERO_VRAM,
                           RADV_BO_PRIORITY_SCRATCH, 0, true, &bo);
   device->spm.bo = bo;
   if (result != VK_SUCCESS)
      return false;

   result = ws->buffer_make_resident(ws, device->spm.bo, true);
   if (result != VK_SUCCESS)
      return false;

   device->spm.ptr = radv_buffer_map(ws, device->spm.bo);
   if (!device->spm.ptr)
      return false;

   return true;
}

static void
radv_spm_finish_bo(struct radv_device *device)
{
   struct radeon_winsys *ws = device->ws;

   if (device->spm.bo) {
      ws->buffer_make_resident(ws, device->spm.bo, false);
      radv_bo_destroy(device, NULL, device->spm.bo);
   }
}

static bool
radv_spm_resize_bo(struct radv_device *device)
{
   /* Destroy the previous SPM bo. */
   radv_spm_finish_bo(device);

   /* Double the size of the SPM bo. */
   device->spm.buffer_size *= 2;

   fprintf(stderr,
           "Failed to get the SPM trace because the buffer "
           "was too small, resizing to %d KB\n",
           device->spm.buffer_size / 1024);

   /* Re-create the SPM bo. */
   return radv_spm_init_bo(device);
}

static void
radv_emit_spm_counters(struct radv_device *device, struct radeon_cmdbuf *cs, enum radv_queue_family qf)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_ip_type ring = radv_queue_family_to_ring(pdev, qf);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   struct ac_spm *spm = &device->spm;

   if (gfx_level >= GFX11) {
      for (uint32_t instance = 0; instance < ARRAY_SIZE(spm->sq_wgp); instance++) {
         uint32_t num_counters = spm->sq_wgp[instance].num_counters;

         if (!num_counters)
            continue;

         radeon_check_space(device->ws, cs, 3 + num_counters * 3);
         radeon_begin(cs);

         radeon_set_uconfig_reg(R_030800_GRBM_GFX_INDEX, spm->sq_wgp[instance].grbm_gfx_index);

         for (uint32_t b = 0; b < num_counters; b++) {
            const struct ac_spm_counter_select *cntr_sel = &spm->sq_wgp[instance].counters[b];
            uint32_t reg_base = R_036700_SQ_PERFCOUNTER0_SELECT;

            radeon_set_uconfig_perfctr_reg_seq(gfx_level, ring, reg_base + b * 4, 1);
            radeon_emit(cntr_sel->sel0);
         }

         radeon_end();
      }
   }

   for (uint32_t instance = 0; instance < ARRAY_SIZE(spm->sqg); instance++) {
      uint32_t num_counters = spm->sqg[instance].num_counters;

      if (!num_counters)
         continue;

      radeon_check_space(device->ws, cs, 3 + num_counters * 3);
      radeon_begin(cs);

      radeon_set_uconfig_reg(R_030800_GRBM_GFX_INDEX, S_030800_SH_BROADCAST_WRITES(1) |
                                                         S_030800_INSTANCE_BROADCAST_WRITES(1) |
                                                         S_030800_SE_INDEX(instance));

      for (uint32_t b = 0; b < num_counters; b++) {
         const struct ac_spm_counter_select *cntr_sel = &spm->sqg[instance].counters[b];
         uint32_t reg_base = R_036700_SQ_PERFCOUNTER0_SELECT;

         radeon_set_uconfig_perfctr_reg_seq(gfx_level, ring, reg_base + b * 4, 1);
         radeon_emit(cntr_sel->sel0 | S_036700_SQC_BANK_MASK(0xf)); /* SQC_BANK_MASK only gfx10 */
      }

      radeon_end();
   }

   for (uint32_t b = 0; b < spm->num_block_sel; b++) {
      struct ac_spm_block_select *block_sel = &spm->block_sel[b];
      struct ac_pc_block_base *regs = block_sel->b->b->b;

      for (unsigned i = 0; i < block_sel->num_instances; i++) {
         struct ac_spm_block_instance *block_instance = &block_sel->instances[i];

         radeon_check_space(device->ws, cs, 3 + (AC_SPM_MAX_COUNTER_PER_BLOCK * 6));
         radeon_begin(cs);

         radeon_set_uconfig_reg(R_030800_GRBM_GFX_INDEX, block_instance->grbm_gfx_index);

         for (unsigned c = 0; c < block_instance->num_counters; c++) {
            const struct ac_spm_counter_select *cntr_sel = &block_instance->counters[c];

            if (!cntr_sel->active)
               continue;

            radeon_set_uconfig_perfctr_reg_seq(gfx_level, ring, regs->select0[c], 1);
            radeon_emit(cntr_sel->sel0);

            radeon_set_uconfig_perfctr_reg_seq(gfx_level, ring, regs->select1[c], 1);
            radeon_emit(cntr_sel->sel1);
         }

         radeon_end();
      }
   }

   /* Restore global broadcasting. */
   radeon_begin(cs);
   radeon_set_uconfig_reg(R_030800_GRBM_GFX_INDEX, S_030800_SE_BROADCAST_WRITES(1) | S_030800_SH_BROADCAST_WRITES(1) |
                                                      S_030800_INSTANCE_BROADCAST_WRITES(1));
   radeon_end();
}

static void
radv_emit_spm_muxsel(struct radv_device *device, struct radeon_cmdbuf *cs, enum radv_queue_family qf)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_ip_type ring = radv_queue_family_to_ring(pdev, qf);
   const struct ac_spm *spm = &device->spm;

   /* Upload each muxsel ram to the RLC. */
   for (unsigned s = 0; s < AC_SPM_SEGMENT_TYPE_COUNT; s++) {
      unsigned rlc_muxsel_addr, rlc_muxsel_data;
      unsigned grbm_gfx_index = S_030800_SH_BROADCAST_WRITES(1) | S_030800_INSTANCE_BROADCAST_WRITES(1);

      if (!spm->num_muxsel_lines[s])
         continue;

      if (s == AC_SPM_SEGMENT_TYPE_GLOBAL) {
         grbm_gfx_index |= S_030800_SE_BROADCAST_WRITES(1);

         rlc_muxsel_addr =
            pdev->info.gfx_level >= GFX11 ? R_037220_RLC_SPM_GLOBAL_MUXSEL_ADDR : R_037224_RLC_SPM_GLOBAL_MUXSEL_ADDR;
         rlc_muxsel_data =
            pdev->info.gfx_level >= GFX11 ? R_037224_RLC_SPM_GLOBAL_MUXSEL_DATA : R_037228_RLC_SPM_GLOBAL_MUXSEL_DATA;
      } else {
         grbm_gfx_index |= S_030800_SE_INDEX(s);

         rlc_muxsel_addr =
            pdev->info.gfx_level >= GFX11 ? R_037228_RLC_SPM_SE_MUXSEL_ADDR : R_03721C_RLC_SPM_SE_MUXSEL_ADDR;
         rlc_muxsel_data =
            pdev->info.gfx_level >= GFX11 ? R_03722C_RLC_SPM_SE_MUXSEL_DATA : R_037220_RLC_SPM_SE_MUXSEL_DATA;
      }

      radeon_check_space(device->ws, cs, 3 + spm->num_muxsel_lines[s] * (7 + AC_SPM_MUXSEL_LINE_SIZE));
      radeon_begin(cs);

      radeon_set_uconfig_reg(R_030800_GRBM_GFX_INDEX, grbm_gfx_index);

      for (unsigned l = 0; l < spm->num_muxsel_lines[s]; l++) {
         uint32_t *data = (uint32_t *)spm->muxsel_lines[s][l].muxsel;

         /* Select MUXSEL_ADDR to point to the next muxsel. */
         radeon_set_uconfig_perfctr_reg(pdev->info.gfx_level, ring, rlc_muxsel_addr, l * AC_SPM_MUXSEL_LINE_SIZE);

         /* Write the muxsel line configuration with MUXSEL_DATA. */
         radeon_emit(PKT3(PKT3_WRITE_DATA, 2 + AC_SPM_MUXSEL_LINE_SIZE, 0));
         radeon_emit(S_370_DST_SEL(V_370_MEM_MAPPED_REGISTER) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_ME) |
                     S_370_WR_ONE_ADDR(1));
         radeon_emit(rlc_muxsel_data >> 2);
         radeon_emit(0);
         radeon_emit_array(data, AC_SPM_MUXSEL_LINE_SIZE);
      }

      radeon_end();
   }
}

void
radv_emit_spm_setup(struct radv_device *device, struct radeon_cmdbuf *cs, enum radv_queue_family qf)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct ac_spm *spm = &device->spm;
   uint64_t va = radv_buffer_get_va(spm->bo);
   uint64_t ring_size = spm->buffer_size;

   /* It's required that the ring VA and the size are correctly aligned. */
   assert(!(va & (SPM_RING_BASE_ALIGN - 1)));
   assert(!(ring_size & (SPM_RING_BASE_ALIGN - 1)));
   assert(spm->sample_interval >= 32);

   radeon_check_space(device->ws, cs, 27);
   radeon_begin(cs);

   /* Configure the SPM ring buffer. */
   radeon_set_uconfig_reg(R_037200_RLC_SPM_PERFMON_CNTL,
                          S_037200_PERFMON_RING_MODE(0) | /* no stall and no interrupt on overflow */
                             S_037200_PERFMON_SAMPLE_INTERVAL(spm->sample_interval)); /* in sclk */
   radeon_set_uconfig_reg(R_037204_RLC_SPM_PERFMON_RING_BASE_LO, va);
   radeon_set_uconfig_reg(R_037208_RLC_SPM_PERFMON_RING_BASE_HI, S_037208_RING_BASE_HI(va >> 32));
   radeon_set_uconfig_reg(R_03720C_RLC_SPM_PERFMON_RING_SIZE, ring_size);

   /* Configure the muxsel. */
   uint32_t total_muxsel_lines = 0;
   for (unsigned s = 0; s < AC_SPM_SEGMENT_TYPE_COUNT; s++) {
      total_muxsel_lines += spm->num_muxsel_lines[s];
   }

   radeon_set_uconfig_reg(R_03726C_RLC_SPM_ACCUM_MODE, 0);

   if (pdev->info.gfx_level >= GFX11) {
      radeon_set_uconfig_reg(R_03721C_RLC_SPM_PERFMON_SEGMENT_SIZE,
                             S_03721C_TOTAL_NUM_SEGMENT(total_muxsel_lines) |
                                S_03721C_GLOBAL_NUM_SEGMENT(spm->num_muxsel_lines[AC_SPM_SEGMENT_TYPE_GLOBAL]) |
                                S_03721C_SE_NUM_SEGMENT(spm->max_se_muxsel_lines));

      radeon_set_uconfig_reg(R_037210_RLC_SPM_RING_WRPTR, 0);
   } else {
      radeon_set_uconfig_reg(R_037210_RLC_SPM_PERFMON_SEGMENT_SIZE, 0);
      radeon_set_uconfig_reg(R_03727C_RLC_SPM_PERFMON_SE3TO0_SEGMENT_SIZE,
                             S_03727C_SE0_NUM_LINE(spm->num_muxsel_lines[AC_SPM_SEGMENT_TYPE_SE0]) |
                                S_03727C_SE1_NUM_LINE(spm->num_muxsel_lines[AC_SPM_SEGMENT_TYPE_SE1]) |
                                S_03727C_SE2_NUM_LINE(spm->num_muxsel_lines[AC_SPM_SEGMENT_TYPE_SE2]) |
                                S_03727C_SE3_NUM_LINE(spm->num_muxsel_lines[AC_SPM_SEGMENT_TYPE_SE3]));
      radeon_set_uconfig_reg(R_037280_RLC_SPM_PERFMON_GLB_SEGMENT_SIZE,
                             S_037280_PERFMON_SEGMENT_SIZE(total_muxsel_lines) |
                                S_037280_GLOBAL_NUM_LINE(spm->num_muxsel_lines[AC_SPM_SEGMENT_TYPE_GLOBAL]));
   }

   radeon_end();

   /* Upload each muxsel ram to the RLC. */
   radv_emit_spm_muxsel(device, cs, qf);

   /* Select SPM counters. */
   radv_emit_spm_counters(device, cs, qf);
}

bool
radv_spm_init(struct radv_device *device)
{
   struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   struct ac_perfcounters *pc = &pdev->ac_perfcounters;

   /* We failed to initialize the performance counters. */
   if (!pc->blocks) {
      fprintf(stderr, "radv: Failed to initialize SPM because perf counters aren't implemented.\n");
      return false;
   }

   if (!ac_init_spm(gpu_info, pc, &device->spm))
      return false;

   device->spm.buffer_size = 32 * 1024 * 1024; /* Default to 32MB. */
   device->spm.sample_interval = 4096;         /* Default to 4096 clk. */

   if (!radv_spm_init_bo(device))
      return false;

   return true;
}

void
radv_spm_finish(struct radv_device *device)
{
   radv_spm_finish_bo(device);

   ac_destroy_spm(&device->spm);
}

bool
radv_get_spm_trace(struct radv_queue *queue, struct ac_spm_trace *spm_trace)
{
   struct radv_device *device = radv_queue_device(queue);

   if (!ac_spm_get_trace(&device->spm, spm_trace)) {
      if (!radv_spm_resize_bo(device))
         fprintf(stderr, "radv: Failed to resize the SPM buffer.\n");
      return false;
   }

   return true;
}
