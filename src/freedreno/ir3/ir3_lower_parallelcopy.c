/*
 * Copyright © 2021 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "ir3_ra.h"
#include "ir3_shader.h"

struct copy_src {
   unsigned flags;
   union {
      uint32_t imm;
      physreg_t reg;
      unsigned const_num;
   };
};

struct copy_entry {
   physreg_t dst;
   unsigned flags;
   bool done;

   struct copy_src src;
};

static unsigned
copy_entry_size(const struct copy_entry *entry)
{
   return (entry->flags & IR3_REG_HALF) ? 1 : 2;
}

static struct copy_src
get_copy_src(const struct ir3_register *reg, unsigned offset)
{
   if (reg->flags & IR3_REG_IMMED) {
      return (struct copy_src){
         .flags = IR3_REG_IMMED,
         .imm = reg->uim_val,
      };
   } else if (reg->flags & IR3_REG_CONST) {
      return (struct copy_src){
         .flags = IR3_REG_CONST,
         .const_num = reg->num,
      };
   } else {
      return (struct copy_src){
         .flags = 0,
         .reg = ra_reg_get_physreg(reg) + offset,
      };
   }
}

static void
do_xor(struct ir3_instruction *instr, unsigned dst_num, unsigned src1_num,
       unsigned src2_num, unsigned flags)
{
   struct ir3_instruction * xor
      = ir3_instr_create_at(ir3_before_instr(instr), OPC_XOR_B, 1, 2);
   ir3_dst_create(xor, dst_num, flags);
   ir3_src_create(xor, src1_num, flags);
   ir3_src_create(xor, src2_num, flags);
}

static void
do_swap(struct ir3_compiler *compiler, struct ir3_instruction *instr,
        const struct copy_entry *entry)
{
   assert(!entry->src.flags);

   if (entry->flags & IR3_REG_HALF) {
      const unsigned half_size =
         (entry->flags & IR3_REG_SHARED) ? RA_SHARED_HALF_SIZE : RA_HALF_SIZE;

      /* We currently make sure to never emit parallel copies where the
       * source/destination is a half-reg above the range accessable to half
       * registers. However, when a full-reg source overlaps a half-reg
       * destination or vice versa, it can be very, very complicated to come
       * up with a series of "legal" swaps and copies to resolve the
       * parallel copy. So here we provide a fallback to implement the
       * "illegal" swap instead. This may also be useful for implementing
       * "spilling" half-regs to the inaccessable space.
       */
      if (entry->src.reg >= half_size) {
         /* Choose a temporary that doesn't overlap src or dst */
         physreg_t tmp = entry->dst < 2 ? 2 : 0;

         /* Swap src and the temporary */
         do_swap(compiler, instr,
                 &(struct copy_entry){
                    .src = {.reg = entry->src.reg & ~1u},
                    .dst = tmp,
                    .flags = entry->flags & ~IR3_REG_HALF,
                 });

         /* If src and dst are within the same full register, then swapping src
          * with tmp above will also move dst to tmp. Account for that here.
          */
         unsigned dst =
            (entry->src.reg & ~1u) == (entry->dst & ~1u) ?
            tmp + (entry->dst & 1u) : entry->dst;

         /* Do the original swap with src replaced with tmp */
         do_swap(compiler, instr,
                 &(struct copy_entry){
                    .src = {.reg = tmp + (entry->src.reg & 1)},
                    .dst = dst,
                    .flags = entry->flags,
                 });

         /* Swap src and the temporary back */
         do_swap(compiler, instr,
                 &(struct copy_entry){
                    .src = {.reg = entry->src.reg & ~1u},
                    .dst = tmp,
                    .flags = entry->flags & ~IR3_REG_HALF,
                 });
         return;
      }

      /* If dst is not addressable, we only need to swap the arguments and
       * let the case above handle it.
       */
      if (entry->dst >= half_size) {
         do_swap(compiler, instr,
                 &(struct copy_entry){
                    .src = {.reg = entry->dst},
                    .dst = entry->src.reg,
                    .flags = entry->flags,
                 });
         return;
      }
   }

   unsigned src_num = ra_physreg_to_num(entry->src.reg, entry->flags);
   unsigned dst_num = ra_physreg_to_num(entry->dst, entry->flags);

   /* a5xx+ is known to support swz, which enables us to swap two registers
    * in-place. If unsupported we emulate it using the xor trick.
    */
   if (compiler->gen < 5 || (entry->flags & IR3_REG_SHARED)) {
      do_xor(instr, dst_num, dst_num, src_num, entry->flags);
      do_xor(instr, src_num, src_num, dst_num, entry->flags);
      do_xor(instr, dst_num, dst_num, src_num, entry->flags);
   } else {
      struct ir3_instruction *swz =
         ir3_instr_create_at(ir3_before_instr(instr), OPC_SWZ, 2, 2);
      ir3_dst_create(swz, dst_num, entry->flags);
      ir3_dst_create(swz, src_num, entry->flags);
      ir3_src_create(swz, src_num, entry->flags);
      ir3_src_create(swz, dst_num, entry->flags);
      swz->cat1.dst_type = (entry->flags & IR3_REG_HALF) ? TYPE_U16 : TYPE_U32;
      swz->cat1.src_type = (entry->flags & IR3_REG_HALF) ? TYPE_U16 : TYPE_U32;
      swz->repeat = 1;
   }
}

