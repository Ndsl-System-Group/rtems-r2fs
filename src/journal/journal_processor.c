#include "journal_processor.h"

#include "utils/rtfs_log.h"


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


void journalProcessorInit(JournalProcessor *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos)
{
}

void journalProcessorDestroy(JournalProcessor *this)
{
}

void journalProcessorProcessJournal(JournalProcessor *this)
{
}
