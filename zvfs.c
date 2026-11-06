#include "zvfs.h"
#define BUFFER_SIZE 512
// nvme bdev blob blobstore channel
#define DEBUG 0

// fd操作

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
    if (fd < 0 || fd >= YQ_MAX_FD)
    {
        SPDK_NOTICELOG("yq_get_fd_fromlinux:fd error %d", fd);
        return -1;
    }
    return fd;
}

struct zvfs_file_s *yq_get_filefromfd(int fd)
{
    return file_table[fd];
}

// 文件名到文件的映射hash

// 目前只使用俩个函数，一个是增加path->file的映射，一个是查找path->file

static struct rte_hash *path_hash;

static int yq_add_pathtofile(zvfs_file_t *file)
{
    // 这个是插入文件和path之间的映射
    char key[FILENAME_LENGTH] = {0};
    size_t len = strnlen((const char *)file->filename, sizeof(key));
    if (len >= sizeof(key))
    {
        SPDK_NOTICELOG("pathname errno\n");
        return -1;
    }
    memcpy(key, file->filename, len);
    if (path_hash == NULL)
    {
        // 这里是对hash表的初始化
        const struct rte_hash_parameters params =
        {
            .name = "yqpath",
            .entries = 1024,
            .key_len = sizeof(key),
            .socket_id = rte_socket_id(),
        };
        path_hash = rte_hash_create(&params);
        if (path_hash == NULL)
        {
            SPDK_NOTICELOG("create hash errno\n");
            return -1;
        }
    }
    return rte_hash_add_key_data(path_hash, key, file);
}

static zvfs_file_t *yq_get_filefrompath(const char *path)
{
    char key[FILENAME_LENGTH] = {0};
    void *data = NULL; // 这里是用来存放我们取到的file结构体的
    if (path_hash == NULL)
    {
        return NULL;
    }
    size_t len = strnlen(path, sizeof(key));
    if (len >= sizeof(key))
    {
        SPDK_NOTICELOG("pathname errno\n");
        return NULL;
    }
    memcpy(key, path, len);
    if (rte_hash_lookup_data(path_hash, key, &data) < 0)
    {
        return NULL;
    }
    return (zvfs_file_t *)data;
}


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
#if DEBUG
    SPDK_NOTICELOG("unload finish\n");
#endif
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
static void zvfs_spdk_blob_delete_cb(void *cb_arg, int bserrno)
{

    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    spdk_free(file->write_buffer);
    spdk_free(file->read_buffer);
    file->finished = true;
#if DEBUG
    SPDK_NOTICELOG("close finish\n");
#endif
}

static void zvfs_spdk_blob_close_cb(void *cb_arg, int bserrno)
{

    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    spdk_bs_delete_blob(file->fs->blobstore, file->blob_id, zvfs_spdk_blob_delete_cb, file);
}
static void zvfs_do_close(void *cb_arg)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    spdk_blob_close(file->blob, zvfs_spdk_blob_close_cb, file);
}

static void zvfs_spdk_blob_read_cb(void *cb_arg, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
#if DEBUG
    SPDK_NOTICELOG("read buffer: %s\n", file->read_buffer);
#endif
    file->finished = true;
}

static void zvfs_do_read(void *cb_arg)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    memset(file->read_buffer, 0, BUFFER_SIZE);
#if ENABLE_FIO
    uint64_t io_offset = file->offset / file->fs->unit_size;
#else
    uint64_t io_offset = 0;
#endif
    spdk_blob_io_read(file->blob, file->fs->channel, file->read_buffer, io_offset, 1, zvfs_spdk_blob_read_cb, file);
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
#if DEBUG
    SPDK_NOTICELOG("write finish\n");
#endif
}

static void zvfs_do_write(void *cb_arg)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
#if ENABLE_FIO
    uint64_t io_offset = file->offset / file->fs->unit_size;
#else
    uint64_t io_offset = 0;
#endif
    spdk_blob_io_write(file->blob, file->fs->channel, file->write_buffer, io_offset, 1, zvfs_spdk_blob_write_cb, file);
}

static void zvfs_spdk_blob_sync_cb(void *cb_arg, int bserrno)
{

    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    file->write_buffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
    if (file->write_buffer == NULL)
    {
        return;
    }
    file->read_buffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
    if (file->read_buffer == NULL)
    {
        return;
    }
#if ENABLE_FIO
    file->offset = 0;
#endif

    file->finished = true;
#if DEBUG
    SPDK_NOTICELOG("open finish\n");
#endif
}

static void zvfs_spdk_blob_resize_cb(void *cb_arg, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;

    uint64_t total = spdk_blob_get_num_clusters(file->blob);
    file->fs->num_cluster = total;
#if DEBUG
    SPDK_NOTICELOG("resize blob: %" PRIu64 "\n", total);
#endif
    spdk_blob_sync_md(file->blob, zvfs_spdk_blob_sync_cb, file);
}

