#include "sit_utils.h"

#include "fs/fs.h"
#include "fs/fs_manager.h"
#include "utils/rtfs_log.h"
#include "cache/super_cache.h"
#include "cache/sit_nat_cache.h"
#include "journal/journal_container.h"

#include <assert.h>
#include <stddef.h>
#include <stdbool.h>


/**
 * @brief 修改 LPA 状态。
 */
static void sitChangeLpaState(SitOperator *this, uint32_t lpa, int valid);


void sitOperatorInit(SitOperator *this, struct file_system_manager *fsManager)
{
    assert(this && fsManager);

    this->fsManager = fsManager;
    struct RtfsSuperBlock *super = fileSystemManagerGetSuperBlkMem(fsManager);

    this->seg0StartLpa = super->segment0_blkaddr;
    this->segCount = super->segment_count;
    this->sitStartLpa = super->sit_blkaddr;
    this->sitSegmentCnt = super->segment_count_sit;
}

void sitInvalidateLpa(SitOperator *this, uint32_t lpa)
{
    sitChangeLpaState(this, lpa, false);
}

void sitValidateLpa(SitOperator *this, uint32_t lpa)
{
    sitChangeLpaState(this, lpa, true);
}

SegPos sitGetSegPosOfLpa(SitOperator *this, uint32_t lpa)
{
    lpa -= this->seg0StartLpa;
    SegPos pos = {lpa / BLOCK_PER_SEGMENT, lpa % BLOCK_PER_SEGMENT};


    return pos;
}

SitPos sitGetSegIdPosInSit(SitOperator *this, uint32_t segId)
{
    SitPos pos;
    uint32_t sitLpaIdx = segId / SIT_ENTRY_PER_BLOCK;
    uint32_t sitLpaOff = segId % SIT_ENTRY_PER_BLOCK;

    assert(sitLpaIdx < this->sitSegmentCnt * BLOCK_PER_SEGMENT);

    pos.sitLpa = this->sitStartLpa + sitLpaIdx;
    pos.idx = sitLpaOff;


    return pos;
}

uint32_t sitGetFirstLpaOfSegId(SitOperator *this, uint32_t segId)
{
    return this->seg0StartLpa + segId * BLOCK_PER_SEGMENT;
}


void sitChangeLpaState(SitOperator *this, uint32_t lpa, int valid)
{
    if (INVALID_LPA == lpa) return;

    // 计算 LPA 的 segment ID 和 segment 内偏移。
    uint32_t lpaSeg0Off = lpa - this->seg0StartLpa;
    uint32_t segId = lpaSeg0Off / BLOCK_PER_SEGMENT;
    uint32_t segoff = lpaSeg0Off % BLOCK_PER_SEGMENT;
    uint32_t sitLpa = this->sitStartLpa + segId / SIT_ENTRY_PER_BLOCK; // SIT block LPA。

    RTFS_LOG(RTFS_LOG_INFO, "lpa [%u]: segId = %u, segoff = %u, SIT lpa = %u", lpa, segId, segoff, sitLpa);

    // 计算 segid 在 Sit Block 内对应的 entry。
    SitNatCache *sitCache = fileSystemManagerGetSitCache(this->fsManager);
    SitNatCacheEntryHandle sitBlockHandle = sitNatCacheGet(sitCache, sitLpa);
    struct RtfsSitBlock *sitBlock = sitNatCacheEntryHandleGetSitBlockPtr(&sitBlockHandle);
    struct RtfsSitEntry *sitEntry = &sitBlock->entries[segId % SIT_ENTRY_PER_BLOCK];

    // 修改对应 sit entry。
    uint32_t bitmapIdx = segoff / 8;
    uint32_t bitmapOff = segoff % 8;

    if (valid)
    {
        assert(!(sitEntry->valid_map[bitmapIdx] & (1U << bitmapOff)));
        sitEntry->valid_map[bitmapIdx] |= (1U << bitmapOff);

        // 有效块计数字段最多只能是 511（9 位），但实际可能有 512。因此不记录从 511 -> 512 的增加。
        if (GET_SIT_VBLOCKS(sitEntry) < 511) ++sitEntry->vblocks;

        RTFS_LOG(RTFS_LOG_INFO, "validate lpa [%u] in SIT.", lpa);
    }
    else
    {
        assert(sitEntry->valid_map[bitmapIdx] & (1U << bitmapOff));
        sitEntry->valid_map[bitmapIdx] &= ~(1U << bitmapOff);

        if (GET_SIT_VBLOCKS(sitEntry) > 0) --sitEntry->vblocks;

        RTFS_LOG(RTFS_LOG_INFO, "invalidate lpa [%u] in SIT.", lpa);
    }

    // 写下 SIT 日志条目，增加 sit 缓存块主机侧版本。
    JournalContainer *curJournal = fileSystemManagerGetCurJournal(this->fsManager);
    SitJournalEntry journalEntry = {.segID = segId, .newValue = *sitEntry};
    journalContainerAppendSitJournalEntry(curJournal, &journalEntry);
    sitNatCacheEntryHandleAddHostVersion(&sitBlockHandle);
    sitNatCacheEntryHandleDestroy(&sitBlockHandle);
}
