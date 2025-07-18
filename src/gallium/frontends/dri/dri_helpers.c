/*
 * Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <dlfcn.h>
#include "drm-uapi/drm_fourcc.h"
#include "util/u_memory.h"
#include "pipe/p_screen.h"
#include "state_tracker/st_texture.h"
#include "state_tracker/st_context.h"
#include "main/texobj.h"
#include "util/libsync.h"

#include "dri_helpers.h"
#include "loader_dri_helper.h"

static bool
dri2_is_opencl_interop_loaded_locked(struct dri_screen *screen)
{
   return screen->opencl_dri_event_add_ref &&
          screen->opencl_dri_event_release &&
          screen->opencl_dri_event_wait &&
          screen->opencl_dri_event_get_fence;
}

static bool
dri2_load_opencl_interop(struct dri_screen *screen)
{
#if defined(RTLD_DEFAULT)
   bool success;

   mtx_lock(&screen->opencl_func_mutex);

   if (dri2_is_opencl_interop_loaded_locked(screen)) {
      mtx_unlock(&screen->opencl_func_mutex);
      return true;
   }

   screen->opencl_dri_event_add_ref =
      dlsym(RTLD_DEFAULT, "opencl_dri_event_add_ref");
   screen->opencl_dri_event_release =
      dlsym(RTLD_DEFAULT, "opencl_dri_event_release");
   screen->opencl_dri_event_wait =
      dlsym(RTLD_DEFAULT, "opencl_dri_event_wait");
   screen->opencl_dri_event_get_fence =
      dlsym(RTLD_DEFAULT, "opencl_dri_event_get_fence");

   success = dri2_is_opencl_interop_loaded_locked(screen);
   mtx_unlock(&screen->opencl_func_mutex);
   return success;
#else
   return false;
#endif
}

struct dri2_fence {
   struct dri_screen *driscreen;
   struct pipe_fence_handle *pipe_fence;
   void *cl_event;
};

unsigned
dri_fence_get_caps(struct dri_screen *driscreen)
{
   struct pipe_screen *screen = driscreen->base.screen;
   unsigned caps = 0;

   if (screen->caps.native_fence_fd)
      caps |= __DRI_FENCE_CAP_NATIVE_FD;

   return caps;
}

void *
dri_create_fence(struct dri_context *ctx)
{
   struct st_context *st = ctx->st;
   struct dri2_fence *fence = CALLOC_STRUCT(dri2_fence);

   if (!fence)
      return NULL;

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   _mesa_glthread_finish(st->ctx);

   st_context_flush(st, 0, &fence->pipe_fence, NULL, NULL);

   if (!fence->pipe_fence) {
      FREE(fence);
      return NULL;
   }

   fence->driscreen = ctx->screen;
   return fence;
}

void *
dri_create_fence_fd(struct dri_context *dri_ctx, int fd)
{
   struct st_context *st = dri_ctx->st;
   struct pipe_context *ctx = st->pipe;
   struct dri2_fence *fence = CALLOC_STRUCT(dri2_fence);

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   _mesa_glthread_finish(st->ctx);

   if (fd == -1) {
      /* exporting driver created fence, flush: */
      st_context_flush(st, ST_FLUSH_FENCE_FD, &fence->pipe_fence, NULL, NULL);
   } else {
      /* importing a foreign fence fd: */
      ctx->create_fence_fd(ctx, &fence->pipe_fence, fd, PIPE_FD_TYPE_NATIVE_SYNC);
   }
   if (!fence->pipe_fence) {
      FREE(fence);
      return NULL;
   }

   fence->driscreen = dri_ctx->screen;
   return fence;
}

int
dri_get_fence_fd(struct dri_screen *driscreen, void *_fence)
{
   struct pipe_screen *screen = driscreen->base.screen;
   struct dri2_fence *fence = (struct dri2_fence*)_fence;

   return screen->fence_get_fd(screen, fence->pipe_fence);
}