static void zvfs_spdk_bs_open_blob_cb(void *cb_arg, struct spdk_blob *blb, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    file->blob = blb;
    uint64_t free_cluster = spdk_bs_free_cluster_count(file->fs->blobstore);
#if DEBUG
    SPDK_NOTICELOG("free_cluster: %" PRIu64 "\n", free_cluster);
#endif
    file->fs->free_cluster_count = free_cluster;
    spdk_blob_resize(blb, free_cluster, zvfs_spdk_blob_resize_cb, file);
}

static void zvfs_spdk_bs_create_blob_cb(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    file->blob_id = blobid;
#if DEBUG
    SPDK_NOTICELOG("blobid : %" PRIu64 "\n", blobid);
#endif
    spdk_bs_open_blob(file->fs->blobstore, blobid, zvfs_spdk_bs_open_blob_cb, file);
}

static void zvfs_do_open(void *arg)
{
    zvfs_file_t *file = (zvfs_file_t *)arg;
    spdk_bs_create_blob(file->fs->blobstore, zvfs_spdk_bs_create_blob_cb, file);
}

static void zvfs_spdk_bs_init_cb(void *cb_arg, struct spdk_blob_store *bs, int bserrno)
{
    struct zvfs_s *fs = (struct zvfs_s *)cb_arg;
    uint64_t io_unit_size = spdk_bs_get_io_unit_size(bs);
    fs->unit_size = io_unit_size;
    fs->blobstore = bs;
#if DEBUG
    SPDK_NOTICELOG("io_unit_size : %" PRIu64 "\n", io_unit_size);
#endif
    fs->channel = spdk_bs_alloc_io_channel(fs->blobstore);
    if (fs->channel == NULL)
    {
        return;
    }
    fs->finished = true;
#if DEBUG
    SPDK_NOTICELOG("mount finished\n");
#endif
    // spdk_bs_create_blob(bs, zvfs_spdk_bs_create_blob_cb, fs);
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
    int rc = spdk_bdev_create_bs_dev_ext("Nvme0n1", zvfs_spdk_bdev_event_cb, NULL,
                                         &bs_dev);
    if (rc != 0)
    {
        return;
    }
    spdk_bs_init(bs_dev, NULL, zvfs_spdk_bs_init_cb, fs);
#if DEBUG
    SPDK_NOTICELOG("zvfs_entry\n");
#endif
}

// filesystem

// load

static int zvfs_mount(struct zvfs_s *fs)
{
    fs->finished = false;
    return waiter(global_thread, zvfs_entry, fs, &fs->finished);
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
    return waiter(global_thread, zvfs_do_open, file, &file->finished);
}

// read
static int zvfs_read(zvfs_file_t *file, uint8_t *buffer, size_t count)
{

#if ENABLE_FIO
    size_t copied = 0;
    size_t unit_size = file->fs->unit_size;

    while (copied < count)
    {
        size_t buffer_offset = file->offset % unit_size;
        size_t bytes = unit_size - buffer_offset;

        if (bytes > count - copied)
        {
            bytes = count - copied;
        }

        file->finished = false;
        waiter(global_thread, zvfs_do_read, file, &file->finished);

        memcpy(buffer + copied,
               file->read_buffer + buffer_offset,
               bytes);

        file->offset += bytes;
        copied += bytes;
    }

    return copied;
#else
    file->finished = false;
    waiter(global_thread, zvfs_do_read, file, &file->finished);
    memcpy(buffer, file->read_buffer, strlen(file->read_buffer));
    return strlen(file->read_buffer);
#endif
}
// write
static int zvfs_write(zvfs_file_t *file, const uint8_t *buffer, size_t count)
{
#if ENABLE_FIO
    size_t written = 0;
    size_t unit_size = file->fs->unit_size;

    while (written < count)
    {
        size_t buffer_offset = file->offset % unit_size;
        size_t bytes = unit_size - buffer_offset;

        if (bytes > count - written)
        {
            bytes = count - written;
        }

        /* 部分覆盖，先保留该单元的原数据。 */
        if (buffer_offset != 0 || bytes != unit_size)
        {
            file->finished = false;
            waiter(global_thread, zvfs_do_read,
                   file, &file->finished);

            memcpy(file->write_buffer,
                   file->read_buffer,
                   unit_size);
        }

        /* 覆盖本次要修改的部分。 */
        memcpy(file->write_buffer + buffer_offset,
               buffer + written,
               bytes);

        file->finished = false;
        waiter(global_thread, zvfs_do_write,
               file, &file->finished);

        file->offset += bytes;
        if (file->offset > file->size)
        {
            file->size = file->offset;
        }
        written += bytes;
    }

    return written;
#else
    file->finished = false;
    memcpy(file->write_buffer, buffer, count);
    return waiter(global_thread, zvfs_do_write, file, &file->finished);
#endif
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
#if DEBUG
    SPDK_NOTICELOG("JSON loaded: %s\n", json_file);
#endif
}