static void
do_copy(struct ir3_compiler *compiler, struct ir3_instruction *instr,
        const struct copy_entry *entry)
{
   if (entry->flags & IR3_REG_HALF) {
      /* See do_swap() for why this is here. */
      const unsigned half_size =
         (entry->flags & IR3_REG_SHARED) ? RA_SHARED_HALF_SIZE : RA_HALF_SIZE;
      if (entry->dst >= half_size) {
         /* TODO: is there a hw instruction we can use for this case? */
         physreg_t tmp = !entry->src.flags && entry->src.reg < 2 ? 2 : 0;

         do_swap(compiler, instr,
                 &(struct copy_entry){
                    .src = {.reg = entry->dst & ~1u},
                    .dst = tmp,
                    .flags = entry->flags & ~IR3_REG_HALF,
                 });

         /* Similar to in do_swap(), account for src being swapped with tmp if
          * src and dst are in the same register.
          */
         struct copy_src src = entry->src;
         if (!src.flags && (src.reg & ~1u) == (entry->dst & ~1u))
            src.reg = tmp + (src.reg & 1u);

         do_copy(compiler, instr,
                 &(struct copy_entry){
                    .src = src,
                    .dst = tmp + (entry->dst & 1),
                    .flags = entry->flags,
                 });

         do_swap(compiler, instr,
                 &(struct copy_entry){
                    .src = {.reg = entry->dst & ~1u},
                    .dst = tmp,
                    .flags = entry->flags & ~IR3_REG_HALF,
                 });
         return;
      }

      if (!entry->src.flags && entry->src.reg >= half_size) {
         unsigned src_num = ra_physreg_to_num(entry->src.reg & ~1u,
                                              entry->flags & ~IR3_REG_HALF);
         unsigned dst_num = ra_physreg_to_num(entry->dst, entry->flags);

         if (entry->src.reg % 2 == 0) {
            /* cov.u32u16 dst, src */
            struct ir3_instruction *cov =
               ir3_instr_create_at(ir3_before_instr(instr), OPC_MOV, 1, 1);
            ir3_dst_create(cov, dst_num, entry->flags);
            ir3_src_create(cov, src_num, entry->flags & ~IR3_REG_HALF);
            cov->cat1.dst_type = TYPE_U16;
            cov->cat1.src_type = TYPE_U32;
         } else {
            /* shr.b dst, src, (16) */
            struct ir3_instruction *shr =
               ir3_instr_create_at(ir3_before_instr(instr), OPC_SHR_B, 1, 2);
            ir3_dst_create(shr, dst_num, entry->flags);
            ir3_src_create(shr, src_num, entry->flags & ~IR3_REG_HALF);
            ir3_src_create(shr, 0, IR3_REG_IMMED)->uim_val = 16;
         }
         return;
      }
   }

   unsigned src_num = ra_physreg_to_num(entry->src.reg, entry->flags);
   unsigned dst_num = ra_physreg_to_num(entry->dst, entry->flags);

   struct ir3_instruction *mov =
      ir3_instr_create_at(ir3_before_instr(instr), OPC_MOV, 1, 1);
   ir3_dst_create(mov, dst_num, entry->flags);
   if (entry->src.flags & (IR3_REG_IMMED | IR3_REG_CONST))
      ir3_src_create(mov, INVALID_REG, (entry->flags & IR3_REG_HALF) | entry->src.flags);
   else
      ir3_src_create(mov, src_num, entry->flags);
   mov->cat1.dst_type = (entry->flags & IR3_REG_HALF) ? TYPE_U16 : TYPE_U32;
   mov->cat1.src_type = (entry->flags & IR3_REG_HALF) ? TYPE_U16 : TYPE_U32;
   if (entry->src.flags & IR3_REG_IMMED)
      mov->srcs[0]->uim_val = entry->src.imm;
   else if (entry->src.flags & IR3_REG_CONST)
      mov->srcs[0]->num = entry->src.const_num;
}