void *
dri_get_fence_from_cl_event(struct dri_screen *driscreen, intptr_t cl_event)
{
   struct dri2_fence *fence;

   if (!dri2_load_opencl_interop(driscreen))
      return NULL;

   fence = CALLOC_STRUCT(dri2_fence);
   if (!fence)
      return NULL;

   fence->cl_event = (void*)cl_event;

   if (!driscreen->opencl_dri_event_add_ref(fence->cl_event)) {
      free(fence);
      return NULL;
   }

   fence->driscreen = driscreen;
   return fence;
}

void
dri_destroy_fence(struct dri_screen *driscreen, void *_fence)
{
   struct pipe_screen *screen = driscreen->base.screen;
   struct dri2_fence *fence = (struct dri2_fence*)_fence;

   if (fence->pipe_fence)
      screen->fence_reference(screen, &fence->pipe_fence, NULL);
   else if (fence->cl_event)
      driscreen->opencl_dri_event_release(fence->cl_event);
   else
      assert(0);

   FREE(fence);
}

GLboolean
dri_client_wait_sync(struct dri_context *_ctx, void *_fence, unsigned flags,
                      uint64_t timeout)
{
   struct dri2_fence *fence = (struct dri2_fence*)_fence;
   struct dri_screen *driscreen = fence->driscreen;
   struct pipe_screen *screen = driscreen->base.screen;

   /* No need to flush. The context was flushed when the fence was created. */

   if (fence->pipe_fence)
      return screen->fence_finish(screen, NULL, fence->pipe_fence, timeout);
   else if (fence->cl_event) {
      struct pipe_fence_handle *pipe_fence =
         driscreen->opencl_dri_event_get_fence(fence->cl_event);

      if (pipe_fence)
         return screen->fence_finish(screen, NULL, pipe_fence, timeout);
      else
         return driscreen->opencl_dri_event_wait(fence->cl_event, timeout);
   }
   else {
      assert(0);
      return false;
   }
}

void
dri_server_wait_sync(struct dri_context *_ctx, void *_fence, unsigned flags)
{
   struct st_context *st = _ctx->st;
   struct pipe_context *ctx = st->pipe;
   struct dri2_fence *fence = (struct dri2_fence*)_fence;

   /* We might be called here with a NULL fence as a result of WaitSyncKHR
    * on a EGL_KHR_reusable_sync fence. Nothing to do here in such case.
    */
   if (!fence)
      return;

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   _mesa_glthread_finish(st->ctx);

   if (ctx->fence_server_sync)
      ctx->fence_server_sync(ctx, fence->pipe_fence, 0);
}

struct dri_image *
dri_create_image_from_renderbuffer(struct dri_context *dri_ctx,
				     int renderbuffer, void *loaderPrivate,
                                     unsigned *error)
{
   struct st_context *st = dri_ctx->st;
   struct gl_context *ctx = st->ctx;
   struct pipe_context *p_ctx = st->pipe;
   struct gl_renderbuffer *rb;
   struct pipe_resource *tex;
   struct dri_image *img;

   /* Wait for glthread to finish to get up-to-date GL object lookups. */
   _mesa_glthread_finish(st->ctx);

   /* Section 3.9 (EGLImage Specification and Management) of the EGL 1.5
    * specification says:
    *
    *   "If target is EGL_GL_RENDERBUFFER and buffer is not the name of a
    *    renderbuffer object, or if buffer is the name of a multisampled
    *    renderbuffer object, the error EGL_BAD_PARAMETER is generated."
    *
    *   "If target is EGL_GL_TEXTURE_2D , EGL_GL_TEXTURE_CUBE_MAP_*,
    *    EGL_GL_RENDERBUFFER or EGL_GL_TEXTURE_3D and buffer refers to the
    *    default GL texture object (0) for the corresponding GL target, the
    *    error EGL_BAD_PARAMETER is generated."
    *   (rely on _mesa_lookup_renderbuffer returning NULL in this case)
    */
   rb = _mesa_lookup_renderbuffer(ctx, renderbuffer);
   if (!rb || rb->NumSamples > 0) {
      *error = __DRI_IMAGE_ERROR_BAD_PARAMETER;
      return NULL;
   }

   tex = rb->texture;
   if (!tex) {
      *error = __DRI_IMAGE_ERROR_BAD_PARAMETER;
      return NULL;
   }

   img = CALLOC_STRUCT(dri_image);
   if (!img) {
      *error = __DRI_IMAGE_ERROR_BAD_ALLOC;
      return NULL;
   }

   img->dri_format = tex->format;
   img->internal_format = rb->InternalFormat;
   img->loader_private = loaderPrivate;
   img->screen = dri_ctx->screen;
   img->in_fence_fd = -1;

   pipe_resource_reference(&img->texture, tex);

   /* If the resource supports EGL_MESA_image_dma_buf_export, make sure that
    * it's in a shareable state. Do this now while we still have the access to
    * the context.
    */
   if (dri2_get_mapping_by_format(img->dri_format)) {
      p_ctx->flush_resource(p_ctx, tex);
      st_context_flush(st, 0, NULL, NULL, NULL);
   }

   ctx->Shared->HasExternallySharedImages = true;
   *error = __DRI_IMAGE_ERROR_SUCCESS;
   return img;
}

