#include "journal_processor.h"

#include "utils/rtfs_log.h"
#include "uthash/utlist.h"


// 日志处理线程入口。
void rtfsJournalProcessThread(struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos)
{
    JournalProcessor processor;
    journalProcessorInit(&processor, dev, journalStartLpa, journalEndLpa, journalFifoPos);

    journalProcessorProcessJournal(&processor);

    RTFS_LOG(RTFS_LOG_INFO, "journal process thread exit.");
}


void transactionJournalRecordInit(TransactionJournalRecord *this, uint64_t txId, uint64_t startLpa, uint64_t endLpa)
{
    this->txId = txId;
    this->startLpa = startLpa;
    this->endLpa = endLpa;
}

uint64_t transactionJournalRecordGetTxId(TransactionJournalRecord *this)
{
    return this->txId;
}

uint64_t transactionJournalRecordGetStartLpa(TransactionJournalRecord *this)
{
    return this->startLpa;
}

uint64_t transactionJournalRecordGetEndLpa(TransactionJournalRecord *this)
{
    return this->endLpa;
}

bool transactionJournalRecordIsApplied(TransactionJournalRecord *this, uint64_t curHead, uint64_t curTail)
{
    if (this->startLpa < this->endLpa)
    {
        if (curHead <= curTail)
        {
            return curHead >= this->endLpa || curTail < this->startLpa;
        }
        else
        {
            return curHead >= this->endLpa;
        }
    }
    else
    {
        return curHead <= curTail && curHead >= this->endLpa;
    }
}


// 判断日志处理线程是否空闲。
// 当可用 lpa 等于整个日志区域时，说明所有已提交的日志均已应用完，不需要再轮询。当 journalList 和 curJournal 都为空时，从 journal commit queue 中取出的日志均已处理完毕。以上两个条件都满足，则日志处理线程空闲。
static bool journalProcessorIsWorking(JournalProcessor *this);

// 当日志处理线程空闲时，睡眠等待新的日志，否则尝试获取新的日志。当日志处理线程空闲、没有新日志可取，并且外部请求它退出时，就结束线程主循环，并且返回 true。
static bool journalProcessorFetchNewJournal(JournalProcessor *this);

// 处理位于提交队列首部的日志。
static void journalProcessorProcessPendingJournal(JournalProcessor *this);

// 使用 JournalWriter 把当前处理的日志收集到写缓存。
static void journalProcessorWriteJournalToBuffer(JournalProcessor *this);

// 尝试将日志记录写入 SSD，若因空间不足无法写，返回 false，否则返回 true。同时更新 SSD 尾指针和 tailLpa、curAvailLpa 成员。此操作是同步操作，阻塞到全部写完后再返回。
static bool journalProcessorWriteJournalToSsd(JournalProcessor *this);

// 生成本次处理的日志的事务记录。
static void journalProcessorGenerateTxRecord(JournalProcessor *this);

// 轮询并处理 SSD 已应用的日志记录。
static void journalProcessorProcessCpltJournal(JournalProcessor *this);

static void journalProcessorEnablePollTimer(JournalProcessor *this);
static void journalProcessorDisablePollTimer(JournalProcessor *this);
static void journalProcessorWaitPollTimer(JournalProcessor *this);

// 查询 SSD 侧日志头尾指针位置，同时更新 headLpa、curAvailLpa 成员。如果 SSD 侧应用了一部分日志，则返回 true，否则返回 false。
static bool journalProcessorSyncWithSsdJournalPos(JournalProcessor *this);

// 处理日志已经完成应用的事务。将所有已完成应用的事务移出 txRecord 链表，并通知淘汰保护模块。
static void journalProcessorProcessTxRecord(JournalProcessor *this);


void journalProcessorInit(JournalProcessor *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos)
{
    // TODO
    // this->journalPosDmaBuffer = static_cast<uint64_t *>(comm_alloc_dma_mem(16));
    if (NULL == this->journalPosDmaBuffer)
    {
        RTFS_LOG(RTFS_LOG_ERROR, "journal processor: not enough DMA buffer.");


        exit(EXIT_FAILURE);
    }

    // 将日志位置查询任务的定时器设置为阻塞式，到达轮询周期后，日志处理线程被唤醒并进行查询任务
    if (0 != rtfsTimerConstructor(&this->journalPollTimer, 1))
    {
        RTFS_LOG(RTFS_LOG_ERROR, "journal processor: init timer failed.");


        exit(EXIT_FAILURE);
    }

    // 日志位置查询任务，周期 100 us。
    struct timespec journalPollTime = {.tv_sec = 0, .tv_nsec = 100 * 1000};
    // 定时器设置为周期定时器。
    rtfsTimerSet(&this->journalPollTimer, &journalPollTime, 1);
    this->isPollTimerEnabled = false;

    // 由于 SSD 使用循环队列维护日志区域，当 headLpa == tailLpa 时视队列为空。
    // 所以 headLpa = (tailLpa + 1) % n 时队列满，n 为日志区域块数，实际可用块要减 1。
    this->curAvailLpa = this->totalAvailLpa = journalEndLpa - journalStartLpa - 1;

    this->dev = dev;
    this->headLpa = this->tailLpa = journalFifoPos;
    this->startLpa = journalStartLpa;
    this->endLpa = journalEndLpa;
    this->curJournal = NULL;
}

