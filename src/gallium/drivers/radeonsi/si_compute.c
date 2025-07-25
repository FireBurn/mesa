/*
 * Copyright 2013 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_rtld.h"
#include "nir/tgsi_to_nir.h"
#include "si_build_pm4.h"
#include "si_shader_internal.h"
#include "util/u_async_debug.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "si_tracepoints.h"
#include "nir.h"

#define COMPUTE_DBG(sscreen, fmt, args...)                                                         \
   do {                                                                                            \
      if ((sscreen->shader_debug_flags & DBG(COMPUTE)))                                            \
         fprintf(stderr, fmt, ##args);                                                             \
   } while (0);

/* Asynchronous compute shader compilation. */
static void si_create_compute_state_async(void *job, void *gdata, int thread_index)
{
   struct si_compute *program = (struct si_compute *)job;
   struct si_shader_selector *sel = &program->sel;
   struct si_shader *shader = &program->shader;
   struct ac_llvm_compiler **compiler;
   struct util_debug_callback *debug = &sel->compiler_ctx_state.debug;
   struct si_screen *sscreen = sel->screen;

   assert(!debug->debug_message || debug->async);
   assert(thread_index >= 0);
   assert(thread_index < ARRAY_SIZE(sscreen->compiler));
   compiler = &sscreen->compiler[thread_index];

   si_nir_scan_shader(sscreen, sel->nir, &sel->info, false);

   if (!sel->nir->info.use_aco_amd && !*compiler)
      *compiler = si_create_llvm_compiler(sscreen);

   si_get_active_slot_masks(sscreen, &sel->info, &sel->active_const_and_shader_buffers,
                            &sel->active_samplers_and_images);

   program->shader.is_monolithic = true;
   program->shader.wave_size = si_determine_wave_size(sscreen, &program->shader);

   /* Variable block sizes need 10 bits (1 + log2(SI_MAX_VARIABLE_THREADS_PER_BLOCK)) per dim.
    * We pack them into a single user SGPR.
    */
   unsigned user_sgprs = SI_NUM_RESOURCE_SGPRS + (sel->info.uses_grid_size ? 3 : 0) +
                         (sel->info.uses_variable_block_size ? 1 : 0) +
                         sel->nir->info.cs.user_data_components_amd;

   /* Fast path for compute shaders - some descriptors passed via user SGPRs. */
   /* Shader buffers in user SGPRs. */
   for (unsigned i = 0; i < MIN2(3, sel->nir->info.num_ssbos) && user_sgprs <= 12; i++) {
      user_sgprs = align(user_sgprs, 4);
      if (i == 0)
         sel->cs_shaderbufs_sgpr_index = user_sgprs;
      user_sgprs += 4;
      sel->cs_num_shaderbufs_in_user_sgprs++;
   }

   /* Images in user SGPRs. */
   unsigned non_fmask_images = BITFIELD_MASK(sel->nir->info.num_images);

   /* Remove images with FMASK from the bitmask.  We only care about the first
    * 3 anyway, so we can take msaa_images[0] and ignore the rest.
    */
   if (sscreen->info.gfx_level < GFX11)
      non_fmask_images &= ~sel->nir->info.msaa_images[0];

   for (unsigned i = 0; i < 3 && non_fmask_images & (1 << i); i++) {
      unsigned num_sgprs = BITSET_TEST(sel->nir->info.image_buffers, i) ? 4 : 8;

      if (align(user_sgprs, num_sgprs) + num_sgprs > 16)
         break;

      user_sgprs = align(user_sgprs, num_sgprs);
      if (i == 0)
         sel->cs_images_sgpr_index = user_sgprs;
      user_sgprs += num_sgprs;
      sel->cs_num_images_in_user_sgprs++;
   }
   sel->cs_images_num_sgprs = user_sgprs - sel->cs_images_sgpr_index;
   assert(user_sgprs <= 16);

   unsigned char ir_sha1_cache_key[20];
   si_get_ir_cache_key(sel, false, false, shader->wave_size, ir_sha1_cache_key);

   /* Try to load the shader from the shader cache. */
   simple_mtx_lock(&sscreen->shader_cache_mutex);

   if (si_shader_cache_load_shader(sscreen, ir_sha1_cache_key, shader)) {
      simple_mtx_unlock(&sscreen->shader_cache_mutex);

      shader->complete_shader_binary_size = si_get_shader_binary_size(sscreen, shader);

      if (!si_shader_binary_upload(sscreen, shader, 0))
         program->shader.compilation_failed = true;

      si_shader_dump_stats_for_shader_db(sscreen, shader, debug);
      si_shader_dump(sscreen, shader, debug, stderr, true);
   } else {
      simple_mtx_unlock(&sscreen->shader_cache_mutex);

      if (!si_create_shader_variant(sscreen, *compiler, &program->shader, debug)) {
         program->shader.compilation_failed = true;
         return;
      }

      shader->config.rsrc1 = S_00B848_VGPRS((shader->config.num_vgprs - 1) /
                                            ((shader->wave_size == 32 ||
                                              sscreen->info.wave64_vgpr_alloc_granularity == 8) ? 8 : 4)) |
                             S_00B848_DX10_CLAMP(sscreen->info.gfx_level < GFX12) |
                             S_00B848_MEM_ORDERED(si_shader_mem_ordered(shader)) |
                             S_00B848_FLOAT_MODE(shader->config.float_mode) |
                             /* This is needed for CWSR, but it causes halts to work differently. */
                             S_00B848_PRIV(sscreen->info.gfx_level == GFX11);

      if (sscreen->info.gfx_level < GFX10) {
         shader->config.rsrc1 |= S_00B848_SGPRS((shader->config.num_sgprs - 1) / 8);
      }

      shader->config.rsrc2 = S_00B84C_USER_SGPR(user_sgprs) |
                             S_00B84C_SCRATCH_EN(shader->config.scratch_bytes_per_wave > 0) |
                             S_00B84C_TGID_X_EN(sel->info.uses_block_id[0]) |
                             S_00B84C_TGID_Y_EN(sel->info.uses_block_id[1]) |
                             S_00B84C_TGID_Z_EN(sel->info.uses_block_id[2]) |
                             S_00B84C_TG_SIZE_EN(sel->info.uses_tg_size) |
                             S_00B84C_TIDIG_COMP_CNT(sel->info.uses_thread_id[2]
                                                        ? 2
                                                        : sel->info.uses_thread_id[1] ? 1 : 0) |
                             S_00B84C_LDS_SIZE(shader->config.lds_size);

      /* COMPUTE_PGM_RSRC3 is only present on GFX10+ and GFX940+. */
      shader->config.rsrc3 = S_00B8A0_SHARED_VGPR_CNT(shader->config.num_shared_vgprs / 8);

      if (sscreen->info.gfx_level >= GFX12)
         shader->config.rsrc3 |= S_00B8A0_INST_PREF_SIZE_GFX12(si_get_shader_prefetch_size(shader));
      else if (sscreen->info.gfx_level >= GFX11)
         shader->config.rsrc3 |= S_00B8A0_INST_PREF_SIZE_GFX11(si_get_shader_prefetch_size(shader));

      simple_mtx_lock(&sscreen->shader_cache_mutex);
      si_shader_cache_insert_shader(sscreen, ir_sha1_cache_key, shader, true);
      simple_mtx_unlock(&sscreen->shader_cache_mutex);
   }

   ralloc_free(sel->nir);
   sel->nir = NULL;
}

