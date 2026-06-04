#include "rtfs_test.h"

#include "fs/sit_utils.h"

#include "cache/generic_cache_manager.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "fs/fs_manager.h"
#include "journal/journal_container.h"

#include <rtems/thread.h>
#include <stdlib.h>
#include <string.h>


// XXX 同 nat_utils_test.c 的说明。
typedef struct file_system_manager
{
    rtems_recursive_mutex fs_meta_lock_;
    pthread_rwlock_t fs_freeze_lock_;

    SuperCache super_cache_;
    struct RtfsSuperBlock *super_blk_mem_;
    super_manager *sp_manager_;
    NodeBlockCache *node_cache_;

    SrmapUtils *srmap_utils_;
    SitNatCache *sit_cache_;
    SitNatCache *nat_cache_;

    comm_dev *dev_;

    JournalContainer *cur_journal_;
    bool is_unrecoverable_;
} file_system_manager;

void sitValidateLpaRange(SitOperator *this, uint32_t start_lpa, uint32_t count);

typedef struct SitFixture
{
    file_system_manager fs;
    struct RtfsSuperBlock sb;
    SitNatCache sit_cache;
    JournalContainer journal;
    bool has_sit_block;
    uint32_t sit_block_lpa;
} SitFixture;


static void initFsForSit(file_system_manager *fs, struct RtfsSuperBlock *sb, uint32_t seg0StartLpa, uint32_t segCount, uint32_t sitStartLpa, uint32_t sitSegmentCnt)
{
    memset(fs, 0, sizeof(*fs));
    memset(sb, 0, sizeof(*sb));

    sb->segment0_blkaddr = seg0StartLpa;
    sb->segment_count = segCount;
    sb->sit_blkaddr = sitStartLpa;
    sb->segment_count_sit = sitSegmentCnt;

    fs->super_blk_mem_ = sb;
}

static void sitFixtureInit(
    SitFixture *fixture,
    uint32_t seg0_start_lpa,
    uint32_t seg_count,
    uint32_t sit_start_lpa,
    uint32_t sit_segment_cnt
)
{
    memset(fixture, 0, sizeof(*fixture));
    initFsForSit(
        &fixture->fs,
        &fixture->sb,
        seg0_start_lpa,
        seg_count,
        sit_start_lpa,
        sit_segment_cnt
    );
    sitNatCacheInit(&fixture->sit_cache, NULL, 16);
    journalContainerInit(&fixture->journal);
    fixture->fs.sit_cache_ = &fixture->sit_cache;
    fixture->fs.cur_journal_ = &fixture->journal;
}

static void sitFixtureFini(SitFixture *fixture)
{
    if (fixture->has_sit_block)
    {
        SitNatCacheEntry *entry = (SitNatCacheEntry *)genericCacheManagerRemove(
            &fixture->sit_cache.cacheManager,
            fixture->sit_block_lpa
        );
        if (entry != NULL)
        {
            free(entry->cache.buffer);
            free(entry);
            fixture->sit_cache.curSize = 0;
        }
    }

    journalContainerDestroy(&fixture->journal);
    sitNatCacheDestroy(&fixture->sit_cache);
}

static struct RtfsSitBlock *sitFixtureAddSitBlock(
    SitFixture *fixture,
    uint32_t lpa
)
{
    struct RtfsSitBlock *sit_block =
        (struct RtfsSitBlock *)calloc(1, sizeof(struct RtfsSitBlock));
    SitNatCacheEntry *entry = (SitNatCacheEntry *)malloc(sizeof(SitNatCacheEntry));

    TEST_ASSERT_NOT_NULL(sit_block);
    TEST_ASSERT_NOT_NULL(entry);

    sitNatCacheEntryInit(entry, lpa);
    entry->cache.buffer = (char *)sit_block;

    genericCacheManagerAdd(&fixture->sit_cache.cacheManager, lpa, entry);
    fixture->sit_cache.curSize++;
    fixture->has_sit_block = true;
    fixture->sit_block_lpa = lpa;

    return sit_block;
}


RTFS_TEST(SoInitTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);

    sitOperatorInit(&so, &fs);

    TEST_ASSERT_EQUAL_UINT32(1000, so.seg0StartLpa);
    TEST_ASSERT_EQUAL_UINT32(64, so.segCount);
    TEST_ASSERT_EQUAL_UINT32(200, so.sitStartLpa);
    TEST_ASSERT_EQUAL_UINT32(4, so.sitSegmentCnt);
    TEST_ASSERT_EQUAL_PTR(&fs, so.fsManager);
}

RTFS_TEST(SoGetSegPosFirstLpaTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SegPos pos = sitGetSegPosOfLpa(&so, 1000);

    TEST_ASSERT_EQUAL_UINT32(0, pos.segId);
    TEST_ASSERT_EQUAL_UINT32(0, pos.offset);
}

RTFS_TEST(SoGetSegPosLastBlockInFirstSegTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SegPos pos = sitGetSegPosOfLpa(&so, 1000 + BLOCK_PER_SEGMENT - 1);

    TEST_ASSERT_EQUAL_UINT32(0, pos.segId);
    TEST_ASSERT_EQUAL_UINT32(BLOCK_PER_SEGMENT - 1, pos.offset);
}

