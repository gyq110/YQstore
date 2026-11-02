#include <spdk/bdev.h>
#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <stdio.h>
#include <spdk/init.h>
#define BUFFER_SIZE 512
// nvme bdev blob blobstore channel

#if 0
struct spdk_blob_store *blobstore;
spdk_blob_id blob_id;
struct spdk_blob *blob;
uint64_t free_cluster_count = 0;
uint64_t num_cluster;
uint8_t *write_buffer;
uint8_t *read_buffer;
struct spdk_io_channel *channel;
#else

struct spdk_thread *global_thread = NULL;

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

#endif

static void zvfs_spdk_bs_unload_cb(void *cb_arg, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;
    spdk_free(ctx->write_buffer);
    spdk_free(ctx->read_buffer);
    free(ctx);
    spdk_app_stop(0);

}

static void zvfs_spdk_blob_delete_cb(void *cb_arg, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;
    if (ctx->blobstore)
    {
        if (ctx->channel)
        {
            spdk_bs_free_io_channel(ctx->channel);
        }
        spdk_bs_unload(ctx->blobstore, zvfs_spdk_bs_unload_cb, ctx);
    }
}

static void zvfs_spdk_blob_close_cb(void *cb_arg, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;
    spdk_bs_delete_blob(ctx->blobstore, ctx->blob_id, zvfs_spdk_blob_delete_cb, ctx);
}

static void zvfs_spdk_blob_read_cb(void *cb_arg, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;
    SPDK_NOTICELOG("read buffer: %s\n", ctx->read_buffer);
    spdk_blob_close(ctx->blob, zvfs_spdk_blob_close_cb, ctx);
}

static void zvfs_spdk_blob_write_cb(void *cb_arg, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;
    SPDK_NOTICELOG("write complete\n");
    uint8_t *rbuffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL,
                                   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
    if (rbuffer == NULL)
    {
        spdk_app_stop(0);
        return;
    }
    memset(rbuffer, 0, BUFFER_SIZE);
    ctx->read_buffer = rbuffer;
    spdk_blob_io_read(ctx->blob, ctx->channel, rbuffer, 0, 1, zvfs_spdk_blob_read_cb, ctx);
}

static void zvfs_spdk_blob_sync_cb(void *cb_arg, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;
    uint8_t *wbuffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL,
                                   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
    if (wbuffer == NULL)
    {
        spdk_app_stop(0);
        return;
    }
    memset(wbuffer, 'A', BUFFER_SIZE);
    *(wbuffer + BUFFER_SIZE - 1) = '\0';
    ctx->write_buffer = wbuffer;
    ctx->channel = spdk_bs_alloc_io_channel(ctx->blobstore);
    if (ctx->channel == NULL)
    {
        spdk_app_stop(0);
        return;
    }
    spdk_blob_io_write(ctx->blob, ctx->channel, wbuffer, 0, 1, zvfs_spdk_blob_write_cb, ctx);
}

static void zvfs_spdk_blob_resize_cb(void *cb_arg, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;

    uint64_t total = spdk_blob_get_num_clusters(ctx->blob);
    ctx->num_cluster = total;
    SPDK_NOTICELOG("resize blob: %" PRIu64 "\n", total);
    spdk_blob_sync_md(ctx->blob, zvfs_spdk_blob_sync_cb, ctx);
}

static void zvfs_spdk_bs_open_blob_cb(void *cb_arg, struct spdk_blob *blb, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;


    ctx->blob = blb;
    uint64_t free_cluster = spdk_bs_free_cluster_count(ctx->blobstore);
    SPDK_NOTICELOG("free_cluster: %" PRIu64 "\n", free_cluster);

    ctx->free_cluster_count = free_cluster;
    spdk_blob_resize(blb, free_cluster, zvfs_spdk_blob_resize_cb, ctx);
}

static void zvfs_spdk_bs_create_blob_cb(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;

    ctx->blob_id = blobid;

    SPDK_NOTICELOG("blobid : %" PRIu64 "\n", blobid);
    spdk_bs_open_blob(ctx->blobstore, blobid, zvfs_spdk_bs_open_blob_cb, ctx);
}

