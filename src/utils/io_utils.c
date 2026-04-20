#include "io_utils.h"


void asyncVecioSynchronizerInit(AsyncVecioSynchronizer *this, uint64_t ioNum)
{
    atomic_init(&this->ioNum, ioNum);

    this->ioRes = COMM_CMD_SUCCESS;
    this->isCompleted = false;

    rtfsMutexInit(&this->mutex);
    rtfsCondInit(&this->cond);

    // 当不需要 I/O 时，AsyncVecioSynchronizer 也应该能正常工作。
    if (0 == atomic_load(&this->ioNum)) this->isCompleted = true;
}

void asyncVecioSynchronizerDestroy(AsyncVecioSynchronizer *this)
{
    if (NULL == this) return;

    rtfsCondDestroy(&this->cond);
    rtfsMutexDestroy(&this->mutex);
}

void asyncVecioSynchronizerCpltOnce(AsyncVecioSynchronizer *this, comm_cmd_result ioResult)
{
    uint64_t num = atomic_fetch_sub(&this->ioNum, 1);

    {
        rtfsMutexLock(&this->mutex);

        if (COMM_CMD_SUCCESS == this->ioRes) this->ioRes = ioResult;

        // ioNum 由调用线程减至 0，则向量 I/O 完成，发送通知。
        if (1 == num)
        {
            this->isCompleted = true;

            rtfsCondSignal(&this->cond);
        }

        rtfsMutexUnlock(&this->mutex);
    }
}

comm_cmd_result asyncVecioSynchronizerWaitCplt(AsyncVecioSynchronizer *this)
{
    comm_cmd_result res;

    {
        rtfsMutexLock(&this->mutex);

        while (!this->isCompleted) rtfsCondWait(&this->cond, &this->mutex);

        res = this->ioRes;

        rtfsMutexUnlock(&this->mutex);
    }


    return res;
}


void asyncVecioSynchronizerGenericCallback(comm_cmd_result res, void *arg)
{
    AsyncVecioSynchronizer *syr = (AsyncVecioSynchronizer *)arg;

    asyncVecioSynchronizerCpltOnce(syr, res);
}
