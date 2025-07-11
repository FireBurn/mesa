/*
 * Copyright © 2023 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

/* Unit test for disassembly of instructions.
 *
 * The goal is to take instructions we've seen the blob produce, and test that
 * we can disassemble them correctly.
 */

#include "etnaviv/isa/asm.h"
#include "etnaviv/isa/etnaviv-isa.h"
#include "etnaviv/isa/isa.h"
#include <gtest/gtest.h>

struct encoded_instr {
   uint32_t word[4];
};

static const uint32_t FLAG_DUAL_16 = BITFIELD_BIT(0);
static const uint32_t FLAG_FAILING_PARSE = BITFIELD_BIT(1);
static const uint32_t FLAG_FAILING_ASM = BITFIELD_BIT(2);

struct disasm_state {
   uint32_t bin[4];
   const char *disasm;
   uint32_t flags;

   friend std::ostream &operator<<(std::ostream &os, const disasm_state &obj)
   {
      // clang-format off
      return os
         << "binary: "
         << std::showbase << std::internal << std::setfill('0') << std::setw(8) << std::hex << obj.bin[0] << " "
         << std::showbase << std::internal << std::setfill('0') << std::setw(8) << std::hex << obj.bin[1] << " "
         << std::showbase << std::internal << std::setfill('0') << std::setw(8) << std::hex << obj.bin[2] << " "
         << std::showbase << std::internal << std::setfill('0') << std::setw(8) << std::hex << obj.bin[3] << " "
         << "disasm: " << obj.disasm;
      // clang-format on
   }
};

struct DisasmTest : testing::Test, testing::WithParamInterface<disasm_state> {
   char *disasm_output;

   DisasmTest()
   {
      static const struct isa_decode_options options = {.show_errors = true,
                                                        .branch_labels = false};

      constexpr int output_size = 4096;
      disasm_output = (char *)malloc(output_size);
      FILE *fdisasm = fmemopen(disasm_output, output_size, "w+");

      if (!fdisasm) {
         return;
      }

      etnaviv_isa_disasm((void *)GetParam().bin, 16, fdisasm, &options);
      fflush(fdisasm);
   }
};

