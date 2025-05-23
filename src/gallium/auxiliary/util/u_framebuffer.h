/**************************************************************************
 *
 * Copyright 2009-2010 VMware, Inc.  All Rights Reserved.
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
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


#ifndef U_FRAMEBUFFER_H
#define U_FRAMEBUFFER_H


#include "util/compiler.h"
#include "pipe/p_state.h"


#ifdef __cplusplus
extern "C" {
#endif

extern bool
util_framebuffer_state_equal(const struct pipe_framebuffer_state *dst,
                             const struct pipe_framebuffer_state *src);

extern void
util_copy_framebuffer_state(struct pipe_framebuffer_state *dst,
                            const struct pipe_framebuffer_state *src);


extern void
util_unreference_framebuffer_state(struct pipe_framebuffer_state *fb);


extern bool
util_framebuffer_min_size(const struct pipe_framebuffer_state *fb,
                          unsigned *width,
                          unsigned *height);


extern unsigned
util_framebuffer_get_num_layers(const struct pipe_framebuffer_state *fb);


extern unsigned
util_framebuffer_get_num_samples(const struct pipe_framebuffer_state *fb);


extern void
util_sample_locations_flip_y(struct pipe_screen *screen, unsigned fb_height,
                             unsigned samples, uint8_t *locations);


void
#ifndef _WIN32
__attribute__((deprecated))
#endif
util_framebuffer_init(struct pipe_context *pctx, const struct pipe_framebuffer_state *fb, struct pipe_surface **cbufs, struct pipe_surface **zsbuf);

/* if you see this in your driver stop using it */
#define PIPE_FB_SURFACES \
   struct pipe_surface *fb_cbufs[PIPE_MAX_COLOR_BUFS]; \
   struct pipe_surface *fb_zsbuf

#ifdef __cplusplus
}
#endif

#endif /* U_FRAMEBUFFER_H */
