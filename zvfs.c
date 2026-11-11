#include "zvfs.h"
#include<endian.h>
// nvme bdev blob blobstore channel
#define DEBUG 0
zvfs_t *fs = NULL; // 这里我们全局只使用一个fs，所以不需要管理，我们直接定义一个全局的就行
static bool waiter(struct spdk_thread *thread, spdk_msg_fn start_fn, void *ctx, bool *finished)
{
    int rc = spdk_thread_send_msg(thread, start_fn, ctx);
    if (rc != 0)
    {
        SPDK_NOTICELOG("send message failed: rc=%d\n", rc);
        exit(EXIT_FAILURE);
    }

    while (!(*finished))
    {
        spdk_thread_poll(thread, 0, 0);
    }

    return true;
}

static void zvfs_spdk_bs_unload_cb(void *cb_arg, int bserrno)
{
    struct zvfs_s *fs = (struct zvfs_s *)cb_arg;
    fs->finished = true;

}
static void zvfs_do_unmount(void *arg)
{
    struct zvfs_s *fs = (struct zvfs_s *)arg;
    if (fs->blobstore)
    {
        if (fs->channel)
        {
            spdk_bs_free_io_channel(fs->channel);
        }
        spdk_bs_unload(fs->blobstore, zvfs_spdk_bs_unload_cb, fs);
    }
}

static void zvfs_spdk_blob_close_cb(void *cb_arg, int bserrno)
{

    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    spdk_free(file->write_buffer);
    spdk_free(file->read_buffer);
    file->finished = true;
}
static void zvfs_do_close(void *cb_arg)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    spdk_blob_close(file->blob, zvfs_spdk_blob_close_cb, file);
}

static void zvfs_spdk_blob_read_cb(void *cb_arg, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;

    file->finished = true;
}

static void zvfs_do_read(void *cb_arg)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    size_t unit_size = file->fs->io_unit_size;
    uint64_t io_offset = file->offset / unit_size;
    memset(file->read_buffer, 0, file->read_units * unit_size);
    spdk_blob_io_read(file->blob, file->fs->channel, file->read_buffer, io_offset, file->read_units, zvfs_spdk_blob_read_cb, file);
#if 0
    file->finished = true;
#endif
}

static void zvfs_spdk_blob_write_cb(void *cb_arg, int bserrno)
{
    if (bserrno != 0)
    {
        SPDK_ERRLOG("blob write failed: %d\n", bserrno);
        exit(EXIT_FAILURE);
    }
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    file->finished = true;

}

static void zvfs_do_write(void *cb_arg)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;

    uint64_t io_offset = file->offset / file->fs->io_unit_size;

    spdk_blob_io_write(file->blob, file->fs->channel, file->write_buffer, io_offset, file->write_units, zvfs_spdk_blob_write_cb, file);
#if 0
    file->finished = true;
#endif
}

static void zvfs_spdk_bs_open_blob_cb(void *cb_arg, struct spdk_blob *blb, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    file->blob = blb;
    file->finished = true;
}

static void zvfs_spdk_bs_create_blob_cb(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    file->inode->blob_id = blobid;

    spdk_bs_open_blob(file->fs->blobstore, blobid, zvfs_spdk_bs_open_blob_cb, file);
}

static void zvfs_do_open(void *arg)
{
    zvfs_file_t *file = (zvfs_file_t *)arg;
    //这里分为俩种情况，一种是我们blob已经有了只需要找到blob，一种是没有需要创建
    spdk_blob_id blob_id = file->inode->blob_id;
    if (blob_id != SPDK_BLOBID_INVALID)
    {
        //这里就是之前我们创建过一个文件了，这里我们只需要打开就行了
        spdk_bs_open_blob(file->fs->blobstore, blob_id, zvfs_spdk_bs_open_blob_cb, file);
        return;
    }
    struct spdk_blob_opts opts;
    spdk_blob_opts_init(&opts, sizeof(opts));
    opts.num_clusters = 1;
    spdk_bs_create_blob_ext(file->fs->blobstore, &opts, zvfs_spdk_bs_create_blob_cb, file);
}


