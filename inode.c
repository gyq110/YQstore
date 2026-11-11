#include"inode.h"
#include"zvfs.h"
#include<string.h>
struct zvfs_file_s *file_table[YQ_MAX_FD] = {0};
struct spdk_thread *global_thread = NULL;
void yq_destory_fd(int fd)
{
    // 把这个fd销毁
    if (fd < 0 || fd >= YQ_MAX_FD)
    {
        SPDK_NOTICELOG("yq_destory_fd:fd error %d", fd);
        return;
    }
    syscall(SYS_close, fd);
}

int yq_get_fd_fromlinux()
{
    // 这里就是从linux获取一个fd
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0)
    {
        return -1;
    }
    if (fd >= YQ_MAX_FD)
    {
        syscall(SYS_close, fd);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

struct zvfs_file_s *yq_get_filefromfd(int fd)
{
    return file_table[fd];
}


//这个函数的作用是创建一个inode并挂载在目录树上面，在mount和以后创建文件或者文件夹的时候复用
zvfs_inode_t *zvfs_inode_create(zvfs_t *fs, zvfs_inode_t *parent, const char *filename, mode_t mode, spdk_blob_id blod_id)
{
    uint64_t ino = fs->next_ino++;
    zvfs_inode_t *inode = calloc(1, sizeof(*inode));
    inode->ino = ino;
    inode->mode = mode;
    inode->blob_id = blod_id;
    strcpy(inode->filename, filename);
    fs->inode_table = realloc(fs->inode_table, fs->next_ino * sizeof(*fs->inode_table));
    fs->inode_table[ino] = inode;
    fs->inode_count++;
    if (parent == NULL)
    {
        //这里根目录的创建初始化化
        inode->parent_ino = ino;
        fs->inode_table[0] = NULL;
        fs->root = inode;
    }
    else
    {
        inode->parent_ino = parent->ino;
        inode->next_sibling_ino = parent->first_child_ino;
        parent->first_child_ino = ino;//这里是头插法
    }
    return inode;
}


void zvfs_inode_io_done(void *arg, int bserrno)
{
    *(bool *)arg = true;
}

void zvfs_inode_io_wait(bool *done)
{
    while (!*done)
    {
        spdk_thread_poll(global_thread, 0, 0);
    }

    *done = false;
}
//下面是整张inode的加载和保存的函数

void zvfs_inode_table_save(zvfs_t *fs)
{
    //这个就是把我们这次mount之后的fs->inode_table重新写到fs->inode_table_blob上面
    uint64_t header[2] = {fs->inode_count, fs->next_ino};
    uint64_t bytes = sizeof(header) + fs->inode_count * sizeof(zvfs_inode_t);
    uint64_t unit = fs->io_unit_size;
    uint64_t units = (bytes + unit - 1) / unit;
    uint64_t clusters = (units * unit + fs->cluster_size - 1) / fs->cluster_size;
    bool done = false;
    uint8_t *buffer = spdk_zmalloc(units * unit, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
    memcpy(buffer, header, sizeof(header));
    uint8_t *record = buffer + sizeof(header);
    for (uint64_t ino = 1; ino < fs->next_ino; ino++)
    {
        zvfs_inode_t *inode = fs->inode_table[ino];
        if (inode != NULL)
        {
            memcpy(record, inode, sizeof(*inode));
            record += sizeof(*inode);
        }
    }
    struct spdk_thread *saved_thread = spdk_get_thread();
    spdk_set_thread(global_thread);

    if (clusters > spdk_blob_get_num_clusters(fs->inode_table_blob))
    {
        spdk_blob_resize(fs->inode_table_blob, clusters, zvfs_inode_io_done, &done);
        zvfs_inode_io_wait(&done);
        spdk_blob_sync_md(fs->inode_table_blob, zvfs_inode_io_done, &done);
        zvfs_inode_io_wait(&done);
    }

    spdk_blob_io_write(fs->inode_table_blob, fs->channel, buffer, 0,
                       units, zvfs_inode_io_done, &done);
    zvfs_inode_io_wait(&done);

    spdk_set_thread(saved_thread);
    spdk_free(buffer);
}
void zvfs_inode_table_load(zvfs_t *fs)
{
    uint64_t unit = fs->io_unit_size;
    uint64_t header[2];
    bool done = false;
    uint8_t *buffer = spdk_zmalloc(unit, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
    spdk_blob_io_read(fs->inode_table_blob, fs->channel, buffer, 0, 1, zvfs_inode_io_done, &done);
    zvfs_inode_io_wait(&done);
    memcpy(header, buffer, sizeof(header));
    fs->inode_count = header[0];
    fs->next_ino = header[1];
    uint64_t bytes = sizeof(header) + fs->inode_count * sizeof(zvfs_inode_t);
    uint64_t units = (bytes + unit - 1) / unit;
    if (units > 1)
    {
        spdk_free(buffer);
        buffer = spdk_zmalloc(units * unit, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        spdk_blob_io_read(fs->inode_table_blob, fs->channel, buffer, 0, units, zvfs_inode_io_done, &done);
        zvfs_inode_io_wait(&done);
    }
    fs->inode_table = calloc(fs->next_ino, sizeof(*fs->inode_table));
    const uint8_t *record = buffer + sizeof(header);
    for (uint64_t i = 0; i < fs->inode_count; i++)
    {
        zvfs_inode_t *inode = malloc(sizeof(*inode));
        memcpy(inode, record, sizeof(*inode));
        fs->inode_table[inode->ino] = inode;
        record += sizeof(*inode);
    }
    fs->root = fs->inode_table[1];
    spdk_free(buffer);
}

//这个函数是根据路径找到对应的inode结构体
zvfs_inode_t *zvfs_inode_lookup(zvfs_t *fs, zvfs_inode_t **start, const char *pathname)
{
    zvfs_inode_t *inode = *start;
    if (*pathname == '/')
    {
        inode = fs->root;
    }
    *start = NULL;
    while (*pathname != '\0')
    {
        if (!S_ISDIR(inode->mode))
        {
            errno = ENOTDIR;
            return NULL;
        }
        while (*pathname == '/')
        {
            pathname++;
        }
        if (*pathname == '\0')
        {
            break;
        }
        const char *name = pathname;
        while (*pathname != '\0' && *pathname != '/')
        {
            pathname++;
        }
        size_t len = pathname - name;
        if (len > YQ_MAX_FILENAME)
        {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (len == 1 && name[0] == '.')
        {
            continue;
        }
        if (len == 2 && name[0] == '.' && name[1] == '.')
        {
            inode = fs->inode_table[inode->parent_ino];
            continue;
        }
        uint64_t child_ino = inode->first_child_ino;
        while (child_ino != 0)
        {
            zvfs_inode_t *child = fs->inode_table[child_ino];

            if (strlen(child->filename) == len &&
                    memcmp(child->filename, name, len) == 0)
            {
                inode = child;
                break;
            }

            child_ino = child->next_sibling_ino;
        }
        if (child_ino == 0)
        {
            if (*pathname == '\0')
            {
                //这里之后，我们的start就指向父级目录的inode了
                *start = inode;
            }
            errno = ENOENT;
            return NULL;
        }
    }
    return inode;
}


