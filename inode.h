#ifndef __YQ_INODE_H__
#define __YQ_INODE_H__
//这个文件用来实现我们文件目录的管理和实现
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <spdk/blob.h>


#define YQ_MAX_FILENAME 255
#define YQ_SPDK_ROOT_DIR "/"
#define YQ_MOUNT_PREFIX "/zvfs"
#define YQ_MAX_FD 2048


typedef struct zvfs_s zvfs_t;
typedef struct zvfs_inode_s zvfs_inode_t;
typedef struct zvfs_file_s zvfs_file_t;

struct zvfs_s
{
    //这个是文件系统的信息，和之前的一样
    struct spdk_blob_store *blobstore;
    struct spdk_blob *inode_table_blob;
    struct spdk_io_channel *channel;
    uint64_t io_unit_size;
    uint64_t cluster_size;
    zvfs_inode_t *root;
    uint64_t next_ino;
    uint64_t inode_count;
    zvfs_inode_t **inode_table;
    bool finished;
};

struct zvfs_inode_s
{
    //这个是一个文件本身的属性
    uint64_t ino;//这个是节点编号
    mode_t mode;//这个存的的是文件的类型还有文件的权限等信息
    off_t size;//存的文件的实际长度
    spdk_blob_id blob_id;
    char filename[YQ_MAX_FILENAME + 1];
    uint64_t parent_ino;
    uint64_t next_sibling_ino;
    uint64_t first_child_ino;

};

struct zvfs_file_s
{
    zvfs_inode_t *inode;
    struct spdk_blob *blob;
    off_t offset;
    int flags;
    zvfs_t *fs;
    bool stream_eof;
    bool stream_error;
    uint8_t *read_buffer;
    uint8_t *write_buffer;
    bool finished;
};
struct zvfs_file_s *yq_get_filefromfd(int fd);
void yq_destory_fd(int fd);
int yq_get_fd_fromlinux();
void zvfs_inode_io_done(void *arg, int bserrno);
void zvfs_inode_io_wait(bool *done);
zvfs_inode_t *zvfs_inode_create(zvfs_t *fs, zvfs_inode_t *parent, const char *filename, mode_t mode, spdk_blob_id blod_id);
void zvfs_inode_table_load(zvfs_t *fs);
void zvfs_inode_table_save(zvfs_t *fs);
zvfs_inode_t *zvfs_inode_lookup(zvfs_t *fs, zvfs_inode_t **start, const char *pathname);
#endif