#include "rtfs_test.h"

#include <stdbool.h>
#include <pthread.h>
#include <rtems/thread.h>
#include <stdlib.h>
#include <string.h>

#include "cache/generic_cache_manager.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "fs/fs.h"
#include "fs/fs_manager.h"
#include "fs/super_manager.h"
#include "journal/journal_container.h"
#include "uthash/utarray.h"


void superManagerInit(super_manager *this, file_system_manager *fs_manager);
uint32_t superManagerAllocDataLpaRange(
    super_manager *this,
    uint32_t requested_count,
    uint32_t *allocated_count
);


struct file_system_manager
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
};

struct super_manager
{
    file_system_manager *fs_manager_;
    RtfsSuperBlock *super_block_;
    UT_array *uncommit_node_segs;
    UT_array *uncommit_data_segs;
};

typedef struct
{
    file_system_manager fs_manager;
    RtfsSuperBlock super_block;
    SitNatCache sit_cache;
    SitNatCache nat_cache;
    JournalContainer journal;
    bool has_nat_block;
    uint32_t nat_block_lpa;
    bool has_sit_block;
    uint32_t sit_block_lpa;
} SuperManagerFixture;


static void superManagerFixtureInit(SuperManagerFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    sitNatCacheInit(&fixture->sit_cache, NULL, 16);
    sitNatCacheInit(&fixture->nat_cache, NULL, 16);
    journalContainerInit(&fixture->journal);
    fixture->fs_manager.super_blk_mem_ = &fixture->super_block;
    fixture->fs_manager.sit_cache_ = &fixture->sit_cache;
    fixture->fs_manager.nat_cache_ = &fixture->nat_cache;
    fixture->fs_manager.cur_journal_ = &fixture->journal;
    fixture->super_block.segment0_blkaddr = 1000;
    fixture->super_block.segment_count = 64;
    fixture->super_block.sit_blkaddr = 600;
    fixture->super_block.segment_count_sit = 1;
}

static void superManagerFixtureFini(SuperManagerFixture *fixture)
{
    if (fixture->has_sit_block)
    {
        SitNatCacheEntry *entry = (SitNatCacheEntry *)genericCacheManagerRemove(&fixture->sit_cache.cacheManager, fixture->sit_block_lpa);
        if (entry != NULL)
        {
            free(entry->cache.buffer);
            free(entry);
            fixture->sit_cache.curSize = 0;
        }
    }

    if (fixture->has_nat_block)
    {
        SitNatCacheEntry *entry = (SitNatCacheEntry *)genericCacheManagerRemove(&fixture->nat_cache.cacheManager, fixture->nat_block_lpa);
        if (entry != NULL)
        {
            free(entry->cache.buffer);
            free(entry);
            fixture->nat_cache.curSize = 0;
        }
    }

    journalContainerDestroy(&fixture->journal);
    sitNatCacheDestroy(&fixture->sit_cache);
    sitNatCacheDestroy(&fixture->nat_cache);
}

static struct RtfsNatBlock *superManagerFixtureAddNatBlock(SuperManagerFixture *fixture, uint32_t lpa)
{
    struct RtfsNatBlock *nat_block = (struct RtfsNatBlock *)calloc(1, sizeof(struct RtfsNatBlock));
    SitNatCacheEntry *entry = (SitNatCacheEntry *)malloc(sizeof(SitNatCacheEntry));

    sitNatCacheEntryInit(entry, lpa);
    entry->cache.buffer = (char *)nat_block;

    genericCacheManagerAdd(&fixture->nat_cache.cacheManager, lpa, entry);
    fixture->nat_cache.curSize++;
    fixture->has_nat_block = true;
    fixture->nat_block_lpa = lpa;

    return nat_block;
}

static struct RtfsSitBlock *superManagerFixtureAddSitBlock(SuperManagerFixture *fixture, uint32_t lpa)
{
    struct RtfsSitBlock *sit_block = (struct RtfsSitBlock *)calloc(1, sizeof(struct RtfsSitBlock));
    SitNatCacheEntry *entry = (SitNatCacheEntry *)malloc(sizeof(SitNatCacheEntry));

    sitNatCacheEntryInit(entry, lpa);
    entry->cache.buffer = (char *)sit_block;

    genericCacheManagerAdd(&fixture->sit_cache.cacheManager, lpa, entry);
    fixture->sit_cache.curSize++;
    fixture->has_sit_block = true;
    fixture->sit_block_lpa = lpa;

    return sit_block;
}

