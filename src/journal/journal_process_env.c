#include "journal_process_env.h"


JournalProcessEnv *journalProcessEnvGetInstance()
{
}

uint64_t journalProcessEnvAllocTxId(JournalProcessEnv *this)
{
}

void journalProcessEnvCommitJournal(JournalProcessEnv *this, JournalContainer *journal)
{
}

void journalProcessEnvInit(JournalProcessEnv *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos)
{
}

void journalProcessEnvStopProcessThread(JournalProcessEnv *this)
{
}