RTFS_TEST(SoGetSegPosNextSegFirstBlockTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SegPos pos = sitGetSegPosOfLpa(&so, 1000 + BLOCK_PER_SEGMENT);

    TEST_ASSERT_EQUAL_UINT32(1, pos.segId);
    TEST_ASSERT_EQUAL_UINT32(0, pos.offset);
}

RTFS_TEST(SoGetSegPosMiddleTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 5000, 128, 300, 8);
    sitOperatorInit(&so, &fs);

    uint32_t lpa = 5000 + BLOCK_PER_SEGMENT * 3 + 17;

    SegPos pos = sitGetSegPosOfLpa(&so, lpa);

    TEST_ASSERT_EQUAL_UINT32(3, pos.segId);
    TEST_ASSERT_EQUAL_UINT32(17, pos.offset);
}

RTFS_TEST(SoGetSegIdPosFirstEntryTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SitPos pos = sitGetSegIdPosInSit(&so, 0);

    TEST_ASSERT_EQUAL_UINT32(200, pos.sitLpa);
    TEST_ASSERT_EQUAL_UINT32(0, pos.idx);
}

RTFS_TEST(SoGetSegIdPosLastEntryFirstBlockTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SitPos pos = sitGetSegIdPosInSit(&so, SIT_ENTRY_PER_BLOCK - 1);

    TEST_ASSERT_EQUAL_UINT32(200, pos.sitLpa);
    TEST_ASSERT_EQUAL_UINT32(SIT_ENTRY_PER_BLOCK - 1, pos.idx);
}

RTFS_TEST(SoGetSegIdPosSecondBlockFirstEntryTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SitPos pos = sitGetSegIdPosInSit(&so, SIT_ENTRY_PER_BLOCK);

    TEST_ASSERT_EQUAL_UINT32(201, pos.sitLpa);
    TEST_ASSERT_EQUAL_UINT32(0, pos.idx);
}

RTFS_TEST(SoGetSegIdPosMiddleTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 256, 400, 8);
    sitOperatorInit(&so, &fs);

    uint32_t segId = SIT_ENTRY_PER_BLOCK * 3 + 29;

    SitPos pos = sitGetSegIdPosInSit(&so, segId);

    TEST_ASSERT_EQUAL_UINT32(403, pos.sitLpa);
    TEST_ASSERT_EQUAL_UINT32(29, pos.idx);
}

RTFS_TEST(SoGetFirstLpaOfSegId0Test)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    TEST_ASSERT_EQUAL_UINT32(1000, sitGetFirstLpaOfSegId(&so, 0));
}

RTFS_TEST(SoGetFirstLpaOfSegId1Test)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    TEST_ASSERT_EQUAL_UINT32(1000 + BLOCK_PER_SEGMENT, sitGetFirstLpaOfSegId(&so, 1));
}

RTFS_TEST(SoGetFirstLpaOfSegIdMiddleTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;
    uint32_t segId = 7;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    TEST_ASSERT_EQUAL_UINT32(1000 + segId * BLOCK_PER_SEGMENT, sitGetFirstLpaOfSegId(&so, segId));
}

RTFS_TEST(SitValidateLpaRange_WhenRangeCrossesSegmentBoundary_ShouldAppendOneJournalPerSegment)
{
    SitFixture fixture;
    SitOperator so;
    struct RtfsSitBlock *sit_block;
    uint32_t seg0_tail_off = BLOCK_PER_SEGMENT - 2u;

    sitFixtureInit(&fixture, 1000, 64, 200, 1);
    sit_block = sitFixtureAddSitBlock(&fixture, 200);
    sitOperatorInit(&so, &fixture.fs);

    sitValidateLpaRange(&so, 1000u + seg0_tail_off, 4u);

    TEST_ASSERT_TRUE(
        (sit_block->entries[0].valid_map[seg0_tail_off / 8u] &
         (1u << (seg0_tail_off % 8u))) != 0
    );
    TEST_ASSERT_TRUE(
        (sit_block->entries[0].valid_map[(seg0_tail_off + 1u) / 8u] &
         (1u << ((seg0_tail_off + 1u) % 8u))) != 0
    );
    TEST_ASSERT_TRUE((sit_block->entries[1].valid_map[0] & 0x1u) != 0);
    TEST_ASSERT_TRUE((sit_block->entries[1].valid_map[0] & 0x2u) != 0);
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)GET_SIT_VBLOCKS(&sit_block->entries[0]));
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)GET_SIT_VBLOCKS(&sit_block->entries[1]));
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL_UINT32(
        0u,
        kv_a(SitJournalEntry, fixture.journal.sitJournal, 0).segID
    );
    TEST_ASSERT_EQUAL_UINT32(
        1u,
        kv_a(SitJournalEntry, fixture.journal.sitJournal, 1).segID
    );

    sitFixtureFini(&fixture);
}
