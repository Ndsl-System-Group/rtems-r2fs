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
int comm_submit_sync_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count, comm_io_direction dir)
{
    int ret = 0;
    rtems_status_code sc;
    rtems_blkdev_request *req = NULL;
    comm_sync_rw_ctx ctx;
    uint64_t total_bytes64;
    uint64_t end_lba;
    size_t req_size;

    if (dev == NULL || buffer == NULL)
        return EINVAL;

    if (dev->diskDevice == NULL)
        return ENODEV;

    if (dev->blockSize == 0 || dev->blockCount == 0)
        return EINVAL;

    if (dir != COMM_IO_READ && dir != COMM_IO_WRITE)
        return EINVAL;

    if (lba_count == 0)
        return 0;

    end_lba = lba + (uint64_t)lba_count;
    if (end_lba < lba) /* 加法溢出 */
        return EOVERFLOW;

    if (lba >= dev->blockCount || end_lba > dev->blockCount)
        return EINVAL;

    total_bytes64 = (uint64_t)lba_count * (uint64_t)dev->blockSize;
    if (dev->blockSize != 0 && total_bytes64 / (uint64_t)dev->blockSize != (uint64_t)lba_count)
        return EOVERFLOW;

    if (total_bytes64 > UINT32_MAX)
        return EOVERFLOW;

    req_size = sizeof(*req) + sizeof(rtems_blkdev_sg_buffer);
    req = (rtems_blkdev_request *)malloc(req_size);
    if (req == NULL)
        return ENOMEM;

    memset(req, 0, req_size);

    sc = rtems_semaphore_create(
        rtems_build_name('C', 'R', 'W', '0'),
        0,
        RTEMS_SIMPLE_BINARY_SEMAPHORE,
        0,
        &ctx.sem);
    if (sc != RTEMS_SUCCESSFUL)
    {
        free(req);
        return EIO;
    }

    ctx.status = RTEMS_IO_ERROR;

    req->req = (dir == COMM_IO_READ) ? RTEMS_BLKDEV_REQ_READ : RTEMS_BLKDEV_REQ_WRITE;
    req->done = comm_sync_rw_done;
    req->done_arg = &ctx;
    req->io_task = rtems_task_self();
    req->bufnum = 1;

    req->bufs[0].block = (rtems_blkdev_bnum)lba;
    req->bufs[0].length = (uint32_t)total_bytes64;
    req->bufs[0].buffer = buffer;
    req->bufs[0].user = NULL;

    /*
     * 向 RTEMS 块设备提交请求。
     * 这里返回值主要看“是否成功把请求送出去”，
     * 真正的传输结果在 done callback 里拿。
     */
    ret = rtems_blkdev_ioctl(dev->diskDevice, RTEMS_BLKIO_REQUEST, req);
    if (ret != 0)
    {
        rtems_semaphore_delete(ctx.sem);
        free(req);
        return EIO;
    }

    sc = rtems_semaphore_obtain(ctx.sem, RTEMS_WAIT, RTEMS_NO_TIMEOUT);
    rtems_semaphore_delete(ctx.sem);
    free(req);

    if (sc != RTEMS_SUCCESSFUL) return EIO;

    return (ctx.status == RTEMS_SUCCESSFUL) ? 0 : EIO;
}

int comm_submit_async_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count, comm_async_cb_func cb_func, void *cb_arg, comm_io_direction dir)
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
