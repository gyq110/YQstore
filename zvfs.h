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

#define ENABLE_HOOK 1
#define BUFFER_SIZE 512

#define YQ_MAX_FD 2048
extern struct spdk_thread *global_thread;

typedef struct zvfs_context_s
{
    struct spdk_blob_store *blobstore;
    spdk_blob_id blob_id;
    struct spdk_blob *blob;
    uint64_t free_cluster_count;
    uint64_t num_cluster;
    uint64_t unit_size;
    uint8_t *write_buffer;
    uint8_t *read_buffer;
    struct spdk_io_channel *channel;
} zvfs_context_t;

#define FILENAME_LENGTH 128

typedef struct zvfs_s
{
    struct spdk_blob_store *blobstore;
    uint64_t unit_size;
    uint64_t free_cluster_count;
    uint64_t num_cluster;
    struct spdk_io_channel *channel;
    bool finished;
} zvfs_t;

#if !ENABLE_HOOK
typedef struct zvfs_operation_s
{
    int (*mount)(struct zvfs_s *fs);
    int (*unmount)(struct zvfs_s *fs);

} zvfs_operation_t;

#endif
typedef struct zvfs_file_s
{
#if ENABLE_HOOK
    int fd;
#endif

    uint8_t filename[FILENAME_LENGTH];
    spdk_blob_id blob_id;
    struct spdk_blob *blob;
    zvfs_t *fs;
    uint8_t *write_buffer;
    uint8_t *read_buffer;
    bool finished;

} zvfs_file_t;
#if !ENABLE_HOOK

typedef struct zvfs_file_operation_s
{
    int (*open)(struct zvfs_file_s *fs);
    int (*write)(struct zvfs_file_s *fs);
    int (*read)(struct zvfs_file_s *fs);
    int (*close)(struct zvfs_file_s *fs);

} zvfs_file_operation_t;

#endif
extern struct zvfs_file_s *file_table[YQ_MAX_FD];
#if ENABLE_HOOK

// 四个接口的声明
int nopen(const char *pathname, int flags);
ssize_t nread(int fd, void *buf, size_t count);
ssize_t nwrite(int fd, const void *buf, size_t count);
int nclose(int fd);
#endif

#endif