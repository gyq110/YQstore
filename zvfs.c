
#include"zvfs.h"
#define BUFFER_SIZE 512
// nvme bdev blob blobstore channel


#if ENABLE_HOOK
//fd操作




static struct zvfs_file_s *file_table[YQ_MAX_FD] = {0};

void yq_destory_fd(int fd)
{
    //把这个fd销毁
    if (fd < 0 || fd >= YQ_MAX_FD)
    {
        printf("yq_destory_fd:fd error %d", fd);
        return ;
    }
    syscall(SYS_close, fd);
}

int yq_get_fd_fromlinux()
{
    //这里就是从linux获取一个fd
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0 || fd >= YQ_MAX_FD)
    {
        printf("yq_get_fd_fromlinux:fd error %d", fd);
        return -1;
    }
    return -1;

}

struct zvfs_file_s *yq_get_filefromfd(int fd)
{
    return file_table[fd];
}

#endif


static bool waiter(struct spdk_thread *thread, spdk_msg_fn start_fn, void *ctx, bool *finished)
{
    int rc = spdk_thread_send_msg(thread, start_fn, ctx);
    if (rc != 0)
    {
        SPDK_ERRLOG("send message failed: rc=%d\n", rc);
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
    SPDK_NOTICELOG("unload finish\n");

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
    SPDK_NOTICELOG("close finish\n");

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
    SPDK_NOTICELOG("read buffer: %s\n", file->read_buffer);
    file->finished = true;
}

static void zvfs_do_read(void *cb_arg)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    memset(file->read_buffer, 0, BUFFER_SIZE);
    spdk_blob_io_read(file->blob, file->fs->channel, file->read_buffer, 0, 1, zvfs_spdk_blob_read_cb, file);
}


static void zvfs_spdk_blob_write_cb(void *cb_arg, int bserrno)
{

    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    file->finished = true;
    SPDK_NOTICELOG("write finish\n");
}




static void zvfs_do_write(void *cb_arg)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    spdk_blob_io_write(file->blob, file->fs->channel, file->write_buffer, 0, 1, zvfs_spdk_blob_write_cb, file);
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

    file->finished = true;
    SPDK_NOTICELOG("open finish\n");

}

static void zvfs_spdk_blob_resize_cb(void *cb_arg, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;

    uint64_t total = spdk_blob_get_num_clusters(file->blob);
    file->fs->num_cluster = total;
    SPDK_NOTICELOG("resize blob: %" PRIu64 "\n", total);
    spdk_blob_sync_md(file->blob, zvfs_spdk_blob_sync_cb, file);
}

static void zvfs_spdk_bs_open_blob_cb(void *cb_arg, struct spdk_blob *blb, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    file->blob = blb;
    uint64_t free_cluster = spdk_bs_free_cluster_count(file->fs->blobstore);
    SPDK_NOTICELOG("free_cluster: %" PRIu64 "\n", free_cluster);

    file->fs->free_cluster_count = free_cluster;
    spdk_blob_resize(blb, free_cluster, zvfs_spdk_blob_resize_cb, file);

}

static void zvfs_spdk_bs_create_blob_cb(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
    zvfs_file_t *file = (zvfs_file_t *)cb_arg;
    file->blob_id = blobid;
    SPDK_NOTICELOG("blobid : %" PRIu64 "\n", blobid);
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
    SPDK_NOTICELOG("io_unit_size : %" PRIu64 "\n", io_unit_size);
    fs->channel = spdk_bs_alloc_io_channel(fs->blobstore);
    if (fs->channel == NULL)
    {
        return;
    }
    fs->finished = true;
    SPDK_NOTICELOG("mount finished\n");
    //spdk_bs_create_blob(bs, zvfs_spdk_bs_create_blob_cb, fs);
}

static void zvfs_spdk_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
    SPDK_NOTICELOG("zvfs_spdk_bdev_cb\n");
}

static void zvfs_entry(void *arg)
{
    struct zvfs_s *fs = (struct zvfs_s *)arg;
    struct spdk_bs_dev *bs_dev = NULL;
    int rc = spdk_bdev_create_bs_dev_ext("Malloc0", zvfs_spdk_bdev_event_cb, NULL,
                                         &bs_dev);
    if (rc != 0)
    {
        return ;
    }
    spdk_bs_init(bs_dev, NULL, zvfs_spdk_bs_init_cb, fs);
    SPDK_NOTICELOG("zvfs_entry\n");
}

//filesystem

//load

static int zvfs_mount(struct zvfs_s *fs)
{
    fs->finished = false;
    return waiter(global_thread, zvfs_entry, fs, &fs->finished);
}
//unload

static int zvfs_unmount(struct zvfs_s *fs)
{
    fs->finished = false;
    return waiter(global_thread, zvfs_do_unmount, fs, &fs->finished);
}


//file

// open

static int zvfs_open(struct zvfs_file_s *file)
{
    file->finished = false;
    return waiter(global_thread, zvfs_do_open, file, &file->finished);

}

// read
static int zvfs_read(zvfs_file_t *file, uint8_t *buffer, size_t count)
{
    file->finished = false;
    waiter(global_thread, zvfs_do_read, file, &file->finished);
    memcpy(buffer, file->read_buffer, strlen(file->read_buffer));
    return strlen(file->read_buffer);
}
// write
static int zvfs_write(zvfs_file_t *file, const uint8_t *buffer, size_t count)
{
    file->finished = false;
    memcpy(file->write_buffer, buffer, count);
    return waiter(global_thread, zvfs_do_write, file, &file->finished);


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
    SPDK_NOTICELOG("JSON loaded: %s\n", json_file);
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
    SPDK_NOTICELOG("json_app_load_done complete\n");
    return 0;
}


#if ENABLE_HOOK
//这里是对上面的接口的整合
int nopen(const char *pathname, int flags)
{

}
ssize_t nread(int fd, void *buf, size_t count)
{

}
ssize_t nwrite(int fd, const void *buf, size_t count)
{

}
int nclose(int fd)
{

}
#endif
// int main(int argc, char *argv[])
// {
//     if (0 != zvfs_env_setup())
//     {
//         return -1;
//     }
//     SPDK_NOTICELOG("init success\n");
//     zvfs_t *fs = calloc(1, sizeof(zvfs_t));
//     zvfs_mount(fs);

//     zvfs_file_t *file = calloc(1, sizeof(zvfs_file_t));
//     file->fs = fs;
//     zvfs_open(file);
//     char *buffer = "abcdefghigklmnopqrstuvwxyz";
//     zvfs_write(file, buffer, strlen(buffer));
//     char rbuffer[BUFFER_SIZE] = {0};
//     zvfs_read(file, rbuffer, BUFFER_SIZE);
//     zvfs_close(file);
//     free(file);
//     zvfs_unmount(fs);
//     free(fs);
//     return 0;
// }