void
dri2_destroy_image(struct dri_image *img)
{
   const __DRIimageLoaderExtension *imgLoader = img->screen->image.loader;

   if (imgLoader && imgLoader->base.version >= 4 &&
         imgLoader->destroyLoaderImageState) {
      imgLoader->destroyLoaderImageState(img->loader_private);
   }

   pipe_resource_reference(&img->texture, NULL);

   if (img->in_fence_fd != -1)
      close(img->in_fence_fd);

   FREE(img);
}


struct dri_image *
dri2_create_from_texture(struct dri_context *dri_ctx, int target, unsigned texture,
                         int depth, int level, unsigned *error,
                         void *loaderPrivate)
{
   struct dri_image *img;
   struct st_context *st = dri_ctx->st;
   struct gl_context *ctx = st->ctx;
   struct pipe_context *p_ctx = st->pipe;
   struct gl_texture_object *obj;
   struct gl_texture_image *glimg;
   GLuint face = 0;

   /* Wait for glthread to finish to get up-to-date GL object lookups. */
   _mesa_glthread_finish(st->ctx);

   obj = _mesa_lookup_texture(ctx, texture);
   if (!obj || obj->Target != target) {
      *error = __DRI_IMAGE_ERROR_BAD_PARAMETER;
      return NULL;
   }

   if (target == GL_TEXTURE_CUBE_MAP)
      face = depth;

   _mesa_test_texobj_completeness(ctx, obj);
   if (!obj->_BaseComplete || (level > 0 && !obj->_MipmapComplete)) {
      *error = __DRI_IMAGE_ERROR_BAD_PARAMETER;
      return NULL;
   }

   if (level < obj->Attrib.BaseLevel || level > obj->_MaxLevel) {
      *error = __DRI_IMAGE_ERROR_BAD_MATCH;
      return NULL;
   }

   glimg = obj->Image[face][level];
   if (!glimg || !glimg->pt) {
      *error = __DRI_IMAGE_ERROR_BAD_PARAMETER;
      return NULL;
   }

   if (target == GL_TEXTURE_3D && glimg->Depth < depth) {
      *error = __DRI_IMAGE_ERROR_BAD_MATCH;
      return NULL;
   }

   img = CALLOC_STRUCT(dri_image);
   if (!img) {
      *error = __DRI_IMAGE_ERROR_BAD_ALLOC;
      return NULL;
   }

   img->level = level;
   img->layer = depth;
   img->in_fence_fd = -1;
   img->dri_format = glimg->pt->format;
   img->internal_format = glimg->InternalFormat;

   img->loader_private = loaderPrivate;
   img->screen = dri_ctx->screen;

   pipe_resource_reference(&img->texture, glimg->pt);

   /* If the resource supports EGL_MESA_image_dma_buf_export, make sure that
    * it's in a shareable state. Do this now while we still have the access to
    * the context.
    */
   if (dri2_get_mapping_by_format(img->dri_format)) {
      p_ctx->flush_resource(p_ctx, glimg->pt);
      st_context_flush(st, 0, NULL, NULL, NULL);
   }

   ctx->Shared->HasExternallySharedImages = true;
   *error = __DRI_IMAGE_ERROR_SUCCESS;
   return img;
}

