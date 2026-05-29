#include "comm_api.h"

#include "communication/dev.h"
#include "utils/rtfs_log.h"

#include <errno.h>
#include <stdlib.h>
#include <memory.h>


typedef struct comm_sync_rw_ctx
{
    rtems_id sem;
    rtems_status_code status;
} comm_sync_rw_ctx;

typedef struct comm_async_ctx
{
    comm_async_cb_func user_cb;
    void *user_arg;
} comm_async_ctx;

static comm_test_sync_rw_hook g_comm_test_sync_rw_hook = NULL;
static comm_test_async_rw_hook g_comm_test_async_rw_hook = NULL;
static comm_test_get_metajournal_head_hook g_comm_test_get_metajournal_head_hook = NULL;
static comm_test_update_metajournal_tail_hook g_comm_test_update_metajournal_tail_hook = NULL;
static comm_test_fs_recover_hook g_comm_test_fs_recover_hook = NULL;


static void comm_sync_rw_done(rtems_blkdev_request *req, rtems_status_code status);

static void comm_async_rw_done(rtems_blkdev_request *req, rtems_status_code status);

// syncCtx 是同步请求时使用的上下文，asyncCtx 是异步请求时使用的上下文。
static int comm_submit_rw_request_common(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_io_direction dir, comm_sync_rw_ctx *syncCtx, comm_async_ctx **asyncCtx);


int comm_submit_sync_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_io_direction dir)
{
    if (g_comm_test_sync_rw_hook != NULL) {
        return g_comm_test_sync_rw_hook(dev, buffer, lba, lbaCount, dir);
    }

    comm_sync_rw_ctx syncCtx;
    int res = comm_submit_rw_request_common(dev, buffer, lba, lbaCount, dir, &syncCtx, NULL);
    RTFS_LOG(RTFS_LOG_DEBUG, "comm_submit_sync_rw_request res: %d", res);
    if (0 != res) return res;

    rtems_semaphore_obtain(syncCtx.sem, RTEMS_WAIT, RTEMS_MILLISECONDS_TO_TICKS(5000));
    rtems_semaphore_delete(syncCtx.sem);


    return (RTEMS_SUCCESSFUL == syncCtx.status) ? 0 : EIO;
}

int comm_submit_async_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_async_cb_func cbFunc, void *cbArg, comm_io_direction dir)
{
    if (g_comm_test_async_rw_hook != NULL) {
        return g_comm_test_async_rw_hook(dev, buffer, lba, lbaCount, cbFunc, cbArg, dir);
    }

    comm_async_ctx *ctx = (comm_async_ctx *)malloc(sizeof(comm_async_ctx));
    if (NULL == ctx) return ENOMEM;

    ctx->user_cb = cbFunc;
    ctx->user_arg = cbArg;

    int res = comm_submit_rw_request_common(dev, buffer, lba, lbaCount, dir, NULL, &ctx);
    RTFS_LOG(RTFS_LOG_DEBUG, "comm_submit_async_rw_request res: %d", res);
    if (0 != res)
    {
        RTFS_LOG(
            RTFS_LOG_WARNING,
            "async rw submit failed res=%d dev=%p disk=%p lba=%llu lbaCount=%u dir=%d",
            res,
            (void *)dev,
            dev != NULL ? (void *)dev->diskDevice : NULL,
            (unsigned long long)lba,
            (unsigned int)lbaCount,
            (int)dir
        );
        free(ctx);


        return res;
    }


    return 0;
}

// int comm_submit_sync_migrate_request(struct comm_dev *dev, migrate_task *task)
// {
// }

// int comm_submit_async_migrate_request(struct comm_dev *dev, migrate_task *task, comm_async_cb_func cbFunc, void *cbArg)
// {
// }

// int comm_submit_sync_path_lookup_request(struct comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res)
// {
// }

// int comm_submit_async_path_lookup_request(struct comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res, comm_async_cb_func cbFunc, void *cbArg)
// {
// }

// int comm_submit_sync_filemapping_search_request(struct comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len)
// {
// }

// int comm_submit_async_filemapping_search_request(struct comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len, comm_async_cb_func cbFunc, void *cbArg)
// {
// }

