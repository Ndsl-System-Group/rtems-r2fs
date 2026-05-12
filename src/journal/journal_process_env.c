#include "journal_process_env.h"

#include "uthash/utlist.h"

#include <stdlib.h>


// 日志处理线程入口。
extern void rtfsJournalProcessThread(struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos);

// 单例实例。
static JournalProcessEnv gEnv;
static bool gEnvTxIdBootstrapped = false;

// 日志处理线程参数。
typedef struct JournalProcessThreadArgs
{
    struct comm_dev *dev;

    uint64_t start;
    uint64_t end;
    uint64_t fifo;
} JournalProcessThreadArgs;

// 日志处理线程入口包装函数。Posix 的接口规定，所以需要包装一下。
static void *journalProcessThreadEntry(void *arg)
{
    JournalProcessThreadArgs *args = arg;

    rtfsJournalProcessThread(args->dev, args->start, args->end, args->fifo);

    free(args);


    return NULL;
}


JournalProcessEnv *journalProcessEnvGetInstance()
{
    if (!gEnvTxIdBootstrapped)
    {
        atomic_init(&gEnv.txIdToAlloc, 1);
        gEnvTxIdBootstrapped = true;
    }
    return &gEnv;
}

void journalProcessEnvInit(JournalProcessEnv *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos)
{
    rtfsMutexInit(&this->mtx);
    rtfsCondInit(&this->cond);

    this->commitQueueHead = NULL;
    this->exitReq = false;

    atomic_init(&this->txIdToAlloc, 1);

    JournalProcessThreadArgs *args = malloc(sizeof(*args));

    args->dev = dev;
    args->start = journalStartLpa;
    args->end = journalEndLpa;
    args->fifo = journalFifoPos;

    pthread_create(&this->processThreadHandle, NULL, journalProcessThreadEntry, args);
}

void journalProcessEnvDestroy(JournalProcessEnv *this)
{
    if (!this) return;

    {
        rtfsMutexLock(&this->mtx);
        this->exitReq = true;
        rtfsMutexUnlock(&this->mtx);
    }

    rtfsCondBroadcast(&this->cond);

    pthread_join(this->processThreadHandle, NULL);

    // 清理 commit queue。
    {
        rtfsMutexLock(&this->mtx);

        JournalCommitNode *el = NULL, *tmp = NULL;
        DL_FOREACH_SAFE(this->commitQueueHead, el, tmp)
        {
            DL_DELETE(this->commitQueueHead, el);
            free(el);
        }

        this->commitQueueHead = NULL;

        rtfsMutexUnlock(&this->mtx);
    }


    rtfsMutexDestroy(&this->mtx);
    rtfsCondDestroy(&this->cond);
}

// 同时超过 UNIT64_MAX 个事务运行则会分配重复 txId，暂不考虑这种情况。
uint64_t journalProcessEnvAllocTxId(JournalProcessEnv *this)
{
    return atomic_fetch_add_explicit(&this->txIdToAlloc, 1, memory_order_relaxed);
}

void journalProcessEnvCommitJournal(JournalProcessEnv *this, JournalContainer *journal)
{
    bool needNotify = false;

    {
        rtfsMutexLock(&this->mtx);

        needNotify = (this->commitQueueHead == NULL);

        JournalCommitNode *node = malloc(sizeof(*node));
        node->journal = journal;

        DL_APPEND(this->commitQueueHead, node);

        rtfsMutexUnlock(&this->mtx);
    }

    if (needNotify) rtfsCondBroadcast(&this->cond);
}

void journalProcessEnvStopProcessThread(JournalProcessEnv *this)
{
    {
        rtfsMutexLock(&this->mtx);
        this->exitReq = true;
        rtfsMutexUnlock(&this->mtx);
    }

    rtfsCondBroadcast(&this->cond);

    pthread_join(this->processThreadHandle, NULL);
}

bool journalProcessEnvIsCommitQueueEmpty(JournalProcessEnv *this)
{
    return this != NULL && this->commitQueueHead == NULL;
}

bool journalProcessEnvIsExitRequested(JournalProcessEnv *this)
{
    return this != NULL && this->exitReq;
}