static void *si_create_compute_state(struct pipe_context *ctx, const struct pipe_compute_state *cso)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_screen *sscreen = (struct si_screen *)ctx->screen;
   struct si_compute *program = CALLOC_STRUCT(si_compute);
   struct si_shader_selector *sel = &program->sel;

   pipe_reference_init(&sel->base.reference, 1);
   sel->stage = MESA_SHADER_COMPUTE;
   sel->screen = sscreen;
   simple_mtx_init(&sel->mutex, mtx_plain);
   sel->const_and_shader_buf_descriptors_index =
      si_const_and_shader_buffer_descriptors_idx(PIPE_SHADER_COMPUTE);
   sel->sampler_and_images_descriptors_index =
      si_sampler_and_image_descriptors_idx(PIPE_SHADER_COMPUTE);
   sel->info.base.shared_size = cso->static_shared_mem;
   program->shader.selector = &program->sel;

   if (cso->ir_type == PIPE_SHADER_IR_TGSI) {
      sel->nir = tgsi_to_nir(cso->prog, ctx->screen, true);
   } else {
      assert(cso->ir_type == PIPE_SHADER_IR_NIR);
      sel->nir = (struct nir_shader *)cso->prog;
   }

   sel->nir->info.shared_size = cso->static_shared_mem;

   if (si_can_dump_shader(sscreen, sel->stage, SI_DUMP_INIT_NIR))
      nir_print_shader(sel->nir, stderr);

   sel->compiler_ctx_state.debug = sctx->debug;
   sel->compiler_ctx_state.is_debug_context = sctx->is_debug;
   p_atomic_inc(&sscreen->num_shaders_created);

   si_schedule_initial_compile(sctx, MESA_SHADER_COMPUTE, &sel->ready, &sel->compiler_ctx_state,
                               program, si_create_compute_state_async);
   return program;
}

static void si_get_compute_state_info(struct pipe_context *ctx, void *state,
                                      struct pipe_compute_state_object_info *info)
{
   struct si_compute *program = (struct si_compute *)state;
   struct si_shader_selector *sel = &program->sel;

   /* Wait because we need the compilation to finish first */
   util_queue_fence_wait(&sel->ready);

   uint8_t wave_size = program->shader.wave_size;
   info->private_memory = DIV_ROUND_UP(program->shader.config.scratch_bytes_per_wave, wave_size);
   info->preferred_simd_size = wave_size;
   info->simd_sizes = wave_size;
   info->max_threads = si_get_max_workgroup_size(&program->shader);
}

static void si_bind_compute_state(struct pipe_context *ctx, void *state)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_compute *program = (struct si_compute *)state;
   struct si_shader_selector *sel = &program->sel;

   sctx->cs_shader_state.program = program;
   if (!program)
      return;

   /* Wait because we need active slot usage masks. */
   util_queue_fence_wait(&sel->ready);

   si_update_common_shader_state(sctx, sel, PIPE_SHADER_COMPUTE);

   sctx->compute_shaderbuf_sgprs_dirty = true;
   sctx->compute_image_sgprs_dirty = true;

   if (unlikely((sctx->screen->debug_flags & DBG(SQTT)) && sctx->sqtt)) {
      uint32_t pipeline_code_hash = _mesa_hash_data_with_seed(
         program->shader.binary.code_buffer,
         program->shader.binary.code_size,
         0);

      if (!si_sqtt_pipeline_is_registered(sctx->sqtt, pipeline_code_hash)) {
         /* Short lived fake pipeline: we don't need to reupload the compute shaders,
          * as we do for the gfx ones so just create a temp pipeline to be able to
          * call si_sqtt_register_pipeline, and then drop it.
          */
         struct si_sqtt_fake_pipeline pipeline = { 0 };
         pipeline.code_hash = pipeline_code_hash;
         pipeline.bo = program->shader.bo;

         si_sqtt_register_pipeline(sctx, &pipeline, NULL);
      }

      si_sqtt_describe_pipeline_bind(sctx, pipeline_code_hash, 1);
   }
}

static void si_set_global_binding(struct pipe_context *ctx, unsigned first, unsigned n,
                                  struct pipe_resource **resources, uint32_t **handles)
{
   unsigned i;
   struct si_context *sctx = (struct si_context *)ctx;

   if (first + n > sctx->max_global_buffers) {
      unsigned old_max = sctx->max_global_buffers;
      sctx->max_global_buffers = first + n;
      sctx->global_buffers = realloc(
         sctx->global_buffers, sctx->max_global_buffers * sizeof(sctx->global_buffers[0]));
      if (!sctx->global_buffers) {
         mesa_loge("failed to allocate compute global_buffers");
         return;
      }

      memset(&sctx->global_buffers[old_max], 0,
             (sctx->max_global_buffers - old_max) * sizeof(sctx->global_buffers[0]));
   }

   if (!resources) {
      for (i = 0; i < n; i++) {
         pipe_resource_reference(&sctx->global_buffers[first + i], NULL);
      }
      return;
   }

   for (i = 0; i < n; i++) {
      uint64_t va;
      uint32_t offset;
      pipe_resource_reference(&sctx->global_buffers[first + i], resources[i]);
      va = si_resource(resources[i])->gpu_address;
      offset = util_le32_to_cpu(*handles[i]);
      va += offset;
      va = util_cpu_to_le64(va);
      memcpy(handles[i], &va, sizeof(va));
   }
}

static bool si_setup_compute_scratch_buffer(struct si_context *sctx, struct si_shader *shader)
{
   uint64_t scratch_bo_size =
      sctx->compute_scratch_buffer ? sctx->compute_scratch_buffer->b.b.width0 : 0;
   uint64_t scratch_needed = (uint64_t)sctx->max_seen_compute_scratch_bytes_per_wave *
                             sctx->screen->info.max_scratch_waves;
   assert(scratch_needed);

   if (scratch_bo_size < scratch_needed) {
      si_resource_reference(&sctx->compute_scratch_buffer, NULL);

      sctx->compute_scratch_buffer =
         si_aligned_buffer_create(&sctx->screen->b,
                                  PIPE_RESOURCE_FLAG_UNMAPPABLE | SI_RESOURCE_FLAG_DRIVER_INTERNAL |
                                  SI_RESOURCE_FLAG_DISCARDABLE,
                                  PIPE_USAGE_DEFAULT,
                                  scratch_needed, sctx->screen->info.pte_fragment_size);

      if (!sctx->compute_scratch_buffer)
         return false;
   }

   /* Set the scratch address in the shader binary. */
   if (!sctx->screen->info.has_scratch_base_registers) {
      uint64_t scratch_va = sctx->compute_scratch_buffer->gpu_address;

      if (shader->scratch_va != scratch_va) {
         if (!si_shader_binary_upload(sctx->screen, shader, scratch_va))
            return false;

         shader->scratch_va = scratch_va;
      }
   }

   return true;
}

