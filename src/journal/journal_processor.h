#ifndef _JOURNAL_PROCESSOR_H_
#define _JOURNAL_PROCESSOR_H_

#include "journal/journal_container.h"
#include "journal/journal_process_env.h"
#include "journal/journal_writer.h"

#include "utils/types.h"
#include "utils/rtfs_timer.h"


struct comm_dev;


/**
 * @brief 事务日志记录，一个对象代表一个事务，记录该事务日志在 SSD 上持久化的 LPA 范围 [startLpa, endLpa)。
 */
typedef struct TransactionJournalRecord
{
    uint64_t txId;
    uint64_t startLpa;
    uint64_t endLpa;
} TransactionJournalRecord;


void transactionJournalRecordInit(TransactionJournalRecord *this, uint64_t txId, uint64_t startLpa, uint64_t endLpa);

uint64_t transactionJournalRecordGetTxId(TransactionJournalRecord *this);

uint64_t transactionJournalRecordGetStartLpa(TransactionJournalRecord *this);

uint64_t transactionJournalRecordGetEndLpa(TransactionJournalRecord *this);

/**
 * @brief 日志处理线程中，只有在事务日志记录移除后，才释放该事务占用的日志区域资源，增加 curAvailLpa。所以，在此事务日志记录未移除时，curTail 不会二次进入 [startLpa, endLpa) 区域。可以用如下方法判断日志是否已经应用完。
 */
bool transactionJournalRecordIsApplied(TransactionJournalRecord *this, uint64_t curHead, uint64_t curTail);


/**
 * @brief 当前日志记录处理状态。
 */
typedef enum JournalProcessState
{
    JOURNAL_PROCESS_NEWLY_FETCHED,    // 获取到，但还没写入 buffer。
    JOURNAL_PROCESS_WRITTEN_IN_BUFFER // 写入 buffer，但还没写入 SSD（可能由于 SSD 侧日志空间不足）。
} JournalProcessState;

/**
 * @brief 日志处理线程与其工作环境。系统确保此环境构造时，已完成故障恢复，因此可用日志资源为整个 SSD 日志区域。日志处理线程中，写日志、写日志尾指针、查询 SSD 日志位置，都使用同步阻塞式 I/O。
 */
typedef struct JournalProcessor
{
    struct comm_dev *dev;

    uint64_t headLpa, tailLpa;           // 当前日志区域 FIFO 的首尾 LPA：[headLpa, tailLpa)。
    uint64_t startLpa, endLpa;           // SSD 日志区域起止 LPA：[startLpa, endLpa)。
    uint64_t *journalPosDmaBuffer;       // 从 SSD 获取日志头尾指针的 DMA 缓存区。
    uint64_t curAvailLpa, totalAvailLpa; // 当前可用 lpa 数量与总共可用 lpa 数量。

    JournalCommitNode *pendingJournalListHead; // 日志提交列表，从 JournalProcessEnv 中取到此处。

    // TODO 事务记录表。确定首事务日志已经应用完毕后，移除它并通知淘汰保护模块。表中事务按提交顺序排列，提交时按顺序写入 SSD 的 Journal FIFO，所以也一定按顺序被应用。
    // std::list<transaction_journal_record> tx_record;

    JournalWriter journalWriter;

    uint64_t curJournalBlockNum;                   // 当前日志记录占用的 SSD block 数目。
    JournalContainer *curJournal;                  // 当前正在处理的日志记录。
    JournalProcessState curProcState;              // 当前日志记录处理状态。
    uint64_t curJournalStartLpa, curJournalEndLpa; // 当前日志记录的持久化区域。

    RtfsTimer journalPollTimer; // 控制日志位置查询的定时器。
    bool isPollTimerEnabled;
} JournalProcessor;


void journalProcessorInit(JournalProcessor *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos);

void journalProcessorDestroy(JournalProcessor *this);

void journalProcessorProcessJournal(JournalProcessor *this);


#endif