struct copy_ctx {
   /* For each physreg, the number of pending copy entries that use it as a
    * source. Once this drops to zero, then the physreg is unblocked and can
    * be moved to.
    */
   unsigned physreg_use_count[RA_MAX_FILE_SIZE];

   /* For each physreg, the pending copy_entry that uses it as a dest. */
   struct copy_entry *physreg_dst[RA_MAX_FILE_SIZE];

   struct copy_entry entries[RA_MAX_FILE_SIZE];
   unsigned entry_count;
};

static bool
entry_blocked(struct copy_entry *entry, struct copy_ctx *ctx)
{
   for (unsigned i = 0; i < copy_entry_size(entry); i++) {
      if (ctx->physreg_use_count[entry->dst + i] != 0)
         return true;
   }

   return false;
}

static void
split_32bit_copy(struct copy_ctx *ctx, struct copy_entry *entry)
{
   assert(!entry->done);
   assert(!(entry->src.flags & (IR3_REG_IMMED | IR3_REG_CONST)));
   assert(copy_entry_size(entry) == 2);
   struct copy_entry *new_entry = &ctx->entries[ctx->entry_count++];

   new_entry->dst = entry->dst + 1;
   new_entry->src.flags = entry->src.flags;
   new_entry->src.reg = entry->src.reg + 1;
   new_entry->done = false;
   entry->flags |= IR3_REG_HALF;
   new_entry->flags = entry->flags;
   ctx->physreg_dst[entry->dst + 1] = new_entry;
}

static void
_handle_copies(struct ir3_compiler *compiler, struct ir3_instruction *instr,
               struct copy_ctx *ctx)
{
   /* Set up the bookkeeping */
   memset(ctx->physreg_dst, 0, sizeof(ctx->physreg_dst));
   memset(ctx->physreg_use_count, 0, sizeof(ctx->physreg_use_count));

   for (unsigned i = 0; i < ctx->entry_count; i++) {
      struct copy_entry *entry = &ctx->entries[i];
      for (unsigned j = 0; j < copy_entry_size(entry); j++) {
         if (!entry->src.flags)
            ctx->physreg_use_count[entry->src.reg + j]++;

         /* Copies should not have overlapping destinations. */
         assert(!ctx->physreg_dst[entry->dst + j]);
         ctx->physreg_dst[entry->dst + j] = entry;
      }
   }