static bool si_switch_compute_shader(struct si_context *sctx, struct si_compute *program,
                                     struct si_shader *shader, bool *prefetch,
                                     unsigned variable_shared_size)
{
   struct radeon_cmdbuf *cs = &sctx->gfx_cs;
   const struct ac_shader_config *config = &shader->config;
   unsigned rsrc2;

   *prefetch = false;

   if (sctx->cs_shader_state.emitted_program == program &&
       sctx->cs_shader_state.variable_shared_size == variable_shared_size)
      return true;

   /* copy rsrc2 so we don't have to change it inside the si_shader object */
   rsrc2 = config->rsrc2;

   /* only do this for OpenCL */
   if (variable_shared_size) {
      unsigned shared_size = program->sel.info.base.shared_size + variable_shared_size;
      unsigned lds_blocks = 0;

      if (sctx->gfx_level <= GFX6) {
         lds_blocks += align(shared_size, 256) >> 8;
      } else {
         lds_blocks += align(shared_size, 512) >> 9;
      }

      /* TODO: use si_multiwave_lds_size_workaround */
      assert(lds_blocks <= 0xFF);

      rsrc2 &= C_00B84C_LDS_SIZE;
      rsrc2 |= S_00B84C_LDS_SIZE(lds_blocks);
   }

   if (config->scratch_bytes_per_wave) {
      /* Prevent race conditions for accesses to shader->scratch_va and shader->bo, which
       * can change when scratch_va is updated. Any accesses to shader->bo must also be inside
       * the lock.
       *
       * TODO: This lock could be removed if the scratch address was passed via user SGPRs instead
       *       of the shader binary.
       */
      if (!sctx->screen->info.has_scratch_base_registers)
         simple_mtx_lock(&shader->selector->mutex);

      /* Update max_seen_compute_scratch_bytes_per_wave and compute_tmpring_size. */
      si_get_scratch_tmpring_size(sctx, config->scratch_bytes_per_wave, true,
                                  &sctx->compute_tmpring_size);

      if (!si_setup_compute_scratch_buffer(sctx, shader))
         return false;

      radeon_add_to_buffer_list(sctx, &sctx->gfx_cs, sctx->compute_scratch_buffer,
                                RADEON_USAGE_READWRITE | RADEON_PRIO_SCRATCH_BUFFER);
   }

   uint64_t shader_va = shader->bo->gpu_address;

   radeon_add_to_buffer_list(sctx, &sctx->gfx_cs, shader->bo,
                             RADEON_USAGE_READ | RADEON_PRIO_SHADER_BINARY);

   /* shader->bo can't be used after this if the scratch address is inserted into the shader
    * binary.
    */
   if (config->scratch_bytes_per_wave && !sctx->screen->info.has_scratch_base_registers)
      simple_mtx_unlock(&shader->selector->mutex);

   if (sctx->gfx_level >= GFX12) {
      gfx12_push_compute_sh_reg(R_00B830_COMPUTE_PGM_LO, shader_va >> 8);
      gfx12_opt_push_compute_sh_reg(R_00B848_COMPUTE_PGM_RSRC1,
                                    SI_TRACKED_COMPUTE_PGM_RSRC1, config->rsrc1);
      gfx12_opt_push_compute_sh_reg(R_00B84C_COMPUTE_PGM_RSRC2,
                                    SI_TRACKED_COMPUTE_PGM_RSRC2, rsrc2);
      gfx12_opt_push_compute_sh_reg(R_00B8A0_COMPUTE_PGM_RSRC3,
                                    SI_TRACKED_COMPUTE_PGM_RSRC3, config->rsrc3);
      gfx12_opt_push_compute_sh_reg(R_00B860_COMPUTE_TMPRING_SIZE,
                                    SI_TRACKED_COMPUTE_TMPRING_SIZE, sctx->compute_tmpring_size);
      if (config->scratch_bytes_per_wave) {
         gfx12_opt_push_compute_sh_reg(R_00B840_COMPUTE_DISPATCH_SCRATCH_BASE_LO,
                                       SI_TRACKED_COMPUTE_DISPATCH_SCRATCH_BASE_LO,
                                       sctx->compute_scratch_buffer->gpu_address >> 8);
         gfx12_opt_push_compute_sh_reg(R_00B844_COMPUTE_DISPATCH_SCRATCH_BASE_HI,
                                       SI_TRACKED_COMPUTE_DISPATCH_SCRATCH_BASE_HI,
                                       sctx->compute_scratch_buffer->gpu_address >> 40);
      }
   } else if (sctx->screen->info.has_set_sh_pairs_packed) {
      gfx11_push_compute_sh_reg(R_00B830_COMPUTE_PGM_LO, shader_va >> 8);
      gfx11_opt_push_compute_sh_reg(R_00B848_COMPUTE_PGM_RSRC1,
                                    SI_TRACKED_COMPUTE_PGM_RSRC1, config->rsrc1);
      gfx11_opt_push_compute_sh_reg(R_00B84C_COMPUTE_PGM_RSRC2,
                                    SI_TRACKED_COMPUTE_PGM_RSRC2, rsrc2);
      gfx11_opt_push_compute_sh_reg(R_00B8A0_COMPUTE_PGM_RSRC3,
                                    SI_TRACKED_COMPUTE_PGM_RSRC3, config->rsrc3);
      gfx11_opt_push_compute_sh_reg(R_00B860_COMPUTE_TMPRING_SIZE,
                                    SI_TRACKED_COMPUTE_TMPRING_SIZE, sctx->compute_tmpring_size);
      if (config->scratch_bytes_per_wave) {
         gfx11_opt_push_compute_sh_reg(R_00B840_COMPUTE_DISPATCH_SCRATCH_BASE_LO,
                                       SI_TRACKED_COMPUTE_DISPATCH_SCRATCH_BASE_LO,
                                       sctx->compute_scratch_buffer->gpu_address >> 8);
         gfx11_opt_push_compute_sh_reg(R_00B844_COMPUTE_DISPATCH_SCRATCH_BASE_HI,
                                       SI_TRACKED_COMPUTE_DISPATCH_SCRATCH_BASE_HI,
                                       sctx->compute_scratch_buffer->gpu_address >> 40);
      }
   } else {
      radeon_begin(cs);
      radeon_set_sh_reg(R_00B830_COMPUTE_PGM_LO, shader_va >> 8);
      radeon_opt_set_sh_reg2(R_00B848_COMPUTE_PGM_RSRC1,
                             SI_TRACKED_COMPUTE_PGM_RSRC1,
                             config->rsrc1, rsrc2);
      radeon_opt_set_sh_reg(R_00B860_COMPUTE_TMPRING_SIZE,
                            SI_TRACKED_COMPUTE_TMPRING_SIZE, sctx->compute_tmpring_size);

      if (config->scratch_bytes_per_wave && sctx->screen->info.has_scratch_base_registers) {
         radeon_opt_set_sh_reg2(R_00B840_COMPUTE_DISPATCH_SCRATCH_BASE_LO,
                                SI_TRACKED_COMPUTE_DISPATCH_SCRATCH_BASE_LO,
                                sctx->compute_scratch_buffer->gpu_address >> 8,
                                sctx->compute_scratch_buffer->gpu_address >> 40);
      }

      if (sctx->gfx_level >= GFX10) {
         radeon_opt_set_sh_reg(R_00B8A0_COMPUTE_PGM_RSRC3,
                               SI_TRACKED_COMPUTE_PGM_RSRC3, config->rsrc3);
      }
      radeon_end();
   }

   COMPUTE_DBG(sctx->screen,
               "COMPUTE_PGM_RSRC1: 0x%08x "
               "COMPUTE_PGM_RSRC2: 0x%08x\n",
               config->rsrc1, config->rsrc2);

   sctx->cs_shader_state.emitted_program = program;
   sctx->cs_shader_state.variable_shared_size = variable_shared_size;

   *prefetch = true;
   return true;
}