//下面这三个函数用来找到我们的superblob也就是存储我们目录表的blob
static void zvfs_root_opened_cb(void *arg, struct spdk_blob *blob,
                                int bserrno)
{
    zvfs_t *fs = arg;

    fs->inode_table_blob = blob;
    fs->finished = true;
}

static void zvfs_root_created_cb(void *arg, spdk_blob_id blob_id,
                                 int bserrno)
{
    zvfs_t *fs = arg;

    spdk_bs_open_blob(fs->blobstore, blob_id, zvfs_root_opened_cb, fs);
}

static void zvfs_get_root_cb(void *arg, spdk_blob_id blob_id, int bserrno)
{
    zvfs_t *fs = arg;

    if (blob_id == SPDK_BLOBID_INVALID)
    {
        spdk_bs_create_blob(fs->blobstore, zvfs_root_created_cb, fs);
    }
    else
    {
        spdk_bs_open_blob(fs->blobstore, blob_id, zvfs_root_opened_cb, fs);
    }
}

//这里我们加载bs的时候不能每次都初始化，需要找以前init话并操作过的blobstore
static void zvfs_spdk_bs_load_cb(void *cb_arg, struct spdk_blob_store *bs, int bserrno)
{
    struct zvfs_s *fs = (struct zvfs_s *)cb_arg;
    uint64_t io_unit_size = spdk_bs_get_io_unit_size(bs);
    fs->io_unit_size = io_unit_size;
    fs->blobstore = bs;
    fs->channel = spdk_bs_alloc_io_channel(fs->blobstore);
    fs->cluster_size = spdk_bs_get_cluster_size(bs);
    //fs->finished = true;
    //这里是第一步初始化spdk，loadblobstore，并初始化fs的信息，后面是要加载根目录了
    spdk_bs_get_super(fs->blobstore, zvfs_get_root_cb, fs);
}

static void zvfs_spdk_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
#if DEBUG
    SPDK_NOTICELOG("zvfs_spdk_bdev_cb\n");
#endif
}

static void zvfs_entry(void *arg)
{
    struct zvfs_s *fs = (struct zvfs_s *)arg;
    struct spdk_bs_dev *bs_dev = NULL;
    spdk_bdev_create_bs_dev_ext("Nvme0n1", zvfs_spdk_bdev_event_cb, NULL,
                                &bs_dev);

    spdk_bs_load(bs_dev, NULL, zvfs_spdk_bs_load_cb, fs);

}


static void zvfs_resize(zvfs_file_t *file, off_t length)
{
    zvfs_t *fs = file->fs;
    uint64_t clusters = ((uint64_t)length - 1) / fs->cluster_size + 1;
    bool done = false;
    if (clusters > spdk_blob_get_num_clusters(file->blob))
    {
        spdk_blob_resize(file->blob, clusters, zvfs_inode_io_done, &done);
        zvfs_inode_io_wait(&done);
    }
    //这里扩容之后能就算结束了，我们还需要做的是把扩容了的空间置为0
    uint64_t old_size = file->inode->size;
    uint64_t unit_size = fs->io_unit_size;
    uint64_t first_unit = old_size / unit_size;
    uint64_t end_unit = ((uint64_t)length - 1) / unit_size + 1;
    size_t tail = old_size % unit_size;
    if (tail != 0)
    {
        //这里tail是最后一个unit里面有数据的结尾，如果不是0的话说明我们需要把tail到这个unit后面的数据也置零
        spdk_blob_io_read(file->blob, fs->channel, file->read_buffer, first_unit, 1, zvfs_inode_io_done, &done);
        zvfs_inode_io_wait(&done);
        memset(file->read_buffer + tail, 0, unit_size - tail);
        spdk_blob_io_write(file->blob, fs->channel, file->read_buffer, first_unit, 1, zvfs_inode_io_done, &done);
        zvfs_inode_io_wait(&done);
        first_unit++;

    }
    if (first_unit < end_unit)
    {
        spdk_blob_io_write_zeroes(file->blob, fs->channel, first_unit,
                                  end_unit - first_unit,
                                  zvfs_inode_io_done, &done);
        zvfs_inode_io_wait(&done);
    }



}


// filesystem

// load