   bool progress = true;
   while (progress) {
      progress = false;

      /* Step 1: resolve paths in the transfer graph. This means finding
       * copies whose destination aren't blocked by something else and then
       * emitting them, continuing this process until every copy is blocked
       * and there are only cycles left.
       *
       * TODO: We should note that src is also available in dst to unblock
       * cycles that src is involved in.
       */

      for (unsigned i = 0; i < ctx->entry_count; i++) {
         struct copy_entry *entry = &ctx->entries[i];
         if (!entry->done && !entry_blocked(entry, ctx)) {
            entry->done = true;
            progress = true;
            do_copy(compiler, instr, entry);
            for (unsigned j = 0; j < copy_entry_size(entry); j++) {
               if (!entry->src.flags)
                  ctx->physreg_use_count[entry->src.reg + j]--;
               ctx->physreg_dst[entry->dst + j] = NULL;
            }
         }
      }

      if (progress)
         continue;

      /* Step 2: Find partially blocked copies and split them. In the
       * mergedregs case, we can 32-bit copies which are only blocked on one
       * 16-bit half, and splitting them helps get things moving.
       *
       * We can skip splitting copies if the source isn't a register,
       * however, because it does not unblock anything and therefore doesn't
       * contribute to making forward progress with step 1. These copies
       * should still be resolved eventually in step 1 because they can't be
       * part of a cycle.
       */
      for (unsigned i = 0; i < ctx->entry_count; i++) {
         struct copy_entry *entry = &ctx->entries[i];
         if (entry->done || entry->flags & IR3_REG_HALF)
            continue;

         if (((ctx->physreg_use_count[entry->dst] == 0 ||
               ctx->physreg_use_count[entry->dst + 1] == 0)) &&
             !(entry->src.flags & (IR3_REG_IMMED | IR3_REG_CONST))) {
            split_32bit_copy(ctx, entry);
            progress = true;
         }
      }
   }

   /* Step 3: resolve cycles through swapping.
    *
    * At this point, the transfer graph should consist of only cycles.
    * The reason is that, given any physreg n_1 that's the source of a
    * remaining entry, it has a destination n_2, which (because every
    * copy is blocked) is the source of some other copy whose destination
    * is n_3, and so we can follow the chain until we get a cycle. If we
    * reached some other node than n_1:
    *
    *  n_1 -> n_2 -> ... -> n_i
    *          ^             |
    *          |-------------|
    *
    *  then n_2 would be the destination of 2 copies, which is illegal
    *  (checked above in an assert). So n_1 must be part of a cycle:
    *
    *  n_1 -> n_2 -> ... -> n_i
    *  ^                     |
    *  |---------------------|
    *
    *  and this must be only cycle n_1 is involved in, because any other
    *  path starting from n_1 would also have to end in n_1, resulting in
    *  a node somewhere along the way being the destination of 2 copies
    *  when the 2 paths merge.
    *
    *  The way we resolve the cycle is through picking a copy (n_1, n_2)
    *  and swapping n_1 and n_2. This moves n_1 to n_2, so n_2 is taken
    *  out of the cycle:
    *
    *  n_1 -> ... -> n_i
    *  ^              |
    *  |--------------|
    *
    *  and we can keep repeating this until the cycle is empty.
    */

   for (unsigned i = 0; i < ctx->entry_count; i++) {
      struct copy_entry *entry = &ctx->entries[i];
      if (entry->done)
         continue;

      assert(!entry->src.flags);

      /* catch trivial copies */
      if (entry->dst == entry->src.reg) {
         entry->done = true;
         continue;
      }

      do_swap(compiler, instr, entry);

      /* Split any blocking copies whose sources are only partially
       * contained within our destination.
       */
      if (entry->flags & IR3_REG_HALF) {
         for (unsigned j = 0; j < ctx->entry_count; j++) {
            struct copy_entry *blocking = &ctx->entries[j];

            if (blocking->done)
               continue;

            if (blocking->src.reg <= entry->dst &&
                blocking->src.reg + 1 >= entry->dst &&
                !(blocking->flags & IR3_REG_HALF)) {
               split_32bit_copy(ctx, blocking);
            }
         }
      }

      /* Update sources of blocking copies.
       *
       * Note: at this point, every blocking copy's source should be
       * contained within our destination.
       */
      for (unsigned j = 0; j < ctx->entry_count; j++) {
         struct copy_entry *blocking = &ctx->entries[j];
         if (blocking->src.reg >= entry->dst &&
             blocking->src.reg < entry->dst + copy_entry_size(entry)) {
            blocking->src.reg =
               entry->src.reg + (blocking->src.reg - entry->dst);
         }
      }

      entry->done = true;
   }
}

