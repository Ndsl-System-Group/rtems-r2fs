#include "nat_utils.h"

#include "cache/sit_nat_cache.h"
#include "fs/fs.h"
#include "fs/fs_manager.h"
#include "utils/rtfs_log.h"
#include "journal/journal_container.h"

#include <assert.h>
#include <stddef.h>


void natLpaMappingInit(NatLpaMapping *this, struct file_system_manager *fsManager)
{
    assert(this);
    assert(fsManager);

    this->fsManager = fsManager;
    this->natStartLpa = fileSystemManagerGetSuperBlkMem(fsManager)->nat_blkaddr;
    this->natSegmentCnt = fileSystemManagerGetSuperBlkMem(fsManager)->segment_count_nat;
}

NatNidPos natGetNidPos(NatLpaMapping *this, uint32_t nid)
{
    NatNidPos pos;
    uint32_t natLpaIdx = nid / NAT_ENTRY_PER_BLOCK;
    uint32_t natLpaOff = nid % NAT_ENTRY_PER_BLOCK;

    assert(natLpaIdx < this->natSegmentCnt * BLOCK_PER_SEGMENT);

    pos.lpa = this->natStartLpa + natLpaIdx;
    pos.idx = natLpaOff;
    return pos;
}

uint32_t natGetLpaOfNid(NatLpaMapping *this, uint32_t nid)
{
    NatNidPos pos = natGetNidPos(this, nid);

    uint32_t natBlockLpa = pos.lpa;
    uint32_t natEntryIdx = pos.idx;
    RTFS_LOG(RTFS_LOG_INFO, "nat entry pos of nid %u: lpa=%u, idx=%u", nid, natBlockLpa, natEntryIdx);

    SitNatCacheEntryHandle natHandle = sitNatCacheGet(fileSystemManagerGetNatCache(this->fsManager), natBlockLpa);
    struct RtfsNatEntry natEntry = sitNatCacheEntryHandleGetNatBlockPtr(&natHandle)->entries[natEntryIdx];

    uint32_t nidLpa = natEntry.block_addr;
    RTFS_LOG(RTFS_LOG_INFO, "lpa of nid %u: %u", nid, nidLpa);


    return nidLpa;
}

void natSetLpaOfNid(NatLpaMapping *this, uint32_t nid, uint32_t newLpa)
{
    struct RtfsNatBlock *nat_block;
    struct RtfsNatEntry *nat_entry;
    struct RtfsNatEntry new_value;
    JournalContainer *curJournal;

    NatNidPos pos = natGetNidPos(this, nid);
    uint32_t natBlockLpa = pos.lpa;
    uint32_t natEntryIdx = pos.idx;

    SitNatCacheEntryHandle natHandle = sitNatCacheGet(fileSystemManagerGetNatCache(this->fsManager), natBlockLpa);
    nat_block = sitNatCacheEntryHandleGetNatBlockPtr(&natHandle);
    nat_entry = &nat_block->entries[natEntryIdx];

    nat_entry->block_addr = newLpa;
    RTFS_LOG(RTFS_LOG_DEBUG, "set nid(%u)'s lpa to %u.", nid, newLpa);

    new_value = *nat_entry;
    curJournal = fileSystemManagerGetCurJournal(this->fsManager);
    NatJournalEntry natJournal = {.nid = nid, .newValue = new_value};
    journalContainerAppendNatJournalEntry(curJournal, &natJournal);
    sitNatCacheEntryHandleAddHostVersion(&natHandle);
    sitNatCacheEntryHandleDestroy(&natHandle);
}