static int zvfs_mount(struct zvfs_s *fs)
{
    fs->finished = false;
    waiter(global_thread, zvfs_entry, fs, &fs->finished);
    if (spdk_blob_get_num_clusters(fs->inode_table_blob) == 0)
    {
        //这里就是没有根目录，需要我们创建一个
        fs->next_ino = 1;
        fs->inode_count = 0;
        zvfs_inode_create(fs, NULL, YQ_SPDK_ROOT_DIR, S_IFDIR | 0755, SPDK_BLOBID_INVALID);
        zvfs_inode_table_save(fs);
        bool done = false;
        spdk_bs_set_super(fs->blobstore, spdk_blob_get_id(fs->inode_table_blob), zvfs_inode_io_done, &done);
        zvfs_inode_io_wait(&done);
    }
    else
    {
        //这里就是有根目录，需要我们加载整个文件目录树
        zvfs_inode_table_load(fs);
    }
    return 0;
}
// unload

static int zvfs_unmount(struct zvfs_s *fs)
{
    fs->finished = false;
    return waiter(global_thread, zvfs_do_unmount, fs, &fs->finished);
}

// file

// open

static int zvfs_open(struct zvfs_file_s *file)
{
    file->finished = false;
    waiter(global_thread, zvfs_do_open, file, &file->finished);
    return 0;
}

// read
static int zvfs_read(zvfs_file_t *file, uint8_t *buffer, size_t count)
{

    size_t copied = 0;
    size_t unit_size = file->fs->io_unit_size;

    while (copied < count)
    {
        size_t buffer_offset = file->offset % unit_size;
        size_t bytes = count - copied;
        size_t max_bytes = (BUFFER_SIZE / unit_size) * unit_size - buffer_offset;

        if (bytes > max_bytes)
        {
            bytes = max_bytes;
        }

        file->read_units = (buffer_offset + bytes + unit_size - 1) / unit_size;
        file->finished = false;
        waiter(global_thread, zvfs_do_read, file, &file->finished);

        memcpy(buffer + copied,
               file->read_buffer + buffer_offset,
               bytes);

        file->offset += bytes;
        copied += bytes;
    }

    return copied;

}
// write
static int zvfs_write(zvfs_file_t *file, const uint8_t *buffer, size_t count)
{
    size_t written = 0;
    size_t unit_size = file->fs->io_unit_size;

    while (written < count)
    {
        if (count == 0)
        {
            return 0;
        }

        off_t end;
        if (__builtin_add_overflow(file->offset, count - written, &end))
        {
            errno = EFBIG;
            return -1;
        }

        uint64_t capacity = spdk_blob_get_num_clusters(file->blob) *
                            file->fs->cluster_size;

        if ((uint64_t)end > capacity)
        {
            off_t new_capacity = end;
            if (capacity <= INT64_MAX / 2 && capacity * 2 > (uint64_t)end)
            {
                new_capacity = (off_t)(capacity * 2);
            }

            struct spdk_thread *saved_thread = spdk_get_thread();
            spdk_set_thread(global_thread);
            zvfs_resize(file, new_capacity);
            spdk_set_thread(saved_thread);
        }
        size_t buffer_offset = file->offset % unit_size;
        size_t bytes = count - written;

        if (buffer_offset == 0 && bytes >= unit_size)
        {
            if (bytes > BUFFER_SIZE)
            {
                bytes = BUFFER_SIZE;
            }
            bytes -= bytes % unit_size;
        }
        else
        {
            size_t available = unit_size - buffer_offset;
            if (bytes > available)
            {
                bytes = available;
            }
            file->read_units = 1;
            file->finished = false;
            waiter(global_thread, zvfs_do_read, file, &file->finished);
            memcpy(file->write_buffer, file->read_buffer, unit_size);
        }

        memcpy(file->write_buffer + buffer_offset, buffer + written, bytes);

        file->write_units = bytes < unit_size ? 1 : bytes / unit_size;
        file->finished = false;
        waiter(global_thread, zvfs_do_write, file, &file->finished);

        file->offset += bytes;
        if (file->offset > file->inode->size)
        {
            file->inode->size = file->offset;
        }
        written += bytes;
    }

    return written;

}
// close

static int zvfs_close(zvfs_file_t *file)
{
    file->finished = false;
    return waiter(global_thread, zvfs_do_close, file, &file->finished);
}