static void si_setup_nir_user_data(struct si_context *sctx, const struct pipe_grid_info *info)
{
   struct si_compute *program = sctx->cs_shader_state.program;
   struct si_shader_selector *sel = &program->sel;
   struct radeon_cmdbuf *cs = &sctx->gfx_cs;
   unsigned grid_size_reg = R_00B900_COMPUTE_USER_DATA_0 + 4 * SI_NUM_RESOURCE_SGPRS;
   unsigned block_size_reg = grid_size_reg +
                             /* 12 bytes = 3 dwords. */
                             12 * sel->info.uses_grid_size;
   unsigned cs_user_data_reg = block_size_reg + 4 * program->sel.info.uses_variable_block_size;

   if (sel->info.uses_grid_size && info->indirect) {
      for (unsigned i = 0; i < 3; ++i) {
         si_cp_copy_data(sctx, &sctx->gfx_cs, COPY_DATA_REG, NULL, (grid_size_reg >> 2) + i,
                         COPY_DATA_SRC_MEM, si_resource(info->indirect),
                         info->indirect_offset + 4 * i);
      }
   }

   if (sctx->gfx_level >= GFX12) {
      if (sel->info.uses_grid_size && !info->indirect) {
         gfx12_push_compute_sh_reg(grid_size_reg, info->grid[0]);
         gfx12_push_compute_sh_reg(grid_size_reg + 4, info->grid[1]);
         gfx12_push_compute_sh_reg(grid_size_reg + 8, info->grid[2]);
      }

      if (sel->info.uses_variable_block_size) {
         uint32_t value = info->block[0] | (info->block[1] << 10) | (info->block[2] << 20);
         gfx12_push_compute_sh_reg(block_size_reg, value);
      }

      if (sel->info.base.cs.user_data_components_amd) {
         unsigned num = sel->info.base.cs.user_data_components_amd;
         for (unsigned i = 0; i < num; i++)
            gfx12_push_compute_sh_reg(cs_user_data_reg + i * 4, sctx->cs_user_data[i]);
      }
   } else if (sctx->screen->info.has_set_sh_pairs_packed) {
      if (sel->info.uses_grid_size && !info->indirect) {
         gfx11_push_compute_sh_reg(grid_size_reg, info->grid[0]);
         gfx11_push_compute_sh_reg(grid_size_reg + 4, info->grid[1]);
         gfx11_push_compute_sh_reg(grid_size_reg + 8, info->grid[2]);
      }

      if (sel->info.uses_variable_block_size) {
         uint32_t value = info->block[0] | (info->block[1] << 10) | (info->block[2] << 20);
         gfx11_push_compute_sh_reg(block_size_reg, value);
      }

      if (sel->info.base.cs.user_data_components_amd) {
         unsigned num = sel->info.base.cs.user_data_components_amd;
         for (unsigned i = 0; i < num; i++)
            gfx11_push_compute_sh_reg(cs_user_data_reg + i * 4, sctx->cs_user_data[i]);
      }
   } else {
      radeon_begin(cs);

      if (sel->info.uses_grid_size && !info->indirect) {
         radeon_set_sh_reg_seq(grid_size_reg, 3);
         radeon_emit(info->grid[0]);
         radeon_emit(info->grid[1]);
         radeon_emit(info->grid[2]);
      }

      if (sel->info.uses_variable_block_size) {
         uint32_t value = info->block[0] | (info->block[1] << 10) | (info->block[2] << 20);
         radeon_set_sh_reg(block_size_reg, value);
      }

      if (sel->info.base.cs.user_data_components_amd) {
         unsigned num = sel->info.base.cs.user_data_components_amd;
         radeon_set_sh_reg_seq(cs_user_data_reg, num);
         radeon_emit_array(sctx->cs_user_data, num);
      }
      radeon_end();
   }
}

