/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

/**
 * \file
 * Identify sequences of logical operations to convert to bfi
 *
 * It is difficult for opt_algebraic to match general expressions like
 *
 *    (a & some_constant) | (b & ~some_constant)
 *
 * Common cases like some_constant = 0x7fffffff can be added, but this may
 * miss other opportunities. This pass implements that general pattern
 * matching.
 *
 * Either nir_op_bfi or nir_op_bitfield_select may be generated by this pass.
 *
 * Future work may also detect cases like:
 *
 *    (a & some_constant) | ~(b | some_constant)
 *    ~((a | some_constant) & (b | ~some_constant))
 *    etc.
 */

#include "nir_builder.h"

static bool
parse_iand(nir_scalar alu, nir_scalar *value, uint32_t *mask)
{
   if (nir_scalar_alu_op(alu) == nir_op_iand) {
      /* If both source are constants, do not perform the conversion. There
       * are lowerings in opt_algebraic that can generate this pattern on
       * platforms that set has_bfi and avoid_ternary_with_two_constants.
       * Undoing that lowering would result in infinite optimization loops.
       */
      nir_scalar left = nir_scalar_chase_alu_src(alu, 0);
      nir_scalar right = nir_scalar_chase_alu_src(alu, 1);
      if (nir_scalar_is_const(left) && nir_scalar_is_const(right))
         return false;

      if (nir_scalar_is_const(left)) {
         *mask = nir_scalar_as_uint(left);
         *value = right;
         return true;
      } else if (nir_scalar_is_const(right)) {
         *mask = nir_scalar_as_uint(right);
         *value = left;
         return true;
      }
   } else if (nir_scalar_alu_op(alu) == nir_op_extract_u16 ||
              nir_scalar_alu_op(alu) == nir_op_extract_u8) {
      /* There may be leftovers from opt_algebraic that haven't been constant
       * folded yet.
       */
      nir_scalar left = nir_scalar_chase_alu_src(alu, 0);
      if (nir_scalar_is_const(left))
         return false;

      if (nir_scalar_as_uint(nir_scalar_chase_alu_src(alu, 1)) == 0) {
         *mask = nir_scalar_alu_op(alu) == nir_op_extract_u16 ? 0x0000ffff : 0x000000ff;
         *value = left;
         return true;
      }
   }

   return false;
}

static bool
nir_opt_generate_bfi_instr(nir_builder *b,
                           nir_alu_instr *alu,
                           UNUSED void *cb_data)
{
   /* Since none of the source bits will overlap, these are equvalent. */
   if ((alu->op != nir_op_ior &&
        alu->op != nir_op_ixor &&
        alu->op != nir_op_iadd) ||
       alu->def.num_components != 1 || alu->def.bit_size != 32)
      return false;

   nir_scalar alu_scalar = nir_get_scalar(&alu->def, 0);
   nir_scalar left = nir_scalar_chase_alu_src(alu_scalar, 0);
   nir_scalar right = nir_scalar_chase_alu_src(alu_scalar, 1);

   if (!nir_scalar_is_alu(left) || !nir_scalar_is_alu(right))
      return false;

   nir_scalar src1;
   nir_scalar src2;
   uint32_t mask1;
   uint32_t mask2;

   if (!parse_iand(left, &src1, &mask1))
      return false;

   if (!parse_iand(right, &src2, &mask2))
      return false;

   if (mask1 != ~mask2)
      return false;

   nir_scalar insert;
   nir_scalar base;
   uint32_t mask;

   /* The mask used by the bfi instruction must be odd. When the mask is odd,
    * the implict shift applied by the bfi is by zero bits. Since one of the
    * masks must be odd, the rule can always be applied.
    *
    * bitfield_select does not have this restriction, but it doesn't hurt.
    */
   if ((mask1 & 1) != 0) {
      /* Because mask1 == ~mask2. */
      assert((mask2 & 1) == 0);

      mask = mask1;
      insert = src1;
      base = src2;
   } else {
      /* Because mask1 == ~mask2. */
      assert((mask2 & 1) != 0);

      mask = mask2;
      insert = src2;
      base = src1;
   }

   b->cursor = nir_before_instr(&alu->instr);

   nir_def *bfi;

   if (b->shader->options->has_bfi) {
      bfi = nir_bfi(b,
                    nir_imm_int(b, mask),
                    nir_mov_scalar(b, insert),
                    nir_mov_scalar(b, base));
   } else {
      assert(b->shader->options->has_bitfield_select);

      bfi = nir_bitfield_select(b,
                                nir_imm_int(b, mask),
                                nir_mov_scalar(b, insert),
                                nir_mov_scalar(b, base));
   }

   nir_def_replace(&alu->def, bfi);
   return true;
}

bool
nir_opt_generate_bfi(nir_shader *shader)
{
   if (!shader->options->has_bfi && !shader->options->has_bitfield_select)
      return false;

   return nir_shader_alu_pass(shader, nir_opt_generate_bfi_instr,
                              nir_metadata_control_flow, NULL);
}
