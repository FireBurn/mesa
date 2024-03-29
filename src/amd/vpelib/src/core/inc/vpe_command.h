/* Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/****************
 * VPE OP Codes
 ****************/
enum VPE_CMD_OPCODE {
    VPE_CMD_OPCODE_NOP         = 0x0,
    VPE_CMD_OPCODE_VPE_DESC    = 0x1,
    VPE_CMD_OPCODE_PLANE_CFG   = 0x2,
    VPE_CMD_OPCODE_VPEP_CFG    = 0x3,
    VPE_CMD_OPCODE_FENCE       = 0x5,
    VPE_CMD_OPCODE_TRAP        = 0x6,
    VPE_CMD_OPCODE_REG_WRITE   = 0x7,
    VPE_CMD_OPCODE_POLL_REGMEM = 0x8,
    VPE_CMD_OPCODE_ATOMIC      = 0xA,
    VPE_CMD_OPCODE_PLANE_FILL  = 0xB,
#ifdef VPE_BUILD_1_1
    VPE_CMD_OPCODE_COLLABORATE_SYNC = 0xC,
#endif
    VPE_CMD_OPCODE_TIMESTAMP = 0xD
};

/** Generic Command Header
 * Generic Commands include:
 *  Noop, Fence, Trap,
 *  RegisterWrite, PollRegisterWriteMemory,
 *  SetLocalTimestamp, GetLocalTimestamp
 *  GetGlobalGPUTimestamp */
#define VPE_HEADER_SUB_OPCODE__SHIFT 8
#define VPE_HEADER_SUB_OPCODE_MASK   0x0000FF00
#define VPE_HEADER_OPCODE__SHIFT     0
#define VPE_HEADER_OPCODE_MASK       0x000000FF

#define VPE_CMD_HEADER(op, subop)                                                                  \
    (((subop << VPE_HEADER_SUB_OPCODE__SHIFT) & VPE_HEADER_SUB_OPCODE_MASK) |                      \
        ((op << VPE_HEADER_OPCODE__SHIFT) & VPE_HEADER_OPCODE_MASK))

/***************************
 * VPE Descriptor
 ***************************/
#define VPE_DESC_CD__SHIFT 16
#define VPE_DESC_CD_MASK   0x000F0000

#define VPE_DESC_ADDR__SHIFT    32
#define VPE_DESC_HIGH_ADDR_MASK 0xFFFFFFFF00000000
/* The lowest bits are reuse and tmz as bit 1 and bit 0.
   Smibs will substract the address with emb gpuva to
   get offset and then reuse bit will be preserved
   So as long as the embedded buffer is allocated
   at correct alignment (currently low addr is [31:2]
   which means we need a 4 byte(2 bit) alignment),
   the offset generated will still cover the
   reuse bit as part of it.
   Ex : Address : 0x200036 GPU Virtual Address : 0x200000
   offset is 0x36 which keeps the reuse bit */
#define VPE_DESC_LOW_ADDR_MASK  0x00000000FFFFFFFF
#define VPE_DESC_REUSE_TMZ_MASK 0x0000000000000003

#define VPE_DESC_NUM_CONFIG_DESCRIPTOR__SHIFT 0
#define VPE_DESC_NUM_CONFIG_DESCRIPTOR_MASK   0x000000FF

#define VPE_DESC_REUSE__MASK 0x00000002

#define VPE_DESC_CMD_HEADER(cd)                                                                    \
    (VPE_CMD_HEADER(VPE_CMD_OPCODE_VPE_DESC, 0) | (((cd) << VPE_DESC_CD__SHIFT) & VPE_DESC_CD_MASK))

/***************************
 * VPE Plane Config
 ***************************/
enum VPE_PLANE_CFG_SUBOP {
    VPE_PLANE_CFG_SUBOP_1_TO_1 = 0x0,
    VPE_PLANE_CFG_SUBOP_2_TO_1 = 0x1,
    VPE_PLANE_CFG_SUBOP_2_TO_2 = 0x2
};

#define VPE_PLANE_CFG_ONE_PLANE  0
#define VPE_PLANE_CFG_TWO_PLANES 1

#define VPE_PLANE_CFG_NPS0__SHIFT 16
#define VPE_PLANE_CFG_NPS0_MASK   0x00030000

#define VPE_PLANE_CFG_NPD0__SHIFT 18
#define VPE_PLANE_CFG_NPD0_MASK   0x000C0000

#define VPE_PLANE_CFG_NPS1__SHIFT 20
#define VPE_PLANE_CFG_NPS1_MASK   0x00300000

#define VPE_PLANE_CFG_NPD1__SHIFT 22
#define VPE_PLANE_CFG_NPD1_MASK   0x00C00000

#define VPE_PLANE_CFG_TMZ__SHIFT 16
#define VPE_PLANE_CFG_TMZ_MASK   0x00010000

#define VPE_PLANE_CFG_SWIZZLE_MODE__SHIFT 3
#define VPE_PLANE_CFG_SWIZZLE_MODE_MASK   0x000000F8

#define VPE_PLANE_CFG_ROTATION__SHIFT 0
#define VPE_PLANE_CFG_ROTATION_MASK   0x00000003

#define VPE_PLANE_CFG_MIRROR__SHIFT 0
#define VPE_PLANE_CFG_MIRROR_MASK   0x00000003

#define VPE_PLANE_ADDR_LO__SHIFT 0
#define VPE_PLANE_ADDR_LO_MASK   0xFFFFFF00

#define VPE_PLANE_CFG_PITCH__SHIFT 0
#define VPE_PLANE_CFG_PITCH_MASK   0x00003FFF

