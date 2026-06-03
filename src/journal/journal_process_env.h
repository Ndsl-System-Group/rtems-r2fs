#ifndef _JOURNAL_PROCESS_ENV_H_
#define _JOURNAL_PROCESS_ENV_H_

#include "journal/journal_container.h"

#include "utils/declare_utils.h"
#include "utils/rtfs_multithread.h"

#include <stdatomic.h>


struct comm_dev;


/**
 * @brief 提交队列节点。由于 JournalContainer 结构本身不能侵入式添加链表指针，因此通过该 wrapper 节点将其加入 UTList 双向链表。
 */
DEFINE_UTLIST_NODE(
    JournalCommitNode,
    JournalContainer *journal // 指向提交的日志容器。
)


/**
 * @brief 日志处理环境。包含日志管理层的日志提交队列，以及保护该队列的锁，用于通知日志处理线程的条件变量。
 *
 * @details 负责管理
 * 1. 日志提交队列。
 * 2. 日志处理线程。
 * 3. 线程同步原语。
 * 4. 事务号分配。
 */
typedef struct JournalProcessEnv
{
    /**
     * @brief 日志提交队列头节点。
     */
    JournalCommitNode *commitQueueHead;

    /**
     * @brief 日志处理线程退出请求标志。
     */
    bool exitReq;

    /**
     * @brief 保护提交队列的互斥锁。
     */
    mutex_t mtx;

    /**
     * @brief 用于唤醒日志处理线程的条件变量。
     */
    cond_t cond;

    /**
     * @brief 日志处理线程句柄。
     */
    pthread_t processThreadHandle;

    /**
     * @brief 日志处理线程当前是否仍可 join。
     */
    bool processThreadRunning;

    /**
     * @brief 下一个待分配的事务号。
     */
    atomic_uint_fast64_t txIdToAlloc;

} JournalProcessEnv;


/**
 * @brief 获取全局唯一的 JournalProcessEnv 单例。
 */
JournalProcessEnv *journalProcessEnvGetInstance();

/**
 * @brief 初始化日志处理环境。创建日志处理线程，并指定日志区域参数。
 */
void journalProcessEnvInit(JournalProcessEnv *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos);

/**
 * @brief 销毁日志处理环境。
 *
 * 释放内部资源：
 * - 等待日志处理线程结束。
 * - 销毁互斥锁和条件变量。
 * - 清理提交队列。
 */
void journalProcessEnvDestroy(JournalProcessEnv *this);

/**
 * @brief 分配新的事务号。使用原子操作递增事务号计数器。
 */
uint64_t journalProcessEnvAllocTxId(JournalProcessEnv *this);

/**
 * @brief 提交一个日志容器到日志处理队列。应在 journalProcessEnvAllocTxId 之后调用，调用者负责使用 journalProcessEnvAllocTxId 为 journal 分配事务号。
 */
void journalProcessEnvCommitJournal(JournalProcessEnv *this, JournalContainer *journal);

/**
 * @brief 向日志处理线程发送停止命令，并等待其停止后返回。日志处理线程在处理完所有已经提交的日志后将会停止。
 */
void journalProcessEnvStopProcessThread(JournalProcessEnv *this);

/**
 * @brief 测试/诊断辅助接口：返回当前提交队列是否为空。
 */
bool journalProcessEnvIsCommitQueueEmpty(JournalProcessEnv *this);

/**
 * @brief 测试/诊断辅助接口：返回当前退出请求标志。
 */
bool journalProcessEnvIsExitRequested(JournalProcessEnv *this);


#endif
