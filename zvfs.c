#include<stdio.h>
#include<spdk/event.h>
#include<spdk/blob.h>
#include<spdk/bdev.h>
#include<spdk/blob_bdev.h>
#include<spdk/env.h>


#define BUFFER_SIZE 512
//nvme bdev blob blobstore channel


struct spdk_blob_store *blobstore;
spdk_blob_id blob_id;
struct spdk_blob *blob;
uint64_t free_cluster_count=0;
uint64_t num_cluster;
uint8_t *write_buffer;
uint8_t *read_buffer;
struct spdk_io_channel *channel;


static void zvfs_spdk_bs_unload_cb(void *cb_arg,int bserrno){
    spdk_app_stop(0);
}


static void zvfs_spdk_blob_delete_cb(void *cb_arg,int bserrno){
if(blobstore){
    if(channel){
        spdk_bs_free_io_channel(channel);
    }
    spdk_bs_unload(blobstore,zvfs_spdk_bs_unload_cb,NULL);
}
}


static void zvfs_spdk_blob_close_cb(void *cb_arg,int bserrno){
    spdk_bs_delete_blob(blobstore,blob_id,zvfs_spdk_blob_delete_cb,NULL);
}



static void zvfs_spdk_blob_read_cb(void *cb_arg,int bserrno){

    SPDK_NOTICELOG("read buffer: %s\n",read_buffer);    
    spdk_blob_close(blob,zvfs_spdk_blob_close_cb,NULL);
}


static void zvfs_spdk_blob_write_cb(void *cb_arg,int bserrno){
    SPDK_NOTICELOG("write complete\n");
    uint8_t *rbuffer=spdk_malloc(BUFFER_SIZE,0x1000,NULL,SPDK_ENV_LCORE_ID_ANY,SPDK_MALLOC_DMA);
    if(rbuffer==NULL){
        spdk_app_stop(0);
        return ;
    }
        memset(rbuffer,0,BUFFER_SIZE);
    read_buffer=rbuffer;
    spdk_blob_io_read(blob,channel,rbuffer,0,1,zvfs_spdk_blob_read_cb,NULL);    
}

static void zvfs_spdk_blob_sync_cb(void *cb_arg,int bserrno){
    uint8_t *wbuffer=spdk_malloc(BUFFER_SIZE,0x1000,NULL,SPDK_ENV_LCORE_ID_ANY,SPDK_MALLOC_DMA);
    if(wbuffer==NULL){
        spdk_app_stop(0);
        return ;
    }
    memset(wbuffer,"A",BUFFER_SIZE);
    *(wbuffer+BUFFER_SIZE-1)='\0';
    write_buffer=wbuffer;
    channel=spdk_bs_alloc_io_channel(blobstore);
    if(channel==NULL){
        spdk_app_stop(0);
        return ;
    }
    spdk_blob_io_write(blob,channel,wbuffer,0,1,zvfs_spdk_blob_write_cb,NULL);
}

static void zvfs_spdk_blob_resize_cb(void *cb_arg,int bserrno){
    struct spdk_blob *blb=(struct spdk_blob *)cb_arg;
    uint64_t total=spdk_blob_get_num_clusters(blb);
    num_cluster=total;
    SPDK_NOTICELOG("resize blob: %"PRIu64"\n",total);
    spdk_blob_sync_md(blb,zvfs_spdk_blob_sync_cb,NULL);
}


static void zvfs_spdk_bs_open_blob_cb(void *cb_arg,struct spdk_blob *blb,int bserrno){
    struct spdk_blob_store *bs=( struct spdk_blob_store*)cb_arg;
    blob=blb;
    uint64_t free_cluster=spdk_bs_free_cluster_count(bs);
    SPDK_NOTICELOG("free_cluster: %"PRIu64"\n",free_cluster);
    free_cluster_count=free_cluster;
    spdk_blob_resize(blb,free_cluster,zvfs_spdk_blob_resize_cb,blb);
}



static void zvfs_spdk_bs_create_blob_cb(void *cb_arg,spdk_blob_id blobid,int bserrno){
    struct spdk_blob_store *bs=( struct spdk_blob_store*)cb_arg;
    blob_id=blobid;

    SPDK_NOTICELOG("blobid : %"PRIu64"\n",blobid);
    spdk_bs_open_blob(bs,blobid,zvfs_spdk_bs_open_blob_cb,bs);
}


static void zvfs_spdk_bs_init_cb(void *cb_arg,struct spdk_blob_store *bs,int bserrno){
    uint64_t io_unit_size=spdk_bs_get_io_unit_size(bs);
    blobstore=bs;
    SPDK_NOTICELOG("io_unit_size : %"PRIu64"\n",io_unit_size);
    spdk_bs_create_blob(bs,zvfs_spdk_bs_create_blob_cb,bs);

}

static void zvfs_spdk_bdev_event_cb(enum spdk_bdev_event_type type,struct spdk_bdev *bdev,void *event_ctx){
    SPDK_NOTICELOG("zvfs_spdk_bdev_cb\n");
}


static void zvfs_entry(void *ctx){

    struct spdk_bs_dev *bs_dev=NULL;
    int rc=spdk_bdev_create_bs_dev_ext("Malloc0",zvfs_spdk_bdev_event_cb,NULL,&bs_dev);
    if(rc!=0){
        spdk_app_stop(0);
    }
    spdk_bs_init(bs_dev,NULL,zvfs_spdk_bs_init_cb,NULL);
    SPDK_NOTICELOG("zvfs_entry\n");
}


//open 



//read




//write




//close


int main(int argc ,char  *argv[])
{
    if(argc!=2) return -1;
    struct spdk_app_opts opts={};
    spdk_app_opts_init(&opts,sizeof(opts));
    opts.name = "zvfs";
    opts.json_config_file=argv[1];
    spdk_app_start(&opts,zvfs_entry,NULL);
    SPDK_NOTICELOG("hello spdk\n");
    return 0;
}