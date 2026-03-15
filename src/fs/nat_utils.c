#include "nat_utils.h"

#include "fs/fs.h"
#include "utils/rtfs_log.h"

#include <assert.h>
#include <stddef.h>


void natLpaMappingInit(NatLpaMapping *this, struct file_system_manager *fsManager, uint32_t natStartLpa, uint32_t natSegmentCnt)
{
    assert(this);
    assert(fsManager);

    this->fsManager = fsManager;
    this->natStartLpa = natStartLpa;
    this->natSegmentCnt = natSegmentCnt;
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

    struct RtfsNatEntry *natEntries = NULL;

    // TODO 依赖 fs_manager 获取 nat cache 的接口和 SIT_NAT_cache_entry_handle 获取 entries 的接口。
    // SIT_NAT_cache_entry_handle natHandle = natCacheGet(this->fsManager, pos.lpa);
    // natEntries = natHandleGetEntries(natHandle);

    assert(natEntries != NULL);

    uint32_t nidLpa = natEntries[pos.idx].block_addr;

    RTFS_LOG(RTFS_LOG_INFO, "nat entry pos of nid %u: lpa=%u, idx=%u", nid, pos.lpa, pos.idx);
    RTFS_LOG(RTFS_LOG_INFO, "lpa of nid %u: %u", nid, nidLpa);

    return nidLpa;
}

void natSetLpaOfNid(NatLpaMapping *this, uint32_t nid, uint32_t newLpa)
{
    NatNidPos pos = natGetNidPos(this, nid);

    struct RtfsNatEntry *natEntries = NULL;

    // TODO 同理依赖相关接口。
    // SIT_NAT_cache_entry_handle natHandle = natCacheGet(this->fsManager, pos.lpa);
    // natEntries = natHandleGetEntries(natHandle);

    assert(natEntries != NULL);

    natEntries[pos.idx].block_addr = newLpa;

    RTFS_LOG(RTFS_LOG_DEBUG, "set nid(%u)'s lpa to %u.", nid, newLpa);

    // TODO 记录 NAT 日志，依赖日志接口。
    // journal_container *curJournal = fsManagerGetCurJournal(this->fsManager);
    // NAT_journal_entry natJournal = {.nid = nid, .newValue = natEntries[pos.idx]};
    // journalAppendNatEntry(curJournal, &natJournal);

    // natHandleAddHostVersion(natHandle);
}