static bool si_get_2d_interleave_size(const struct pipe_grid_info *info,
                                      unsigned *log_x, unsigned *log_y)
{
   /* The following code produces this behavior:
    *
    *     WG size     |   WG block/SE  | Thread block/SE
    *  ( 1, 32) =  32 |  (16,  1) = 16 | ( 16, 32) = 512
    *  ( 2, 16) =  32 |  ( 8,  2) = 16 | ( 16, 32) = 512
    *  ( 2, 32) =  64 |  (16,  1) = 16 | ( 32, 32) = 1024
    *  ( 4,  8) =  32 |  ( 4,  4) = 16 | ( 16, 32) = 512
    *  ( 4, 16) =  64 |  ( 8,  2) = 16 | ( 32, 32) = 1024
    *  ( 4, 32) = 128 |  ( 8,  1) =  8 | ( 32, 32) = 1024
    *  ( 8,  4) =  32 |  ( 2,  8) = 16 | ( 16, 32) = 512
    *  ( 8,  8) =  64 |  ( 4,  4) = 16 | ( 32, 32) = 1024
    *  ( 8, 16) = 128 |  ( 4,  2) =  8 | ( 32, 32) = 1024
    *  ( 8, 32) = 256 |  ( 4,  1) =  4 | ( 32, 32) = 1024
    *  (16,  2) =  32 |  ( 1, 16) = 16 | ( 16, 32) = 512
    *  (16,  4) =  64 |  ( 2,  8) = 16 | ( 32, 32) = 1024
    *  (16,  8) = 128 |  ( 2,  4) =  8 | ( 32, 32) = 1024
    *  (16, 16) = 256 |  ( 2,  2) =  4 | ( 32, 32) = 1024
    *  (16, 32) = 512 |  ( 2,  1) =  2 | ( 32, 32) = 1024
    *  (32,  1) =  32 |  ( 1, 16) = 16 | ( 32, 16) = 512
    *  (32,  2) =  64 |  ( 1, 16) = 16 | ( 32, 32) = 1024
    *  (32,  4) = 128 |  ( 1,  8) =  8 | ( 32, 32) = 1024
    *  (32,  8) = 256 |  ( 1,  4) =  4 | ( 32, 32) = 1024
    *  (32, 16) = 512 |  ( 1,  2) =  2 | ( 32, 32) = 1024
    *
    * For 3D workgroups, the total 2D thread count is divided by Z.
    * Example with Z=8, showing only a 2D slice of the grid:
    *
    *     WG size     |   WG block/SE  | Thread block/SE
    *  ( 1, 32) =  32 |  ( 4,  1) =  4 | (  4, 32) = 128
    *  ( 2, 16) =  32 |  ( 4,  1) =  4 | (  8, 16) = 128
    *  ( 2, 32) =  64 |  ( 2,  1) =  2 | (  4, 32) = 128
    *  ( 4,  8) =  32 |  ( 2,  2) =  4 | (  8, 16) = 128
    *  ( 4, 16) =  64 |  ( 2,  1) =  2 | (  8, 16) = 128
    *  ( 8,  4) =  32 |  ( 1,  4) =  4 | (  8, 16) = 128
    *  ( 8,  8) =  64 |  ( 1,  2) =  2 | (  8, 16) = 128
    *  (16,  2) =  32 |  ( 1,  4) =  4 | ( 16,  8) = 128
    *  (16,  4) =  64 |  ( 1,  2) =  2 | ( 16,  8) = 128
    *  (32,  1) =  32 |  ( 1,  4) =  4 | ( 32,  4) = 128
    *  (32,  2) =  64 |  ( 1,  2) =  2 | ( 32,  4) = 128
    *
    * It tries to find a WG block size that corresponds to (N, N) or (N, 2*N) threads,
    * but it's limited by the maximum WGs/SE, which is 16, and the number of threads/SE,
    * which we set to 1024.
    */
   unsigned max_threads_per_se = 1024;
   unsigned threads_per_threadgroup = info->block[0] * info->block[1] * info->block[2];
   unsigned workgroups_per_se = MIN2(max_threads_per_se / threads_per_threadgroup, 16);
   unsigned log_workgroups_per_se = util_logbase2(workgroups_per_se);

   if (!log_workgroups_per_se)
      return false;

   assert(log_workgroups_per_se <= 4);

   *log_x = MIN2(log_workgroups_per_se, 4);
   *log_y = log_workgroups_per_se - *log_x;

   while (*log_x > 0 && *log_y < 4 &&
          info->block[0] * (1 << *log_x) > info->block[1] * (1 << *log_y)) {
      (*log_x)--;
      (*log_y)++;
   }

   assert(*log_x + *log_y <= 4);
   return true;
}

