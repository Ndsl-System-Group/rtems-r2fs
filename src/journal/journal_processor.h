#ifndef _JOURNAL_PROCESSOR_H_
#define _JOURNAL_PROCESSOR_H_

#include "utils/types.h"


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


#endif