static void
handle_copies(struct ir3_shader_variant *v, struct ir3_instruction *instr,
              struct copy_entry *entries, unsigned entry_count)
{
   struct copy_ctx ctx;

   /* handle shared copies first */
   ctx.entry_count = 0;
   for (unsigned i = 0; i < entry_count; i++) {
      if (entries[i].flags & IR3_REG_SHARED)
         ctx.entries[ctx.entry_count++] = entries[i];
   }
   _handle_copies(v->compiler, instr, &ctx);

   if (v->mergedregs) {
      /* Half regs and full regs are in the same file, so handle everything
       * at once.
       */
      ctx.entry_count = 0;
      for (unsigned i = 0; i < entry_count; i++) {
         if (!(entries[i].flags & IR3_REG_SHARED))
            ctx.entries[ctx.entry_count++] = entries[i];
      }
      _handle_copies(v->compiler, instr, &ctx);
   } else {
      /* There may be both half copies and full copies, so we have to split
       * them up since they don't interfere.
       */
      ctx.entry_count = 0;
      for (unsigned i = 0; i < entry_count; i++) {
         if (entries[i].flags & IR3_REG_HALF)
            ctx.entries[ctx.entry_count++] = entries[i];
      }
      _handle_copies(v->compiler, instr, &ctx);

      ctx.entry_count = 0;
      for (unsigned i = 0; i < entry_count; i++) {
         if (!(entries[i].flags & (IR3_REG_HALF | IR3_REG_SHARED)))
            ctx.entries[ctx.entry_count++] = entries[i];
      }
      _handle_copies(v->compiler, instr, &ctx);
   }
}