static const struct dri2_format_mapping dri2_format_table[] = {

      /*
       * RGB formats:
       */
      { DRM_FORMAT_ABGR16161616F, __DRI_IMAGE_FORMAT_ABGR16161616F,
        PIPE_FORMAT_R16G16B16A16_FLOAT, 1, },
      { DRM_FORMAT_XBGR16161616F, __DRI_IMAGE_FORMAT_XBGR16161616F,
        PIPE_FORMAT_R16G16B16X16_FLOAT, 1, },
      { DRM_FORMAT_ABGR16161616, __DRI_IMAGE_FORMAT_ABGR16161616,
        PIPE_FORMAT_R16G16B16A16_UNORM, 1, },
      { DRM_FORMAT_XBGR16161616, __DRI_IMAGE_FORMAT_XBGR16161616,
        PIPE_FORMAT_R16G16B16X16_UNORM, 1, },
      { DRM_FORMAT_ARGB2101010,   __DRI_IMAGE_FORMAT_ARGB2101010,
        PIPE_FORMAT_B10G10R10A2_UNORM, 1, },
      { DRM_FORMAT_XRGB2101010,   __DRI_IMAGE_FORMAT_XRGB2101010,
        PIPE_FORMAT_B10G10R10X2_UNORM, 1, },
      { DRM_FORMAT_ABGR2101010,   __DRI_IMAGE_FORMAT_ABGR2101010,
        PIPE_FORMAT_R10G10B10A2_UNORM, 1, },
      { DRM_FORMAT_XBGR2101010,   __DRI_IMAGE_FORMAT_XBGR2101010,
        PIPE_FORMAT_R10G10B10X2_UNORM, 1, },
      { DRM_FORMAT_ARGB8888,      __DRI_IMAGE_FORMAT_ARGB8888,
        PIPE_FORMAT_BGRA8888_UNORM, 1, },
      { DRM_FORMAT_ABGR8888,      __DRI_IMAGE_FORMAT_ABGR8888,
        PIPE_FORMAT_RGBA8888_UNORM, 1, },
      { __DRI_IMAGE_FOURCC_SARGB8888,     __DRI_IMAGE_FORMAT_SARGB8,
        PIPE_FORMAT_BGRA8888_SRGB, 1, },
      { DRM_FORMAT_XRGB8888,      __DRI_IMAGE_FORMAT_XRGB8888,
        PIPE_FORMAT_BGRX8888_UNORM, 1, },
      { DRM_FORMAT_RGB888,        __DRI_IMAGE_FORMAT_RGB888,
        PIPE_FORMAT_B8G8R8_UNORM, 1, },
      { DRM_FORMAT_XBGR8888,      __DRI_IMAGE_FORMAT_XBGR8888,
        PIPE_FORMAT_RGBX8888_UNORM, 1, },
      { DRM_FORMAT_BGR888,        __DRI_IMAGE_FORMAT_BGR888,
        PIPE_FORMAT_R8G8B8_UNORM, 1, },
      { DRM_FORMAT_ARGB1555,      __DRI_IMAGE_FORMAT_ARGB1555,
        PIPE_FORMAT_B5G5R5A1_UNORM, 1, },
      { DRM_FORMAT_ABGR1555,      __DRI_IMAGE_FORMAT_ABGR1555,
        PIPE_FORMAT_R5G5B5A1_UNORM, 1, },
      { DRM_FORMAT_ARGB4444,      __DRI_IMAGE_FORMAT_ARGB4444,
        PIPE_FORMAT_B4G4R4A4_UNORM, 1, },
      { DRM_FORMAT_ABGR4444,      __DRI_IMAGE_FORMAT_ABGR4444,
        PIPE_FORMAT_R4G4B4A4_UNORM, 1, },
      { DRM_FORMAT_RGB565,        __DRI_IMAGE_FORMAT_RGB565,
        PIPE_FORMAT_B5G6R5_UNORM, 1, },
      { DRM_FORMAT_R8,            __DRI_IMAGE_FORMAT_R8,
        PIPE_FORMAT_R8_UNORM, 1, },
      { DRM_FORMAT_R16,           __DRI_IMAGE_FORMAT_R16,
        PIPE_FORMAT_R16_UNORM, 1, },
      { DRM_FORMAT_GR88,          __DRI_IMAGE_FORMAT_GR88,
        PIPE_FORMAT_RG88_UNORM, 1, },
      { DRM_FORMAT_GR1616,        __DRI_IMAGE_FORMAT_GR1616,
        PIPE_FORMAT_RG1616_UNORM, 1, },
      { DRM_FORMAT_R16F,          PIPE_FORMAT_R16_FLOAT,
         PIPE_FORMAT_R16_FLOAT, 1 },
      { DRM_FORMAT_R32F,          PIPE_FORMAT_R32_FLOAT,
         PIPE_FORMAT_R32_FLOAT, 1 },
      { DRM_FORMAT_GR1616F,       PIPE_FORMAT_R16G16_FLOAT,
         PIPE_FORMAT_R16G16_FLOAT, 1 },
      { DRM_FORMAT_GR3232F,       PIPE_FORMAT_R32G32_FLOAT,
         PIPE_FORMAT_R32G32_FLOAT, 1 },
      { DRM_FORMAT_BGR161616,     PIPE_FORMAT_R16G16B16_UNORM,
         PIPE_FORMAT_R16G16B16_UNORM, 1 },
      { DRM_FORMAT_BGR161616F,    PIPE_FORMAT_R16G16B16_FLOAT,
         PIPE_FORMAT_R16G16B16_FLOAT, 1 },
      { DRM_FORMAT_BGR323232F,    PIPE_FORMAT_R32G32B32_FLOAT,
         PIPE_FORMAT_R32G32B32_FLOAT, 1 },
      { DRM_FORMAT_ABGR32323232F, PIPE_FORMAT_R32G32B32A32_FLOAT,
         PIPE_FORMAT_R32G32B32A32_FLOAT, 1 },

      /*
       * YUV formats:
       */
      { DRM_FORMAT_YUV410, __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 2, 2, __DRI_IMAGE_FORMAT_R8 },
          { 2, 2, 2, __DRI_IMAGE_FORMAT_R8 } } },
      { DRM_FORMAT_YUV411, __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 2, 0, __DRI_IMAGE_FORMAT_R8 },
          { 2, 2, 0, __DRI_IMAGE_FORMAT_R8 } } },
      { DRM_FORMAT_YUV420,        __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_R8 },
          { 2, 1, 1, __DRI_IMAGE_FORMAT_R8 } } },
      { DRM_FORMAT_YUV422,        __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 1, 0, __DRI_IMAGE_FORMAT_R8 },
          { 2, 1, 0, __DRI_IMAGE_FORMAT_R8 } } },
      { DRM_FORMAT_YUV444,        __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 2, 0, 0, __DRI_IMAGE_FORMAT_R8 } } },

      { DRM_FORMAT_YVU410,        __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 2, 2, 2, __DRI_IMAGE_FORMAT_R8 },
          { 1, 2, 2, __DRI_IMAGE_FORMAT_R8 } } },
      { DRM_FORMAT_YVU411,        __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 2, 2, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 2, 0, __DRI_IMAGE_FORMAT_R8 } } },
      { DRM_FORMAT_YVU420,        __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 2, 1, 1, __DRI_IMAGE_FORMAT_R8 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_R8 } } },
      { DRM_FORMAT_YVU422,        __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 2, 1, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 1, 0, __DRI_IMAGE_FORMAT_R8 } } },
      { DRM_FORMAT_YVU444,        __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_IYUV, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 2, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 0, 0, __DRI_IMAGE_FORMAT_R8 } } },

      { DRM_FORMAT_S010,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y10X6_U10X6_V10X6_420_UNORM, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_R16 },
          { 2, 1, 1, __DRI_IMAGE_FORMAT_R16 } } },
      { DRM_FORMAT_S210,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y10X6_U10X6_V10X6_422_UNORM, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 0, __DRI_IMAGE_FORMAT_R16 },
          { 2, 1, 0, __DRI_IMAGE_FORMAT_R16 } } },
      { DRM_FORMAT_S410,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y10X6_U10X6_V10X6_444_UNORM, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 2, 0, 0, __DRI_IMAGE_FORMAT_R16 } } },
      { DRM_FORMAT_S012,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y12X4_U12X4_V12X4_420_UNORM, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_R16 },
          { 2, 1, 1, __DRI_IMAGE_FORMAT_R16 } } },
      { DRM_FORMAT_S212,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y12X4_U12X4_V12X4_422_UNORM, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 0, __DRI_IMAGE_FORMAT_R16 },
          { 2, 1, 0, __DRI_IMAGE_FORMAT_R16 } } },
      { DRM_FORMAT_S412,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y12X4_U12X4_V12X4_444_UNORM, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 2, 0, 0, __DRI_IMAGE_FORMAT_R16 } } },
      { DRM_FORMAT_S016,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y16_U16_V16_420_UNORM, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_R16 },
          { 2, 1, 1, __DRI_IMAGE_FORMAT_R16 } } },
      { DRM_FORMAT_S216,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y16_U16_V16_422_UNORM, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 0, __DRI_IMAGE_FORMAT_R16 },
          { 2, 1, 0, __DRI_IMAGE_FORMAT_R16 } } },
      { DRM_FORMAT_S416,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y16_U16_V16_444_UNORM, 3,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 2, 0, 0, __DRI_IMAGE_FORMAT_R16 } } },

      { DRM_FORMAT_NV12,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_NV12, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_GR88 } } },
      { DRM_FORMAT_NV21,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_NV21, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_GR88 } } },

      /* 10 bit 4:2:0 and 4:2:2 formats; the components
         are tightly packed, so the planes don't correspond
         to any native DRI format */
      { DRM_FORMAT_NV15,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_NV15, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_NONE },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_NONE } } },
      { DRM_FORMAT_NV20,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_NV20, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_NONE },
          { 1, 1, 0, __DRI_IMAGE_FORMAT_NONE } } },

      { DRM_FORMAT_P010,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_P010, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_GR1616 } } },
      { DRM_FORMAT_P012,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_P012, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_GR1616 } } },
      { DRM_FORMAT_P016,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_P016, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_GR1616 } } },
      { DRM_FORMAT_P030,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_P030, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R16 },
          { 1, 1, 1, __DRI_IMAGE_FORMAT_GR1616 } } },

      { DRM_FORMAT_NV16,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_NV16, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_R8 },
          { 1, 1, 0, __DRI_IMAGE_FORMAT_GR88 } } },

      { DRM_FORMAT_AYUV,      __DRI_IMAGE_FORMAT_ABGR8888,
        PIPE_FORMAT_AYUV, 1,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_ABGR8888 } } },
      { DRM_FORMAT_XYUV8888,      __DRI_IMAGE_FORMAT_XBGR8888,
        PIPE_FORMAT_XYUV, 1,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_XBGR8888 } } },

      { DRM_FORMAT_Y410,          __DRI_IMAGE_FORMAT_ABGR2101010,
        PIPE_FORMAT_Y410, 1,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_ABGR2101010 } } },

      /* Y412 is an unusual format.  It has the same layout as Y416 (i.e.,
       * 16-bits of physical storage per channel), but the low 4 bits of each
       * component are unused padding.  The writer is supposed to write zeros
       * to these bits.
       */
      { DRM_FORMAT_Y412,          __DRI_IMAGE_FORMAT_ABGR16161616,
        PIPE_FORMAT_Y412, 1,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_ABGR16161616 } } },
      { DRM_FORMAT_Y416,          __DRI_IMAGE_FORMAT_ABGR16161616,
        PIPE_FORMAT_Y416, 1,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_ABGR16161616 } } },

      /* For YUYV and UYVY buffers, we set up two overlapping DRI images
       * and treat them as planar buffers in the compositors.
       * Plane 0 is GR88 and samples YU or YV pairs and places Y into
       * the R component, while plane 1 is ARGB/ABGR and samples YUYV/UYVY
       * clusters and places pairs and places U into the G component and
       * V into A.  This lets the texture sampler interpolate the Y
       * components correctly when sampling from plane 0, and interpolate
       * U and V correctly when sampling from plane 1. */
      { DRM_FORMAT_YUYV,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_YUYV, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_GR88 },
          { 0, 1, 0, __DRI_IMAGE_FORMAT_ARGB8888 } } },
      { DRM_FORMAT_YVYU,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_YVYU, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_GR88 },
          { 0, 1, 0, __DRI_IMAGE_FORMAT_ARGB8888 } } },
      { DRM_FORMAT_UYVY,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_UYVY, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_GR88 },
          { 0, 1, 0, __DRI_IMAGE_FORMAT_ABGR8888 } } },
      { DRM_FORMAT_VYUY,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_VYUY, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_GR88 },
          { 0, 1, 0, __DRI_IMAGE_FORMAT_ABGR8888 } } },

      /* The Y21x formats work in a similar fashion to the YUYV and UYVY
       * formats.
       */
      { DRM_FORMAT_Y210,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y210, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_GR1616 },
          { 0, 1, 0, __DRI_IMAGE_FORMAT_ABGR16161616 } } },
      /* Y212 is an unusual format.  It has the same layout as Y216 (i.e.,
       * 16-bits of physical storage per channel), but the low 4 bits of each
       * component are unused padding.  The writer is supposed to write zeros
       * to these bits.
       */
      { DRM_FORMAT_Y212,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y212, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_GR1616 },
          { 0, 1, 0, __DRI_IMAGE_FORMAT_ABGR16161616 } } },
      { DRM_FORMAT_Y216,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y216, 2,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_GR1616 },
          { 0, 1, 0, __DRI_IMAGE_FORMAT_ABGR16161616 } } },

      /* YUV420_8BIT is a single plane with all components, but in an
         unspecified order */
      { DRM_FORMAT_YUV420_8BIT,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y8U8V8_420_UNORM_PACKED, 1,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_NONE } } },

      /* YUV420_10BIT is a single plane with all components, but in an
         unspecified order */
      { DRM_FORMAT_YUV420_10BIT,          __DRI_IMAGE_FORMAT_NONE,
        PIPE_FORMAT_Y10U10V10_420_UNORM_PACKED, 1,
        { { 0, 0, 0, __DRI_IMAGE_FORMAT_NONE } } },
};

