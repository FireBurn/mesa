/*
 * Copyright 2016 Bas Nieuwenhuizen
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_LLVM_UTIL_H
#define AC_LLVM_UTIL_H

#include "amd_family.h"
#include "util/macros.h"
#include <llvm-c/TargetMachine.h>
#include <llvm/Config/llvm-config.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ac_compiler_passes;
struct ac_llvm_context;

/* Attributes at call sites of intrinsics. */
enum ac_call_site_attr {
   AC_ATTR_INVARIANT_LOAD = 1 << 0,
   AC_ATTR_CONVERGENT = 1 << 1,
};

enum ac_target_machine_options
{
   AC_TM_SUPPORTS_SPILL       = 1 << 0,
   AC_TM_CHECK_IR             = 1 << 1,
};

enum ac_float_mode
{
   AC_FLOAT_MODE_DEFAULT,
   AC_FLOAT_MODE_DEFAULT_OPENGL,
   AC_FLOAT_MODE_DENORM_FLUSH_TO_ZERO,
};

/* Per-thread persistent LLVM objects. */
struct ac_llvm_compiler {
   /* Default compiler. */
   LLVMTargetMachineRef tm;
   struct ac_midend_optimizer *meo;
   struct ac_backend_optimizer *beo;
};

LLVMTargetRef ac_get_llvm_target(const char *triple);
void ac_llvm_run_atexit_for_destructors(void);
bool ac_is_llvm_processor_supported(LLVMTargetMachineRef tm, const char *processor);
void ac_reset_llvm_all_options_occurrences();
void ac_add_attr_dereferenceable(LLVMValueRef val, uint64_t bytes);
void ac_add_attr_alignment(LLVMValueRef val, uint64_t bytes);
LLVMAttributeRef ac_get_llvm_attribute(LLVMContextRef ctx, const char *str);
void ac_add_function_attr(LLVMContextRef ctx, LLVMValueRef function, int attr_idx,
                          const char *attr);
void ac_dump_module(LLVMModuleRef module);
LLVMModuleRef ac_create_module(LLVMTargetMachineRef tm, LLVMContextRef ctx);
LLVMBuilderRef ac_create_builder(LLVMContextRef ctx, enum ac_float_mode float_mode);
void ac_enable_signed_zeros(struct ac_llvm_context *ctx);
void ac_disable_signed_zeros(struct ac_llvm_context *ctx);

void ac_llvm_add_target_dep_function_attr(LLVMValueRef F, const char *name, unsigned value);
void ac_llvm_set_workgroup_size(LLVMValueRef F, unsigned size);
void ac_llvm_set_target_features(LLVMValueRef F, struct ac_llvm_context *ctx, bool wgp_mode);

PUBLIC void ac_init_shared_llvm_once(void); /* Do not use directly, use ac_init_llvm_once */
void ac_init_llvm_once(void);

bool ac_init_llvm_compiler(struct ac_llvm_compiler *compiler, enum radeon_family family,
                           enum ac_target_machine_options tm_options);
void ac_destroy_llvm_compiler(struct ac_llvm_compiler *compiler);

struct ac_midend_optimizer *ac_create_midend_optimizer(LLVMTargetMachineRef tm,
                                                       bool check_ir);
void ac_destroy_midend_optimiser(struct ac_midend_optimizer *meo);
bool ac_llvm_optimize_module(struct ac_midend_optimizer *meo, LLVMModuleRef module);

struct ac_backend_optimizer *ac_create_backend_optimizer(LLVMTargetMachineRef tm);
void ac_destroy_backend_optimizer(struct ac_backend_optimizer *beo);
bool ac_compile_module_to_elf(struct ac_backend_optimizer *beo, LLVMModuleRef module,
                              char **pelf_buffer, size_t *pelf_size);

static inline bool ac_has_vec3_support(enum amd_gfx_level chip, bool use_format)
{
   /* GFX6 only supports vec3 with load/store format. */
   return chip != GFX6 || use_format;
}

#ifdef __cplusplus
}
#endif

#endif /* AC_LLVM_UTIL_H */