static const SuperBlockJournalEntry *superManagerFixtureFindSuperJournalByOff(
    SuperManagerFixture *fixture,
    uint32_t off
)
{
    size_t i;
    const SuperBlockJournalEntry *res = NULL;

    for (i = 0; i < kv_size(fixture->journal.superBlockJournal); ++i) {
        const SuperBlockJournalEntry *entry =
            &kv_a(SuperBlockJournalEntry, fixture->journal.superBlockJournal, i);
        if (entry->Off == off) {
            res = entry;
        }
    }

    return res;
}


RTFS_TEST(SuperManagerInit_ShouldBindFsManagerAndInitializeArrays)
{
    SuperManagerFixture fixture;
    struct super_manager manager;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));

    superManagerInit(&manager, &fixture.fs_manager);

    TEST_ASSERT_EQUAL_PTR(&fixture.fs_manager, manager.fs_manager_);
    TEST_ASSERT_EQUAL_PTR(&fixture.super_block, manager.super_block_);
    TEST_ASSERT_NOT_NULL(manager.uncommit_node_segs);
    TEST_ASSERT_NOT_NULL(manager.uncommit_data_segs);
    TEST_ASSERT_EQUAL(0, utarray_len(manager.uncommit_node_segs));
    TEST_ASSERT_EQUAL(0, utarray_len(manager.uncommit_data_segs));

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerCreate_ShouldAllocateAndInitializeOpaqueManager)
{
    SuperManagerFixture fixture;
    super_manager *manager;
    struct super_manager *impl;

    superManagerFixtureInit(&fixture);

    manager = superManagerCreate(&fixture.fs_manager);

    TEST_ASSERT_NOT_NULL(manager);
    impl = (struct super_manager *)manager;
    TEST_ASSERT_EQUAL_PTR(&fixture.fs_manager, impl->fs_manager_);
    TEST_ASSERT_EQUAL_PTR(&fixture.super_block, impl->super_block_);
    TEST_ASSERT_NOT_NULL(impl->uncommit_node_segs);
    TEST_ASSERT_NOT_NULL(impl->uncommit_data_segs);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)utarray_len(impl->uncommit_node_segs));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)utarray_len(impl->uncommit_data_segs));

    superManagerDestroy(manager);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerDestroy_WhenNull_ShouldBeSafe)
{
    superManagerDestroy(NULL);
}