const struct dri2_format_mapping *
dri2_get_mapping_by_fourcc(int fourcc)
{
   for (unsigned i = 0; i < ARRAY_SIZE(dri2_format_table); i++) {
      if (dri2_format_table[i].dri_fourcc == fourcc)
         return &dri2_format_table[i];
   }

   return NULL;
}

const struct dri2_format_mapping *
dri2_get_mapping_by_format(int format)
{
   if (format == __DRI_IMAGE_FORMAT_NONE)
      return NULL;

   for (unsigned i = 0; i < ARRAY_SIZE(dri2_format_table); i++) {
      if (dri2_format_table[i].dri_format == format)
         return &dri2_format_table[i];
   }

   return NULL;
}

static enum pipe_format
alt_pipe_format(enum pipe_format yuv_fmt)
{
   switch(yuv_fmt) {
   case PIPE_FORMAT_NV12:
      return PIPE_FORMAT_R8_G8B8_420_UNORM;
   case PIPE_FORMAT_NV16:
      return PIPE_FORMAT_R8_G8B8_422_UNORM;
   case PIPE_FORMAT_NV21:
      return PIPE_FORMAT_R8_B8G8_420_UNORM;
   case PIPE_FORMAT_NV15:
      return PIPE_FORMAT_R10_G10B10_420_UNORM;
   case PIPE_FORMAT_NV20:
      return PIPE_FORMAT_R10_G10B10_422_UNORM;
   case PIPE_FORMAT_Y8U8V8_420_UNORM_PACKED:
      return PIPE_FORMAT_R8G8B8_420_UNORM_PACKED;
   case PIPE_FORMAT_Y10U10V10_420_UNORM_PACKED:
      return PIPE_FORMAT_R10G10B10_420_UNORM_PACKED;
   default:
      return yuv_fmt;
   }
}

