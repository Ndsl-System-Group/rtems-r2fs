#include "journal_container.h"


static const UT_icd superBlockJournalIcd = {
    sizeof(struct SuperBlockJournalEntry),
    NULL,
    NULL,
    NULL,
};

static const UT_icd natJournalIcd = {
    sizeof(struct NatJournalEntry),
    NULL,
    NULL,
    NULL,
};

static const UT_icd sitJournalIcd = {
    sizeof(struct SitJournalEntry),
    NULL,
    NULL,
    NULL,
};


void journalContainerInit(JournalContainer *this)
{
    utarray_init(&this->superBlockJournal, &superBlockJournalIcd);
    utarray_init(&this->natJournal, &natJournalIcd);
    utarray_init(&this->sitJournal, &sitJournalIcd);

    this->txId = 0;
}

void journalContainerDestroy(JournalContainer *this)
{
    utarray_done(&this->superBlockJournal);
    utarray_done(&this->natJournal);
    utarray_done(&this->sitJournal);

    this->txId = 0;
}

void journalContainerAppendSuperBlockJournalEntry(JournalContainer *this, SuperBlockJournalEntry *sbje)
{
    utarray_push_back(&this->superBlockJournal, sbje);
}

void journalContainerAppendNatJournalEntry(JournalContainer *this, NatJournalEntry *nje)
{
    utarray_push_back(&this->natJournal, nje);
}

void journalContainerAppendSitJournalEntry(JournalContainer *this, SitJournalEntry *sje)
{
    utarray_push_back(&this->sitJournal, sje);
}

uint64_t journalContainerGetTxId(JournalContainer *this)
{
    return this->txId;
}

void journalContainerSetTxId(JournalContainer *this, uint64_t id)
{
    this->txId = id;
}

UT_array *journalContainerGetSuperBlockJournal(JournalContainer *this)
{
    return &this->superBlockJournal;
}

UT_array *journalContainerGetNatJournal(JournalContainer *this)
{
    return &this->natJournal;
}

UT_array *journalContainerGetSitJournal(JournalContainer *this)
{
    return &this->sitJournal;
}

bool journalContainerIsEmpty(JournalContainer *this)
{
    return (0 == utarray_len(&this->superBlockJournal)) && (0 == utarray_len(&this->natJournal)) && (0 == utarray_len(&this->sitJournal));
}
