#include "journal_process_env.h"

#include "uthash/utlist.h"

#include <stdlib.h>


// 日志处理线程入口。
extern void rtfsJournalProcessThread(struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos);

// 单例实例。
static JournalProcessEnv gEnv;

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
    return &gEnv;
}

void journalProcessEnvDestroy(JournalProcessEnv *this)
{
    if (!this) return;

    {
        pthread_mutex_lock(&this->mtx);
        this->exitReq = true;
        pthread_mutex_unlock(&this->mtx);
    }

    pthread_cond_broadcast(&this->cond);

    pthread_join(this->processThreadHandle, NULL);

    pthread_mutex_destroy(&this->mtx);
    pthread_cond_destroy(&this->cond);
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
        pthread_mutex_lock(&this->mtx);

        needNotify = (this->commitQueueHead == NULL);

        JournalCommitNode *node = malloc(sizeof(*node));
        node->journal = journal;

        DL_APPEND(this->commitQueueHead, node);

        pthread_mutex_unlock(&this->mtx);
    }

    if (needNotify) pthread_cond_broadcast(&this->cond);
}

void journalProcessEnvInit(JournalProcessEnv *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos)
{
    pthread_mutex_init(&this->mtx, NULL);
    pthread_cond_init(&this->cond, NULL);

    this->commitQueueHead = NULL;
    this->exitReq = false;

    atomic_init(&this->txIdToAlloc, 0);

    JournalProcessThreadArgs *args = malloc(sizeof(*args));

    args->dev = dev;
    args->start = journalStartLpa;
    args->end = journalEndLpa;
    args->fifo = journalFifoPos;

    pthread_create(&this->processThreadHandle, NULL, journalProcessThreadEntry, args);
}

void journalProcessEnvStopProcessThread(JournalProcessEnv *this)
{
    {
        pthread_mutex_lock(&this->mtx);
        this->exitReq = true;
        pthread_mutex_unlock(&this->mtx);
    }

    pthread_cond_broadcast(&this->cond);

    pthread_join(this->processThreadHandle, NULL);
}