bool
dri2_yuv_dma_buf_supported(struct dri_screen *screen,
                           const struct dri2_format_mapping *map)
{
   struct pipe_screen *pscreen = screen->base.screen;

   if (pscreen->is_format_supported(pscreen, alt_pipe_format(map->pipe_format),
                                    screen->target, 0, 0, PIPE_BIND_SAMPLER_VIEW))
      return true;

   if (!util_format_is_yuv(map->pipe_format))
      return false;

   for (unsigned i = 0; i < map->nplanes; i++) {
      if (!pscreen->is_format_supported(pscreen, map->planes[i].dri_format,
            screen->target, 0, 0, PIPE_BIND_SAMPLER_VIEW))
         return false;
   }
   return true;
}

bool
dri_query_dma_buf_formats(struct dri_screen *screen, int max, int *formats,
                           int *count)
{
   struct pipe_screen *pscreen = screen->base.screen;
   int i, j;

   for (i = 0, j = 0; (i < ARRAY_SIZE(dri2_format_table)) &&
         (j < max || max == 0); i++) {
      const struct dri2_format_mapping *map = &dri2_format_table[i];

      /* The sRGB format is not a real FourCC as defined by drm_fourcc.h, so we
       * must not leak it out to clients. */
      if (dri2_format_table[i].dri_fourcc == __DRI_IMAGE_FOURCC_SARGB8888)
         continue;

      if (pscreen->is_format_supported(pscreen, map->pipe_format,
                                       screen->target, 0, 0,
                                       PIPE_BIND_RENDER_TARGET) ||
          pscreen->is_format_supported(pscreen, map->pipe_format,
                                       screen->target, 0, 0,
                                       PIPE_BIND_SAMPLER_VIEW) ||
          pscreen->is_format_supported(pscreen, map->pipe_format,
                                       screen->target, 0, 0,
                                       PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SAMPLER_VIEW_SUBOPTIMAL) ||
          dri2_yuv_dma_buf_supported(screen, map)) {
         if (j < max)
            formats[j] = map->dri_fourcc;
         j++;
      }
   }
   *count = j;
   return true;
}


