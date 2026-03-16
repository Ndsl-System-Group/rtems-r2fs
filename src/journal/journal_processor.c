#include "journal_processor.h"


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
