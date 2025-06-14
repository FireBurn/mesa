/**************************************************************************
 *
 * Copyright 2022 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#ifndef SI_VPE_H
#define SI_VPE_H

#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"
#include "vpelib/inc/vpelib.h"
#include "radeon_video.h"

/* The buffer size of cmd_buf and emb_buf in bytes
 *
 * TODO: vpe-utils also use this value. Need to be reviewed further.
 */
#define VPE_FENCE_TIMEOUT_NS 1000000000

/* VPE 1st generation only support 1 input stram */
#define VPE_STREAM_MAX_NUM   1

#define VPE_BUFFERS_NUM      6
#define VPE_EMBBUF_SIZE      50000
#define VPE_LUT_DIM          17

#define VPE_MAX_GEOMETRIC_DOWNSCALE 4.f

struct vpe_scaling_lanczos_info {
    float scaling_ratios[2];
    struct vpe_scaling_filter_coeffs filterCoeffs;
};

/* For Hooking VPE as a decoder instance */
struct vpe_video_processor {
    struct pipe_video_codec base;

    struct pipe_screen *screen;
    struct radeon_winsys *ws;
    struct radeon_cmdbuf cs;

    uint8_t bufs_num;
    uint8_t cur_buf;
    struct rvid_buffer *emb_buffers;

    /* VPE HW version */
    uint8_t ver_major;
    uint8_t ver_minor;

    struct vpe *vpe_handle;
    struct vpe_init_data vpe_data;
    struct vpe_build_bufs *vpe_build_bufs;
    struct vpe_build_param *vpe_build_param;

    uint8_t log_level;

    struct pipe_surface src_surfaces[VL_MAX_SURFACES];
    struct pipe_surface dst_surfaces[VL_MAX_SURFACES];

    /* For HDR content display */
    void *gm_handle;
    uint16_t *lut_data;

    /* For Geometric scaling */
    float scaling_ratios[2];
    float *geometric_scaling_ratios;
    uint8_t geometric_passes;
    struct pipe_video_buffer *geometric_buf[2];

    /* For Lanczos Coeff */
    struct vpe_scaling_lanczos_info *lanczos_info;
};

struct pipe_video_codec*
si_vpe_create_processor(struct pipe_context *context,
                        const struct pipe_video_codec *templ);

#endif
