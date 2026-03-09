#ifndef _JOURNAL_CONTAINER_H_
#define _JOURNAL_CONTAINER_H_

#include "journal_type.h"

#include "uthash/utarray.h"
#include "utils/types.h"


/**
 * @brief 日志容器，用于存储事务生成的日志项。
 */
typedef struct JournalContainer
{
    UT_array superBlockJournal;
    UT_array natJournal;
    UT_array sitJournal;

    uint64_t txId; // 事务号。
} JournalContainer;


void journalContainerInit(JournalContainer *this);

void journalContainerDestroy(JournalContainer *this);

void journalContainerAppendSuperBlockJournalEntry(JournalContainer *this, SuperBlockJournalEntry *spje);

void journalContainerAppendNatJournalEntry(JournalContainer *this, NatJournalEntry *nje);

void journalContainerAppendSitJournalEntry(JournalContainer *this, SitJournalEntry *sje);

uint64_t journalContainerGetTxId(JournalContainer *this);

void journalContainerSetTxId(JournalContainer *this, uint64_t id);

UT_array journalContainerGetSuperBlockJournal(JournalContainer *this);

UT_array journalContainerGetNatJournal(JournalContainer *this);

UT_array journalContainerGetSitJournal(JournalContainer *this);

bool journalContainerIsEmpty(JournalContainer *this);


#endif
