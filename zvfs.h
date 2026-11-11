#ifndef __YQ_COMMON_H__
#define __YQ_COMMON_H__

#include <spdk/bdev.h>
#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <stdio.h>
#include <spdk/init.h>
#include <spdk/file.h>
#include <spdk/rpc.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <rte_hash.h>
#include <rte_lcore.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>
#include<sys/stat.h>
#include"inode.h"
#include<sys/statvfs.h>
#define BUFFER_SIZE (128U*1024U)
extern struct spdk_thread *global_thread;


extern zvfs_t *fs;
extern struct zvfs_file_s *file_table[YQ_MAX_FD];

// 四个接口的声明
int nopen(const char *pathname, int flags);
ssize_t nread(int fd, void *buf, size_t count);
ssize_t nwrite(int fd, const void *buf, size_t count);
int nclose(int fd);

off64_t nlseek(int fd, off64_t offset, int whence);
int nstat(const char *pathname, struct stat64 *st);
ssize_t npread(int fd, void *buf, size_t count, off64_t offset);
ssize_t npwrite(int fd, const void *buf, size_t count, off64_t offset);
int nmkdir(const char *pathname, mode_t mode);
int nftruncate(int fd, off_t length);
int nunlink(const char *pathname);
bool zvfs_reclaim_inode(zvfs_inode_t *inode);
int nrmdir(const char *pathname);
int nrename(const char *oldpath, const char *newpath);
int nstatvfs(const char *pathname, struct statvfs *buf);
zvfs_inode_t *zvfs_lookup_dir(const char *pathname, zvfs_t **out_fs);

#endif