void
ir3_lower_copies(struct ir3_shader_variant *v)
{
   DECLARE_ARRAY(struct copy_entry, copies);
   copies_count = copies_sz = 0;
   copies = NULL;

   foreach_block (block, &v->ir->block_list) {
      foreach_instr_safe (instr, &block->instr_list) {
         if (instr->opc == OPC_META_PARALLEL_COPY) {
            copies_count = 0;
            for (unsigned i = 0; i < instr->dsts_count; i++) {
               struct ir3_register *dst = instr->dsts[i];
               struct ir3_register *src = instr->srcs[i];
               unsigned flags = dst->flags & (IR3_REG_HALF | IR3_REG_SHARED);
               unsigned dst_physreg = ra_reg_get_physreg(dst);
               for (unsigned j = 0; j < reg_elems(dst); j++) {
                  array_insert(
                     NULL, copies,
                     (struct copy_entry){
                        .dst = dst_physreg + j * reg_elem_size(dst),
                        .src = get_copy_src(src, j * reg_elem_size(dst)),
                        .flags = flags,
                     });
               }
            }
            handle_copies(v, instr, copies, copies_count);
            list_del(&instr->node);
         } else if (instr->opc == OPC_META_COLLECT) {
            copies_count = 0;
            struct ir3_register *dst = instr->dsts[0];
            unsigned flags = dst->flags & (IR3_REG_HALF | IR3_REG_SHARED);
            for (unsigned i = 0; i < instr->srcs_count; i++) {
               struct ir3_register *src = instr->srcs[i];
               if ((src->flags & (IR3_REG_CONST | IR3_REG_IMMED)) ||
                   src->num != INVALID_REG) {
                  array_insert(NULL, copies,
                               (struct copy_entry){
                                  .dst = ra_num_to_physreg(dst->num + i, flags),
                                  .src = get_copy_src(src, 0),
                                  .flags = flags,
                               });
               }
            }
            handle_copies(v, instr, copies, copies_count);
            list_del(&instr->node);
         } else if (instr->opc == OPC_META_SPLIT) {
            copies_count = 0;
            struct ir3_register *dst = instr->dsts[0];
            struct ir3_register *src = instr->srcs[0];
            unsigned flags = src->flags & (IR3_REG_HALF | IR3_REG_SHARED);
            array_insert(NULL, copies,
                         (struct copy_entry){
                            .dst = ra_reg_get_physreg(dst),
                            .src = get_copy_src(
                               src, instr->split.off * reg_elem_size(dst)),
                            .flags = flags,
                         });
            handle_copies(v, instr, copies, copies_count);
            list_del(&instr->node);
         } else if (instr->opc == OPC_META_PHI) {
            list_del(&instr->node);
         } else if (instr->opc == OPC_MOV) {
            /* There seems to be a HW bug where moves where the source is 16-bit
             * non-shared and the destination is 16-bit shared don't work when
             * only fibers 64-127 are active. We work around it by instead
             * generating a narrowing mov, which only works with even-numbered
             * registers (i.e. .x and .z), but for odd numbers we can swap the
             * components of the normal src and its even neighbor and then
             * unswap afterwords to make it work for everything.
             */
            if (v->compiler->mov_half_shared_quirk &&
                (instr->dsts[0]->flags & IR3_REG_SHARED) &&
                (instr->dsts[0]->flags & IR3_REG_HALF) &&
                !(instr->srcs[0]->flags & (IR3_REG_SHARED | IR3_REG_IMMED |
                                           IR3_REG_CONST)) &&
                (instr->srcs[0]->flags & IR3_REG_HALF)) {
               unsigned src_num = instr->srcs[0]->num;
               unsigned dst_num = instr->dsts[0]->num;

               for (unsigned i = 0; i <= instr->repeat; i++,
                    src_num++, dst_num++) {
                  if (src_num & 1) {
                     for (unsigned i = 0; i < 2; i++) {
                        struct ir3_cursor cursor = i == 0
                                                      ? ir3_before_instr(instr)
                                                      : ir3_after_instr(instr);
                        struct ir3_instruction *swz =
                           ir3_instr_create_at(cursor, OPC_SWZ, 2, 2);
                        ir3_dst_create(swz, src_num - 1, IR3_REG_HALF);
                        ir3_dst_create(swz, src_num, IR3_REG_HALF);
                        ir3_src_create(swz, src_num, IR3_REG_HALF);
                        ir3_src_create(swz, src_num - 1, IR3_REG_HALF);
                        swz->cat1.dst_type = TYPE_U16;
                        swz->cat1.src_type = TYPE_U16;
                        swz->repeat = 1;
                     }
                  }

                  struct ir3_instruction *mov = ir3_instr_create_at(
                     ir3_before_instr(instr), OPC_MOV, 1, 1);

                  ir3_dst_create(mov, dst_num, instr->dsts[0]->flags);
                  ir3_src_create(mov, src_num / 2,
                                 instr->srcs[0]->flags & ~IR3_REG_HALF);

                  /* Float conversions are banned in this case in
                   * ir3_valid_flags(), so we only have to worry about normal
                   * non-converting moves.
                   */
                  assert(instr->cat1.src_type == TYPE_U16 ||
                         instr->cat1.src_type == TYPE_S16);
                  mov->cat1.src_type = TYPE_U32;
                  mov->cat1.dst_type = TYPE_U16;
               }

               list_del(&instr->node);
            }
         }
      }
   }

   if (copies)
      ralloc_free(copies);
}