static void si_emit_dispatch_packets(struct si_context *sctx, const struct pipe_grid_info *info)
{
   struct si_screen *sscreen = sctx->screen;
   struct radeon_cmdbuf *cs = &sctx->gfx_cs;
   bool render_cond_bit = sctx->render_cond_enabled;
   unsigned threads_per_threadgroup = info->block[0] * info->block[1] * info->block[2];
   unsigned waves_per_threadgroup =
      DIV_ROUND_UP(threads_per_threadgroup, sctx->cs_shader_state.program->shader.wave_size);
   unsigned threadgroups_per_cu = 1;

   if (sctx->gfx_level >= GFX10 && waves_per_threadgroup == 1)
      threadgroups_per_cu = 2;

   if (unlikely(sctx->sqtt_enabled)) {
      if (info->indirect) {
         si_sqtt_write_event_marker(sctx, &sctx->gfx_cs,
                                    EventCmdDispatchIndirect,
                                    UINT_MAX, UINT_MAX, UINT_MAX);
      } else {
         si_write_event_with_dims_marker(sctx, &sctx->gfx_cs,
                                         EventCmdDispatch,
                                         info->grid[0], info->grid[1], info->grid[2]);
      }
   }

   radeon_begin(cs);
   unsigned compute_resource_limits =
      ac_get_compute_resource_limits(&sscreen->info, waves_per_threadgroup,
                                     sctx->cs_max_waves_per_sh,
                                     threadgroups_per_cu);

   if (sctx->gfx_level >= GFX12) {
      gfx12_opt_push_compute_sh_reg(R_00B854_COMPUTE_RESOURCE_LIMITS,
                                    SI_TRACKED_COMPUTE_RESOURCE_LIMITS,
                                    compute_resource_limits);
   } else if (sctx->screen->info.has_set_sh_pairs_packed) {
      gfx11_opt_push_compute_sh_reg(R_00B854_COMPUTE_RESOURCE_LIMITS,
                                    SI_TRACKED_COMPUTE_RESOURCE_LIMITS,
                                    compute_resource_limits);
   } else {
      radeon_opt_set_sh_reg(R_00B854_COMPUTE_RESOURCE_LIMITS,
                            SI_TRACKED_COMPUTE_RESOURCE_LIMITS,
                            compute_resource_limits);
   }

   unsigned dispatch_initiator = S_00B800_COMPUTE_SHADER_EN(1) | S_00B800_FORCE_START_AT_000(1) |
                                 /* If the KMD allows it (there is a KMD hw register for it),
                                  * allow launching waves out-of-order. (same as Vulkan)
                                  * Not available in gfx940.
                                  */
                                 S_00B800_ORDER_MODE(!sctx->cs_shader_state.program->sel.info.uses_atomic_ordered_add &&
                                                     sctx->gfx_level >= GFX7 &&
                                                     (sctx->family < CHIP_GFX940 || sctx->screen->info.has_graphics)) |
                                 S_00B800_CS_W32_EN(sctx->cs_shader_state.program->shader.wave_size == 32);

   const uint *last_block = info->last_block;
   bool partial_block_en = last_block[0] || last_block[1] || last_block[2];
   uint32_t num_threads[3];

   if (sctx->gfx_level >= GFX12) {
      num_threads[0] = S_00B81C_NUM_THREAD_FULL_GFX12(info->block[0]);
      num_threads[1] = S_00B820_NUM_THREAD_FULL_GFX12(info->block[1]);
   } else {
      num_threads[0] = S_00B81C_NUM_THREAD_FULL_GFX6(info->block[0]);
      num_threads[1] = S_00B820_NUM_THREAD_FULL_GFX6(info->block[1]);
   }
   num_threads[2] = S_00B824_NUM_THREAD_FULL(info->block[2]);

   if (partial_block_en) {
      unsigned partial[3];

      /* If no partial_block, these should be an entire block size, not 0. */
      partial[0] = last_block[0] ? last_block[0] : info->block[0];
      partial[1] = last_block[1] ? last_block[1] : info->block[1];
      partial[2] = last_block[2] ? last_block[2] : info->block[2];

      num_threads[0] |= S_00B81C_NUM_THREAD_PARTIAL(partial[0]);
      num_threads[1] |= S_00B820_NUM_THREAD_PARTIAL(partial[1]);
      num_threads[2] |= S_00B824_NUM_THREAD_PARTIAL(partial[2]);

      dispatch_initiator |= S_00B800_PARTIAL_TG_EN(1);
   }

   if (sctx->gfx_level >= GFX12) {
      /* Set PING_PONG_EN for every other dispatch.
       * Only allowed on a gfx queue, and PARTIAL_TG_EN and USE_THREAD_DIMENSIONS must be 0.
       */
      if (sctx->is_gfx_queue && !partial_block_en &&
          !sctx->cs_shader_state.program->sel.info.uses_atomic_ordered_add) {
         dispatch_initiator |= S_00B800_PING_PONG_EN(sctx->compute_ping_pong_launch);
         sctx->compute_ping_pong_launch ^= 1;
      }

      /* Thread tiling within a workgroup. */
      switch (sctx->cs_shader_state.program->shader.selector->info.base.derivative_group) {
      case DERIVATIVE_GROUP_LINEAR:
         break;
      case DERIVATIVE_GROUP_QUADS:
         num_threads[0] |= S_00B81C_INTERLEAVE_BITS_X(1); /* 2x2 */
         num_threads[1] |= S_00B820_INTERLEAVE_BITS_Y(1);
         break;
      case DERIVATIVE_GROUP_NONE:
         /* These are the only legal combinations. */
         if (info->block[0] % 8 == 0 && info->block[1] % 8 == 0) {
            num_threads[0] |= S_00B81C_INTERLEAVE_BITS_X(3); /* 8x8 */
            num_threads[1] |= S_00B820_INTERLEAVE_BITS_Y(3);
         } else if (info->block[0] % 4 == 0 && info->block[1] % 8 == 0) {
            num_threads[0] |= S_00B81C_INTERLEAVE_BITS_X(2); /* 4x8 */
            num_threads[1] |= S_00B820_INTERLEAVE_BITS_Y(3);
         } else if (info->block[0] % 4 == 0 && info->block[1] % 4 == 0) {
            num_threads[0] |= S_00B81C_INTERLEAVE_BITS_X(2); /* 4x4 */
            num_threads[1] |= S_00B820_INTERLEAVE_BITS_Y(2);
         } else if (info->block[0] % 2 == 0 && info->block[1] % 2 == 0) {
            num_threads[0] |= S_00B81C_INTERLEAVE_BITS_X(1); /* 2x2 */
            num_threads[1] |= S_00B820_INTERLEAVE_BITS_Y(1);
         }
         break;
      }

      /* How many threads should go to 1 SE before moving onto the next if INTERLEAVE_2D_EN == 0.
       * Only these values are valid: 0 (disabled), 64, 128, 256, 512
       * 64 = RT, 256 = non-RT (run benchmarks to be sure)
       */
      unsigned dispatch_interleave = S_00B8BC_INTERLEAVE_1D(256);
      unsigned log_x, log_y;

      /* Launch a 2D subgrid on each SE instead of a 1D subgrid. If enabled, INTERLEAVE_1D is
       * ignored and each SE gets 1 subgrid up to a certain number of threads.
       *
       * Constraints:
       * - Only supported by the gfx queue.
       * - Max 16 workgroups per SE can be launched, max 4 in each dimension.
       * - PARTIAL_TG_EN, USE_THREAD_DIMENSIONS, and ORDERED_APPEND_ENBL must be 0.
       * - COMPUTE_START_X/Y are in units of 2D subgrids, not workgroups
       *   (program COMPUTE_START_X to start_x >> log_x, COMPUTE_START_Y to start_y >> log_y).
       */
      if (sctx->is_gfx_queue && !partial_block_en &&
          (info->indirect || info->grid[1] >= 4) && MIN2(info->block[0], info->block[1]) >= 4 &&
          si_get_2d_interleave_size(info, &log_x, &log_y)) {
         dispatch_interleave = S_00B8BC_INTERLEAVE_1D(1) || /* 1D is disabled */
                               S_00B8BC_INTERLEAVE_2D_X_SIZE(log_x) |
                               S_00B8BC_INTERLEAVE_2D_Y_SIZE(log_y);
         dispatch_initiator |= S_00B800_INTERLEAVE_2D_EN(1);
      }

      if (sctx->is_gfx_queue) {
         radeon_opt_set_sh_reg_idx(R_00B8BC_COMPUTE_DISPATCH_INTERLEAVE,
                                   SI_TRACKED_COMPUTE_DISPATCH_INTERLEAVE, 2, dispatch_interleave);
      } else {
         gfx12_opt_push_compute_sh_reg(R_00B8BC_COMPUTE_DISPATCH_INTERLEAVE,
                                       SI_TRACKED_COMPUTE_DISPATCH_INTERLEAVE, dispatch_interleave);
      }
   }

   if (sctx->gfx_level >= GFX12) {
      gfx12_opt_push_compute_sh_reg(R_00B81C_COMPUTE_NUM_THREAD_X,
                                    SI_TRACKED_COMPUTE_NUM_THREAD_X, num_threads[0]);
      gfx12_opt_push_compute_sh_reg(R_00B820_COMPUTE_NUM_THREAD_Y,
                                    SI_TRACKED_COMPUTE_NUM_THREAD_Y, num_threads[1]);
      gfx12_opt_push_compute_sh_reg(R_00B824_COMPUTE_NUM_THREAD_Z,
                                    SI_TRACKED_COMPUTE_NUM_THREAD_Z, num_threads[2]);
   } else if (sctx->screen->info.has_set_sh_pairs_packed) {
      gfx11_opt_push_compute_sh_reg(R_00B81C_COMPUTE_NUM_THREAD_X,
                                    SI_TRACKED_COMPUTE_NUM_THREAD_X, num_threads[0]);
      gfx11_opt_push_compute_sh_reg(R_00B820_COMPUTE_NUM_THREAD_Y,
                                    SI_TRACKED_COMPUTE_NUM_THREAD_Y, num_threads[1]);
      gfx11_opt_push_compute_sh_reg(R_00B824_COMPUTE_NUM_THREAD_Z,
                                    SI_TRACKED_COMPUTE_NUM_THREAD_Z, num_threads[2]);
   } else {
      radeon_opt_set_sh_reg3(R_00B81C_COMPUTE_NUM_THREAD_X,
                             SI_TRACKED_COMPUTE_NUM_THREAD_X,
                             num_threads[0], num_threads[1], num_threads[2]);
   }

   if (sctx->gfx_level >= GFX12 || sctx->screen->info.has_set_sh_pairs_packed) {
      radeon_end();
      si_emit_buffered_compute_sh_regs(sctx);
      radeon_begin_again(cs);
   }

   if (info->indirect) {
      uint64_t base_va = si_resource(info->indirect)->gpu_address;

      radeon_add_to_buffer_list(sctx, &sctx->gfx_cs, si_resource(info->indirect),
                                RADEON_USAGE_READ | RADEON_PRIO_DRAW_INDIRECT);

      radeon_emit(PKT3(PKT3_SET_BASE, 2, 0) | PKT3_SHADER_TYPE_S(1));
      radeon_emit(1);
      radeon_emit(base_va);
      radeon_emit(base_va >> 32);

      unsigned pkt = PKT3_DISPATCH_INDIRECT;

      if (sctx->gfx_level >= GFX12 && G_00B800_INTERLEAVE_2D_EN(dispatch_initiator))
          pkt = PKT3_DISPATCH_INDIRECT_INTERLEAVED;

      radeon_emit(PKT3(pkt, 1, render_cond_bit) | PKT3_SHADER_TYPE_S(1));
      radeon_emit(info->indirect_offset);
      radeon_emit(dispatch_initiator);
   } else {
      unsigned pkt = PKT3_DISPATCH_DIRECT;

      if (sctx->gfx_level >= GFX12 && G_00B800_INTERLEAVE_2D_EN(dispatch_initiator))
         pkt = PKT3_DISPATCH_DIRECT_INTERLEAVED;

      radeon_emit(PKT3(pkt, 3, render_cond_bit) | PKT3_SHADER_TYPE_S(1));
      radeon_emit(info->grid[0]);
      radeon_emit(info->grid[1]);
      radeon_emit(info->grid[2]);
      radeon_emit(dispatch_initiator);
   }

   if (unlikely(sctx->sqtt_enabled && sctx->gfx_level >= GFX9))
      radeon_event_write(V_028A90_THREAD_TRACE_MARKER);

   radeon_end();
}

