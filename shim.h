#ifndef __YQ_SHIM_H__
#define __YQ_SHIM_H__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <stdarg.h>
#include "zvfs.h"
#include"inode.h"
#include <sys/vfs.h>
int open(const char *pathname, int flags, ...);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
int mkdir(const char *pathname, mode_t mode);

off64_t lseek64(int fd, off64_t offset, int whence);
int stat64(const char *pathname, struct stat64 *st);
ssize_t pread64(int fd, void *buf, size_t count, off64_t offset);
ssize_t pwrite64(int fd, const void *buf, size_t count, off64_t offset);
DIR *opendir(const char *pathname);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
int stat(const char *pathname, struct stat *st);
int fstat(int fd, struct stat *st);
int access(const char *pathname, int mode);
int fstatfs(int fd, struct statfs *buf);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
off_t lseek(int fd, off_t offset, int whence);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
int ftruncate(int fd, off_t length);
int unlink(const char *pathname);
int rmdir(const char *pathname);
int rename(const char *oldpath, const char *newpath);
int statvfs(const char *pathname, struct statvfs *buf);
FILE *fdopen(int fd, const char *mode);
int fallocate(int fd, int mode, off_t offset, off_t length);
int sync_file_range(int fd, off64_t offset, off64_t length, unsigned int flags);
int fsync(int fd);
int fdatasync(int fd);
int fcntl(int fd, int cmd, ...);

#endif
