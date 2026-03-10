#ifndef _JOURNAL_CONTAINER_H_
#define _JOURNAL_CONTAINER_H_

#include "journal_type.h"

#include "uthash/utarray.h"
#include "utils/types.h"


/**
 * @brief 日志容器，用于存储一个事务（transaction）产生的所有日志项。
 *
 * 每个事务在执行过程中可能产生多种类型的日志，例如：
 * - SuperBlock 日志。
 * - NAT 日志。
 * - SIT 日志。
 *
 * 这些日志会被暂存在该容器中，随后统一写入 journal 区域。内部使用 UT_array（动态数组）来存储不同类型的日志项。
 */
typedef struct JournalContainer
{
    UT_array superBlockJournal; // SuperBlock 类型日志数组。
    UT_array natJournal;        // NAT 类型日志数组。
    UT_array sitJournal;        // SIT 类型日志数组。

    uint64_t txId; // 事务号。
} JournalContainer;


/**
 * @brief 初始化 JournalContainer。
 */
void journalContainerInit(JournalContainer *this);

/**
 * @brief 销毁 JournalContainer，释放内部 UT_array 占用的资源。
 */
void journalContainerDestroy(JournalContainer *this);

/**
 * @brief 添加 SuperBlock 日志项，将一个 SuperBlockJournalEntry 追加到容器中，内部会对 entry 进行一次拷贝。
 */
void journalContainerAppendSuperBlockJournalEntry(JournalContainer *this, SuperBlockJournalEntry *sbje);

/**
 * @brief 添加 Nat 日志项，将一个 NatJournalEntry 追加到 Nat 日志数组。
 */
void journalContainerAppendNatJournalEntry(JournalContainer *this, NatJournalEntry *nje);

/**
 * @brief 添加 Sit 日志项，将一个 SitJournalEntry 追加到 Sit 日志数组。
 */
void journalContainerAppendSitJournalEntry(JournalContainer *this, SitJournalEntry *sje);

/**
 * @brief 获取事务 ID。
 */
uint64_t journalContainerGetTxId(JournalContainer *this);

/**
 * @brief 设置事务 ID。
 */
void journalContainerSetTxId(JournalContainer *this, uint64_t id);

/**
 * @brief 获取 SuperBlock 日志数组。
 */
UT_array *journalContainerGetSuperBlockJournal(JournalContainer *this);

/**
 * @brief 获取 Nat 日志数组。
 */
UT_array *journalContainerGetNatJournal(JournalContainer *this);

/**
 * @brief 获取 Sit 日志数组。
 */
UT_array *journalContainerGetSitJournal(JournalContainer *this);

/**
 * @brief 判断容器是否为空。如果三种日志数组均为空，则返回 true。
 */
bool journalContainerIsEmpty(JournalContainer *this);


#endif