#define VPE_PLANE_CFG_VIEWPORT_Y__SHIFT 16
#define VPE_PLANE_CFG_VIEWPORT_Y_MASK   0x3FFF0000
#define VPE_PLANE_CFG_VIEWPORT_X__SHIFT 0
#define VPE_PLANE_CFG_VIEWPORT_X_MASK   0x00003FFF

#define VPE_PLANE_CFG_VIEWPORT_HEIGHT__SHIFT       16
#define VPE_PLANE_CFG_VIEWPORT_HEIGHT_MASK         0x1FFF0000
#define VPE_PLANE_CFG_VIEWPORT_ELEMENT_SIZE__SHIFT 13
#define VPE_PLANE_CFG_VIEWPORT_ELEMENT_SIZE_MASK   0x0000E000
#define VPE_PLANE_CFG_VIEWPORT_WIDTH__SHIFT        0
#define VPE_PLANE_CFG_VIEWPORT_WIDTH_MASK          0x00001FFF

enum VPE_PLANE_CFG_ELEMENT_SIZE {
    VPE_PLANE_CFG_ELEMENT_SIZE_8BPE  = 0,
    VPE_PLANE_CFG_ELEMENT_SIZE_16BPE = 1,
    VPE_PLANE_CFG_ELEMENT_SIZE_32BPE = 2,
    VPE_PLANE_CFG_ELEMENT_SIZE_64BPE = 3
};

#define VPE_PLANE_CFG_CMD_HEADER(subop, nps0, npd0, nps1, npd1)                                    \
    (VPE_CMD_HEADER(VPE_CMD_OPCODE_PLANE_CFG, subop) |                                             \
        (((nps0) << VPE_PLANE_CFG_NPS0__SHIFT) & VPE_PLANE_CFG_NPS0_MASK) |                        \
        (((npd0) << VPE_PLANE_CFG_NPD0__SHIFT) & VPE_PLANE_CFG_NPD0_MASK) |                        \
        (((nps1) << VPE_PLANE_CFG_NPS1__SHIFT) & VPE_PLANE_CFG_NPS1_MASK) |                        \
        (((npd0) << VPE_PLANE_CFG_NPD1__SHIFT) & VPE_PLANE_CFG_NPD1_MASK))

/************************
 * VPEP Config
 ************************/
enum VPE_VPEP_CFG_SUBOP {
    VPE_VPEP_CFG_SUBOP_DIR_CFG = 0x0,
    VPE_VPEP_CFG_SUBOP_IND_CFG = 0x1
};

// Direct Config Command Header
#define VPE_DIR_CFG_HEADER_ARRAY_SIZE__SHIFT 16
#define VPE_DIR_CFG_HEADER_ARRAY_SIZE_MASK   0xFFFF0000

#define VPE_DIR_CFG_CMD_HEADER(arr_sz)                                                             \
    (VPE_CMD_HEADER(VPE_CMD_OPCODE_VPEP_CFG, VPE_VPEP_CFG_SUBOP_DIR_CFG) |                         \
        (((arr_sz) << VPE_DIR_CFG_HEADER_ARRAY_SIZE__SHIFT) & VPE_DIR_CFG_HEADER_ARRAY_SIZE_MASK))

#define VPE_DIR_CFG_PKT_REGISTER_OFFSET__SHIFT 2
#define VPE_DIR_CFG_PKT_REGISTER_OFFSET_MASK   0x000FFFFC

#define VPE_DIR_CFG_PKT_DATA_SIZE__SHIFT 20
#define VPE_DIR_CFG_PKT_DATA_SIZE_MASK   0xFFF00000

// InDirect Config Command Header
#define VPE_IND_CFG_HEADER_NUM_DST__SHIFT 28
#define VPE_IND_CFG_HEADER_NUM_DST_MASK   0xF0000000

#define VPE_IND_CFG_CMD_HEADER(num_dst)                                                            \
    (VPE_CMD_HEADER(VPE_CMD_OPCODE_VPEP_CFG, VPE_VPEP_CFG_SUBOP_IND_CFG) |                         \
        ((((uint32_t)num_dst) << VPE_IND_CFG_HEADER_NUM_DST__SHIFT) &                              \
            VPE_IND_CFG_HEADER_NUM_DST_MASK))

#define VPE_IND_CFG_DATA_ARRAY_SIZE__SHIFT 0
#define VPE_IND_CFG_DATA_ARRAY_SIZE_MASK   0x0007FFFF

#define VPE_IND_CFG_PKT_REGISTER_OFFSET__SHIFT 2
#define VPE_IND_CFG_PKT_REGISTER_OFFSET_MASK   0x000FFFFC

#ifdef VPE_BUILD_1_1
// Collaborate sync Command Header
#define VPE_COLLABORATE_SYNC_HEADER_MASK                 0x000000FF
#define VPE_COLLABORATE_SYNC_DATA_MASK(collaborate_data) ((collaborate_data) & 0xFFFFFFFF)
#define VPE_COLLABORATE_SYNC_CMD_HEADER                                                            \
    (VPE_CMD_HEADER(VPE_CMD_OPCODE_COLLABORATE_SYNC, 0) & VPE_COLLABORATE_SYNC_HEADER_MASK)
#endif

/**************************
 * Poll Reg/Mem Sub-OpCode
 **************************/
enum VPE_POLL_REGMEM_SUBOP {
    VPE_POLL_REGMEM_SUBOP_REGMEM       = 0x0,
    VPE_POLL_REGMEM_SUBOP_REGMEM_WRITE = 0x1
};

#ifdef __cplusplus
}
#endif
