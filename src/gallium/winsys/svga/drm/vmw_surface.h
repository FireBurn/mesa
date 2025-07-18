/*
 * Copyright (c) 2009-2024 Broadcom. All Rights Reserved.
 * The term “Broadcom” refers to Broadcom Inc.
 * and/or its subsidiaries.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * Surfaces for VMware SVGA winsys.
 *
 * @author Jose Fonseca <jfonseca@vmware.com>
 */


#ifndef VMW_SURFACE_H_
#define VMW_SURFACE_H_


#include "util/compiler.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "util/u_thread.h"
#include "pipebuffer/pb_buffer.h"

#define VMW_MAX_PRESENTS 3



struct vmw_svga_winsys_surface
{
   int32_t validated; /* atomic */
   struct pipe_reference refcnt;

   struct vmw_winsys_screen *screen;
   uint32_t sid;

   /* FIXME: make this thread safe */
   unsigned next_present_no;
   uint32_t present_fences[VMW_MAX_PRESENTS];

   mtx_t mutex;
   struct svga_winsys_buffer *buf; /* Current backing guest buffer */
   uint32_t mapcount; /* Number of mappers */
   uint32_t map_mode; /* PIPE_MAP_[READ|WRITE] */
   void *data; /* Pointer to data if mapcount != 0*/
   bool nodiscard; /* Never discard */
   uint32_t size; /* Size of backing buffer */
   bool rebind; /* Surface needs a rebind after next unmap */
};


static inline struct svga_winsys_surface *
svga_winsys_surface(struct vmw_svga_winsys_surface *surf)
{
   assert(!surf || surf->sid != SVGA3D_INVALID_ID);
   return (struct svga_winsys_surface *)surf;
}


static inline struct vmw_svga_winsys_surface *
vmw_svga_winsys_surface(struct svga_winsys_surface *surf)
{
   return (struct vmw_svga_winsys_surface *)surf;
}


void
vmw_svga_winsys_surface_reference(struct vmw_svga_winsys_surface **pdst,
                                  struct vmw_svga_winsys_surface *src);
void *
vmw_svga_winsys_surface_map(struct svga_winsys_context *swc,
                            struct svga_winsys_surface *srf,
                            unsigned flags, bool *retry,
                            bool *rebind);
void
vmw_svga_winsys_surface_unmap(struct svga_winsys_context *swc,
                              struct svga_winsys_surface *srf,
                              bool *rebind);

void
vmw_svga_winsys_surface_init(struct svga_winsys_screen *sws,
                             struct svga_winsys_surface *surface,
                             unsigned surf_size, SVGA3dSurfaceAllFlags flags);

void
vmw_svga_winsys_userspace_surface_destroy(struct svga_winsys_context *swc,
                                          uint32 sid);

#endif /* VMW_SURFACE_H_ */
