#include "comm_api.h"

#include "communication/dev.h"

#include <errno.h>
#include <stdlib.h>
#include <memory.h>


typedef struct comm_sync_rw_ctx
{
    rtems_id sem;
    rtems_status_code status;
} comm_sync_rw_ctx;

static void comm_sync_rw_done(rtems_blkdev_request *req, rtems_status_code status)
{
    comm_sync_rw_ctx *ctx = (comm_sync_rw_ctx *)req->done_arg;

    if (ctx != NULL)
    {
        ctx->status = status;
        rtems_semaphore_release(ctx->sem);
    }
}


// TODO
int comm_submit_sync_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_io_direction dir)
{
    if (NULL == dev || NULL == buffer) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;
    if (0 == dev->blockSize || 0 == dev->blockCount) return EINVAL;
    if (COMM_IO_READ != dir && COMM_IO_WRITE != dir) return EINVAL;
    if (0 == lbaCount) return 0;

    uint64_t endLba = lba + (uint64_t)lbaCount;
    if (endLba < lba) return EOVERFLOW; // 加法溢出。

    if (lba >= dev->blockCount || endLba > dev->blockCount) return EINVAL;

    uint64_t totalBytes64 = (uint64_t)lbaCount * (uint64_t)dev->blockSize;
    if (0 != dev->blockSize && totalBytes64 / (uint64_t)dev->blockSize != (uint64_t)lbaCount) return EOVERFLOW;

    if (totalBytes64 > UINT32_MAX) return EOVERFLOW;

    size_t reqSize = sizeof(rtems_blkdev_request) + sizeof(rtems_blkdev_sg_buffer);

    rtems_blkdev_request *req = (rtems_blkdev_request *)malloc(reqSize);
    if (NULL == req) return ENOMEM;
    memset(req, 0, reqSize);

    comm_sync_rw_ctx ctx;
    rtems_status_code sc = rtems_semaphore_create(rtems_build_name('C', 'R', 'W', '0'), 0, RTEMS_SIMPLE_BINARY_SEMAPHORE, 0, &ctx.sem);
    if (RTEMS_SUCCESSFUL != sc)
    {
        free(req);


        return EIO;
    }

    ctx.status = RTEMS_IO_ERROR;

    req->req = (COMM_IO_READ == dir) ? RTEMS_BLKDEV_REQ_READ : RTEMS_BLKDEV_REQ_WRITE;
    req->done = comm_sync_rw_done;
    req->done_arg = &ctx;
    req->io_task = rtems_task_self();
    req->bufnum = 1;

    req->bufs[0].block = (rtems_blkdev_bnum)lba;
    req->bufs[0].length = (uint32_t)totalBytes64;
    req->bufs[0].buffer = buffer;
    req->bufs[0].user = NULL;

    // 向 RTEMS 块设备提交请求。这里返回值主要看“是否成功把请求送出去”，真正的传输结果在 done callback 里拿。
    int res = rtems_blkdev_ioctl(dev->diskDevice, RTEMS_BLKIO_REQUEST, req);
    if (0 != res)
    {
        rtems_semaphore_delete(ctx.sem);
        free(req);


        return EIO;
    }

    sc = rtems_semaphore_obtain(ctx.sem, RTEMS_WAIT, RTEMS_NO_TIMEOUT);
    rtems_semaphore_delete(ctx.sem);
    free(req);

    if (RTEMS_SUCCESSFUL != sc) return EIO;


    return (RTEMS_SUCCESSFUL == ctx.status) ? 0 : EIO;
}

int comm_submit_async_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_async_cb_func cb_func, void *cb_arg, comm_io_direction dir)
{
}

// int comm_submit_sync_migrate_request(struct comm_dev *dev, migrate_task *task)
// {
// }

// int comm_submit_async_migrate_request(struct comm_dev *dev, migrate_task *task, comm_async_cb_func cb_func, void *cb_arg)
// {
// }

// int comm_submit_sync_path_lookup_request(struct comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res)
// {
// }

// int comm_submit_async_path_lookup_request(struct comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res, comm_async_cb_func cb_func, void *cb_arg)
// {
// }

// int comm_submit_sync_filemapping_search_request(struct comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len)
// {
// }

// int comm_submit_async_filemapping_search_request(struct comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len, comm_async_cb_func cb_func, void *cb_arg)
// {
// }

int comm_submit_sync_update_metajournal_tail_request(struct comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num)
{
}

int comm_submit_async_update_metajournal_tail_request(struct comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num, comm_async_cb_func cb_func, void *cb_arg)
{
}

int comm_submit_sync_get_metajournal_head_request(struct comm_dev *dev, uint64_t *head_lpa)
{
}

int comm_submit_async_get_metajournal_head_request(struct comm_dev *dev, uint64_t *head_lpa, comm_async_cb_func cb_func, void *cb_arg)
{
}

int comm_submit_fs_module_init_request(struct comm_dev *dev)
{
}

int comm_submit_fs_db_init_request(struct comm_dev *dev)
{
}

int comm_submit_fs_recover_from_db_request(struct comm_dev *dev)
{
}

int comm_submit_clear_metajournal_request(struct comm_dev *dev)
{
}

int comm_submit_start_apply_journal_request(struct comm_dev *dev)
{
}

int comm_submit_stop_apply_journal_request(struct comm_dev *dev)
{
}
