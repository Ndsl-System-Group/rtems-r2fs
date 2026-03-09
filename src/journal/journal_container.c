#include "journal_container.h"


void journalContainerInit(JournalContainer *this)
{
}

void journalContainerDestroy(JournalContainer *this)
{
}

void journalContainerAppendSuperBlockJournalEntry(JournalContainer *this, SuperBlockJournalEntry *spje)
{
}

void journalContainerAppendNatJournalEntry(JournalContainer *this, NatJournalEntry *nje)
{
}

void journalContainerAppendSitJournalEntry(JournalContainer *this, SitJournalEntry *sje)
{
}

uint64_t journalContainerGetTxId(JournalContainer *this)
{
    return this->txId;
}

void journalContainerSetTxId(JournalContainer *this, uint64_t id)
{
    this->txId = id;
}

UT_array journalContainerGetSuperBlockJournal(JournalContainer *this)
{
}

UT_array journalContainerGetNatJournal(JournalContainer *this)
{
}

UT_array journalContainerGetSitJournal(JournalContainer *this)
{
}

bool journalContainerIsEmpty(JournalContainer *this)
{
    return (0 == utarray_len(&this->superBlockJournal)) && (0 == utarray_len(&this->natJournal)) && (0 == utarray_len(&this->sitJournal));
}
