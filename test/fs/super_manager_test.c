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
#include "uthash/utarray.h"


void superManagerInit(super_manager *this, file_system_manager *fs_manager);


struct file_system_manager
{
    rtems_recursive_mutex fs_meta_lock_;
    pthread_rwlock_t fs_freeze_lock_;

    SuperCache super_cache_;
    struct RtfsSuperBlock *super_blk_mem_;
    super_manager *sp_manager_;
    node_block_cache *node_cache_;
    dir_data_block_cache *dir_data_cache_;

    SrmapUtils *srmap_utils_;
    SitNatCache *sit_cache_;
    SitNatCache *nat_cache_;

    comm_dev *dev_;
    fd_array *fd_arr_;

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
    SitNatCache nat_cache;
    bool has_nat_block;
    uint32_t nat_block_lpa;
} SuperManagerFixture;


static void superManagerFixtureInit(SuperManagerFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    sitNatCacheInit(&fixture->nat_cache, NULL, 16);
    fixture->fs_manager.super_blk_mem_ = &fixture->super_block;
    fixture->fs_manager.nat_cache_ = &fixture->nat_cache;
}

static void superManagerFixtureFini(SuperManagerFixture *fixture)
{
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