struct dri_image *
dri_create_image_with_modifiers(struct dri_screen *screen,
                                 uint32_t width, uint32_t height,
                                 uint32_t dri_format, uint32_t dri_usage,
                                 const uint64_t *modifiers,
                                 unsigned int modifiers_count,
                                 void *loaderPrivate)
{
   if (modifiers && modifiers_count > 0) {
      bool has_valid_modifier = false;
      int i;

      /* It's acceptable to create an image with INVALID modifier in the list,
       * but it cannot be on the only modifier (since it will certainly fail
       * later). While we could easily catch this after modifier creation, doing
       * the check here is a convenient debug check likely pointing at whatever
       * interface the client is using to build its modifier list.
       */
      for (i = 0; i < modifiers_count; i++) {
         if (modifiers[i] != DRM_FORMAT_MOD_INVALID) {
            has_valid_modifier = true;
            break;
         }
      }
      if (!has_valid_modifier)
         return NULL;
   }

   return dri_create_image(screen, width, height, dri_format,
                           modifiers, modifiers_count, dri_usage,
                           loaderPrivate);
}

void
dri_image_fence_sync(struct dri_context *ctx, struct dri_image *img)
{
   struct pipe_context *pipe = ctx->st->pipe;
   struct pipe_fence_handle *fence;
   int fd = img->in_fence_fd;

   if (fd == -1)
      return;

   validate_fence_fd(fd);

   img->in_fence_fd = -1;

   pipe->create_fence_fd(pipe, &fence, fd, PIPE_FD_TYPE_NATIVE_SYNC);
   pipe->fence_server_sync(pipe, fence, 0);
   pipe->screen->fence_reference(pipe->screen, &fence, NULL);

   close(fd);
}
/* vim: set sw=3 ts=8 sts=3 expandtab: */