static bool si_check_needs_implicit_sync(struct si_context *sctx, uint32_t usage)
{
   /* If the compute shader is going to read from a texture/image written by a
    * previous draw, we must wait for its completion before continuing.
    * Buffers and image stores (from the draw) are not taken into consideration
    * because that's the app responsibility.
    *
    * The OpenGL 4.6 spec says:
    *
    *    buffer object and texture stores performed by shaders are not
    *    automatically synchronized
    *
    * TODO: Bindless textures are not handled, and thus are not synchronized.
    */
   struct si_shader_info *info = &sctx->cs_shader_state.program->sel.info;
   struct si_samplers *samplers = &sctx->samplers[PIPE_SHADER_COMPUTE];
   unsigned mask = samplers->enabled_mask & info->base.textures_used;

   while (mask) {
      int i = u_bit_scan(&mask);
      struct si_sampler_view *sview = (struct si_sampler_view *)samplers->views[i];

      struct si_resource *res = si_resource(sview->base.texture);
      if (sctx->ws->cs_is_buffer_referenced(&sctx->gfx_cs, res->buf, usage))
         return true;
   }

   struct si_images *images = &sctx->images[PIPE_SHADER_COMPUTE];
   mask = BITFIELD_MASK(info->base.num_images) & images->enabled_mask;

   while (mask) {
      int i = u_bit_scan(&mask);
      struct pipe_image_view *sview = &images->views[i];

      struct si_resource *res = si_resource(sview->resource);
      if (sctx->ws->cs_is_buffer_referenced(&sctx->gfx_cs, res->buf, usage))
         return true;
   }
   return false;
}