int comm_submit_sync_update_metajournal_tail_request(struct comm_dev *dev, uint64_t originLpa, uint32_t writeBlockNum)
{
    if (g_comm_test_update_metajournal_tail_hook != NULL) {
        return g_comm_test_update_metajournal_tail_hook(dev, originLpa, writeBlockNum);
    }

    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;
    if (0 == writeBlockNum) return 0;
    if (dev->metaJournalEndLpa <= dev->metaJournalStartLpa) return EINVAL;

    int res = rtfsMutexLock(&dev->metaJournalMutex);
    if (0 != res) return res;

    int result = 0;

    // 这里要求 originLpa 必须等于当前 tail。也就是说，调用者提交的这批 journal 必须紧跟着当前尾部写入。
    // 先检查 originLpa 是否落在 journal 合法范围内。
    if (originLpa < dev->metaJournalStartLpa || originLpa >= dev->metaJournalEndLpa)
    {
        result = EINVAL;
        goto out;
    }

    // 再检查它是否等于当前 tail。
    if (originLpa != dev->metaJournalTailLpa)
    {
        result = EINVAL;
        goto out;
    }

    // journal 区间长度，按环形缓冲区处理。
    uint64_t journalSize = dev->metaJournalEndLpa - dev->metaJournalStartLpa;
    if (0 == journalSize)
    {
        result = EINVAL;
        goto out;
    }

    // 计算 tail 前移后的新位置。
    uint64_t offset = originLpa - dev->metaJournalStartLpa;
    uint64_t step = (uint64_t)writeBlockNum % journalSize;
    uint64_t newOffset = offset + step;

    // 环形回绕。
    if (newOffset >= journalSize) newOffset -= journalSize;

    dev->metaJournalTailLpa = dev->metaJournalStartLpa + newOffset;

    /*
     * 当前简化实现没有独立的设备侧 apply-journal 线程。
     * 因此一旦尾指针推进，就视为这批 journal 已经被设备“消费”，
     * 需要同步推进头指针，否则 journal processor 会一直认为
     * 仍有未完成事务，卸载阶段会卡在后台线程退出上。
     */
    dev->metaJournalHeadLpa = dev->metaJournalTailLpa;

out:
    res = rtfsMutexUnlock(&dev->metaJournalMutex);
    if (0 == result && 0 != res) result = res;


    return result;
}

int comm_submit_async_update_metajournal_tail_request(struct comm_dev *dev, uint64_t originLpa, uint32_t writeBlockNum, comm_async_cb_func cbFunc, void *cbArg)
{
    // 简化版异步：先同步完成状态更新，再立刻触发回调。
    int res = comm_submit_sync_update_metajournal_tail_request(dev, originLpa, writeBlockNum);

    if (NULL != cbFunc) cbFunc((0 == res) ? COMM_CMD_SUCCESS : COMM_CMD_CQE_ERROR, cbArg);


    return res;
}

int comm_submit_sync_get_metajournal_head_request(struct comm_dev *dev, uint64_t *headLpa)
{
    if (g_comm_test_get_metajournal_head_hook != NULL) {
        return g_comm_test_get_metajournal_head_hook(dev, headLpa);
    }

    if (NULL == dev || NULL == headLpa) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 读取 head 时同样加锁，避免并发更新导致状态不一致。
    int res = rtfsMutexLock(&dev->metaJournalMutex);
    if (0 != res) return res;

    *headLpa = dev->metaJournalHeadLpa;

    res = rtfsMutexUnlock(&dev->metaJournalMutex);
    if (0 != res) return res;


    return 0;
}

int comm_submit_async_get_metajournal_head_request(struct comm_dev *dev, uint64_t *headLpa, comm_async_cb_func cbFunc, void *cbArg)
{
    // 简化版异步：先读 head，再立即回调。
    int res = comm_submit_sync_get_metajournal_head_request(dev, headLpa);

    if (NULL != cbFunc) cbFunc((0 == res) ? COMM_CMD_SUCCESS : COMM_CMD_CQE_ERROR, cbArg);


    return res;
}

int comm_submit_fs_module_init_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;
    if (0 == dev->blockSize) return EINVAL;
    if (0 == dev->blockCount) return EINVAL;

    if (dev->metaJournalStartLpa >= dev->metaJournalEndLpa) return EINVAL;
    if (dev->metaJournalEndLpa > dev->blockCount) return EINVAL;


    return 0;
}

int comm_submit_fs_db_init_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 初始化元数据数据库。
    // 简化版实现为重置 meta journal 状态。
    int res = rtfsMutexLock(&dev->metaJournalMutex);
    if (0 != res) return res;

    dev->metaJournalHeadLpa = dev->metaJournalStartLpa;
    dev->metaJournalTailLpa = dev->metaJournalStartLpa;

    res = rtfsMutexUnlock(&dev->metaJournalMutex);
    if (0 != res) return res;

    return 0;
}

int comm_submit_fs_recover_from_db_request(comm_dev *dev)
{
    if (g_comm_test_fs_recover_hook != NULL) {
        return g_comm_test_fs_recover_hook(dev);
    }

    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 简化版暂无真实数据库恢复逻辑。
    // 后续可在此处增加 journal 扫描与回放流程。
    return 0;
}

