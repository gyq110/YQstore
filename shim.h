#ifndef __YQ_SHIM_H__
#define __YQ_SHIM_H__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <stdarg.h>
#include "zvfs.h"

int open(const char *pathname, int flags, ...);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);

#if ENABLE_FIO
off64_t lseek64(int fd, off64_t offset, int whence);
int stat64(const char *pathname, struct stat64 *st);
ssize_t pread64(int fd, void *buf, size_t count, off64_t offset);
ssize_t pwrite64(int fd, const void *buf, size_t count, off64_t offset);
#endif
#endif