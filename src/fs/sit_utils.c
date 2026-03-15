#include "sit_utils.h"

#include "fs/fs.h"
#include "utils/rtfs_log.h"

#include <assert.h>
#include <stddef.h>
#include <stdbool.h>


/**
 * @brief 修改 LPA 状态。
 */
static void sitChangeLpaState(SitOperator *this, uint32_t lpa, int valid);


void sitOperatorInit(SitOperator *this, struct file_system_manager *fsManager, uint32_t seg0StartLpa, uint32_t segCount, uint32_t sitStartLpa, uint32_t sitSegmentCnt)
{
    assert(this && fsManager);
    this->fsManager = fsManager;
    this->seg0StartLpa = seg0StartLpa;
    this->segCount = segCount;
    this->sitStartLpa = sitStartLpa;
    this->sitSegmentCnt = sitSegmentCnt;
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
    uint32_t lpaOff = lpa - this->seg0StartLpa;
    SegPos pos = {lpaOff / BLOCK_PER_SEGMENT, lpaOff % BLOCK_PER_SEGMENT};


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
    uint32_t lpa_seg0_off = lpa - this->seg0StartLpa;
    uint32_t segId = lpa_seg0_off / BLOCK_PER_SEGMENT;
    uint32_t segoff = lpa_seg0_off % BLOCK_PER_SEGMENT;

    // SIT block LPA
    uint32_t sitLpa = this->sitStartLpa + segId / SIT_ENTRY_PER_BLOCK;

    RTFS_LOG(RTFS_LOG_INFO, "lpa [%u]: segId = %u, segoff = %u, SIT lpa = %u", lpa, segId, segoff, sitLpa);

    // TODO 依赖 fs_manager 获取 sit cache 的接口和 SIT_NAT_cache_entry_handle 获取 entries 的接口。
    // 获取 SIT block
    // SIT_NAT_cache *sitCache = fsManagerGetSitCache(this->fsManager);
    // SIT_NAT_cache_entry_handle sitBlockHandle = sitCacheGet(sitCache, sitLpa);
    // RtfsSitBlock *sitBlock = sitHandleGetSitBlockPtr(sitBlockHandle);

    // 找到对应 SIT entry
    struct RtfsSitEntry *sitEntry = NULL;
    // RtfsSitEntry *sitEntry = &sitBlock->entries[segId % SIT_ENTRY_PER_BLOCK];

    uint32_t bitmapIdx = segoff / 8;
    uint32_t bitmapOff = segoff % 8;

    if (valid)
    {
        assert(!(sitEntry->valid_map[bitmapIdx] & (1U << bitmapOff)));
        sitEntry->valid_map[bitmapIdx] |= (1U << bitmapOff);

        if (GET_SIT_VBLOCKS(sitEntry) < 511)
            sitEntry->vblocks++;

        RTFS_LOG(RTFS_LOG_INFO, "validate lpa [%u] in SIT.", lpa);
    }
    else
    {
        assert(sitEntry->valid_map[bitmapIdx] & (1U << bitmapOff));
        sitEntry->valid_map[bitmapIdx] &= ~(1U << bitmapOff);

        if (GET_SIT_VBLOCKS(sitEntry) > 0)
            sitEntry->vblocks--;

        RTFS_LOG(RTFS_LOG_INFO, "invalidate lpa [%u] in SIT.", lpa);
    }

    // TODO 写 SIT 日志，依赖日志接口。
    // journal_container *curJournal = fsManagerGetCurJournal(this->fsManager);
    // SIT_journal_entry journalEntry = {.segID = segId, .newValue = *sitEntry};
    // journalAppendSitEntry(curJournal, &journalEntry);

    // sitHandleAddHostVersion(sitBlockHandle);
}
