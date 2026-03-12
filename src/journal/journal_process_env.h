#ifndef _JOURNAL_PROCESS_ENV_H_
#define _JOURNAL_PROCESS_ENV_H_

#include "journal_container.h"

#include <pthread.h>
#include <stdatomic.h>


struct comm_dev;


/**
 * @brief 提交队列节点。由于 JournalContainer 结构本身不能侵入式添加链表指针，因此通过该 wrapper 节点将其加入 UTList 双向链表。
 */
typedef struct JournalCommitNode
{
    JournalContainer *journal; // 指向提交的日志容器。

    struct JournalCommitNode *prev;
    struct JournalCommitNode *next;
} JournalCommitNode;


/**
 * @brief 日志处理环境。包含日志管理层的日志提交队列，以及保护该队列的锁，用于通知日志处理线程的条件变量。
 *
 * 负责管理：
 * - 日志提交队列。
 * - 日志处理线程。
 * - 线程同步原语。
 * - 事务号分配。
 */
typedef struct JournalProcessEnv
{
    JournalCommitNode *commitQueueHead; // 日志提交队列头节点。

    bool exitReq; // 日志处理线程退出请求标志。

    pthread_mutex_t mtx; // 保护提交队列的互斥锁。
    pthread_cond_t cond; // 用于唤醒日志处理线程的条件变量。

    pthread_t processThreadHandle; // 日志处理线程句柄。

    atomic_uint_fast64_t txIdToAlloc; // 下一个待分配的事务号。
} JournalProcessEnv;


/**
 * @brief 获取全局唯一的 JournalProcessEnv 单例。
 */
JournalProcessEnv *journalProcessEnvGetInstance();

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
 * @brief 初始化日志处理环境。创建日志处理线程，并指定日志区域参数。
 */
void journalProcessEnvInit(JournalProcessEnv *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos);

/**
 * @brief 向日志处理线程发送停止命令，并等待其停止后返回。日志处理线程在处理完所有已经提交的日志后将会停止。
 */
void journalProcessEnvStopProcessThread(JournalProcessEnv *this);


#endif
