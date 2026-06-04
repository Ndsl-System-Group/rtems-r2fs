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
static void sitChangeLpaStateRange(
    SitOperator *this,
    uint32_t start_lpa,
    uint32_t count,
    int valid
);


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
    sitChangeLpaStateRange(this, lpa, 1, false);
}

void sitValidateLpa(SitOperator *this, uint32_t lpa)
{
    sitChangeLpaStateRange(this, lpa, 1, true);
}

void sitValidateLpaRange(SitOperator *this, uint32_t start_lpa, uint32_t count)
{
    sitChangeLpaStateRange(this, start_lpa, count, true);
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
    sitChangeLpaStateRange(this, lpa, 1, valid);
}

static void sitChangeLpaStateRange(
    SitOperator *this,
    uint32_t start_lpa,
    uint32_t count,
    int valid
)
{
    uint32_t cur_lpa = start_lpa;

    if (INVALID_LPA == start_lpa || count == 0) {
        return;
    }

    while (count > 0) {
        uint32_t lpa_seg0_off = cur_lpa - this->seg0StartLpa;
        uint32_t seg_id = lpa_seg0_off / BLOCK_PER_SEGMENT;
        uint32_t seg_off = lpa_seg0_off % BLOCK_PER_SEGMENT;
        uint32_t sit_lpa = this->sitStartLpa + seg_id / SIT_ENTRY_PER_BLOCK;
        uint32_t seg_count = BLOCK_PER_SEGMENT - seg_off;
        SitNatCache *sit_cache;
        SitNatCacheEntryHandle sit_block_handle;
        struct RtfsSitBlock *sit_block;
        struct RtfsSitEntry *sit_entry;
        JournalContainer *cur_journal;
        SitJournalEntry journal_entry;
        uint32_t i;

        if (seg_count > count) {
            seg_count = count;
        }

        RTFS_LOG(
            RTFS_LOG_INFO,
            "lpa [%u-%u]: segId = %u, segoff = %u, SIT lpa = %u",
            cur_lpa,
            cur_lpa + seg_count - 1,
            seg_id,
            seg_off,
            sit_lpa
        );

        sit_cache = fileSystemManagerGetSitCache(this->fsManager);
        sit_block_handle = sitNatCacheGet(sit_cache, sit_lpa);
        sit_block = sitNatCacheEntryHandleGetSitBlockPtr(&sit_block_handle);
        sit_entry = &sit_block->entries[seg_id % SIT_ENTRY_PER_BLOCK];

        for (i = 0; i < seg_count; ++i) {
            uint32_t bitmap_idx = (seg_off + i) / 8;
            uint32_t bitmap_off = (seg_off + i) % 8;

            if (valid) {
                assert(!(sit_entry->valid_map[bitmap_idx] & (1U << bitmap_off)));
                sit_entry->valid_map[bitmap_idx] |= (uint8_t)(1U << bitmap_off);

                if (GET_SIT_VBLOCKS(sit_entry) < 511) {
                    ++sit_entry->vblocks;
                }
            } else {
                assert(sit_entry->valid_map[bitmap_idx] & (1U << bitmap_off));
                sit_entry->valid_map[bitmap_idx] &=
                    (uint8_t)~(1U << bitmap_off);

                if (GET_SIT_VBLOCKS(sit_entry) > 0) {
                    --sit_entry->vblocks;
                }
            }
        }

        cur_journal = fileSystemManagerGetCurJournal(this->fsManager);
        journal_entry.segID = seg_id;
        journal_entry.newValue = *sit_entry;
        journalContainerAppendSitJournalEntry(cur_journal, &journal_entry);
        sitNatCacheEntryHandleAddHostVersion(&sit_block_handle);
        sitNatCacheEntryHandleDestroy(&sit_block_handle);

        cur_lpa += seg_count;
        count -= seg_count;
    }
}
