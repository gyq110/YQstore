#define _DEFAULT_SOURCE
#include <spdk/bdev.h>
#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/env.h>
#include <spdk/file.h>
#include <spdk/init.h>
#include <spdk/rpc.h>
#include <spdk/thread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct spdk_thread *g_thread;
static struct spdk_bs_dev *g_bs_dev;
static struct spdk_blob_store *g_bs;
static int g_err;
static bool g_done;
static const char *json_file = "/home/gyq2/spdk/build/examples/zvfs.json";

static bool waiter(struct spdk_thread *thread, spdk_msg_fn start_fn, void *ctx, bool *finished)
{
    int rc = spdk_thread_send_msg(thread, start_fn, ctx);
    if (rc != 0)
    {
        fprintf(stderr, "send message failed: rc=%d\n", rc);
        exit(EXIT_FAILURE);
    }

    while (!(*finished))
    {
        spdk_thread_poll(thread, 0, 0);
    }

    return true;
}

static void json_app_load_done(int rc, void *ctx)
{
    static bool json_started = false;

    if (rc != 0)
    {
        fprintf(stderr, "init/load JSON failed: rc=%d\n", rc);
        exit(EXIT_FAILURE);
    }

    if (!json_started)
    {
        size_t size = 0;
        void *json = spdk_posix_file_load_from_name(json_file, &size);
        if (json == NULL)
        {
            fprintf(stderr, "cannot read JSON: %s\n", json_file);
            exit(EXIT_FAILURE);
        }

        json_started = true;
        spdk_rpc_set_state(SPDK_RPC_RUNTIME);
        spdk_subsystem_load_config(json, size, json_app_load_done, ctx, true);
        free(json);
        return;
    }

    *(bool *)ctx = true;
}

static void json_load_fn(void *arg)
{
    spdk_subsystem_init(json_app_load_done, arg);
}

static void bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
    (void)type;
    (void)bdev;
    (void)ctx;
}

static void bs_init_done(void *cb_arg, struct spdk_blob_store *bs, int bserrno)
{
    g_bs = bs;
    g_err = bserrno;
    g_done = true;
}

static void bs_unload_done(void *cb_arg, int bserrno)
{
    g_err = bserrno;
    g_done = true;
}

static void clear_entry(void *arg)
{
    int rc = spdk_bdev_create_bs_dev_ext("Nvme0n1", bdev_event_cb, NULL, &g_bs_dev);
    if (rc != 0)
    {
        g_err = rc;
        g_done = true;
        return;
    }

    spdk_bs_init(g_bs_dev, NULL, bs_init_done, NULL);
}

int main(void)
{
    struct spdk_env_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.opts_size = sizeof(opts);
    spdk_env_opts_init(&opts);
    if (spdk_env_init(&opts) != 0)
    {
        fprintf(stderr, "spdk_env_init failed\n");
        return 1;
    }

    spdk_log_set_print_level(SPDK_LOG_WARN);
    spdk_log_set_level(SPDK_LOG_WARN);
    spdk_log_open(NULL);
    spdk_thread_lib_init(NULL, 0);
    g_thread = spdk_thread_create("clear", NULL);
    spdk_set_thread(g_thread);

    bool done = false;
    waiter(g_thread, json_load_fn, &done, &done);

    g_done = false;
    waiter(g_thread, clear_entry, NULL, &g_done);

    if (g_err != 0 || g_bs == NULL)
    {
        fprintf(stderr, "clear blobstore failed: rc=%d\n", g_err);
        return 1;
    }

    printf("blobstore cleared: cluster_size=%lu total=%lu free=%lu\n",
           (unsigned long)spdk_bs_get_cluster_size(g_bs),
           (unsigned long)spdk_bs_total_data_cluster_count(g_bs),
           (unsigned long)spdk_bs_free_cluster_count(g_bs));

    g_done = false;
    g_err = 0;
    spdk_bs_unload(g_bs, bs_unload_done, NULL);
    while (!g_done)
    {
        spdk_thread_poll(g_thread, 0, 0);
    }

    if (g_err != 0)
    {
        fprintf(stderr, "blobstore unload failed: rc=%d\n", g_err);
        return 1;
    }

    return 0;
}
