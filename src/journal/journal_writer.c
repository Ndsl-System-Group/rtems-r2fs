#include "journal_writer.h"


typedef enum JournalOutputState
{
    JOURNAL_OUTPUT_OK,
    JOURNAL_OUTPUT_NO_ENOUGH_BUFFER,
    JOURNAL_OUTPUT_REACH_END
} JournalOutputState;


void journalWriterInit(JournalWriter *this, struct comm_dev *dev, uint64_t journalAreaStartLpa, uint64_t journalAreaEndLpa)
{
    this->startLpa = journalAreaStartLpa;
    this->endLpa = journalAreaEndLpa;
    this->curJournal = NULL;
    this->bufferTailIdx = 0;
    this->bufferTailOff = 0;
    this->dev = dev;

    kv_init(this->journalBuffer);
}

void journalWriterSetPendingJournal(JournalWriter *this, JournalContainer *journal)
{
    this->curJournal = journal;
}

// TODO
uint64_t journalWriterCollectPendingJournalToWriteBuffer(JournalWriter *this)
{
}

void journalWriterWriteToSsd(JournalWriter *this, uint64_t curTail)
{
}
