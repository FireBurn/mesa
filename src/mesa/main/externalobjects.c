/*
 * Copyright © 2016 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "macros.h"
#include "mtypes.h"
#include "bufferobj.h"
#include "context.h"
#include "enums.h"
#include "externalobjects.h"
#include "teximage.h"
#include "texobj.h"
#include "glformats.h"
#include "texstorage.h"
#include "util/u_memory.h"

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "api_exec_decl.h"

#include "state_tracker/st_cb_bitmap.h"
#include "state_tracker/st_texture.h"

struct st_context;

#include "frontend/drm_driver.h"
#ifdef HAVE_LIBDRM
#include "drm-uapi/drm_fourcc.h"
#endif

static struct gl_memory_object *
memoryobj_alloc(struct gl_context *ctx, GLuint name)
{
   struct gl_memory_object *obj = CALLOC_STRUCT(gl_memory_object);
   if (!obj)
      return NULL;

   obj->Name = name;
   obj->Dedicated = GL_FALSE;
   return obj;
}

static void
import_memoryobj_fd(struct gl_context *ctx,
                    struct gl_memory_object *obj,
                    GLuint64 size,
                    int fd)
{
#if !defined(_WIN32)
   struct pipe_screen *screen = ctx->pipe->screen;
   struct winsys_handle whandle = {
      .type = WINSYS_HANDLE_TYPE_FD,
      .handle = fd,
#ifdef HAVE_LIBDRM
      .modifier = DRM_FORMAT_MOD_INVALID,
#endif
   };

   obj->memory = screen->memobj_create_from_handle(screen,
                                                   &whandle,
                                                   obj->Dedicated);

   /* We own fd, but we no longer need it. So get rid of it */
   close(fd);
#endif
}

static void
import_memoryobj_win32(struct gl_context *ctx,
                       struct gl_memory_object *obj,
                       GLuint64 size,
                       void *handle,
                       const void *name)
{
   struct pipe_screen *screen = ctx->pipe->screen;
   struct winsys_handle whandle = {
      .type = handle ? WINSYS_HANDLE_TYPE_WIN32_HANDLE : WINSYS_HANDLE_TYPE_WIN32_NAME,
#ifdef _WIN32
      .handle = handle,
#else
      .handle = 0,
#endif
#ifdef HAVE_LIBDRM
      .modifier = DRM_FORMAT_MOD_INVALID,
#endif
      .name = name,
   };

   obj->memory = screen->memobj_create_from_handle(screen,
                                                   &whandle,
                                                   obj->Dedicated);
}

/**
 * Delete a memory object.
 * Not removed from hash table here.
 */
void
_mesa_delete_memory_object(struct gl_context *ctx,
                           struct gl_memory_object *memObj)
{
   struct pipe_screen *screen = ctx->pipe->screen;
   if (memObj->memory)
      screen->memobj_destroy(screen, memObj->memory);
   FREE(memObj);
}

void GLAPIENTRY
_mesa_DeleteMemoryObjectsEXT(GLsizei n, const GLuint *memoryObjects)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & (VERBOSE_API)) {
      _mesa_debug(ctx, "glDeleteMemoryObjectsEXT(%d, %p)\n", n,
                  memoryObjects);
   }

   if (!_mesa_has_EXT_memory_object(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glDeleteMemoryObjectsEXT(unsupported)");
      return;
   }

   if (n < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glDeleteMemoryObjectsEXT(n < 0)");
      return;
   }

   if (!memoryObjects)
      return;

   _mesa_HashLockMutex(&ctx->Shared->MemoryObjects);
   for (GLint i = 0; i < n; i++) {
      if (memoryObjects[i] > 0) {
         struct gl_memory_object *delObj
            = _mesa_lookup_memory_object_locked(ctx, memoryObjects[i]);

         if (delObj) {
            _mesa_HashRemoveLocked(&ctx->Shared->MemoryObjects,
                                   memoryObjects[i]);
            _mesa_delete_memory_object(ctx, delObj);
         }
      }
   }
   _mesa_HashUnlockMutex(&ctx->Shared->MemoryObjects);
}

GLboolean GLAPIENTRY
_mesa_IsMemoryObjectEXT(GLuint memoryObject)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!_mesa_has_EXT_memory_object(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glIsMemoryObjectEXT(unsupported)");
      return GL_FALSE;
   }

   struct gl_memory_object *obj =
      _mesa_lookup_memory_object(ctx, memoryObject);

   return obj ? GL_TRUE : GL_FALSE;
}

