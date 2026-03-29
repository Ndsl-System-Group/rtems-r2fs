#include "journal_container.h"


void journalContainerInit(JournalContainer *this)
{
    kv_init(this->superBlockJournal);
    kv_init(this->natJournal);
    kv_init(this->sitJournal);

    this->txId = 0;
}

void journalContainerDestroy(JournalContainer *this)
{
    kv_destroy(this->sitJournal);
    kv_destroy(this->natJournal);
    kv_destroy(this->superBlockJournal);

    this->txId = 0;
}

void journalContainerAppendSuperBlockJournalEntry(JournalContainer *this, SuperBlockJournalEntry *sbje)
{
    kv_push(SuperBlockJournalEntry, this->superBlockJournal, *sbje);
}

void journalContainerAppendNatJournalEntry(JournalContainer *this, NatJournalEntry *nje)
{
    kv_push(NatJournalEntry, this->natJournal, *nje);
}

void journalContainerAppendSitJournalEntry(JournalContainer *this, SitJournalEntry *sje)
{
    kv_push(SitJournalEntry, this->sitJournal, *sje);
}

uint64_t journalContainerGetTxId(JournalContainer *this)
{
    return this->txId;
}

void journalContainerSetTxId(JournalContainer *this, uint64_t id)
{
    this->txId = id;
}

SuperBlockJournalVector *journalContainerGetSuperBlockJournal(JournalContainer *this)
{
    return &this->superBlockJournal;
}

NatJournalVector *journalContainerGetNatJournal(JournalContainer *this)
{
    return &this->natJournal;
}

SitJournalVector *journalContainerGetSitJournal(JournalContainer *this)
{
    return &this->sitJournal;
}

bool journalContainerIsEmpty(JournalContainer *this)
{
    return (0 == kv_size(this->superBlockJournal)) && (0 == kv_size(this->natJournal)) && (0 == kv_size(this->sitJournal));
}