int comm_submit_clear_metajournal_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 清空 meta journal：head/tail 回到起始位置。
    int res = rtfsMutexLock(&dev->metaJournalMutex);
    if (0 != res) return res;

    dev->metaJournalHeadLpa = dev->metaJournalStartLpa;
    dev->metaJournalTailLpa = dev->metaJournalStartLpa;

    res = rtfsMutexUnlock(&dev->metaJournalMutex);
    if (0 != res) return res;

    return 0;
}

int comm_submit_start_apply_journal_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 简化版暂无后台 apply journal 线程。
    // 保留接口供后续扩展。
    return 0;
}

int comm_submit_stop_apply_journal_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 简化版暂无后台 apply journal 线程。
    // 保留接口供后续扩展。
    return 0;
}

void commSetTestSyncRwHook(comm_test_sync_rw_hook hook)
{
    g_comm_test_sync_rw_hook = hook;
}

void commSetTestAsyncRwHook(comm_test_async_rw_hook hook)
{
    g_comm_test_async_rw_hook = hook;
}

void commSetTestGetMetaJournalHeadHook(comm_test_get_metajournal_head_hook hook)
{
    g_comm_test_get_metajournal_head_hook = hook;
}

void commSetTestUpdateMetaJournalTailHook(comm_test_update_metajournal_tail_hook hook)
{
    g_comm_test_update_metajournal_tail_hook = hook;
}

void commSetTestFsRecoverHook(comm_test_fs_recover_hook hook)
{
    g_comm_test_fs_recover_hook = hook;
}


void comm_sync_rw_done(rtems_blkdev_request *req, rtems_status_code status)
{
    comm_sync_rw_ctx *ctx = (comm_sync_rw_ctx *)req->done_arg;

    if (NULL != ctx)
    {
        ctx->status = status;
        rtems_semaphore_release(ctx->sem);
    }

    // request 已完成，释放申请的内存。
    free(req);
}

void comm_async_rw_done(rtems_blkdev_request *req, rtems_status_code status)
{
    comm_async_ctx *ctx = (comm_async_ctx *)req->done_arg;

    if (NULL != ctx && NULL != ctx->user_cb)
    {
        // 完成后触发用户回调，传递命令结果和上下文参数。
        if (RTEMS_SUCCESSFUL == status)
        {
            ctx->user_cb(COMM_CMD_SUCCESS, ctx->user_arg);
        }
        else
        {
            ctx->user_cb(COMM_CMD_CQE_ERROR, ctx->user_arg);
        }

        free(ctx); // 回调后释放资源。
    }

    // request 已完成，释放申请的内存。
    free(req);
}

int comm_submit_rw_request_common(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_io_direction dir, comm_sync_rw_ctx *syncCtx, comm_async_ctx **asyncCtx)
{
    if (NULL == dev || NULL == buffer) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;
    if (NULL == dev->diskDevice->ioctl) return ENODEV;
    if (NULL == dev->diskDevice->phys_dev) return ENODEV;
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

    // 处理同步请求。
    if (NULL != syncCtx)
    {
        rtems_status_code sc = rtems_semaphore_create(rtems_build_name('C', 'R', 'W', '0'), 0, RTEMS_SIMPLE_BINARY_SEMAPHORE, 0, &syncCtx->sem);
        if (RTEMS_SUCCESSFUL != sc)
        {
            free(req);


            return EIO;
        }

        syncCtx->status = RTEMS_IO_ERROR;
        req->done = comm_sync_rw_done;
        req->done_arg = syncCtx;
    }
    // 处理异步请求。
    else if (NULL != asyncCtx)
    {
        req->done = comm_async_rw_done;
        req->done_arg = *asyncCtx;
    }
    else
    {
        free(req);


        return EINVAL;
    }

    req->io_task = rtems_task_self();
    req->bufnum = 1;
    req->req = (dir == COMM_IO_READ) ? RTEMS_BLKDEV_REQ_READ : RTEMS_BLKDEV_REQ_WRITE;

    req->bufs[0].block = (rtems_blkdev_bnum)lba;
    req->bufs[0].length = (uint32_t)totalBytes64;
    req->bufs[0].buffer = buffer;
    req->bufs[0].user = NULL;

    // 向 RTEMS 块设备提交请求。这里返回值主要看“是否成功把请求送出去”，真正的传输结果在 done callback 里拿。
    int res = dev->diskDevice->ioctl(
        dev->diskDevice->phys_dev,
        RTEMS_BLKIO_REQUEST,
        req
    );
    if (0 != res)
    {
        int saved_errno = errno;
        free(req);

        return saved_errno != 0 ? saved_errno : EIO;
    }


    return 0;
}