static const char *json_file = "/home/gyq2/spdk/build/examples/zvfs.json";

static void json_app_load_done(int rc, void *ctx)
{
    static bool json_started = false;

    if (rc != 0)
    {
        SPDK_ERRLOG("init/load JSON failed: rc=%d\n", rc);
        exit(EXIT_FAILURE);
    }

    if (!json_started)
    {
        size_t size = 0;
        void *json = spdk_posix_file_load_from_name(json_file, &size);
        if (json == NULL)
        {
            SPDK_ERRLOG("cannot read JSON: %s\n", json_file);
            exit(EXIT_FAILURE);
        }

        json_started = true;
        spdk_rpc_set_state(SPDK_RPC_RUNTIME);
        spdk_subsystem_load_config(
            json, size, json_app_load_done, ctx, true);
        free(json);
        return;
    }

    *(bool *)ctx = true;

}

static void zvfs_json_load_fn(void *arg)
{
    spdk_subsystem_init(json_app_load_done, arg);
}

static int zvfs_env_setup(void)
{
    struct spdk_env_opts opts;
    opts.opts_size = sizeof(opts);
    spdk_env_opts_init(&opts);
    if (0 != spdk_env_init(&opts))
    {
        return -1;
    }
    spdk_log_set_print_level(SPDK_LOG_NOTICE);
    spdk_log_set_level(SPDK_LOG_NOTICE);
    spdk_log_open(NULL);
    spdk_thread_lib_init(NULL, 0);
    global_thread = spdk_thread_create("global", NULL);
    spdk_set_thread(global_thread);
    bool done = false;
    waiter(global_thread, zvfs_json_load_fn, &done, &done);

    return 0;
}


// 这里是对上面的接口的整合
static int zvfs_ensure_mounted(void)
{
    if (fs != NULL)
    {
        return 0;
    }

    zvfs_t *new_fs = calloc(1, sizeof(*new_fs));
    if (new_fs == NULL)
    {
        return -1;
    }

    if (zvfs_env_setup() != 0 || zvfs_mount(new_fs) != 0)
    {
        free(new_fs);
        errno = EIO;
        return -1;
    }

    fs = new_fs;
    return 0;
}

zvfs_inode_t *zvfs_lookup_dir(const char *pathname, zvfs_t **out_fs)
{
    if (zvfs_ensure_mounted() != 0)
    {
        return NULL;
    }

    zvfs_inode_t *start = fs->root;
    zvfs_inode_t *inode = zvfs_inode_lookup(fs, &start, pathname);
    if (inode == NULL)
    {
        return NULL;
    }
    if (!S_ISDIR(inode->mode))
    {
        errno = ENOTDIR;
        return NULL;
    }

    *out_fs = fs;
    return inode;
}

int nmkdir(const char *pathname, mode_t mode)
{
    if (zvfs_ensure_mounted() != 0)
    {
        return -1;
    }

    size_t len = strlen(pathname);
    while (len > 1 && pathname[len - 1] == '/')
    {
        len--;
    }

    char *path = strndup(pathname, len);
    if (path == NULL)
    {
        return -1;
    }

    zvfs_inode_t *parent = fs->root;
    zvfs_inode_t *inode = zvfs_inode_lookup(fs, &parent, path);
    if (inode != NULL || parent == NULL)
    {
        int error = inode != NULL ? EEXIST : errno;
        free(path);
        errno = error;
        return -1;
    }

    const char *name = strrchr(path, '/') + 1;
    zvfs_inode_create(fs, parent, name, S_IFDIR | (mode & 0777),
                      SPDK_BLOBID_INVALID);
    free(path);

    zvfs_inode_table_save(fs);
    return 0;
}