RTFS_TEST(SuperManagerAllocNid_WhenFreeListIsEmpty_ShouldReturnInvalidNid)
{
    SuperManagerFixture fixture;
    struct super_manager manager;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));
    fixture.super_block.next_free_nid = INVALID_NID;

    superManagerInit(&manager, &fixture.fs_manager);

    TEST_ASSERT_EQUAL(INVALID_NID, superManagerAllocNid(&manager, 123, true));
    TEST_ASSERT_EQUAL(INVALID_NID, fixture.super_block.next_free_nid);

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerAllocNid_WhenNatCacheIsMissing_ShouldReturnInvalidNid)
{
    SuperManagerFixture fixture;
    struct super_manager manager;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));
    fixture.super_block.next_free_nid = 3;
    fixture.super_block.nat_blkaddr = 100;
    fixture.super_block.segment_count_nat = 1;
    fixture.fs_manager.nat_cache_ = NULL;

    superManagerInit(&manager, &fixture.fs_manager);

    TEST_ASSERT_EQUAL(INVALID_NID, superManagerAllocNid(&manager, 55, false));
    TEST_ASSERT_EQUAL(3, fixture.super_block.next_free_nid);

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerAllocNid_WhenAllocatingInode_ShouldPopFreeListAndBindSelfIno)
{
    SuperManagerFixture fixture;
    struct super_manager manager;
    struct RtfsNatBlock *nat_block;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));

    fixture.super_block.next_free_nid = 5;
    fixture.super_block.nat_blkaddr = 100;
    fixture.super_block.segment_count_nat = 1;

    nat_block = superManagerFixtureAddNatBlock(&fixture, 100);
    nat_block->entries[5].ino = 0;
    nat_block->entries[5].block_addr = 9;

    superManagerInit(&manager, &fixture.fs_manager);

    TEST_ASSERT_EQUAL(5, superManagerAllocNid(&manager, 777, true));
    TEST_ASSERT_EQUAL(9, fixture.super_block.next_free_nid);
    TEST_ASSERT_EQUAL(5, nat_block->entries[5].ino);
    TEST_ASSERT_EQUAL(INVALID_LPA, nat_block->entries[5].block_addr);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.superBlockJournal));
    TEST_ASSERT_NOT_NULL(
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, next_free_nid)
        )
    );
    TEST_ASSERT_EQUAL_UINT32(
        9u,
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, next_free_nid)
        )->newVal
    );

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerAllocNid_WhenAllocatingNode_ShouldUseProvidedIno)
{
    SuperManagerFixture fixture;
    struct super_manager manager;
    struct RtfsNatBlock *nat_block;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));

    fixture.super_block.next_free_nid = 7;
    fixture.super_block.nat_blkaddr = 200;
    fixture.super_block.segment_count_nat = 1;

    nat_block = superManagerFixtureAddNatBlock(&fixture, 200);
    nat_block->entries[7].ino = 0;
    nat_block->entries[7].block_addr = 11;

    superManagerInit(&manager, &fixture.fs_manager);

    TEST_ASSERT_EQUAL(7, superManagerAllocNid(&manager, 1234, false));
    TEST_ASSERT_EQUAL(11, fixture.super_block.next_free_nid);
    TEST_ASSERT_EQUAL(1234, nat_block->entries[7].ino);
    TEST_ASSERT_EQUAL(INVALID_LPA, nat_block->entries[7].block_addr);

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerAllocNid_WhenNatEntryHasNoNextFreeNid_ShouldReturnInvalidNid)
{
    SuperManagerFixture fixture;
    struct super_manager manager;
    struct RtfsNatBlock *nat_block;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));

    fixture.super_block.next_free_nid = 4;
    fixture.super_block.nat_blkaddr = 300;
    fixture.super_block.segment_count_nat = 1;

    nat_block = superManagerFixtureAddNatBlock(&fixture, 300);
    nat_block->entries[4].block_addr = INVALID_LPA;

    superManagerInit(&manager, &fixture.fs_manager);

    TEST_ASSERT_EQUAL(INVALID_NID, superManagerAllocNid(&manager, 99, false));
    TEST_ASSERT_EQUAL(4, fixture.super_block.next_free_nid);
    TEST_ASSERT_EQUAL(INVALID_LPA, nat_block->entries[4].block_addr);

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerFreeNid_ShouldPushNidBackToFreeListHead)
{
    SuperManagerFixture fixture;
    struct super_manager manager;
    struct RtfsNatBlock *nat_block;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));

    fixture.super_block.next_free_nid = 12;
    fixture.super_block.nat_blkaddr = 400;
    fixture.super_block.segment_count_nat = 1;

    nat_block = superManagerFixtureAddNatBlock(&fixture, 400);
    nat_block->entries[7].ino = 999;
    nat_block->entries[7].block_addr = 555;

    superManagerInit(&manager, &fixture.fs_manager);

    superManagerFreeNid(&manager, 7);

    TEST_ASSERT_EQUAL(7, fixture.super_block.next_free_nid);
    TEST_ASSERT_EQUAL(INVALID_NID, nat_block->entries[7].ino);
    TEST_ASSERT_EQUAL(12, nat_block->entries[7].block_addr);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.superBlockJournal));
    TEST_ASSERT_EQUAL_UINT32(
        7u,
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, next_free_nid)
        )->newVal
    );

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerFreeNid_WhenNatCacheIsMissing_ShouldLeaveFreeListUntouched)
{
    SuperManagerFixture fixture;
    struct super_manager manager;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));

    fixture.super_block.next_free_nid = 15;
    fixture.super_block.nat_blkaddr = 500;
    fixture.super_block.segment_count_nat = 1;
    fixture.fs_manager.nat_cache_ = NULL;

    superManagerInit(&manager, &fixture.fs_manager);

    superManagerFreeNid(&manager, 9);

    TEST_ASSERT_EQUAL(15, fixture.super_block.next_free_nid);

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerAllocNodeLpa_WhenCurrentSegmentHasSpace_ShouldAdvanceNodeCursorAndAppendSitJournal)
{
    SuperManagerFixture fixture;
    struct super_manager manager;
    struct RtfsSitBlock *sit_block;
    uint32_t lpa;
    uint32_t segid = 2;
    uint32_t blkoff = 3;
    uint32_t bitmap_idx = blkoff / 8u;
    uint32_t bitmap_off = blkoff % 8u;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));
    fixture.super_block.current_node_segment_id = segid;
    fixture.super_block.current_node_segment_blkoff = blkoff;

    sit_block = superManagerFixtureAddSitBlock(&fixture, fixture.super_block.sit_blkaddr);
    SET_NEXT_SEG(&sit_block->entries[segid], 9);

    superManagerInit(&manager, &fixture.fs_manager);

    lpa = superManagerAllocNodeLpa(&manager);

    TEST_ASSERT_EQUAL_UINT32(1000u + segid * BLOCK_PER_SEGMENT + blkoff, lpa);
    TEST_ASSERT_EQUAL_UINT32(segid, fixture.super_block.current_node_segment_id);
    TEST_ASSERT_EQUAL_UINT32(blkoff + 1u, fixture.super_block.current_node_segment_blkoff);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)utarray_len(manager.uncommit_node_segs));
    TEST_ASSERT_TRUE((sit_block->entries[segid].valid_map[bitmap_idx] & (1u << bitmap_off)) != 0);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)GET_SIT_VBLOCKS(&sit_block->entries[segid]));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL_UINT32(segid, kv_a(SitJournalEntry, fixture.journal.sitJournal, 0).segID);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.superBlockJournal));
    TEST_ASSERT_EQUAL_UINT32(
        blkoff + 1u,
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, current_node_segment_blkoff)
        )->newVal
    );

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerAllocDataLpa_WhenCurrentSegmentHasSpace_ShouldAdvanceDataCursorAndAppendSitJournal)
{
    SuperManagerFixture fixture;
    struct super_manager manager;
    struct RtfsSitBlock *sit_block;
    uint32_t lpa;
    uint32_t segid = 1;
    uint32_t blkoff = 7;
    uint32_t bitmap_idx = blkoff / 8u;
    uint32_t bitmap_off = blkoff % 8u;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));
    fixture.super_block.current_data_segment_id = segid;
    fixture.super_block.current_data_segment_blkoff = blkoff;

    sit_block = superManagerFixtureAddSitBlock(&fixture, fixture.super_block.sit_blkaddr);
    SET_NEXT_SEG(&sit_block->entries[segid], 10);

    superManagerInit(&manager, &fixture.fs_manager);

    lpa = superManagerAllocDataLpa(&manager);

    TEST_ASSERT_EQUAL_UINT32(1000u + segid * BLOCK_PER_SEGMENT + blkoff, lpa);
    TEST_ASSERT_EQUAL_UINT32(segid, fixture.super_block.current_data_segment_id);
    TEST_ASSERT_EQUAL_UINT32(blkoff + 1u, fixture.super_block.current_data_segment_blkoff);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)utarray_len(manager.uncommit_data_segs));
    TEST_ASSERT_TRUE((sit_block->entries[segid].valid_map[bitmap_idx] & (1u << bitmap_off)) != 0);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)GET_SIT_VBLOCKS(&sit_block->entries[segid]));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL_UINT32(segid, kv_a(SitJournalEntry, fixture.journal.sitJournal, 0).segID);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.superBlockJournal));
    TEST_ASSERT_EQUAL_UINT32(
        blkoff + 1u,
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, current_data_segment_blkoff)
        )->newVal
    );

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerAllocDataLpaRange_WhenRangeFitsCurrentSegment_ShouldAdvanceOnceAndAppendOneSitJournal)
{
    SuperManagerFixture fixture;
    struct super_manager manager;
    struct RtfsSitBlock *sit_block;
    uint32_t first_lpa;
    uint32_t allocated_count = 0;
    uint32_t segid = 1;
    uint32_t blkoff = 7;
    uint32_t i;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));
    fixture.super_block.current_data_segment_id = segid;
    fixture.super_block.current_data_segment_blkoff = blkoff;

    sit_block = superManagerFixtureAddSitBlock(
        &fixture,
        fixture.super_block.sit_blkaddr
    );
    SET_NEXT_SEG(&sit_block->entries[segid], 10);

    superManagerInit(&manager, &fixture.fs_manager);

    first_lpa = superManagerAllocDataLpaRange(&manager, 4, &allocated_count);

    TEST_ASSERT_EQUAL_UINT32(
        1000u + segid * BLOCK_PER_SEGMENT + blkoff,
        first_lpa
    );
    TEST_ASSERT_EQUAL_UINT32(4u, allocated_count);
    TEST_ASSERT_EQUAL_UINT32(segid, fixture.super_block.current_data_segment_id);
    TEST_ASSERT_EQUAL_UINT32(blkoff + 4u, fixture.super_block.current_data_segment_blkoff);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)utarray_len(manager.uncommit_data_segs));
    for (i = 0; i < 4u; ++i) {
        uint32_t cur_off = blkoff + i;
        TEST_ASSERT_TRUE(
            (sit_block->entries[segid].valid_map[cur_off / 8u] &
             (1u << (cur_off % 8u))) != 0
        );
    }
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)GET_SIT_VBLOCKS(&sit_block->entries[segid]));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL_UINT32(
        segid,
        kv_a(SitJournalEntry, fixture.journal.sitJournal, 0).segID
    );
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.superBlockJournal));
    TEST_ASSERT_EQUAL_UINT32(
        blkoff + 4u,
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, current_data_segment_blkoff)
        )->newVal
    );

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerAllocNodeLpa_WhenCurrentSegmentIsFull_ShouldSwitchSegmentAndTrackRetiredSegment)
{
    SuperManagerFixture fixture;
    struct super_manager manager;
    struct RtfsSitBlock *sit_block;
    uint32_t lpa;
    uint32_t old_segid = 2;
    uint32_t new_segid = 5;
    uint32_t next_free_seg = 8;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));
    fixture.super_block.current_node_segment_id = old_segid;
    fixture.super_block.current_node_segment_blkoff = BLOCK_PER_SEGMENT;
    fixture.super_block.first_free_segment_id = new_segid;
    fixture.super_block.free_segment_count = 3;

    sit_block = superManagerFixtureAddSitBlock(&fixture, fixture.super_block.sit_blkaddr);
    SET_NEXT_SEG(&sit_block->entries[new_segid], next_free_seg);

    superManagerInit(&manager, &fixture.fs_manager);

    lpa = superManagerAllocNodeLpa(&manager);

    TEST_ASSERT_EQUAL_UINT32(1000u + new_segid * BLOCK_PER_SEGMENT, lpa);
    TEST_ASSERT_EQUAL_UINT32(new_segid, fixture.super_block.current_node_segment_id);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.super_block.current_node_segment_blkoff);
    TEST_ASSERT_EQUAL_UINT32(next_free_seg, fixture.super_block.first_free_segment_id);
    TEST_ASSERT_EQUAL_UINT32(2u, fixture.super_block.free_segment_count);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)utarray_len(manager.uncommit_node_segs));
    TEST_ASSERT_EQUAL_UINT32(old_segid, *(uint32_t *)utarray_front(manager.uncommit_node_segs));
    TEST_ASSERT_TRUE((sit_block->entries[new_segid].valid_map[0] & 0x1u) != 0);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)GET_SIT_VBLOCKS(&sit_block->entries[new_segid]));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL_UINT32(new_segid, kv_a(SitJournalEntry, fixture.journal.sitJournal, 0).segID);
    TEST_ASSERT_EQUAL_UINT32(5u, (uint32_t)kv_size(fixture.journal.superBlockJournal));
    TEST_ASSERT_EQUAL_UINT32(
        next_free_seg,
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, first_free_segment_id)
        )->newVal
    );
    TEST_ASSERT_EQUAL_UINT32(
        2u,
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, free_segment_count)
        )->newVal
    );
    TEST_ASSERT_EQUAL_UINT32(
        new_segid,
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, current_node_segment_id)
        )->newVal
    );
    TEST_ASSERT_EQUAL_UINT32(
        1u,
        superManagerFixtureFindSuperJournalByOff(
            &fixture,
            (uint32_t)offsetof(struct RtfsSuperBlock, current_node_segment_blkoff)
        )->newVal
    );

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}

