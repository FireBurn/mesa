/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright © 2014 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <archive.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <archive_entry.h>

#include "io.h"

struct io {
   struct archive *a;
   struct archive_entry *entry;
   unsigned offset;
};

static void
io_error(struct io *io)
{
   fprintf(stderr, "%s\n", archive_error_string(io->a));
   io_close(io);
}

static struct io *
io_new(void)
{
   struct io *io = calloc(1, sizeof(*io));
   int ret;

   if (!io)
      return NULL;

   io->a = archive_read_new();
   ret = archive_read_support_filter_gzip(io->a);
   if (ret != ARCHIVE_OK) {
      io_error(io);
      return NULL;
   }

   ret = archive_read_support_filter_none(io->a);
   if (ret != ARCHIVE_OK) {
      io_error(io);
      return NULL;
   }

   ret = archive_read_support_format_all(io->a);
   if (ret != ARCHIVE_OK) {
      io_error(io);
      return NULL;
   }

   ret = archive_read_support_format_raw(io->a);
   if (ret != ARCHIVE_OK) {
      io_error(io);
      return NULL;
   }

   return io;
}

struct io *
io_open(const char *filename)
{
   struct io *io = io_new();
   int ret;

   if (!io)
      return NULL;

   ret = archive_read_open_filename(io->a, filename, 10240);
   if (ret != ARCHIVE_OK) {
      io_error(io);
      return NULL;
   }

   ret = archive_read_next_header(io->a, &io->entry);
   if (ret != ARCHIVE_OK) {
      io_error(io);
      return NULL;
   }

   return io;
}

struct io *
io_openfd(int fd)
{
   struct io *io = io_new();
   int ret;

   if (!io)
      return NULL;

   ret = archive_read_open_fd(io->a, fd, 10240);
   if (ret != ARCHIVE_OK) {
      io_error(io);
      return NULL;
   }

   ret = archive_read_next_header(io->a, &io->entry);
   if (ret != ARCHIVE_OK) {
      io_error(io);
      return NULL;
   }

   return io;
}

void
io_close(struct io *io)
{
   archive_read_free(io->a);
   free(io);
}

unsigned
io_offset(struct io *io)
{
   return io->offset;
}

#include <assert.h>
int
io_readn(struct io *io, void *buf, int nbytes)
{
   char *ptr = buf;
   int ret = 0;
   while (nbytes > 0) {
      int n = archive_read_data(io->a, ptr, nbytes);
      if (n < 0) {
         fprintf(stderr, "%s\n", archive_error_string(io->a));
         return n;
      }
      if (n == 0)
         break;
      ptr += n;
      nbytes -= n;
      ret += n;
      io->offset += n;
   }
   return ret;
}
