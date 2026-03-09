#ifndef _JOURNAL_TYPE_H_
#define _JOURNAL_TYPE_H_


#include "fs/fs.h"


enum
{
    JOURNAL_TYPE_NATS,
    JOURNAL_TYPE_SITS,
    JOURNAL_TYPE_SUPER_BLOCK,
    JOURNAL_TYPES,
    JOURNAL_TYPE_NOP = 0x7e,
    JOURNAL_TYPE_END
};


typedef struct MetaJournalEntry
{
    u16 len;
    u8 type;
    u8 rsv;
    u8 journalData[0];
} __attribute__((packed)) MetaJournalEntry;

typedef struct NatJournalEntry
{
    u32 nid;
    struct RtfsNatEntry newValue;
} __attribute__((packed)) NatJournalEntry;

typedef struct SitJournalEntry
{
    u32 segID;
    struct RtfsSitEntry newValue;
} __attribute__((packed)) SitJournalEntry;

typedef struct SuperBlockJournalEntry
{
    u32 Off;
    u32 newVal;
} __attribute__((packed)) SuperBlockJournalEntry;


#endif