RTFS_TEST(SuperManagerAllocNodeLpa_WhenNoFreeSegmentIsAvailable_ShouldReturnInvalidLpaWithoutAdvancingState)
{
    SuperManagerFixture fixture;
    struct super_manager manager;
    uint32_t lpa;

    superManagerFixtureInit(&fixture);
    memset(&manager, 0, sizeof(manager));
    fixture.super_block.current_node_segment_id = 3;
    fixture.super_block.current_node_segment_blkoff = BLOCK_PER_SEGMENT;
    fixture.super_block.first_free_segment_id = 9;
    fixture.super_block.free_segment_count = 0;

    superManagerInit(&manager, &fixture.fs_manager);

    lpa = superManagerAllocNodeLpa(&manager);

    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, lpa);
    TEST_ASSERT_EQUAL_UINT32(3u, fixture.super_block.current_node_segment_id);
    TEST_ASSERT_EQUAL_UINT32(BLOCK_PER_SEGMENT, fixture.super_block.current_node_segment_blkoff);
    TEST_ASSERT_EQUAL_UINT32(9u, fixture.super_block.first_free_segment_id);
    TEST_ASSERT_EQUAL_UINT32(0u, fixture.super_block.free_segment_count);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)utarray_len(manager.uncommit_node_segs));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));

    utarray_free(manager.uncommit_node_segs);
    utarray_free(manager.uncommit_data_segs);
    superManagerFixtureFini(&fixture);
}
