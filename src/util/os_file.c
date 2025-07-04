/*
 * Copyright 2019 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "os_file.h"
#include "detect_os.h"
#include "util/detect.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

#if DETECT_OS_WINDOWS
#include <direct.h>
#include <io.h>
#define open _open
#define fdopen _fdopen
#define close _close
#define dup _dup
#define read _read
#define O_CREAT _O_CREAT
#define O_EXCL _O_EXCL
#define O_WRONLY _O_WRONLY
#define mkdir(dir, mode) _mkdir(dir)
#else
#include <unistd.h>
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif
#endif


FILE *
os_file_create_unique(const char *filename, int filemode)
{
   int fd = open(filename, O_CREAT | O_EXCL | O_WRONLY, filemode);
   if (fd == -1)
      return NULL;
   return fdopen(fd, "w");
}


#if DETECT_OS_WINDOWS
int
os_dupfd_cloexec(int fd)
{
   /*
    * On Windows child processes don't inherit handles by default:
    * https://devblogs.microsoft.com/oldnewthing/20111216-00/?p=8873
    */
   return dup(fd);
}
#else
int
os_dupfd_cloexec(int fd)
{
   int minfd = 3;
   int newfd = fcntl(fd, F_DUPFD_CLOEXEC, minfd);

   if (newfd >= 0)
      return newfd;

   if (errno != EINVAL)
      return -1;

   newfd = fcntl(fd, F_DUPFD, minfd);

   if (newfd < 0)
      return -1;

   long flags = fcntl(newfd, F_GETFD);
   if (flags == -1) {
      close(newfd);
      return -1;
   }

   if (fcntl(newfd, F_SETFD, flags | FD_CLOEXEC) == -1) {
      close(newfd);
      return -1;
   }

   return newfd;
}
#endif

#include <fcntl.h>
#include <sys/stat.h>

#if DETECT_OS_WINDOWS
typedef ptrdiff_t ssize_t;
#endif

static ssize_t
readN(int fd, char *buf, size_t len)
{
   /* err was initially set to -ENODATA but in some BSD systems
    * ENODATA is not defined and ENOATTR is used instead.
    * As err is not returned by any function it can be initialized
    * to -EFAULT that exists everywhere.
    */
   int err = -EFAULT;
   size_t total = 0;
   do {
      ssize_t ret = read(fd, buf + total, len - total);

      if (ret < 0)
         ret = -errno;

      if (ret == -EINTR || ret == -EAGAIN)
         continue;

      if (ret <= 0) {
         err = ret;
         break;
      }

      total += ret;
   } while (total != len);

   return total ? (ssize_t)total : err;
}

#ifndef O_BINARY
/* Unix makes no distinction between text and binary files. */
#define O_BINARY 0
#endif

char *
os_read_file(const char *filename, size_t *size)
{
   /* Note that this also serves as a slight margin to avoid a 2x grow when
    * the file is just a few bytes larger when we read it than when we
    * fstat'ed it.
    * The string's NULL terminator is also included in here.
    */
   size_t len = 64;

   int fd = open(filename, O_RDONLY | O_BINARY);
   if (fd == -1) {
      /* errno set by open() */
      return NULL;
   }

   /* Pre-allocate a buffer at least the size of the file if we can read
    * that information.
    */
   struct stat stat;
   if (fstat(fd, &stat) == 0)
      len += stat.st_size;

   char *buf = malloc(len);
   if (!buf) {
      close(fd);
      errno = -ENOMEM;
      return NULL;
   }

   ssize_t actually_read;
   size_t offset = 0, remaining = len - 1;
   while ((actually_read = readN(fd, buf + offset, remaining)) == (ssize_t)remaining) {
      char *newbuf = realloc(buf, 2 * len);
      if (!newbuf) {
         free(buf);
         close(fd);
         errno = -ENOMEM;
         return NULL;
      }

      buf = newbuf;
      len *= 2;
      offset += actually_read;
      remaining = len - offset - 1;
   }

   close(fd);

   if (actually_read > 0)
      offset += actually_read;

   /* Final resize to actual size */
   len = offset + 1;
   char *newbuf = realloc(buf, len);
   if (!newbuf) {
      free(buf);
      errno = -ENOMEM;
      return NULL;
   }
   buf = newbuf;

   buf[offset] = '\0';

   if (size)
      *size = offset;

   return buf;
}

#if (DETECT_OS_LINUX || DETECT_OS_FREEBSD) && ALLOW_KCMP

#include <sys/syscall.h>
#include <unistd.h>

#if DETECT_OS_LINUX
/* copied from <linux/kcmp.h> */
#define KCMP_FILE 0
#endif

#endif

#if DETECT_OS_DRAGONFLY || DETECT_OS_FREEBSD

#include "macros.h" /* ARRAY_SIZE */

#include <sys/sysctl.h>
#if DETECT_OS_DRAGONFLY
#include <sys/kinfo.h>
typedef void *kvaddr_t;
#elif DETECT_OS_FREEBSD
#include <sys/file.h>
#define kinfo_file xfile
#define f_pid xf_pid
#define f_fd xf_fd
#define f_file xf_file
#endif

#endif /* DETECT_OS_DRAGONFLY || DETECT_OS_FREEBSD */

int
os_same_file_description(int fd1, int fd2)
{
#ifdef SYS_kcmp
   pid_t pid = getpid();
#endif

   /* Same file descriptor trivially implies same file description */
   if (fd1 == fd2)
      return 0;

#ifdef SYS_kcmp
   return syscall(SYS_kcmp, pid, pid, KCMP_FILE, fd1, fd2);
#elif DETECT_OS_DRAGONFLY || DETECT_OS_FREEBSD
   int mib[] = { CTL_KERN, KERN_FILE };
   size_t len;
   if (sysctl(mib, ARRAY_SIZE(mib), NULL, &len, NULL, 0))
      return -1;
   struct kinfo_file *kf = malloc(len);
   if (sysctl(mib, ARRAY_SIZE(mib), kf, &len, NULL, 0))
      return -1;

   size_t count = len / sizeof(*kf);
   pid_t pid = getpid();
   kvaddr_t fd1_kfile = 0, fd2_kfile = 0;
   for (size_t i = 0; i < count; i++) {
      if (pid != kf[i].f_pid)
          continue;
      if (fd1 == kf[i].f_fd)
          fd1_kfile = kf[i].f_file;
      if (fd2 == kf[i].f_fd)
          fd2_kfile = kf[i].f_file;
   }
   free(kf);

   if (fd1_kfile == 0 || fd2_kfile == 0)
      return -1;

   return (fd1_kfile < fd2_kfile) | ((fd1_kfile > fd2_kfile) << 1);
#else
   /* Otherwise we can't tell */
   return -1;
#endif
}


int
os_mkdir(const char *pathname, int mode)
{
#if DETECT_OS_WINDOWS
   return _mkdir(pathname);
#else
   return mkdir(pathname, mode);
#endif
}