int nopen(const char *pathname, int flags)
{
    if (zvfs_ensure_mounted() != 0)
    {
        return -1;
    }
    zvfs_inode_t *parent = fs->root;
    zvfs_inode_t *inode = zvfs_inode_lookup(fs, &parent, pathname);
    bool create = inode == NULL;
    if (create)
    {
        if (!(flags & O_CREAT) || parent == NULL)
        {
            return -1;
        }
        //这里就需要创建一个inode了
        const char *slash = strrchr(pathname, '/');
        const char *name = slash != NULL ? slash + 1 : pathname;

        if (strlen(name) > YQ_MAX_FILENAME)
        {
            SPDK_NOTICELOG("name too long\n");
            return -1;
        }
        inode = zvfs_inode_create(fs, parent, name, S_IFREG | 0644, SPDK_BLOBID_INVALID);
    }
    else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
    {
        SPDK_NOTICELOG("flag errno\n");
        return -1;
    }
    if (S_ISDIR(inode->mode))
    {
        //这里其实文件夹也是可以被打开的
        if ((flags & O_ACCMODE) != O_RDONLY ||
                (flags & (O_CREAT | O_TRUNC)) != 0)
        {
            errno = EISDIR;
            return -1;
        }

        zvfs_file_t *file = calloc(1, sizeof(*file));
        if (file == NULL)
        {
            return -1;
        }

        int fd = yq_get_fd_fromlinux();
        if (fd < 0)
        {
            free(file);
            return -1;
        }

        file->fs = fs;
        file->inode = inode;
        file->flags = flags;
        file_table[fd] = file;
        return fd;

    }
    zvfs_file_t *file = calloc(1, sizeof(*file));
    file->fs = fs;
    file->inode = inode;
    file->flags = flags;
//这里mount就完了，我们需要改一我们的zvfs_open,如果我们的文件是之前就有的，那我们需要通过blobid找到blob，而不是每次都创建一个blob
    int fd = yq_get_fd_fromlinux();
    size_t size = BUFFER_SIZE;
    file->read_buffer = spdk_malloc(size, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
    file->write_buffer = spdk_malloc(size, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
    zvfs_open(file);
    if (create)
    {
        zvfs_inode_table_save(fs);//这里是创建完之后的落盘
    }
    file_table[fd] = file;
    return fd;
}
ssize_t nread(int fd, void *buf, size_t count)
{
    zvfs_file_t *file = yq_get_filefromfd(fd);
    if (file == NULL)
    {
        SPDK_NOTICELOG("nread: file dont open\n");
        return -1;
    }

    off_t remaining = file->inode->size - file->offset;
    if (count > (uint64_t)remaining)
    {
        count = (size_t)remaining;
    }
    return zvfs_read(file, buf, count);
}
ssize_t nwrite(int fd, const void *buf, size_t count)
{
    zvfs_file_t *file = yq_get_filefromfd(fd);
    if (file == NULL || (file->flags & O_ACCMODE) == O_RDONLY)
    {
        errno = EBADF;
        return -1;
    }

    return zvfs_write(file, buf, count);
}
int nclose(int fd)
{
    if (fd < 0 || fd >= YQ_MAX_FD || file_table[fd] == NULL)
    {
        SPDK_NOTICELOG("nclose: invalid fd %d\n", fd);
        return -1;
    }

    zvfs_file_t *file = file_table[fd];
    file_table[fd] = NULL;

    if (S_ISDIR(file->inode->mode))
    {
        free(file->read_buffer);
        //这里目录也可以删掉
        if (file->inode->parent_ino == 0)
        {
            zvfs_reclaim_inode(file->inode);
        }
    }
    else
    {
        zvfs_close(file);
        if (file->inode->parent_ino == 0)
        {
            //这里是unlink的删除
            zvfs_reclaim_inode(file->inode);
        }
        else if ((file->flags & O_ACCMODE) != O_RDONLY)
        {
            zvfs_inode_table_save(file->fs);
        }
    }
    yq_destory_fd(fd);
    free(file);

    return 0;
}
off64_t nlseek(int fd, off64_t offset, int whence)
{
    zvfs_file_t *file = yq_get_filefromfd(fd);
    if (file == NULL)
    {
        SPDK_NOTICELOG("dont open this fd\n");
        return -1;
    }
    if (whence != SEEK_SET || offset < 0)
    {
        SPDK_NOTICELOG("dont have this function\n");
        return -1;
    }
    file->offset = offset;
    return file->offset;
}
int nstat(const char *pathname, struct stat64 *st)
{
    if (zvfs_ensure_mounted() != 0)
    {
        return -1;
    }
    zvfs_inode_t *start = fs->root;
    zvfs_inode_t *inode = zvfs_inode_lookup(fs, &start, pathname);
    if (inode == NULL)
    {
        errno = ENOENT;
        return -1;
    }

    memset(st, 0, sizeof(*st));
    st->st_ino = inode->ino;
    st->st_mode = inode->mode;
    st->st_nlink = 1;
    st->st_size = inode->size;
    st->st_blksize = fs->io_unit_size;

    return 0;

}
ssize_t npread(int fd, void *buf, size_t count, off64_t offset)
{
    zvfs_file_t *file = yq_get_filefromfd(fd);
    off64_t saved_offset = file->offset;

    if (nlseek(fd, offset, SEEK_SET) < 0)
    {
        return -1;
    }
    ssize_t result = nread(fd, buf, count);
    nlseek(fd, saved_offset, SEEK_SET);

    return result;
}

ssize_t npwrite(int fd, const void *buf, size_t count, off64_t offset)
{
    zvfs_file_t *file = yq_get_filefromfd(fd);
    off64_t saved_offset = file->offset;

    if (nlseek(fd, offset, SEEK_SET) < 0)
    {
        return -1;
    }
    ssize_t result = nwrite(fd, buf, count);
    nlseek(fd, saved_offset, SEEK_SET);

    return result;
}

int nftruncate(int fd, off_t length)
{
    //这个函数的作用是根据这个fd来给文件设置总大小，如果我们现在的文件大小小于我们的length，我们就需要对文件精细扩容操作
    zvfs_file_t *file = yq_get_filefromfd(fd);
    if (length > file->inode->size)
    {
        zvfs_resize(file, length);
    }
    file->inode->size = length;
    return 0;

}

//这个函数是用来检查是不是unlink之后最后一个fd的应用了

bool zvfs_reclaim_inode(zvfs_inode_t *inode)
{
    for (int fd = 0; fd < YQ_MAX_FD; fd++)
    {
        if (file_table[fd] != NULL && file_table[fd]->inode == inode)
        {
            return false;
        }
    }
    //这里就是整个表都没有这个fd持有这个inode了，可以释放了
    fs->inode_table[inode->ino] = NULL;
    fs->inode_count--;
    zvfs_inode_table_save(fs);
    //下面是释放blob
    if (inode->blob_id != SPDK_BLOBID_INVALID)
    {
        bool done = false;
        spdk_bs_delete_blob(fs->blobstore, inode->blob_id, zvfs_inode_io_done, &done);
        zvfs_inode_io_wait(&done);
    }
    free(inode);
    return true;

}



//这个nunlink的作用是首先减除这个文件和目录树的关联，等到这个最后一个使用的fd被close之后，彻底删除这个文件

int nunlink(const char *pathname)
{
    if (zvfs_ensure_mounted() != 0)
    {
        return -1;
    }
    zvfs_inode_t *start = fs->root;
    zvfs_inode_t *inode = zvfs_inode_lookup(fs, &start, pathname);
    if (inode == NULL)
    {
        return -1;
    }
    if (S_ISDIR(inode->mode))
    {
        errno = EISDIR; //这里不能参数为目录
        return -1;
    }
    //下面就是取消关联这个文件
    zvfs_inode_t *parent = fs->inode_table[inode->parent_ino];
    uint64_t *link = &parent->first_child_ino;
    while (*link != inode->ino)
    {
        link = &fs->inode_table[*link]->next_sibling_ino;
    }
    *link = inode->next_sibling_ino;
    inode->parent_ino = 0;
    //这里是对readir的时候操作的unlink
    for (int fd = 0; fd < YQ_MAX_FD; fd++)
    {
        zvfs_file_t *file = file_table[fd];
        if (file != NULL && file->inode == parent &&
                file->offset == (off_t)inode->ino)
        {
            file->offset = inode->next_sibling_ino;
        }
    }
    if (!zvfs_reclaim_inode(inode))
    {
        zvfs_inode_table_save(fs);
    }
    return 0;


}


int nrmdir(const char *pathname)
{
    //这个函数是删除空目录，文件夹
    if (zvfs_ensure_mounted() != 0)
    {
        return -1;

    }
    zvfs_inode_t *start = fs->root;
    zvfs_inode_t *inode = zvfs_inode_lookup(fs, &start, pathname);
    if (inode == NULL)
    {
        return -1;
    }

    if (!S_ISDIR(inode->mode))
    {
        errno = ENOTDIR;
        return -1;
    }
    //接下来就是
    const char *end = pathname + strlen(pathname);
    while (end > pathname && end[-1] == '/')
    {
        end--;
    }
    const char *name = end;
    while (name > pathname && name[-1] != '/')
    {
        name--;
    }
    if (end - name == 1 && name[0] == '.')
    {
        errno = EINVAL;
        return -1;
    }
    if (end - name == 2 && name[0] == '.' && name[1] == '.')
    {
        errno = ENOTEMPTY;
        return -1;
    }

    if (inode == fs->root)
    {
        errno = EBUSY;
        return -1;
    }
    if (inode->first_child_ino != 0)
    {
        errno = ENOTEMPTY;
        return -1;
    }

    zvfs_inode_t *parent = fs->inode_table[inode->parent_ino];
    uint64_t *link = &parent->first_child_ino;
    while (*link != inode->ino)
    {
        link = &fs->inode_table[*link]->next_sibling_ino;
    }

    *link = inode->next_sibling_ino;
    inode->parent_ino = 0;

    for (int fd = 0; fd < YQ_MAX_FD; fd++)
    {
        zvfs_file_t *file = file_table[fd];
        if (file != NULL && file->inode == parent &&
                file->offset == (off_t)inode->ino)
        {
            file->offset = inode->next_sibling_ino;
        }
    }

    if (!zvfs_reclaim_inode(inode))
    {
        zvfs_inode_table_save(fs);
    }
    return 0;
}
int nrename(const char *oldpath, const char *newpath)
{
    if (zvfs_ensure_mounted() != 0)
        return -1;

    zvfs_inode_t *start = fs->root;
    zvfs_inode_t *source = zvfs_inode_lookup(fs, &start, oldpath);
    if (source == NULL)
        return -1;

    if (!S_ISREG(source->mode))
    {
        errno = EOPNOTSUPP;
        return -1;
    }

    zvfs_inode_t *parent = fs->root;
    zvfs_inode_t *target = zvfs_inode_lookup(fs, &parent, newpath);
    if (target == source)
        return 0;

    if (target != NULL)
    {
        if (S_ISDIR(target->mode))
        {
            errno = EISDIR;
            return -1;
        }
        parent = fs->inode_table[target->parent_ino];
    }
    if (parent == NULL)
        return -1;

    if (parent->ino != source->parent_ino)
    {
        errno = EOPNOTSUPP;
        return -1;
    }

    if (target != NULL)
    {
        uint64_t *link = &parent->first_child_ino;
        while (*link != target->ino)
            link = &fs->inode_table[*link]->next_sibling_ino;

        *link = target->next_sibling_ino;
        target->parent_ino = 0;

        for (int fd = 0; fd < YQ_MAX_FD; fd++)
        {
            zvfs_file_t *file = file_table[fd];
            if (file != NULL && file->inode == parent &&
                    file->offset == (off_t)target->ino)
            {
                file->offset = target->next_sibling_ino;
            }
        }
    }

    strcpy(source->filename, strrchr(newpath, '/') + 1);

    if (target == NULL || !zvfs_reclaim_inode(target))
        zvfs_inode_table_save(fs);

    return 0;
}
int nstatvfs(const char *pathname, struct statvfs *buf)
{
    if (zvfs_ensure_mounted() != 0)
        return -1;

    zvfs_inode_t *start = fs->root;
    if (zvfs_inode_lookup(fs, &start, pathname) == NULL)
        return -1;

    uint64_t free_clusters = spdk_bs_free_cluster_count(fs->blobstore);

    *buf = (struct statvfs)
    {
        .f_bsize = fs->cluster_size,
        .f_frsize = fs->cluster_size,
        .f_blocks = spdk_bs_total_data_cluster_count(fs->blobstore),
        .f_bfree = free_clusters,
        .f_bavail = free_clusters,
        .f_namemax = YQ_MAX_FILENAME,
    };
    return 0;
}