void GLAPIENTRY
_mesa_CreateMemoryObjectsEXT(GLsizei n, GLuint *memoryObjects)
{
   GET_CURRENT_CONTEXT(ctx);

   const char *func = "glCreateMemoryObjectsEXT";

   if (MESA_VERBOSE & (VERBOSE_API))
      _mesa_debug(ctx, "%s(%d, %p)\n", func, n, memoryObjects);

   if (!_mesa_has_EXT_memory_object(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (n < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(n < 0)", func);
      return;
   }

   if (!memoryObjects)
      return;

   _mesa_HashLockMutex(&ctx->Shared->MemoryObjects);
   if (_mesa_HashFindFreeKeys(&ctx->Shared->MemoryObjects, memoryObjects, n)) {
      for (GLsizei i = 0; i < n; i++) {
         struct gl_memory_object *memObj;

         /* allocate memory object */
         memObj = memoryobj_alloc(ctx, memoryObjects[i]);
         if (!memObj) {
            _mesa_error(ctx, GL_OUT_OF_MEMORY, "%s()", func);
            _mesa_HashUnlockMutex(&ctx->Shared->MemoryObjects);
            return;
         }

         /* insert into hash table */
         _mesa_HashInsertLocked(&ctx->Shared->MemoryObjects, memoryObjects[i],
                                memObj);
      }
   }

   _mesa_HashUnlockMutex(&ctx->Shared->MemoryObjects);
}

void GLAPIENTRY
_mesa_MemoryObjectParameterivEXT(GLuint memoryObject,
                                 GLenum pname,
                                 const GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_memory_object *memObj;

   const char *func = "glMemoryObjectParameterivEXT";

   if (!_mesa_has_EXT_memory_object(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   memObj = _mesa_lookup_memory_object(ctx, memoryObject);
   if (!memObj)
      return;

   if (memObj->Immutable) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(memoryObject is immutable", func);
      return;
   }

   switch (pname) {
   case GL_DEDICATED_MEMORY_OBJECT_EXT:
      memObj->Dedicated = (GLboolean) params[0];
      break;
   case GL_PROTECTED_MEMORY_OBJECT_EXT:
      /* EXT_protected_textures not supported */
      goto invalid_pname;
   default:
      goto invalid_pname;
   }
   return;

invalid_pname:
   _mesa_error(ctx, GL_INVALID_ENUM, "%s(pname=0x%x)", func, pname);
}

void GLAPIENTRY
_mesa_GetMemoryObjectParameterivEXT(GLuint memoryObject,
                                    GLenum pname,
                                    GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_memory_object *memObj;

   const char *func = "glMemoryObjectParameterivEXT";

   if (!_mesa_has_EXT_memory_object(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   memObj = _mesa_lookup_memory_object(ctx, memoryObject);
   if (!memObj)
      return;

   switch (pname) {
      case GL_DEDICATED_MEMORY_OBJECT_EXT:
         *params = (GLint) memObj->Dedicated;
         break;
      case GL_PROTECTED_MEMORY_OBJECT_EXT:
         /* EXT_protected_textures not supported */
         goto invalid_pname;
      default:
         goto invalid_pname;
   }
   return;

invalid_pname:
   _mesa_error(ctx, GL_INVALID_ENUM, "%s(pname=0x%x)", func, pname);
}

static struct gl_memory_object *
lookup_memory_object_err(struct gl_context *ctx, unsigned memory,
                         const char* func)
{
   if (memory == 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(memory=0)", func);
      return NULL;
   }

   struct gl_memory_object *memObj = _mesa_lookup_memory_object(ctx, memory);
   if (!memObj)
      return NULL;

   if (!memObj->Immutable) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(no associated memory)",
                  func);
      return NULL;
   }

   return memObj;
}

/**
 * Helper used by _mesa_TexStorageMem1/2/3DEXT().
 */
static void
texstorage_memory(GLuint dims, GLenum target, GLsizei levels,
                  GLenum internalFormat, GLsizei width, GLsizei height,
                  GLsizei depth, GLuint memory, GLuint64 offset,
                  const char *func)
{
   struct gl_texture_object *texObj;
   struct gl_memory_object *memObj;

   GET_CURRENT_CONTEXT(ctx);

   if (!_mesa_has_EXT_memory_object(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (!_mesa_is_legal_tex_storage_target(ctx, dims, target)) {
      _mesa_error(ctx, GL_INVALID_ENUM,
                  "%s(illegal target=%s)",
                  func, _mesa_enum_to_string(target));
      return;
   }

   /* Check the format to make sure it is sized. */
   if (!_mesa_is_legal_tex_storage_format(ctx, internalFormat)) {
      _mesa_error(ctx, GL_INVALID_ENUM,
                  "%s(internalformat = %s)", func,
                  _mesa_enum_to_string(internalFormat));
      return;
   }

   texObj = _mesa_get_current_tex_object(ctx, target);
   if (!texObj)
      return;

   memObj = lookup_memory_object_err(ctx, memory, func);
   if (!memObj)
      return;

   _mesa_texture_storage_memory(ctx, dims, texObj, memObj, target,
                                levels, internalFormat,
                                width, height, depth, offset, false);
}

static void
texstorage_memory_ms(GLuint dims, GLenum target, GLsizei samples,
                     GLenum internalFormat, GLsizei width, GLsizei height,
                     GLsizei depth, GLboolean fixedSampleLocations,
                     GLuint memory, GLuint64 offset, const char* func)
{
   struct gl_texture_object *texObj;
   struct gl_memory_object *memObj;

   GET_CURRENT_CONTEXT(ctx);

   if (!_mesa_has_EXT_memory_object(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   texObj = _mesa_get_current_tex_object(ctx, target);
   if (!texObj)
      return;

   memObj = lookup_memory_object_err(ctx, memory, func);
   if (!memObj)
      return;

   _mesa_texture_storage_ms_memory(ctx, dims, texObj, memObj, target, samples,
                                   internalFormat, width, height, depth,
                                   fixedSampleLocations, offset, func);
}

/**
 * Helper used by _mesa_TextureStorageMem1/2/3DEXT().
 */
static void
texturestorage_memory(GLuint dims, GLuint texture, GLsizei levels,
                      GLenum internalFormat, GLsizei width, GLsizei height,
                      GLsizei depth, GLuint memory, GLuint64 offset,
                      const char *func)
{
   struct gl_texture_object *texObj;
   struct gl_memory_object *memObj;

   GET_CURRENT_CONTEXT(ctx);

   if (!_mesa_has_EXT_memory_object(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   /* Check the format to make sure it is sized. */
   if (!_mesa_is_legal_tex_storage_format(ctx, internalFormat)) {
      _mesa_error(ctx, GL_INVALID_ENUM,
                  "%s(internalformat = %s)", func,
                  _mesa_enum_to_string(internalFormat));
      return;
   }

   texObj = _mesa_lookup_texture(ctx, texture);
   if (!texObj)
      return;

   if (!_mesa_is_legal_tex_storage_target(ctx, dims, texObj->Target)) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "%s(illegal target=%s)", func,
                  _mesa_enum_to_string(texObj->Target));
      return;
   }

   memObj = lookup_memory_object_err(ctx, memory, func);
   if (!memObj)
      return;

   _mesa_texture_storage_memory(ctx, dims, texObj, memObj, texObj->Target,
                                levels, internalFormat,
                                width, height, depth, offset, true);
}

static void
texturestorage_memory_ms(GLuint dims, GLuint texture, GLsizei samples,
                         GLenum internalFormat, GLsizei width, GLsizei height,
                         GLsizei depth, GLboolean fixedSampleLocations,
                         GLuint memory, GLuint64 offset, const char* func)
{
   struct gl_texture_object *texObj;
   struct gl_memory_object *memObj;

   GET_CURRENT_CONTEXT(ctx);

   if (!_mesa_has_EXT_memory_object(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   texObj = _mesa_lookup_texture(ctx, texture);
   if (!texObj)
      return;

   memObj = lookup_memory_object_err(ctx, memory, func);
   if (!memObj)
      return;

   _mesa_texture_storage_ms_memory(ctx, dims, texObj, memObj, texObj->Target,
                                   samples, internalFormat, width, height,
                                   depth, fixedSampleLocations, offset, func);
}

void GLAPIENTRY
_mesa_TexStorageMem2DEXT(GLenum target,
                         GLsizei levels,
                         GLenum internalFormat,
                         GLsizei width,
                         GLsizei height,
                         GLuint memory,
                         GLuint64 offset)
{
   texstorage_memory(2, target, levels, internalFormat, width, height, 1,
                     memory, offset, "glTexStorageMem2DEXT");
}

void GLAPIENTRY
_mesa_TexStorageMem2DMultisampleEXT(GLenum target,
                                    GLsizei samples,
                                    GLenum internalFormat,
                                    GLsizei width,
                                    GLsizei height,
                                    GLboolean fixedSampleLocations,
                                    GLuint memory,
                                    GLuint64 offset)
{
   texstorage_memory_ms(2, target, samples, internalFormat, width, height, 1,
                        fixedSampleLocations, memory, offset,
                        "glTexStorageMem2DMultisampleEXT");
}

void GLAPIENTRY
_mesa_TexStorageMem3DEXT(GLenum target,
                         GLsizei levels,
                         GLenum internalFormat,
                         GLsizei width,
                         GLsizei height,
                         GLsizei depth,
                         GLuint memory,
                         GLuint64 offset)
{
   texstorage_memory(3, target, levels, internalFormat, width, height, depth,
                     memory, offset, "glTexStorageMem3DEXT");
}

void GLAPIENTRY
_mesa_TexStorageMem3DMultisampleEXT(GLenum target,
                                    GLsizei samples,
                                    GLenum internalFormat,
                                    GLsizei width,
                                    GLsizei height,
                                    GLsizei depth,
                                    GLboolean fixedSampleLocations,
                                    GLuint memory,
                                    GLuint64 offset)
{
   texstorage_memory_ms(3, target, samples, internalFormat, width, height,
                        depth, fixedSampleLocations, memory, offset,
                        "glTexStorageMem3DMultisampleEXT");
}

void GLAPIENTRY
_mesa_TextureStorageMem2DEXT(GLuint texture,
                             GLsizei levels,
                             GLenum internalFormat,
                             GLsizei width,
                             GLsizei height,
                             GLuint memory,
                             GLuint64 offset)
{
   texturestorage_memory(2, texture, levels, internalFormat, width, height, 1,
                         memory, offset, "glTexureStorageMem2DEXT");
}

void GLAPIENTRY
_mesa_TextureStorageMem2DMultisampleEXT(GLuint texture,
                                        GLsizei samples,
                                        GLenum internalFormat,
                                        GLsizei width,
                                        GLsizei height,
                                        GLboolean fixedSampleLocations,
                                        GLuint memory,
                                        GLuint64 offset)
{
   texturestorage_memory_ms(2, texture, samples, internalFormat, width, height,
                            1, fixedSampleLocations, memory, offset,
                            "glTextureStorageMem2DMultisampleEXT");
}

void GLAPIENTRY
_mesa_TextureStorageMem3DEXT(GLuint texture,
                             GLsizei levels,
                             GLenum internalFormat,
                             GLsizei width,
                             GLsizei height,
                             GLsizei depth,
                             GLuint memory,
                             GLuint64 offset)
{
   texturestorage_memory(3, texture, levels, internalFormat, width, height,
                         depth, memory, offset, "glTextureStorageMem3DEXT");
}

void GLAPIENTRY
_mesa_TextureStorageMem3DMultisampleEXT(GLuint texture,
                                        GLsizei samples,
                                        GLenum internalFormat,
                                        GLsizei width,
                                        GLsizei height,
                                        GLsizei depth,
                                        GLboolean fixedSampleLocations,
                                        GLuint memory,
                                        GLuint64 offset)
{
   texturestorage_memory_ms(3, texture, samples, internalFormat, width, height,
                            depth, fixedSampleLocations, memory, offset,
                            "glTextureStorageMem3DMultisampleEXT");
}

void GLAPIENTRY
_mesa_TexStorageMem1DEXT(GLenum target,
                         GLsizei levels,
                         GLenum internalFormat,
                         GLsizei width,
                         GLuint memory,
                         GLuint64 offset)
{
   texstorage_memory(1, target, levels, internalFormat, width, 1, 1, memory,
                     offset, "glTexStorageMem1DEXT");
}

void GLAPIENTRY
_mesa_TextureStorageMem1DEXT(GLuint texture,
                             GLsizei levels,
                             GLenum internalFormat,
                             GLsizei width,
                             GLuint memory,
                             GLuint64 offset)
{
   texturestorage_memory(1, texture, levels, internalFormat, width, 1, 1,
                         memory, offset, "glTextureStorageMem1DEXT");
}

static struct gl_semaphore_object *
semaphoreobj_alloc(struct gl_context *ctx, GLuint name)
{
   struct gl_semaphore_object *obj = CALLOC_STRUCT(gl_semaphore_object);
   if (!obj)
      return NULL;

   obj->Name = name;
   return obj;
}

static void
import_semaphoreobj_fd(struct gl_context *ctx,
                          struct gl_semaphore_object *semObj,
                          int fd)
{
   struct pipe_context *pipe = ctx->pipe;

   pipe->create_fence_fd(pipe, &semObj->fence, fd, semObj->type);

#if !defined(_WIN32)
   /* We own fd, but we no longer need it. So get rid of it */
   close(fd);
#endif
}

static void
import_semaphoreobj_win32(struct gl_context *ctx,
                          struct gl_semaphore_object *semObj,
                          void *handle,
                          const void *name,
                          enum pipe_fd_type type)
{
   struct pipe_context *pipe = ctx->pipe;

   pipe->screen->create_fence_win32(pipe->screen, &semObj->fence, handle, name, type);
}

static void
server_wait_semaphore(struct gl_context *ctx,
                      struct gl_semaphore_object *semObj,
                      GLuint numBufferBarriers,
                      struct gl_buffer_object **bufObjs,
                      GLuint numTextureBarriers,
                      struct gl_texture_object **texObjs,
                      const GLenum *srcLayouts)
{
   struct st_context *st = ctx->st;
   struct pipe_context *pipe = ctx->pipe;
   struct gl_buffer_object *bufObj;
   struct gl_texture_object *texObj;

   /* The driver is allowed to flush during fence_server_sync, be prepared */
   st_flush_bitmap_cache(st);
   pipe->fence_server_sync(pipe, semObj->fence, semObj->timeline_value);

   /**
    * According to the EXT_external_objects spec, the memory operations must
    * follow the wait. This is to make sure the flush is executed after the
    * other party is done modifying the memory.
    *
    * Relevant excerpt from section "4.2.3 Waiting for Semaphores":
    *
    * Following completion of the semaphore wait operation, memory will also be
    * made visible in the specified buffer and texture objects.
    *
    */
   for (unsigned i = 0; i < numBufferBarriers; i++) {
      if (!bufObjs[i])
         continue;

      bufObj = bufObjs[i];
      if (bufObj->buffer)
         pipe->flush_resource(pipe, bufObj->buffer);
   }

   for (unsigned i = 0; i < numTextureBarriers; i++) {
      if (!texObjs[i])
         continue;

      texObj = texObjs[i];
      if (texObj->pt)
         pipe->flush_resource(pipe, texObj->pt);
   }
}

static void
server_signal_semaphore(struct gl_context *ctx,
                        struct gl_semaphore_object *semObj,
                        GLuint numBufferBarriers,
                        struct gl_buffer_object **bufObjs,
                        GLuint numTextureBarriers,
                        struct gl_texture_object **texObjs,
                        const GLenum *dstLayouts)
{
   struct st_context *st = ctx->st;
   struct pipe_context *pipe = ctx->pipe;
   struct gl_buffer_object *bufObj;
   struct gl_texture_object *texObj;

   for (unsigned i = 0; i < numBufferBarriers; i++) {
      if (!bufObjs[i])
         continue;

      bufObj = bufObjs[i];
      if (bufObj->buffer)
         pipe->flush_resource(pipe, bufObj->buffer);
   }

   for (unsigned i = 0; i < numTextureBarriers; i++) {
      if (!texObjs[i])
         continue;

      texObj = texObjs[i];
      if (texObj->pt)
         pipe->flush_resource(pipe, texObj->pt);
   }

   /* The driver must flush during fence_server_signal, be prepared */
   st_flush_bitmap_cache(st);
   pipe->fence_server_signal(pipe, semObj->fence, semObj->timeline_value);
}

/**
 * Used as a placeholder for semaphore objects between glGenSemaphoresEXT()
 * and glImportSemaphoreFdEXT(), so that glIsSemaphoreEXT() can work correctly.
 */
static struct gl_semaphore_object DummySemaphoreObject = {
   .type = PIPE_FD_TYPE_SYNCOBJ,
};

static struct gl_semaphore_object *
create_real_semaphore(struct gl_context *ctx, GLuint semaphore, struct gl_semaphore_object *semObj, enum pipe_fd_type type, const char *func)
{
   if (semObj == &DummySemaphoreObject) {
      semObj = semaphoreobj_alloc(ctx, semaphore);
      if (!semObj) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "%s", func);
         return NULL;
      }
      _mesa_HashInsert(&ctx->Shared->SemaphoreObjects, semaphore, semObj);
   } else {
      assert(semObj->type == type);
   }
   semObj->type = type;
   return semObj;
}

/**
 * Delete a semaphore object.
 * Not removed from hash table here.
 */
void
_mesa_delete_semaphore_object(struct gl_context *ctx,
                              struct gl_semaphore_object *semObj)
{
   if (semObj != &DummySemaphoreObject) {
      struct pipe_context *pipe = ctx->pipe;
      pipe->screen->fence_reference(ctx->screen, &semObj->fence, NULL);
      FREE(semObj);
   }
}

static void
create_semaphores(GLsizei n, GLuint *semaphores, bool NV)
{
   GET_CURRENT_CONTEXT(ctx);

   const char *func = NV ? "glCreateSemaphoresNV" : "glGenSemaphoresEXT";

   if (MESA_VERBOSE & (VERBOSE_API))
      _mesa_debug(ctx, "%s(%d, %p)\n", func, n, semaphores);

   if (NV ? !_mesa_has_NV_timeline_semaphore(ctx) : !_mesa_has_EXT_semaphore(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (n < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(n < 0)", func);
      return;
   }

   if (!semaphores)
      return;

   _mesa_HashLockMutex(&ctx->Shared->SemaphoreObjects);
   if (_mesa_HashFindFreeKeys(&ctx->Shared->SemaphoreObjects, semaphores, n)) {
      for (GLsizei i = 0; i < n; i++) {
         _mesa_HashInsertLocked(&ctx->Shared->SemaphoreObjects, semaphores[i],
                                &DummySemaphoreObject);
      }
   }

   _mesa_HashUnlockMutex(&ctx->Shared->SemaphoreObjects);
}

void GLAPIENTRY
_mesa_GenSemaphoresEXT(GLsizei n, GLuint *semaphores)
{
   create_semaphores(n, semaphores, false);
}

void GLAPIENTRY
_mesa_CreateSemaphoresNV(GLsizei n, GLuint *semaphores)
{
   create_semaphores(n, semaphores, true);
}

void GLAPIENTRY
_mesa_DeleteSemaphoresEXT(GLsizei n, const GLuint *semaphores)
{
   GET_CURRENT_CONTEXT(ctx);

   const char *func = "glDeleteSemaphoresEXT";

   if (MESA_VERBOSE & (VERBOSE_API)) {
      _mesa_debug(ctx, "%s(%d, %p)\n", func, n, semaphores);
   }

   if (!_mesa_has_EXT_semaphore(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (n < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(n < 0)", func);
      return;
   }

   if (!semaphores)
      return;

   _mesa_HashLockMutex(&ctx->Shared->SemaphoreObjects);
   for (GLint i = 0; i < n; i++) {
      if (semaphores[i] > 0) {
         struct gl_semaphore_object *delObj
            = _mesa_lookup_semaphore_object_locked(ctx, semaphores[i]);

         if (delObj) {
            _mesa_HashRemoveLocked(&ctx->Shared->SemaphoreObjects,
                                   semaphores[i]);
            _mesa_delete_semaphore_object(ctx, delObj);
         }
      }
   }
   _mesa_HashUnlockMutex(&ctx->Shared->SemaphoreObjects);
}

GLboolean GLAPIENTRY
_mesa_IsSemaphoreEXT(GLuint semaphore)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!_mesa_has_EXT_semaphore(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "glIsSemaphoreEXT(unsupported)");
      return GL_FALSE;
   }

   struct gl_semaphore_object *obj =
      _mesa_lookup_semaphore_object(ctx, semaphore);

   return obj ? GL_TRUE : GL_FALSE;
}

/**
 * Helper that outputs the correct error status for parameter
 * calls where no pnames are defined
 */
static void
semaphore_parameter_stub(const char* func, GLenum pname)
{
}

void GLAPIENTRY
_mesa_SemaphoreParameterui64vEXT(GLuint semaphore,
                                 GLenum pname,
                                 const GLuint64 *params)
{
   GET_CURRENT_CONTEXT(ctx);
   const char *func = "glSemaphoreParameterui64vEXT";

   if (!_mesa_has_EXT_semaphore(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (!_mesa_has_NV_timeline_semaphore(ctx) && pname == GL_TIMELINE_SEMAPHORE_VALUE_NV) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(NV_timeline_semaphore unsupported)", func);
      return;
   }

   if (pname != GL_D3D12_FENCE_VALUE_EXT && pname != GL_TIMELINE_SEMAPHORE_VALUE_NV) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(pname=0x%x)", func, pname);
      return;
   }

   struct gl_semaphore_object *semObj = _mesa_lookup_semaphore_object(ctx,
                                                                      semaphore);
   if (!semObj)
      return;

   if (semObj->type < PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(Not a %s)", func, pname == GL_TIMELINE_SEMAPHORE_VALUE_NV ? "timeline semaphore" : "D3D12 fence");
      return;
   }
   if ((semObj->type == PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12 && pname != GL_D3D12_FENCE_VALUE_EXT) ||
       (semObj->type == PIPE_FD_TYPE_TIMELINE_SEMAPHORE_VK && pname != GL_TIMELINE_SEMAPHORE_VALUE_NV)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(Not a %s)", func, pname == GL_TIMELINE_SEMAPHORE_VALUE_NV ? "timeline semaphore" : "D3D12 fence");
      return;
   }

   semObj->timeline_value = params[0];
}

void GLAPIENTRY
_mesa_GetSemaphoreParameterui64vEXT(GLuint semaphore,
                                    GLenum pname,
                                    GLuint64 *params)
{
   GET_CURRENT_CONTEXT(ctx);
   const char *func = "glGetSemaphoreParameterui64vEXT";

   if (!_mesa_has_EXT_semaphore(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (!_mesa_has_NV_timeline_semaphore(ctx) && pname == GL_TIMELINE_SEMAPHORE_VALUE_NV) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(NV_timeline_semaphore unsupported)", func);
      return;
   }

   if (pname != GL_D3D12_FENCE_VALUE_EXT && pname != GL_TIMELINE_SEMAPHORE_VALUE_NV) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(pname=0x%x)", func, pname);
      return;
   }
   struct gl_semaphore_object *semObj = _mesa_lookup_semaphore_object(ctx,
                                                                      semaphore);
   if (!semObj)
      return;

   if (semObj->type < PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(Not a %s)", func, pname == GL_TIMELINE_SEMAPHORE_VALUE_NV ? "timeline semaphore" : "D3D12 fence");
      return;
   }

   params[0] = semObj->timeline_value;
}

void GLAPIENTRY
_mesa_GetSemaphoreParameterivNV(GLuint semaphore,
                                GLenum pname,
                                GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   const char *func = "glGetSemaphoreParameterivNV";

   if (!_mesa_has_NV_timeline_semaphore(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (pname != GL_SEMAPHORE_TYPE_NV) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(pname=0x%x)", func, pname);
      return;
   }

   struct gl_semaphore_object *semObj = _mesa_lookup_semaphore_object(ctx,
                                                                      semaphore);
   if (!semObj)
      return;

   params[0] = semObj->type == PIPE_FD_TYPE_TIMELINE_SEMAPHORE_VK ?
               GL_TIMELINE_SEMAPHORE_VALUE_NV :
               GL_SEMAPHORE_TYPE_BINARY_NV;
}

void GLAPIENTRY
_mesa_SemaphoreParameterivNV(GLuint semaphore,
                             GLenum pname,
                             const GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   const char *func = "glSemaphoreParameterivNV";

   if (!_mesa_has_NV_timeline_semaphore(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (pname != GL_SEMAPHORE_TYPE_NV) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(pname=0x%x)", func, pname);
      return;
   }

   if (params[0] != GL_SEMAPHORE_TYPE_BINARY_NV && params[0] != GL_SEMAPHORE_TYPE_TIMELINE_NV) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(pname=0x%x)", func, pname);
      return;
   }

   struct gl_semaphore_object *semObj = _mesa_lookup_semaphore_object(ctx,
                                                                      semaphore);
   if (!semObj)
      return;

   enum pipe_fd_type type = params[0] == GL_SEMAPHORE_TYPE_TIMELINE_NV ? PIPE_FD_TYPE_TIMELINE_SEMAPHORE_VK : PIPE_FD_TYPE_SYNCOBJ;
   create_real_semaphore(ctx, semaphore, semObj, type, func);
}

void GLAPIENTRY
_mesa_WaitSemaphoreEXT(GLuint semaphore,
                       GLuint numBufferBarriers,
                       const GLuint *buffers,
                       GLuint numTextureBarriers,
                       const GLuint *textures,
                       const GLenum *srcLayouts)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_semaphore_object *semObj = NULL;
   struct gl_buffer_object **bufObjs = NULL;
   struct gl_texture_object **texObjs = NULL;

   const char *func = "glWaitSemaphoreEXT";

   if (!_mesa_has_EXT_semaphore(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   ASSERT_OUTSIDE_BEGIN_END(ctx);

   semObj = _mesa_lookup_semaphore_object(ctx, semaphore);
   if (!semObj)
      return;

   FLUSH_VERTICES(ctx, 0, 0);

   bufObjs = malloc(sizeof(struct gl_buffer_object *) * numBufferBarriers);
   if (!bufObjs) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "%s(numBufferBarriers=%u)",
                  func, numBufferBarriers);
      goto end;
   }

   for (unsigned i = 0; i < numBufferBarriers; i++) {
      bufObjs[i] = _mesa_lookup_bufferobj(ctx, buffers[i]);
   }

   texObjs = malloc(sizeof(struct gl_texture_object *) * numTextureBarriers);
   if (!texObjs) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "%s(numTextureBarriers=%u)",
                  func, numTextureBarriers);
      goto end;
   }

   for (unsigned i = 0; i < numTextureBarriers; i++) {
      texObjs[i] = _mesa_lookup_texture(ctx, textures[i]);
   }

   server_wait_semaphore(ctx, semObj,
                         numBufferBarriers, bufObjs,
                         numTextureBarriers, texObjs,
                         srcLayouts);

end:
   free(bufObjs);
   free(texObjs);
}

void GLAPIENTRY
_mesa_SignalSemaphoreEXT(GLuint semaphore,
                         GLuint numBufferBarriers,
                         const GLuint *buffers,
                         GLuint numTextureBarriers,
                         const GLuint *textures,
                         const GLenum *dstLayouts)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_semaphore_object *semObj = NULL;
   struct gl_buffer_object **bufObjs = NULL;
   struct gl_texture_object **texObjs = NULL;

   const char *func = "glSignalSemaphoreEXT";

   if (!_mesa_has_EXT_semaphore(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   ASSERT_OUTSIDE_BEGIN_END(ctx);

   semObj = _mesa_lookup_semaphore_object(ctx, semaphore);
   if (!semObj)
      return;

   FLUSH_VERTICES(ctx, 0, 0);

   bufObjs = malloc(sizeof(struct gl_buffer_object *) * numBufferBarriers);
   if (!bufObjs) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "%s(numBufferBarriers=%u)",
                  func, numBufferBarriers);
      goto end;
   }

   for (unsigned i = 0; i < numBufferBarriers; i++) {
      bufObjs[i] = _mesa_lookup_bufferobj(ctx, buffers[i]);
   }

   texObjs = malloc(sizeof(struct gl_texture_object *) * numTextureBarriers);
   if (!texObjs) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "%s(numTextureBarriers=%u)",
                  func, numTextureBarriers);
      goto end;
   }

   for (unsigned i = 0; i < numTextureBarriers; i++) {
      texObjs[i] = _mesa_lookup_texture(ctx, textures[i]);
   }

   server_signal_semaphore(ctx, semObj,
                           numBufferBarriers, bufObjs,
                           numTextureBarriers, texObjs,
                           dstLayouts);

end:
   free(bufObjs);
   free(texObjs);
}

void GLAPIENTRY
_mesa_ImportMemoryFdEXT(GLuint memory,
                        GLuint64 size,
                        GLenum handleType,
                        GLint fd)
{
   GET_CURRENT_CONTEXT(ctx);

   const char *func = "glImportMemoryFdEXT";

   if (!_mesa_has_EXT_memory_object_fd(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (handleType != GL_HANDLE_TYPE_OPAQUE_FD_EXT) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(handleType=%u)", func, handleType);
      return;
   }

   struct gl_memory_object *memObj = _mesa_lookup_memory_object(ctx, memory);
   if (!memObj)
      return;

   import_memoryobj_fd(ctx, memObj, size, fd);
   memObj->Immutable = GL_TRUE;
}

void GLAPIENTRY
_mesa_ImportMemoryWin32HandleEXT(GLuint memory,
                                 GLuint64 size,
                                 GLenum handleType,
                                 void *handle)
{
   GET_CURRENT_CONTEXT(ctx);

   const char *func = "glImportMemoryWin32HandleEXT";

   if (!_mesa_has_EXT_memory_object_win32(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (handleType != GL_HANDLE_TYPE_OPAQUE_WIN32_EXT &&
       handleType != GL_HANDLE_TYPE_D3D11_IMAGE_EXT &&
       handleType != GL_HANDLE_TYPE_D3D12_RESOURCE_EXT &&
       handleType != GL_HANDLE_TYPE_D3D12_TILEPOOL_EXT) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(handleType=%u)", func, handleType);
      return;
   }

   struct gl_memory_object *memObj = _mesa_lookup_memory_object(ctx, memory);
   if (!memObj)
      return;

   import_memoryobj_win32(ctx, memObj, size, handle, NULL);
   memObj->Immutable = GL_TRUE;
}

void GLAPIENTRY
_mesa_ImportMemoryWin32NameEXT(GLuint memory,
                                 GLuint64 size,
                                 GLenum handleType,
                                 const void *name)
{
   GET_CURRENT_CONTEXT(ctx);

   const char *func = "glImportMemoryWin32NameEXT";

   if (!_mesa_has_EXT_memory_object_win32(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (handleType != GL_HANDLE_TYPE_OPAQUE_WIN32_EXT &&
       handleType != GL_HANDLE_TYPE_D3D11_IMAGE_EXT &&
       handleType != GL_HANDLE_TYPE_D3D12_RESOURCE_EXT &&
       handleType != GL_HANDLE_TYPE_D3D12_TILEPOOL_EXT) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(handleType=%u)", func, handleType);
      return;
   }

   struct gl_memory_object *memObj = _mesa_lookup_memory_object(ctx, memory);
   if (!memObj)
      return;

   import_memoryobj_win32(ctx, memObj, size, NULL, name);
   memObj->Immutable = GL_TRUE;
}

void GLAPIENTRY
_mesa_ImportSemaphoreFdEXT(GLuint semaphore,
                           GLenum handleType,
                           GLint fd)
{
   GET_CURRENT_CONTEXT(ctx);

   const char *func = "glImportSemaphoreFdEXT";

   if (!_mesa_has_EXT_semaphore_fd(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (handleType != GL_HANDLE_TYPE_OPAQUE_FD_EXT) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(handleType=%u)", func, handleType);
      return;
   }

   struct gl_semaphore_object *semObj = _mesa_lookup_semaphore_object(ctx,
                                                                      semaphore);
   if (!semObj)
      return;

   semObj = create_real_semaphore(ctx, semaphore, semObj, semObj->type, func);
   if (!semObj)
      return;

   import_semaphoreobj_fd(ctx, semObj, fd);
}

void GLAPIENTRY
_mesa_ImportSemaphoreWin32HandleEXT(GLuint semaphore,
                           GLenum handleType,
                           void *handle)
{
   GET_CURRENT_CONTEXT(ctx);

   const char *func = "glImportSemaphoreWin32HandleEXT";

   if (!_mesa_has_EXT_semaphore_win32(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (handleType != GL_HANDLE_TYPE_OPAQUE_WIN32_EXT &&
       handleType != GL_HANDLE_TYPE_D3D12_FENCE_EXT) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(handleType=%u)", func, handleType);
      return;
   }

   if (handleType == GL_HANDLE_TYPE_D3D12_FENCE_EXT &&
       !ctx->screen->caps.timeline_semaphore_import) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(handleType=%u)", func, handleType);
   }

   struct gl_semaphore_object *semObj = _mesa_lookup_semaphore_object(ctx,
                                                                      semaphore);
   if (!semObj)
      return;

   enum pipe_fd_type type = handleType == GL_HANDLE_TYPE_D3D12_FENCE_EXT || handleType == GL_SEMAPHORE_TYPE_TIMELINE_NV ?
      PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12 : semObj->type;
   semObj = create_real_semaphore(ctx, semaphore, semObj, type, func);
   if (!semObj)
      return;

   import_semaphoreobj_win32(ctx, semObj, handle, NULL, type);
}

void GLAPIENTRY
_mesa_ImportSemaphoreWin32NameEXT(GLuint semaphore,
                                  GLenum handleType,
                                  const void *name)
{
   GET_CURRENT_CONTEXT(ctx);

   const char *func = "glImportSemaphoreWin32HandleEXT";

   if (!_mesa_has_EXT_semaphore_win32(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(unsupported)", func);
      return;
   }

   if (handleType != GL_HANDLE_TYPE_OPAQUE_WIN32_EXT &&
       handleType != GL_HANDLE_TYPE_D3D12_FENCE_EXT) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(handleType=%u)", func, handleType);
      return;
   }

   if (handleType == GL_HANDLE_TYPE_D3D12_FENCE_EXT &&
       !ctx->screen->caps.timeline_semaphore_import) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(handleType=%u)", func, handleType);
   }

   struct gl_semaphore_object *semObj = _mesa_lookup_semaphore_object(ctx,
                                                                      semaphore);
   if (!semObj)
      return;

   enum pipe_fd_type type = handleType == GL_HANDLE_TYPE_D3D12_FENCE_EXT || handleType == GL_SEMAPHORE_TYPE_TIMELINE_NV ?
      PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12 : semObj->type;
   semObj = create_real_semaphore(ctx, semaphore, semObj, type, func);
   if (!semObj)
      return;

   import_semaphoreobj_win32(ctx, semObj, NULL, name, type);
}