void journalProcessorDestroy(JournalProcessor *this)
{
    // TODO
    // comm_free_dma_mem(journal_pos_dma_buffer);
    rtfsTimerStop(&this->journalPollTimer);
    rtfsTimerDestructor(&this->journalPollTimer);
}

void journalProcessorProcessJournal(JournalProcessor *this)
{
    while (true)
    {
        if (journalProcessorFetchNewJournal(this)) break;

        journalProcessorProcessPendingJournal(this);
        journalProcessorProcessCpltJournal(this);
    }
}


bool journalProcessorIsWorking(JournalProcessor *this)
{
    return !(this->curAvailLpa == this->totalAvailLpa && NULL == this->pendingJournalListHead && NULL == this->curJournal);
}

// TODO
bool journalProcessorFetchNewJournal(JournalProcessor *this)
{
}

void journalProcessorProcessPendingJournal(JournalProcessor *this)
{
}

void journalProcessorWriteJournalToBuffer(JournalProcessor *this)
{
    journalWriterSetPendingJournal(&this->journalWriter, this->curJournal);

    this->curJournalBlockNum = journalWriterCollectPendingJournalToWriteBuffer(&this->journalWriter);
    this->curProcState = JOURNAL_PROCESS_WRITTEN_IN_BUFFER;
}

bool journalProcessorWriteJournalToSsd(JournalProcessor *this)
{
}

void journalProcessorGenerateTxRecord(JournalProcessor *this)
{
    TxRecordNode *node = (struct TxRecordNode *)malloc(sizeof(struct TxRecordNode));
    if (!node)
    {
        RTFS_LOG(RTFS_LOG_ERROR, "journal processor generate txRecord: error when allocating TxRecordNode");


        exit(EXIT_FAILURE);
    }

    transactionJournalRecordInit(&node->record, journalContainerGetTxId(this->curJournal), this->curJournalStartLpa, this->curJournalEndLpa);

    node->prev = node->next = NULL;

    DL_APPEND(this->txRecordHead, node);
}

void journalProcessorProcessCpltJournal(JournalProcessor *this)
{
    if (this->curAvailLpa == this->totalAvailLpa)
    {
        journalProcessorDisablePollTimer(this);


        return;
    }
    journalProcessorEnablePollTimer(this);
    journalProcessorWaitPollTimer(this);

    // 如果 SSD 应用了一部分日志，则确认哪些事务日志已经应用完，并做相应处理。
    if (journalProcessorSyncWithSsdJournalPos(this)) journalProcessorProcessTxRecord(this);
}

void journalProcessorEnablePollTimer(JournalProcessor *this)
{
    if (this->isPollTimerEnabled) return;

    if (0 != rtfsTimerStart(&this->journalPollTimer))
    {
        RTFS_LOG(RTFS_LOG_ERROR, "journal processor: enable timer failed.");


        exit(EXIT_FAILURE);
    }

    this->isPollTimerEnabled = true;
}

void journalProcessorDisablePollTimer(JournalProcessor *this)
{
    if (!this->isPollTimerEnabled) return;

    if (0 != rtfsTimerStop(&this->journalPollTimer))
    {
        RTFS_LOG(RTFS_LOG_ERROR, "journal processor: disable timer failed.");


        exit(EXIT_FAILURE);
    }

    this->isPollTimerEnabled = false;
}

void journalProcessorWaitPollTimer(JournalProcessor *this)
{
    if (0 != rtfsTimerCheckExpire(&this->journalPollTimer, NULL))
    {
        RTFS_LOG(RTFS_LOG_ERROR, "journal processor: wait timer failed.");


        exit(EXIT_FAILURE);
    }
}

bool journalProcessorSyncWithSsdJournalPos(JournalProcessor *this)
{
    // TODO
    // if (comm_submit_sync_get_metajournal_head_request(dev, journal_pos_dma_buffer) != 0) throw io_error("journal processor: submit get journal pos failed.");

    uint64_t newHeadLpa = this->journalPosDmaBuffer[0];
    uint64_t newAvailLpa = newHeadLpa >= this->headLpa ? newHeadLpa - this->headLpa : newHeadLpa + this->endLpa - this->startLpa - this->headLpa;

    this->headLpa = newHeadLpa;
    this->curAvailLpa += newAvailLpa;


    return newAvailLpa;
}

void journalProcessorProcessTxRecord(JournalProcessor *this)
{
}