static void si_launch_grid(struct pipe_context *ctx, const struct pipe_grid_info *info)
{
   struct si_context *sctx = (struct si_context *)ctx;
   struct si_screen *sscreen = sctx->screen;
   struct si_compute *program = sctx->cs_shader_state.program;

   if (program->shader.compilation_failed)
      return;

   bool cs_regalloc_hang = sscreen->info.has_cs_regalloc_hang_bug &&
                           info->block[0] * info->block[1] * info->block[2] > 256;
   if (cs_regalloc_hang) {
      sctx->barrier_flags |= SI_BARRIER_SYNC_PS | SI_BARRIER_SYNC_CS;
      si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier);
   }

   si_check_dirty_buffers_textures(sctx);

   if (sctx->is_gfx_queue) {
      if (sctx->num_draw_calls_sh_coherent.with_cb != sctx->num_draw_calls ||
          sctx->num_draw_calls_sh_coherent.with_db != sctx->num_draw_calls) {
         bool sync_cb = sctx->force_shader_coherency.with_cb ||
                        si_check_needs_implicit_sync(sctx, RADEON_USAGE_CB_NEEDS_IMPLICIT_SYNC);
         bool sync_db = sctx->gfx_level >= GFX12 &&
                        (sctx->force_shader_coherency.with_db ||
                         si_check_needs_implicit_sync(sctx, RADEON_USAGE_DB_NEEDS_IMPLICIT_SYNC));

         si_fb_barrier_after_rendering(sctx,
                                       (sync_cb ? SI_FB_BARRIER_SYNC_CB : 0) |
                                       (sync_db ? SI_FB_BARRIER_SYNC_DB : 0));

         if (sync_cb)
            sctx->num_draw_calls_sh_coherent.with_cb = sctx->num_draw_calls;

         if (sync_db)
            sctx->num_draw_calls_sh_coherent.with_db = sctx->num_draw_calls;
      }

      if (sctx->gfx_level < GFX11)
         gfx6_decompress_textures(sctx, 1 << PIPE_SHADER_COMPUTE);
      else if (sctx->gfx_level < GFX12)
         gfx11_decompress_textures(sctx, 1 << PIPE_SHADER_COMPUTE);
   }

   if (info->indirect) {
      /* Indirect buffers are read through L2 on GFX9-GFX11, but not other hw. */
      if ((sctx->gfx_level <= GFX8 || sscreen->info.cp_sdma_ge_use_system_memory_scope) &&
          si_resource(info->indirect)->L2_cache_dirty) {
         sctx->barrier_flags |= SI_BARRIER_WB_L2 | SI_BARRIER_PFP_SYNC_ME;
         si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier);
         si_resource(info->indirect)->L2_cache_dirty = false;
      }
   }

   si_need_gfx_cs_space(sctx, 0, 0);

   /* If we're using a secure context, determine if cs must be secure or not */
   if (unlikely(radeon_uses_secure_bos(sctx->ws))) {
      bool secure = si_compute_resources_check_encrypted(sctx);
      if (secure != sctx->ws->cs_is_secure(&sctx->gfx_cs)) {
         si_flush_gfx_cs(sctx, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW |
                               RADEON_FLUSH_TOGGLE_SECURE_SUBMISSION,
                         NULL);
      }
   }

   if (unlikely(sctx->perfetto_enabled))
      trace_si_begin_compute(&sctx->trace);

   if (sctx->bo_list_add_all_compute_resources)
      si_compute_resources_add_all_to_bo_list(sctx);

   /* Skipping setting redundant registers on compute queues breaks compute. */
   if (!sctx->is_gfx_queue) {
      BITSET_CLEAR_RANGE(sctx->tracked_regs.reg_saved_mask,
                         SI_FIRST_TRACKED_OTHER_REG, SI_NUM_ALL_TRACKED_REGS - 1);
   }

   /* First emit registers. */
   bool prefetch;
   if (!si_switch_compute_shader(sctx, program, &program->shader, &prefetch,
                                 info->variable_shared_mem))
      return;

   si_emit_compute_shader_pointers(sctx);

   /* Global buffers */
   for (unsigned i = 0; i < sctx->max_global_buffers; i++) {
      struct si_resource *buffer = si_resource(sctx->global_buffers[i]);
      if (!buffer) {
         continue;
      }
      radeon_add_to_buffer_list(sctx, &sctx->gfx_cs, buffer,
                                RADEON_USAGE_READWRITE | RADEON_PRIO_SHADER_RW_BUFFER);
   }

   for (unsigned i = 0; i < info->num_globals; i++) {
      struct si_resource *buffer = si_resource(info->globals[i]);
      if (!buffer) {
         continue;
      }
      radeon_add_to_buffer_list(sctx, &sctx->gfx_cs, buffer,
                                RADEON_USAGE_READWRITE | RADEON_PRIO_SHADER_RW_BUFFER);
   }

   /* Registers that are not read from memory should be set before this: */
   si_emit_barrier_direct(sctx);

   if (sctx->is_gfx_queue && si_is_atom_dirty(sctx, &sctx->atoms.s.render_cond)) {
      sctx->atoms.s.render_cond.emit(sctx, -1);
      si_set_atom_dirty(sctx, &sctx->atoms.s.render_cond, false);
   }

   /* Prefetch the compute shader to L2. */
   if (sctx->gfx_level >= GFX7 && sctx->screen->info.has_cp_dma && prefetch)
      si_cp_dma_prefetch(sctx, &program->shader.bo->b.b, 0, program->shader.bo->b.b.width0);

   si_setup_nir_user_data(sctx, info);

   si_emit_dispatch_packets(sctx, info);

   if (unlikely(sctx->current_saved_cs)) {
      si_trace_emit(sctx);
      si_log_compute_state(sctx, sctx->log);
   }

   if (sctx->gfx_level < GFX12) {
      /* Mark displayable DCC as dirty for bound images. */
      unsigned display_dcc_store_mask = sctx->images[PIPE_SHADER_COMPUTE].display_dcc_store_mask &
                                  BITFIELD_MASK(program->sel.info.base.num_images);
      while (display_dcc_store_mask) {
         struct si_texture *tex = (struct si_texture *)
            sctx->images[PIPE_SHADER_COMPUTE].views[u_bit_scan(&display_dcc_store_mask)].resource;

         si_mark_display_dcc_dirty(sctx, tex);
      }

      /* TODO: Bindless images don't set displayable_dcc_dirty after image stores. */
   }

   sctx->compute_is_busy = true;
   sctx->num_compute_calls++;

   if (unlikely(sctx->perfetto_enabled))
      trace_si_end_compute(&sctx->trace, info->grid[0], info->grid[1], info->grid[2]);

   if (cs_regalloc_hang) {
      sctx->barrier_flags |= SI_BARRIER_SYNC_CS;
      si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier);
   }
}

void si_destroy_compute(struct si_compute *program)
{
   struct si_shader_selector *sel = &program->sel;

   util_queue_drop_job(&sel->screen->shader_compiler_queue, &sel->ready);
   util_queue_fence_destroy(&sel->ready);

   si_shader_destroy(&program->shader);
   ralloc_free(program->sel.nir);
   simple_mtx_destroy(&sel->mutex);
   FREE(program);
}

static void si_delete_compute_state(struct pipe_context *ctx, void *state)
{
   struct si_compute *program = (struct si_compute *)state;
   struct si_context *sctx = (struct si_context *)ctx;

   if (!state)
      return;

   if (program == sctx->cs_shader_state.program)
      sctx->cs_shader_state.program = NULL;

   if (program == sctx->cs_shader_state.emitted_program)
      sctx->cs_shader_state.emitted_program = NULL;

   si_compute_reference(&program, NULL);
}

void si_init_compute_functions(struct si_context *sctx)
{
   sctx->b.create_compute_state = si_create_compute_state;
   sctx->b.delete_compute_state = si_delete_compute_state;
   sctx->b.bind_compute_state = si_bind_compute_state;
   sctx->b.get_compute_state_info = si_get_compute_state_info;
   sctx->b.set_global_binding = si_set_global_binding;
   sctx->b.launch_grid = si_launch_grid;

#if 0 /* test for si_get_2d_interleave_size */
   static bool visited = false;
   if (visited)
      return;

   visited = true;
   struct pipe_grid_info info = {};
   info.grid[0] = info.grid[1] = info.grid[2] = 1024;
   info.block[2] = 1;

   for (unsigned block_3d = 0; block_3d < 2; block_3d++) {
      printf("    WG size     |   WG block/SE  | Thread block/SE\n");

      for (unsigned x = 1; x <= 32; x *= 2) {
         for (unsigned y = 1; y <= 32; y *= 2) {
            info.block[0] = x;
            info.block[1] = y;
            info.block[2] = block_3d ? 8 : 1;

            if ((x * y) % 32)
               continue;

            unsigned log_x, log_y;
            if (!si_get_2d_interleave_size(&info, &log_x, &log_y))
               continue;

            printf(" (%2u, %2u) = %3u |  (%2u, %2u) = %2u | (%3u,%3u) = %u\n",
                   info.block[0], info.block[1], info.block[0] * info.block[1],
                   1 << log_x, 1 << log_y, (1 << log_x) * (1 << log_y),
                   info.block[0] * (1 << log_x), info.block[1] * (1 << log_y),
                   info.block[0] * (1 << log_x) * info.block[1] * (1 << log_y));
         }
      }
   }
#endif
}
