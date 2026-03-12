#ifndef _JOURNAL_PROCESS_ENV_H_
#define _JOURNAL_PROCESS_ENV_H_

#include "journal_container.h"

#include <pthread.h>
#include <stdatomic.h>


struct comm_dev;


typedef struct JournalCommitNode
{
    JournalContainer *journal;

    struct JournalCommitNode *prev;
    struct JournalCommitNode *next;
} JournalCommitNode;


typedef struct JournalProcessEnv
{
    JournalCommitNode *commitQueueHead;

    bool exitReq;

    pthread_mutex_t mtx;
    pthread_cond_t cond;

    pthread_t processThreadHandle;

    atomic_uint_fast64_t txIdToAlloc;

} JournalProcessEnv;


JournalProcessEnv *journalProcessEnvGetInstance();

void journalProcessEnvDestroy(JournalProcessEnv *this);

uint64_t journalProcessEnvAllocTxId(JournalProcessEnv *this);

void journalProcessEnvCommitJournal(JournalProcessEnv *this, JournalContainer *journal);

void journalProcessEnvInit(JournalProcessEnv *this, struct comm_dev *dev, uint64_t journalStartLpa, uint64_t journalEndLpa, uint64_t journalFifoPos);

void journalProcessEnvStopProcessThread(JournalProcessEnv *this);


#endif