static void zvfs_json_load_fn(void *arg)
{
    spdk_subsystem_init(json_app_load_done, arg);
}

static int zvfs_env_setup(void)
{
    struct spdk_env_opts opts;
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

#if DEBUG
    SPDK_NOTICELOG("json_app_load_done complete\n");
#endif
    return 0;
}


// 这里是对上面的接口的整合

static zvfs_t *fs = NULL; // 这里我们全局只使用一个fs，所以不需要管理，我们直接定义一个全局的就行

int nopen(const char *pathname, int flags)
{
    // 首先是看一下之前是否mountfs
    static bool mounted; // 这里使用static是我们所有调用open函数的时候，我们只会mounted一次
    if (!mounted)
    {
        if (zvfs_env_setup() != 0)
        {
            SPDK_NOTICELOG("setup errno\n");
            return -1;
        }
        fs = calloc(1, sizeof(zvfs_t));
        if (fs == NULL)
        {
            SPDK_NOTICELOG("room not enough\n");
            return -1;
        }
        zvfs_mount(fs);
        mounted = true;
    }
    if (pathname == NULL)
    {
        SPDK_NOTICELOG("nopen:pathname arg errno\n");
        return -1;
    }
    zvfs_file_t *file = yq_get_filefrompath(pathname);
    if (file == NULL && !(flags & O_CREAT))
    {
        SPDK_NOTICELOG("nopen:dont have this file\n");
        return -1;
    }
    // 接下来就有俩种情况，一种是file==NULL需要我们创建文件，建立映射，分配fd
    // 另一种是file！=NULL，需要我们找到文件，分配fd
    if (file != NULL && file->fd >= 0)
    {
        // 这里打开之前的file的fd需要被我们置为-1
        SPDK_NOTICELOG("nopen:file has been open\n");
        return -1;
    }
    int fd = yq_get_fd_fromlinux();
    if (fd < 0)
    {
        return -1;
    }
    if (file == NULL)
    {
        // 创建file
        file = calloc(1, sizeof(struct zvfs_file_s));
        if (file == NULL)
        {
            yq_destory_fd(fd);
            SPDK_NOTICELOG("nopen:room dont enought\n");
            return -1;
        }
        file->fs = fs;
        size_t len = strlen(pathname);
        memcpy(file->filename, pathname, len + 1);
        zvfs_open(file);
        int rc = yq_add_pathtofile(file);
        if (rc < 0)
        {
            zvfs_close(file);
            free(file);
            yq_destory_fd(fd);
            SPDK_NOTICELOG("add hash errno\n");
            return -1;
        }
    }
    file->fd = fd;
    file_table[fd] = file;
#if ENABLE_FIO
    file->offset = 0;
#endif
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
    return zvfs_read(file, buf, count);
}
ssize_t nwrite(int fd, const void *buf, size_t count)
{
    zvfs_file_t *file = yq_get_filefromfd(fd);
    if (file == NULL)
    {
        SPDK_NOTICELOG("nwrite:file dont open\n");
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
    file->fd = -1;
    yq_destory_fd(fd);
    return 0;
}
#if ENABLE_FIO
off64_t nlseek(int fd, off64_t offset, int whence)
{
    zvfs_file_t *file = yq_get_filefromfd(fd);
    if (file == NULL)
    {
        SPDK_NOTICELOG("dont open this fd\n");
        return -1;
    }
    if (whence != SEEK_SET)
    {
        SPDK_NOTICELOG("dont have this function\n");
        return -1;
    }
    file->offset = offset;
    return file->offset;
}
int nstat(const char *pathname, struct stat64 *st)
{
    zvfs_file_t *file = yq_get_filefrompath(pathname);

    if (file == NULL)
    {
        SPDK_NOTICELOG("dont open this fd\n");
        return -1;
    }

    memset(st, 0, sizeof(*st));

    st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size = file->size;
    st->st_blksize = file->fs->unit_size;

    return 0;

}
ssize_t npread(int fd, void *buf, size_t count, off64_t offset)
{
    zvfs_file_t *file = yq_get_filefromfd(fd);
    off64_t saved_offset = file->offset;

    nlseek(fd, offset, SEEK_SET);
    ssize_t result = nread(fd, buf, count);
    nlseek(fd, saved_offset, SEEK_SET);

    return result;
}

ssize_t npwrite(int fd, const void *buf, size_t count, off64_t offset)
{
    zvfs_file_t *file = yq_get_filefromfd(fd);
    off64_t saved_offset = file->offset;

    nlseek(fd, offset, SEEK_SET);
    ssize_t result = nwrite(fd, buf, count);
    nlseek(fd, saved_offset, SEEK_SET);

    return result;
}
#endif