TEST_P(DisasmTest, basicOpCodes)
{
   auto as = GetParam();
   EXPECT_STREQ(as.disasm, disasm_output);

#ifndef HAVE_ETNAVIV_NO_PEST
   struct etna_asm_result *result = isa_parse_str(disasm_output, as.flags & FLAG_DUAL_16);
   EXPECT_TRUE(result);

   if (as.flags & FLAG_FAILING_PARSE)
      EXPECT_FALSE(result->success);
   else
      EXPECT_TRUE(result->success);

   if (result->success && !(as.flags & FLAG_FAILING_ASM)) {
      uint32_t encoded[4] = {0};
      isa_assemble_instruction(encoded, result->instr);

      EXPECT_EQ(GetParam().bin[0], encoded[0]);
      EXPECT_EQ(GetParam().bin[1], encoded[1]);
      EXPECT_EQ(GetParam().bin[2], encoded[2]);
      EXPECT_EQ(GetParam().bin[3], encoded[3]);
   }

   isa_asm_result_destroy(result);
#endif
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(Default, DisasmTest,
   testing::Values(
      disasm_state{ {0x00000000, 0x00000000, 0x00000000, 0x00000000}, "nop               void, void, void, void\n" },
      disasm_state{ {0x07811018, 0x15001f20, 0x00000000, 0x00000000}, "texld             t1, tex0.xyzw, t1.xyyy, void, void\n" },
      disasm_state{ {0x07831018, 0x39003f20, 0x00000000, 0x00000000}, "texld             t3, tex0.xyzw, t3.xyzw, void, void\n" },
      disasm_state{ {0x07811009, 0x00000000, 0x00000000, 0x20390008}, "mov.pack          t1, void, void, u0.xyzw\n" },
      disasm_state{ {0x01821009, 0x00000000, 0x00000000, 0x00150028}, "mov.pack          t2.xy__, void, void, t2.xyyy\n"},
      disasm_state{ {0x01821009, 0x00000000, 0x00000000, 0x00550028}, "mov.pack          t2.xy__, void, void, -t2.xyyy\n"},
      disasm_state{ {0x01821009, 0x00000000, 0x00000000, 0x00950028}, "mov.pack          t2.xy__, void, void, |t2.xyyy|\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(OperandTypes, DisasmTest,
   testing::Values(
      disasm_state{ {0x01821009, 0x00000000, 0x00000000, 0x00150028}, "mov.pack          t2.xy__, void, void, t2.xyyy\n"},
      disasm_state{ {0x01821009, 0x00000000, 0x40000000, 0x00150028}, "mov.s32.pack      t2.xy__, void, void, t2.xyyy\n"},
      disasm_state{ {0x01821009, 0x00000000, 0x80000000, 0x00150028}, "mov.s8.pack       t2.xy__, void, void, t2.xyyy\n"},
      disasm_state{ {0x01821009, 0x00000000, 0xc0000000, 0x00150028}, "mov.u16.pack      t2.xy__, void, void, t2.xyyy\n"},
      disasm_state{ {0x01821009, 0x00200000, 0x00000000, 0x00150028}, "mov.f16.pack      t2.xy__, void, void, t2.xyyy\n"},
      disasm_state{ {0x01821009, 0x00200000, 0x40000000, 0x00150028}, "mov.s16.pack      t2.xy__, void, void, t2.xyyy\n"},
      disasm_state{ {0x01821009, 0x00200000, 0x80000000, 0x00150028}, "mov.u32.pack      t2.xy__, void, void, t2.xyyy\n"},
      disasm_state{ {0x01821009, 0x00200000, 0xc0000000, 0x00150028}, "mov.u8.pack       t2.xy__, void, void, t2.xyyy\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(Opcodes, DisasmTest,
   testing::Values(
      disasm_state{ {0x00000000, 0x00000000, 0x00000000, 0x00000000}, "nop               void, void, void, void\n" },
      disasm_state{ {0x00801001, 0x00001804, 0x00000000, 0x00000008}, "add               t0.x___, t1.xxxx, void, t0.xxxx\n" },
      disasm_state{ {0x07801002, 0x39002805, 0x01c800c0, 0x00390038}, "mad.rtz           t0, t2.xyzw, t1.xyzw, t3.xyzw\n" },
      disasm_state{ {0x00801003, 0x00001804, 0x00000040, 0x00000000}, "mul               t0.x___, t1.xxxx, t0.xxxx, void\n" },
      disasm_state{ {0x00801005, 0x09000802, 0x00000040, 0x00000002}, "dp3.pack.rtne     t0.x___, t0.xyzx, u0.xxxx, void\n" },
      disasm_state{ {0x00801006, 0x39001804, 0x01c800c0, 0x00000000}, "dp4               t0.x___, t1.xyzw, t1.xyzw, void\n" },
      disasm_state{ {0x00811007, 0x00001804, 0x00000000, 0x00000018}, "dsx               t1.x___, t1.xxxx, void, t1.xxxx\n" },
      disasm_state{ {0x00811008, 0x00001804, 0x00000000, 0x00000018}, "dsy               t1.x___, t1.xxxx, void, t1.xxxx\n" },
      disasm_state{ {0x01821009, 0x00000000, 0x00000000, 0x00150028}, "mov.pack          t2.xy__, void, void, t2.xyyy\n"},
      disasm_state{ {0x0081100c, 0x00000000, 0x00000000, 0x00000018}, "rcp.pack          t1.x___, void, void, t1.xxxx\n" },
      disasm_state{ {0x0080100d, 0x00000004, 0x00000000, 0x00000008}, "rsq               t0.x___, void, void, t0.xxxx\n" },
      disasm_state{ {0x0783108f, 0x39002800, 0x05c800c0, 0x00390028}, "select.lt.pack    t3, t2.xyzw, |t1.xyzw|, t2.xyzw\n"},
      disasm_state{ {0x008010d0, 0x00000800, 0x00000040, 0x00000002}, "set.ge.pack       t0.x___, t0.xxxx, u0.xxxx, void\n" },
      disasm_state{ {0x01001011, 0x00000004, 0x00000000, 0x00154008}, "exp               t0._y__, void, void, t0.yyyy\n" },
      disasm_state{ {0x01801012, 0x00000005, 0x00000000, 0x00000008}, "log.rtz           t0.xy__, void, void, t0.xxxx\n" },
      disasm_state{ {0x07811013, 0x00000004, 0x00000000, 0x000e4018}, "frc               t1, void, void, t1.yzwx\n" },
      disasm_state{ {0x00000014, 0x00000000, 0x00000000, 0x00000380}, "call              void, void, void, 7\n" },
      disasm_state{ {0x00000015, 0x00000000, 0x00000000, 0x00000000}, "ret               void, void, void, void\n" },
      disasm_state{ {0x00000016, 0x00000000, 0x00000000, 0x00001080}, "branch            void, void, void, 33\n"},
      disasm_state{ {0x00000017, 0x00000000, 0x00000000, 0x00000000}, "texkill.pack      void, void, void, void\n" },
      disasm_state{ {0x00000057, 0x00002800, 0x00000040, 0x00000002}, "texkill.gt.pack   void, t2.xxxx, u0.xxxx, void\n", FLAG_FAILING_PARSE | FLAG_FAILING_ASM },
      disasm_state{ {0x07811018, 0x15001f20, 0x00000000, 0x00000000}, "texld             t1, tex0.xyzw, t1.xyyy, void, void\n" },
      disasm_state{ {0x07811019, 0x39002f20, 0x00000000, 0x00000000}, "texldb            t1, tex0.xyzw, t2.xyzw, void, void\n", FLAG_FAILING_PARSE | FLAG_FAILING_ASM },
      disasm_state{ {0x0781101a, 0x15001f20, 0x00a80140, 0x003f8018}, "texldd            t1, tex0.xyzw, t1.xyyy, t2.xyyy, t1.zwww\n" },
      disasm_state{ {0x4781101b, 0x39003f20, 0x00000000, 0x00000000}, "texldl            t1, tex8.xyzw, t3.xyzw, void, void\n", FLAG_FAILING_PARSE | FLAG_FAILING_ASM },
      disasm_state{ {0x00801021, 0x00000004, 0x00000000, 0x00000008}, "sqrt              t0.x___, void, void, t0.xxxx\n" },
      disasm_state{ {0x03001022, 0x00000005, 0x00000000, 0x00154008}, "sin.rtz           t0.zy, void, void, t0.yyyy\n" },
      disasm_state{ {0x01801023, 0x00000005, 0x00000000, 0x00000008}, "cos.rtz           t0.xy__, void, void, t0.xxxx\n" },
      disasm_state{ {0x000001a4, 0x3e401804, 0x500000c0, 0x00000487}, "branch_any.ne.s32 void, t1.yzww, 1, 9\t; dontcare bits in branch_any: 00000000000000000000000400000000\n", FLAG_FAILING_ASM},
      disasm_state{ {0x00801025, 0x00000004, 0x00000000, 0x00000008}, "floor             t0.x___, void, void, t0.xxxx\n"},
      disasm_state{ {0x00801026, 0x00000004, 0x00000000, 0x00000008}, "ceil              t0.x___, void, void, t0.xxxx\n"},
      disasm_state{ {0x00801027, 0x00000004, 0x00000000, 0x00000008}, "sign              t0.x___, void, void, t0.xxxx\n" },
      disasm_state{ {0x0000002a, 0x00000000, 0x00000000, 0x00000000}, "barrier           void, void, void, void\n" },
      disasm_state{ {0x0787102b, 0x39202804, 0xc0a802c0, 0x781fdffa}, "swizzle.u8        t7, t2.xyzw, u5.xyyy, 65535\n", FLAG_FAILING_ASM },
      disasm_state{ {0x0080102c, 0x00200804, 0x50001040, 0x00000007}, "i2i.s16           t0.x___, t0.xxxx, 32, void\n" },
      disasm_state{ {0x0381102d, 0x00201804, 0x40000000, 0x00000000}, "i2f.s16           t1.xyz_, t1.xxxx, void, void\n"},
      disasm_state{ {0x0101102e, 0x00201804, 0x80000020, 0x00002000}, "f2i.u32.t0        t1._y__, th1.xxxx, void, void\n"},
      disasm_state{ {0x0081102f, 0x00000806, 0x40000000, 0x00000000}, "f2irnd.s32.rtne   t1.x___, t0.xxxx, void, void\n"},
      disasm_state{ {0x00811131, 0x80001800, 0x00aa0040, 0x202a800a}, "cmp.le.pack       t1.x___, |t1.xxxx|, u0.yyyy, u0.zzzz\n"},
      disasm_state{ {0x00801032, 0x00000c04, 0x10000050, 0x00000007}, "load.denorm       t0.x___, u0.xxxx, 0, void\n"},
      disasm_state{ {0x00800033, 0x00000c84, 0x10000050, 0x0000000f}, "store.skpHp.denorm mem.x___, u0.xxxx, 0, t0.xxxx\n"},
      disasm_state{ {0x00801036, 0x15400804, 0x01540050, 0x00000002}, "clamp0_max        t0.x___, u0.yyyy, u0.zzzz, void\n"},
      disasm_state{ {0x0080103b, 0x00001804, 0x40000000, 0x00400028}, "iaddsat.s32       t0.x___, t1.xxxx, void, -t2.xxxx\n"},
      disasm_state{ {0x01001008, 0x15400804, 0xd00100c0, 0x00000007}, "imod.u16          t0._y__, t0.yyyy, 1, void\n"},
      disasm_state{ {0x07811009, 0x15001f20, 0x01ff00c0, 0x78021008}, "txf               t1, tex0.xyzw, t1.xyyy, t1.wwww, 4352\n", FLAG_FAILING_ASM},
      disasm_state{ {0x0080103c, 0x00001804, 0x40000140, 0x00000000}, "imullo0.s32       t0.x___, t1.xxxx, t2.xxxx, void\n"},
      disasm_state{ {0x00801000, 0x00001804, 0x40010140, 0x00000000}, "imulhi0.s32       t0.x___, t1.xxxx, t2.xxxx, void\n"},
      disasm_state{ {0x00801004, 0x00201804, 0x40010040, 0x00000000}, "idiv0.s16         t0.x___, t1.xxxx, t0.xxxx, void\n"},
      disasm_state{ {0x0080100e, 0x00001804, 0x40010140, 0x00000038}, "imadlosat0.s32    t0.x___, t1.xxxx, t2.xxxx, t3.xxxx\n"},
      disasm_state{ {0x0082101c, 0x00001804, 0x40010000, 0x00000008}, "or.s32            t2.x___, t1.xxxx, void, t0.xxxx\n"},
      disasm_state{ {0x0082101d, 0x00001804, 0x40010000, 0x00000008}, "and.s32           t2.x___, t1.xxxx, void, t0.xxxx\n"},
      disasm_state{ {0x0080101e, 0x00001804, 0x40010000, 0x00000008}, "xor.s32           t0.x___, t1.xxxx, void, t0.xxxx\n"},
      disasm_state{ {0x0080101f, 0x00000004, 0x40010000, 0x00000018}, "not.s32           t0.x___, void, void, t1.xxxx\n"},
      disasm_state{ {0x00811020, 0x00001804, 0x40010140, 0x00000038}, "bit_extract.s32   t1.x___, t1.xxxx, t2.xxxx, t3.xxxx\n"},
      disasm_state{ {0x00801021, 0x00000004, 0x00010000, 0x00000008}, "popcount          t0.x___, void, void, t0.xxxx\n"},
      disasm_state{ {0x02000016, 0x00200000, 0x80010000, 0x003fc018}, "movai.u32.pack    void, void, void, t1.wwww\n", FLAG_FAILING_ASM},
      disasm_state{ {0x00801017, 0x00000004, 0x40010000, 0x00000018}, "iabs.s32          t0.x___, void, void, t1.xxxx\n"},
      disasm_state{ {0x00801018, 0x00000004, 0x40010000, 0x00000008}, "leadzero.s32      t0.x___, void, void, t0.xxxx\n"},
      disasm_state{ {0x00801019, 0x15400804, 0x40010000, 0x74000028}, "lshift.s32        t0.x___, t0.yyyy, void, 2\n"},
      disasm_state{ {0x0080101a, 0x00001804, 0x40010000, 0x78000018}, "rshift.s32        t0.x___, t1.xxxx, void, 1\n", FLAG_FAILING_ASM},
      disasm_state{ {0x0080101b, 0x00001804, 0x40010000, 0x00000008}, "rotate.s32        t0.x___, t1.xxxx, void, t0.xxxx\n"},
      disasm_state{ {0x03001024, 0x00000005, 0x04098040, 0x0015400f}, "div.rtz           t0.zy, void, 4.500000, t0.yyyy\n"},
      disasm_state{ {0x01061025, 0x2aa00804, 0xa0010050, 0x7800001f}, "atomic_add.u32    t6._y__, u0.zzzz, 0, 1\n", FLAG_FAILING_ASM},
      disasm_state{ {0x00801025, 0x2a800884, 0x50010050, 0x0000000f}, "atomic_add.skpHp.s32 t0.x___, u0.zzzz, 0, t0.xxxx\n"},
      disasm_state{ {0x00821026, 0x2a800884, 0x50010050, 0x0000001f}, "atomic_xchg.skpHp.s32 t2.x___, u0.zzzz, 0, t1.xxxx\n"},
      disasm_state{ {0x00801027, 0x2a800884, 0x50010050, 0x0015000f}, "atomic_cmp_xchg.skpHp.s32 t0.x___, u0.zzzz, 0, t0.xyyy\n"},
      disasm_state{ {0x00821028, 0x2a800884, 0x50010050, 0x0000001f}, "atomic_min.skpHp.s32 t2.x___, u0.zzzz, 0, t1.xxxx\n"},
      disasm_state{ {0x00821029, 0x2a800884, 0x50010050, 0x0000000f}, "atomic_max.skpHp.s32 t2.x___, u0.zzzz, 0, t0.xxxx\n"},
      disasm_state{ {0x0080102a, 0x2a800884, 0x50010050, 0x0000000f}, "atomic_or.skpHp.s32 t0.x___, u0.zzzz, 0, t0.xxxx\n"},
      disasm_state{ {0x0082102b, 0x2a800884, 0x50010050, 0x0000001f}, "atomic_and.skpHp.s32 t2.x___, u0.zzzz, 0, t1.xxxx\n"},
      disasm_state{ {0x0080102c, 0x2a800884, 0x50010050, 0x0000001f}, "atomic_xor.skpHp.s32 t0.x___, u0.zzzz, 0, t1.xxxx\n"},
      disasm_state{ {0x0081102d, 0x00001804, 0x40010000, 0x00000000}, "bit_rev.s32       t1.x___, t1.xxxx, void, void\n"},
      disasm_state{ {0x00811033, 0x04402804, 0x00ef00c0, 0x00000000}, "dp2               t1.x___, t2.yxyx, t1.wywy, void\n"},
      disasm_state{ {0x00811034, 0x15c01804, 0x00010000, 0x00000000}, "norm_dp2          t1.x___, t1.wyyy, void, void\n"},
      disasm_state{ {0x04011035, 0x14801804, 0x00010000, 0x00000000}, "norm_dp3          t1.___w, t1.zxyy, void, void\n"},
      disasm_state{ {0x00821036, 0x0e401804, 0x00010000, 0x00000000}, "norm_dp4          t2.x___, t1.yzwx, void, void\n"},
      disasm_state{ {0x07801039, 0x39204c00, 0x80a90050, 0x00000000}, "img_load.denorm.u32.pack t0, u4.xyzw, t0.xyyy, void\n"},
      disasm_state{ {0x0780083a, 0x39200c00, 0x80a90050, 0x00390018}, "img_store.sat.denorm.u32.pack mem, u0.xyzw, t0.xyyy, t1.xyzw\n"},
      disasm_state{ {0x0381103f, 0x29201804, 0x80010000, 0x780000b8}, "bit_findlsb.u32   t1.xyz_, t1.xyzz, void, void\n"},
      disasm_state{ {0x0081103f, 0x00001804, 0x40010000, 0x780000c8}, "bit_findmsb.s32   t1.x___, t1.xxxx, void, void\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(Branch, DisasmTest,
   testing::Values(
      // taken from deqp2 run on GC2000
      disasm_state{ {0x00000016, 0x00000000, 0x00000000, 0x00001080}, "branch            void, void, void, 33\n"},
      disasm_state{ {0x00000056, 0x00000800, 0x000000d0, 0x00000280}, "branch.gt         void, u0.xxxx, t1.xxxx, 5\n"},
      disasm_state{ {0x00000056, 0x00000800, 0x000000d0, 0x00000280}, "branch.gt         void, u0.xxxx, t1.xxxx, 5\n"},
      disasm_state{ {0x00000096, 0x15402800, 0x00000040, 0x00000082}, "branch.lt         void, t2.yyyy, u0.xxxx, 1\n"},
      disasm_state{ {0x000000d6, 0x00001800, 0x01540250, 0x00000980}, "branch.ge         void, u1.xxxx, t4.zzzz, 19\n"},
      disasm_state{ {0x00000116, 0x3fc01800, 0x000000c0, 0x00000482}, "branch.le         void, t1.wwww, u1.xxxx, 9\n"},
      disasm_state{ {0x00000156, 0x39001800, 0x000002c0, 0x00001282}, "branch.eq         void, t1.xyzw, u5.xxxx, 37\n"},
      disasm_state{ {0x00000196, 0x15401800, 0x00aa0040, 0x00000382}, "branch.ne         void, t1.yyyy, u0.yyyy, 7\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(Abs, DisasmTest,
   testing::Values(
      // taken from deqp2 run on GC2000
      disasm_state{ {0x00811131, 0x80001800, 0x00aa0040, 0x202a800a}, "cmp.le.pack       t1.x___, |t1.xxxx|, u0.yyyy, u0.zzzz\n"},
      disasm_state{ {0x0783108f, 0x39002800, 0x05c800c0, 0x00390028}, "select.lt.pack    t3, t2.xyzw, |t1.xyzw|, t2.xyzw\n"},
      disasm_state{ {0x0383108f, 0xa9001800, 0x05480140, 0x00a90018}, "select.lt.pack    t3.xyz_, |t1.xyzz|, |t2.xyzz|, |t1.xyzz|\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(Minus, DisasmTest,
   testing::Values(
      // taken from deqp2 run on GC2000
      disasm_state{ {0x01021001, 0x3fc00800, 0x00000010, 0x00554018}, "add.pack          t2._y__, u0.wwww, void, -t1.yyyy\n"},
      disasm_state{ {0x00821001, 0x40001800, 0x00000000, 0x00554018}, "add.pack          t2.x___, -t1.xxxx, void, -t1.yyyy\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(AddressRegister, DisasmTest,
   testing::Values(
      // taken from deqp2 run on GC2000
      disasm_state{ {0x00823009, 0x00000000, 0x00000000, 0x00000018}, "mov.pack          t2[a.x].x___, void, void, t1.xxxx\n"},
      disasm_state{ {0x00825009, 0x00000000, 0x00000000, 0x00154028}, "mov.pack          t2[a.y].x___, void, void, t2.yyyy\n"},
      disasm_state{ {0x00827009, 0x00000000, 0x00000000, 0x00000018}, "mov.pack          t2[a.z].x___, void, void, t1.xxxx\n"},
      disasm_state{ {0x00829009, 0x00000000, 0x00000000, 0x00000018}, "mov.pack          t2[a.w].x___, void, void, t1.xxxx\n"},
      disasm_state{ {0x00801009, 0x00000000, 0x00000000, 0x02000028}, "mov.pack          t0.x___, void, void, t2[a.x].xxxx\n"},
      disasm_state{ {0x01031009, 0x00000000, 0x00000000, 0x043fc018}, "mov.pack          t3._y__, void, void, t1[a.y].wwww\n"},
      disasm_state{ {0x02031009, 0x00000000, 0x00000000, 0x063fc018}, "mov.pack          t3.__z_, void, void, t1[a.z].wwww\n"},
      disasm_state{ {0x01811001, 0x15001800, 0x00000000, 0x28150018}, "add.pack          t1.xy__, t1.xyyy, void, u1[a.w].xyyy\n"},
      disasm_state{ {0x01011001, 0x00001800, 0x00000001, 0x04000018}, "add.pack          t1._y__, t1[a.x].xxxx, void, t1[a.y].xxxx\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(Threads, DisasmTest,
   testing::Values(
      // taken from deqp3 run on GC3000
      disasm_state{ {0x01011001, 0x00001804, 0x00000020, 0xa0402008}, "add.hp.t0         t1._y__, th1.xxxx, void, -u0.xxxx\n"},
      disasm_state{ {0x01021001, 0x00002804, 0x00000020, 0xa1400008}, "add.hp.t1         t2._y__, th2.xxxx, void, -u0.xxxx\n"},

      // full dual-16 shader from a deqp3 run on GC3000
      disasm_state{ {0x0101102e, 0x00201804, 0x80000020, 0x00002000}, "f2i.u32.t0        t1._y__, th1.xxxx, void, void\n", FLAG_DUAL_16},
      disasm_state{ {0x0101102e, 0x00202804, 0x80000020, 0x01000000}, "f2i.u32.t1        t1._y__, th2.xxxx, void, void\n", FLAG_DUAL_16},
      disasm_state{ {0x00811171, 0x15601804, 0x80000040, 0x76fffffa}, "cmp.eq.u32.t0     t1.x___, t1.yyyy, u0.xxxx, -1\n", FLAG_DUAL_16},
      disasm_state{ {0x00811171, 0x15601804, 0x80000040, 0x77ffdffa}, "cmp.eq.u32.t1     t1.x___, t1.yyyy, u0.xxxx, -1\n", FLAG_DUAL_16},
      disasm_state{ {0x0081158f, 0x00201804, 0x700000c0, 0x7c00000f}, "select.selmsb.s16 t1.x___, t1.xxxx, 0.000000, 0.000000\n", FLAG_DUAL_16 | FLAG_FAILING_ASM},
      disasm_state{ {0x0381102d, 0x00201804, 0x40000000, 0x00000000}, "i2f.s16           t1.xyz_, t1.xxxx, void, void\n", FLAG_DUAL_16},
      disasm_state{ {0x04011009, 0x00000004, 0x00000000, 0x20154008}, "mov               t1.___w, void, void, u0.yyyy\n", FLAG_DUAL_16}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(ImmediateValues, DisasmTest,
   testing::Values(
      // taken from deqp3 run on GC3000
      disasm_state{ {0x00801001, 0x7e000805, 0x00000038, 0x00800008}, "add.rtz           t0.x___, 0.500000, void, |t0.xxxx|\n"}, /* type: 0 */
      disasm_state{ {0x00811131, 0x95401804, 0x00aa0060, 0x76fffffa}, "cmp.le.t0         t1.x___, |th1.yyyy|, u0.yyyy, -1\n"}, /* type: 1 */
      disasm_state{ {0x0080101a, 0x00001804, 0x40010000, 0x78000018}, "rshift.s32        t0.x___, t1.xxxx, void, 1\n", FLAG_FAILING_ASM}, /* type: 2*/
      disasm_state{ {0x020211b1, 0x00001804, 0x01fe0040, 0x7c1fdffa}, "cmp.ne            t2.__z_, t1.xxxx, u0.wwww, -nan\n", FLAG_FAILING_ASM} /* type: 3 */
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(LoadStore, DisasmTest,
   testing::Values(
      // full opencl shader on GC3000
      disasm_state{ {0x00801032, 0x00000c04, 0x10000050, 0x00000007}, "load.denorm       t0.x___, u0.xxxx, 0, void\n"},
      disasm_state{ {0x00811032, 0x15400c04, 0x10000050, 0x00000007}, "load.denorm       t1.x___, u0.yyyy, 0, void\n"},
      disasm_state{ {0x00801001, 0x00001804, 0x00000000, 0x00000008}, "add               t0.x___, t1.xxxx, void, t0.xxxx\n"},
      disasm_state{ {0x00800033, 0x00000c84, 0x10000050, 0x0000000f}, "store.skpHp.denorm mem.x___, u0.xxxx, 0, t0.xxxx\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(Rounding, DisasmTest,
   testing::Values(
      // taken from opencl shader on GC3000
      disasm_state{ {0x0081102f, 0x00000806, 0x40000000, 0x00000000}, "f2irnd.s32.rtne   t1.x___, t0.xxxx, void, void\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(CLRoundShader, DisasmTest,
   testing::Values(
      // taken from opencl shader on GC3000
      disasm_state{ {0x00801032, 0x15400c04, 0x10000050, 0x00000007}, "load.denorm       t0.x___, u0.yyyy, 0, void\n"},
      disasm_state{ {0x00811027, 0x00000004, 0x00000000, 0x00000008}, "sign              t1.x___, void, void, t0.xxxx\n"},
      disasm_state{ {0x00801001, 0x7e000805, 0x00000038, 0x00800008}, "add.rtz           t0.x___, 0.500000, void, |t0.xxxx|\n"},
      disasm_state{ {0x00801025, 0x00000004, 0x00000000, 0x00000008}, "floor             t0.x___, void, void, t0.xxxx\n"},
      disasm_state{ {0x00801003, 0x00001805, 0x00000040, 0x00000000}, "mul.rtz           t0.x___, t1.xxxx, t0.xxxx, void\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(TFShader, DisasmTest,
   testing::Values(
      // taken from transform shader on GC2000
      disasm_state{ {0x0081102e, 0x00000800, 0x40000020, 0x00000000}, "f2i.s32.pack      t1.x___, th0.xxxx, void, void\n"},
      disasm_state{ {0x07821009, 0x00000000, 0x00000000, 0x00390008}, "mov.pack          t2, void, void, t0.xyzw\n"},
      disasm_state{ {0x01831009, 0x00000000, 0x00000000, 0x00150008}, "mov.pack          t3.xy__, void, void, t0.xyyy\n"},
      disasm_state{ {0x03841009, 0x00000000, 0x00000000, 0x00290008}, "mov.pack          t4.xyz_, void, void, t0.xyzz\n"},
      disasm_state{ {0x0201102d, 0x00000800, 0x40000010, 0x00000000}, "i2f.s32.pack      t1.__z_, u0.xxxx, void, void\n"},
      disasm_state{ {0x00000156, 0x2a801800, 0x01540040, 0x00000402}, "branch.eq         void, t1.zzzz, u0.zzzz, 8\n"},
      disasm_state{ {0x0081100c, 0x3fc00800, 0x400100d0, 0x20154008}, "imadlo0.s32.pack  t1.x___, u0.wwww, t1.xxxx, u0.yyyy\n"},
      disasm_state{ {0x07820033, 0x00001800, 0x01540040, 0x0039002a}, "store.pack        mem, t1.xxxx, u0.zzzz, t2.xyzw\t; dontcare bits in store: 00000000000000000000000000020000\n", FLAG_FAILING_ASM}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(texldlpcf, DisasmTest,
   testing::Values(
      // taken from dEQP-GLES3.functional.shaders.texture_functions.texturelod.sampler2dshadow_vertex (GC7000)
      disasm_state{ {0x04011809, 0x00000004, 0x00000000, 0x002a8018}, "mov.sat           t1.___w, void, void, t1.zzzz\n"},
      disasm_state{ {0x0081102f, 0x29001800, 0x00010140, 0x003fc018}, "texldlpcf         t1.x___, tex0.xxxx, t1.xyzz, t2.xxxx, t1.wwww\n"},
      disasm_state{ {0x07011009, 0x00000004, 0x00000000, 0x20390018}, "mov               t1._yzw, void, void, u1.xyzw\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(texldgpcf, DisasmTest,
   testing::Values(
      // taken from dEQP-GLES3.functional.shaders.texture_functions.texturegrad.sampler2dshadow_fragment (GC7000)
      disasm_state{ {0x04011809, 0x00000004, 0x00000000, 0x003fc018}, "mov.sat           t1.___w, void, void, t1.wwww\n"},
      disasm_state{ {0x00811030, 0x39001800, 0x01490140, 0x00290038}, "texldgpcf         t1.x___, tex0.xxxx, t1.xyzw, t2.xyzz, t3.xyzz\n"},
      disasm_state{ {0x07011009, 0x00000004, 0x00000000, 0x20390018}, "mov               t1._yzw, void, void, u1.xyzw\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(LoadStoreVariants, DisasmTest,
   testing::Values(
      // seen on GC7000
      disasm_state{ {0x01001032, 0x15400c14, 0x00000050, 0x00000000}, "load.denorm.ls2   t0._y__, u0.yyyy, t0.xxxx, void\n"},
      disasm_state{ {0x01001032, 0x15400d14, 0x00000040, 0x00000000}, "load.denorm.local.ls2 t0._y__, t0.yyyy, t0.xxxx, void\n"},
      disasm_state{ {0x00800033, 0x00000c14, 0x00000050, 0x00154008}, "store.denorm.ls2  mem.x___, u0.xxxx, t0.xxxx, t0.yyyy\n"},
      disasm_state{ {0x00861033, 0x15400d04, 0x100efe40, 0x7085860f}, "store.denorm.local mem.x___, t0.yyyy, 4092, 99.000000\t; dontcare bits in store: 00000000000000000000000000061000\n", FLAG_FAILING_ASM},
      disasm_state{ {0x07800033, 0x00200c34, 0x80000050, 0x00390018}, "store.denorm.u32.ls6 mem, u0.xxxx, t0.xxxx, t1.xyzw\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(ConvVariants, DisasmTest,
   testing::Values(
      // seen on GC7000
      disasm_state{ {0x01001032, 0x15400800, 0x100100c0, 0x00000007}, "conv.pack         t0._y__, t0.yyyy, 1, void\n"},
      disasm_state{ {0x01001032, 0x15600800, 0x10010040, 0x00000007}, "conv.f16.pack     t0._y__, t0.yyyy, 0, void\n"},
      disasm_state{ {0x01001832, 0x15400800, 0x100100c0, 0x00000007}, "conv.sat.pack     t0._y__, t0.yyyy, 1, void\n"}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(ShadowSampler, DisasmTest,
   testing::Values(
      disasm_state{ {0x00811018, 0x15001800, 0x00000000, 0x002a8018}, "texld             t1.x___, tex0.xxxx, t1.xyyy, void, t1.zzzz\n", FLAG_FAILING_PARSE | FLAG_FAILING_ASM}
   )
);
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(SwizzleVariants, DisasmTest,
   testing::Values(
      // seen on GC7000
      disasm_state{ {0x0782102b, 0x00000804, 0xa000007c ,0x7800001f}, "swizzle.s8        t2, 0, 0, 1\n", FLAG_FAILING_ASM },
      disasm_state{ {0x0782102b, 0x00002804, 0xa0000040, 0x7800002f}, "swizzle.s8        t2, t2.xxxx, 0, 2\n", FLAG_FAILING_ASM },
      disasm_state{ {0x0780102b, 0x00002804, 0xa0000040, 0x7800001f}, "swizzle.s8        t0, t2.xxxx, 0, 1\n", FLAG_FAILING_ASM },
      disasm_state{ {0x0781102b, 0x00002804, 0xa00000c0, 0x7800001f}, "swizzle.s8        t1, t2.xxxx, 1, 1\n", FLAG_FAILING_ASM },
      disasm_state{ {0x0781102b, 0x00200804, 0x6000007e, 0x7800003f}, "swizzle.s16       t1, 0.000000, 0, 3\n", FLAG_FAILING_ASM },
      disasm_state{ {0x0781102b, 0x00201804, 0x60000040, 0x780000cf}, "swizzle.s16       t1, t1.xxxx, 0, 12\n", FLAG_FAILING_ASM },
      disasm_state{ {0x0780102b, 0x00201804, 0x60000040, 0x7800003f}, "swizzle.s16       t0, t1.xxxx, 0, 3\n", FLAG_FAILING_ASM },
      disasm_state{ {0x0781102b, 0x00202804, 0x600000c0, 0x7800003f}, "swizzle.s16       t1, t2.xxxx, 1, 3\n", FLAG_FAILING_ASM }
   )
);
// clang-format on