static void zvfs_spdk_bs_init_cb(void *cb_arg, struct spdk_blob_store *bs, int bserrno)
{
    zvfs_context_t *ctx = (zvfs_context_t *)cb_arg;
    uint64_t io_unit_size = spdk_bs_get_io_unit_size(bs);
    ctx->unit_size = io_unit_size;
    ctx->blobstore = bs;
    SPDK_NOTICELOG("io_unit_size : %" PRIu64 "\n", io_unit_size);
    spdk_bs_create_blob(bs, zvfs_spdk_bs_create_blob_cb, ctx);
}

static void zvfs_spdk_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
    SPDK_NOTICELOG("zvfs_spdk_bdev_cb\n");
}

static void zvfs_entry(void *arg)
{
    zvfs_context_t *ctx = (zvfs_context_t *)arg;
    struct spdk_bs_dev *bs_dev = NULL;
    int rc = spdk_bdev_create_bs_dev_ext("Malloc0", zvfs_spdk_bdev_event_cb, NULL,
                                         &bs_dev);
    if (rc != 0)
    {
        spdk_app_stop(0);
    }
    spdk_bs_init(bs_dev, NULL, zvfs_spdk_bs_init_cb, ctx);
    SPDK_NOTICELOG("zvfs_entry\n");
}

// open

// read

// write

// close

#if 0

#define _GNU_SOURCE
#include <dlfcn.h>
#include<stdio.h>
//open


//read


//write


//close


typedef int (*open_t)(const char *pathname, int flags);
open_t open_f = NULL;
typedef ssize_t (*read_t)(int fd, void *buf, size_t count);
read_t read_f = NULL;
typedef ssize_t (*write_t)(int fd, const void *buf, size_t count);
write_t write_f = NULL;
typedef int (*close_t)(int fd);
close_t close_f = NULL;


int open(const char *pathname, int flags)
{
    if (open_f == NULL)
    {
        open_f = dlsym(RTLD_NEXT, "open");
    }
    printf("open..%s\n", pathname);
    return open_f(pathname, flags);
}



ssize_t write(int fd, const void *buf, size_t count)
{
    if (write_f == NULL)
    {
        write_f = dlsym(RTLD_NEXT, "write");
    }
    printf("write..\n");
    return write_f(fd, buf, count);
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (read_f == NULL)
    {
        read_f = dlsym(RTLD_NEXT, "read");
    }
    printf("read..\n");
    return read_f(fd, buf, count);
}


int close(int fd)
{
    if (close_f == NULL)
    {
        close_f = dlsym(RTLD_NEXT, "close");
    }
    printf("close..\n");
    return close_f(fd);

}



int main()
{
    int fd = open("a.txt", O_CREAT | O_RDWR);
    char *buffer = "abcdefghigklmnopqrstuvwxyz";
    write(fd, buffer, strlen(buffer));
    char *rbuffer[128] = {0};
    read(fd, buffer, 128);
    close(fd);
}

#elif 1



static const int WAITER_MAX_TIME = 100000;

static bool waiter(struct spdk_thread *thread, spdk_msg_fn start_fn, void *ctx, bool *finished)
{
    spdk_thread_send_msg(thread, start_fn, ctx);
    int wait_count = 0;
    do
    {
        spdk_thread_poll(thread, 0, 0);
        wait_count++;
    }
    while (!(*finished) && wait_count < WAITER_MAX_TIME);
    if (!(*finished) && wait_count >= WAITER_MAX_TIME)
    {
        return false; //timeout
    }
    return true;//finished

}

//zvfs.load


//static const char *json_file = "/home/gyq2/spdk/build/examples/zvfs.json";

static void json_app_load_done(int rc, void *ctx)
{
    bool *done = ctx;
    *done = true;
    SPDK_NOTICELOG("json_app_load_done\n");
}


static void zvfs_json_load_fn(void *arg)
{
    spdk_subsystem_init(json_app_load_done, arg);
}


int main(int argc, char *argv[])
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
}






#else




int main(int argc, char *argv[])
{
    if (argc != 2)
        return -1;
    struct spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "zvfs";
    opts.json_config_file = argv[1];
    zvfs_context_t *ctx = calloc(1, sizeof(zvfs_context_t));
    spdk_app_start(&opts, zvfs_entry, ctx);
    SPDK_NOTICELOG("hello spdk\n");
    return 0;
}

#endif