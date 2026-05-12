#ifndef _JOURNAL_PROCESSOR_H_
#define _JOURNAL_PROCESSOR_H_

#include "journal/journal_container.h"
#include "journal/journal_process_env.h"
#include "journal/journal_writer.h"

#include "utils/types.h"
#include "utils/rtfs_timer.h"
#include "utils/declare_utils.h"


struct comm_dev;

typedef void (*journal_tx_complete_hook)(uint64_t tx_id, void *arg);


/**
 * @brief 事务日志记录，一个对象代表一个事务，记录该事务日志在 SSD 上持久化的 LPA 范围 [startLpa, endLpa)。
 */
typedef struct TransactionJournalRecord
{
    /**
     * @brief 事务唯一编号。
     */
    uint64_t txId;

    /**
     * @brief 该事务日志起始位置（包含）。
     */
    uint64_t startLpa;

    /**
     * @brief 该事务日志结束位置（不包含）。
     */
    uint64_t endLpa;
} TransactionJournalRecord;


/**
 * @brief 初始化事务日志记录对象。
 */
void transactionJournalRecordInit(TransactionJournalRecord *this, uint64_t txId, uint64_t startLpa, uint64_t endLpa);

/**
 * @brief 获取事务编号。
 */
uint64_t transactionJournalRecordGetTxId(TransactionJournalRecord *this);

/**
 * @brief 获取事务日志起始位置。
 */
uint64_t transactionJournalRecordGetStartLpa(TransactionJournalRecord *this);

/**
 * @brief 获取事务日志结束位置。
 */
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
 * @brief 事务记录表。确定首事务日志已经应用完毕后，移除它并通知淘汰保护模块。表中事务按提交顺序排列，提交时按顺序写入 SSD 的 Journal FIFO，所以也一定按顺序被应用。
 */
DEFINE_UTLIST_NODE(TxRecordNode, TransactionJournalRecord record)

/**
 * @brief 日志处理线程与其工作环境。系统确保此环境构造时，已完成故障恢复，因此可用日志资源为整个 SSD 日志区域。日志处理线程中，写日志、写日志尾指针、查询 SSD 日志位置，都使用同步阻塞式 I/O。
 */
typedef struct JournalProcessor
{
    /**
     * @brief 底层通讯设备对象。
     */
    struct comm_dev *dev;

    /**
     * @brief 当前日志区域 FIFO 的首尾 LPA：[headLpa, tailLpa)。
     */
    uint64_t headLpa, tailLpa;

    /**
     * @brief SSD 日志区域起止 LPA：[startLpa, endLpa)。
     */
    uint64_t startLpa, endLpa;

    /**
     * @brief 从 SSD 获取日志头尾指针的 DMA 缓存区。
     */
    uint64_t *journalPosDmaBuffer;

    /**
     * @brief 当前可用 lpa 数量与总共可用 lpa 数量。
     */
    uint64_t curAvailLpa, totalAvailLpa;

    /**
     * @brief 日志提交列表，从 JournalProcessEnv 中取到此处。
     */
    JournalCommitNode *pendingJournalListHead;

    /**
     * @brief 已写入 SSD、等待完成应用的事务记录链表头。
     */
    TxRecordNode *txRecordHead;

    /**
     * @brief Journal 写入器，负责将事务日志整理到写缓存并写入 SSD。
     */
    JournalWriter journalWriter;

    /**
     * @brief 当前日志记录占用的 SSD block 数目。
     */
    uint64_t curJournalBlockNum;

    /**
     * @brief 当前正在处理的日志记录。
     */
    JournalContainer *curJournal;

    /**
     * @brief 当前日志记录处理状态。
     */
    JournalProcessState curProcState;

    /**
     * @brief 当前日志记录的持久化区域。
     */
    uint64_t curJournalStartLpa, curJournalEndLpa;

    /**
     * @brief 控制日志位置查询的定时器。
     */
    RtfsTimer journalPollTimer;

    /**
     * @brief 轮询定时器是否已启动。
     */
    bool isPollTimerEnabled;

    /**
     * @brief 事务完成通知 hook。仅在确认某个 tx 已完成应用时调用。
     */
    journal_tx_complete_hook txCompleteHook;

    /**
     * @brief 传递给 txCompleteHook 的用户上下文。
     */
    void *txCompleteHookArg;
} JournalProcessor;


/**
 * @brief 初始化日志处理器对象。
 */
void journalProcessorInit(JournalProcessor *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos);

/**
 * @brief 销毁日志处理器对象。
 */
void journalProcessorDestroy(JournalProcessor *this);

/**
 * @brief 设置事务完成通知 hook。
 */
void journalProcessorSetTxCompleteHook(
    JournalProcessor *this,
    journal_tx_complete_hook hook,
    void *arg
);

void journalProcessorSetDefaultTxCompleteHook(
    journal_tx_complete_hook hook,
    void *arg
);

/**
 * @brief 日志处理线程主循环。
 *
 * @details 该函数通常作为日志后台线程入口逻辑调用。循环执行以下任务：
 * 1. 获取新提交事务日志；
 * 2. 将日志写入 SSD Journal；
 * 3. 轮询 SSD Journal 应用进度；
 * 4. 回收已完成事务对应日志空间；
 * 5. 收到退出请求后结束线程。
 */
void journalProcessorProcessJournal(JournalProcessor *this);

/**
 * @brief 测试/最小驱动辅助入口：仅处理当前已完成应用的事务记录。
 */
void journalProcessorDrainCompletedTxRecords(JournalProcessor *this);


#endif
