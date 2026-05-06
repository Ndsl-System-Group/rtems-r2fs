#include "rtfs_test.h"

#include "cache/generic_cache_manager.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/dev.h"
#include "fs/cow_reclaim_registry.h"
#include "fs/fs.h"
#include "fs/fs_manager.h"
#include "fs/super_manager.h"
#include "journal/journal_container.h"
#include "journal/journal_processor.h"

#include <memory.h>
#include <pthread.h>
#include <rtems/thread.h>

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

typedef struct CowReclaimFixture
{
    file_system_manager fs_manager;
    struct RtfsSuperBlock super_block;
    NodeBlockCache node_cache;
    SitNatCache sit_cache;
    SitNatCache nat_cache;
    super_manager *sp_manager;
    JournalContainer journal;
    comm_dev dev;
    struct RtfsSitBlock sit_block;
    struct RtfsNatBlock nat_block;
} CowReclaimFixture;

static void cowReclaimFixtureEnsureSitEntry(CowReclaimFixture *fixture, uint32_t sit_lpa)
{
    SitNatCacheEntry *entry;

    entry = (SitNatCacheEntry *)genericCacheManagerGet(
        &fixture->sit_cache.cacheManager,
        sit_lpa,
        false
    );
    if (entry != NULL) {
        return;
    }

    entry = (SitNatCacheEntry *)malloc(sizeof(*entry));
    TEST_ASSERT_NOT_NULL(entry);
    sitNatCacheEntryInit(entry, sit_lpa);
    memset(blockBufferGetPtr(&entry->cache), 0, BLOCK_BUFFER_SIZE);
    genericCacheManagerAdd(&fixture->sit_cache.cacheManager, sit_lpa, entry);
    fixture->sit_cache.curSize++;
}

static void cowReclaimFixtureEnsureNatEntry(CowReclaimFixture *fixture, uint32_t nat_lpa)
{
    SitNatCacheEntry *entry;

    entry = (SitNatCacheEntry *)genericCacheManagerGet(
        &fixture->nat_cache.cacheManager,
        nat_lpa,
        false
    );
    if (entry != NULL) {
        return;
    }

    entry = (SitNatCacheEntry *)malloc(sizeof(*entry));
    TEST_ASSERT_NOT_NULL(entry);
    sitNatCacheEntryInit(entry, nat_lpa);
    memset(blockBufferGetPtr(&entry->cache), 0, BLOCK_BUFFER_SIZE);
    genericCacheManagerAdd(&fixture->nat_cache.cacheManager, nat_lpa, entry);
    fixture->nat_cache.curSize++;
}

static void cowReclaimFixtureMarkSitValid(
    CowReclaimFixture *fixture,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    uint32_t bitmap_idx = seg_off / 8u;
    uint32_t bitmap_off = seg_off % 8u;
    uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);
    SitNatCacheEntryHandle handle;
    struct RtfsSitEntry *sit_entry;

    cowReclaimFixtureEnsureSitEntry(fixture, sit_lpa);

    handle = sitNatCacheGet(&fixture->sit_cache, sit_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    sit_entry = &sitNatCacheEntryHandleGetSitBlockPtr(&handle)->entries[sit_idx];

    if ((sit_entry->valid_map[bitmap_idx] & (1u << bitmap_off)) == 0) {
        sit_entry->valid_map[bitmap_idx] |= (uint8_t)(1u << bitmap_off);
        if (GET_SIT_VBLOCKS(sit_entry) < 511u) {
            sit_entry->vblocks += 1u;
        }
    }

    sitNatCacheEntryHandleDestroy(&handle);
}

static void cowReclaimFixtureSetNatEntry(
    CowReclaimFixture *fixture,
    uint32_t nid,
    uint32_t ino,
    uint32_t block_addr
)
{
    uint32_t nat_lpa = fixture->super_block.nat_blkaddr + (nid / NAT_ENTRY_PER_BLOCK);
    uint32_t nat_idx = nid % NAT_ENTRY_PER_BLOCK;
    SitNatCacheEntryHandle handle;
    struct RtfsNatEntry *nat_entry;

    cowReclaimFixtureEnsureNatEntry(fixture, nat_lpa);

    handle = sitNatCacheGet(&fixture->nat_cache, nat_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    nat_entry = &sitNatCacheEntryHandleGetNatBlockPtr(&handle)->entries[nat_idx];
    nat_entry->ino = ino;
    nat_entry->block_addr = block_addr;
    sitNatCacheEntryHandleDestroy(&handle);
}

static struct RtfsSitEntry *cowReclaimFixtureGetSitEntry(
    CowReclaimFixture *fixture,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);
    SitNatCacheEntryHandle handle;
    struct RtfsSitEntry *entry;
    struct RtfsSitEntry *copy;

    handle = sitNatCacheGet(&fixture->sit_cache, sit_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    copy = (struct RtfsSitEntry *)malloc(sizeof(*copy));
    TEST_ASSERT_NOT_NULL(copy);
    entry = &sitNatCacheEntryHandleGetSitBlockPtr(&handle)->entries[sit_idx];
    *copy = *entry;
    sitNatCacheEntryHandleDestroy(&handle);

    return copy;
}

static struct RtfsNatEntry *cowReclaimFixtureGetNatEntry(
    CowReclaimFixture *fixture,
    uint32_t nid
)
{
    uint32_t nat_lpa = fixture->super_block.nat_blkaddr + (nid / NAT_ENTRY_PER_BLOCK);
    uint32_t nat_idx = nid % NAT_ENTRY_PER_BLOCK;
    SitNatCacheEntryHandle handle;
    struct RtfsNatEntry *entry;
    struct RtfsNatEntry *copy;

    handle = sitNatCacheGet(&fixture->nat_cache, nat_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    copy = (struct RtfsNatEntry *)malloc(sizeof(*copy));
    TEST_ASSERT_NOT_NULL(copy);
    entry = &sitNatCacheEntryHandleGetNatBlockPtr(&handle)->entries[nat_idx];
    *copy = *entry;
    sitNatCacheEntryHandleDestroy(&handle);

    return copy;
}

static void cowReclaimFixtureInit(CowReclaimFixture *fixture)
{
    SitNatCacheEntry *sit_entry;
    SitNatCacheEntry *nat_entry;
    uint32_t nat_lpa = 0;

    memset(fixture, 0, sizeof(*fixture));
    sitNatCacheSetReadBlockHook(NULL);

    fixture->super_block.segment0_blkaddr = 0;
    fixture->super_block.segment_count = 64;
    fixture->super_block.sit_blkaddr = 200;
    fixture->super_block.segment_count_sit = 1;
    fixture->super_block.nat_blkaddr = 100;
    fixture->super_block.segment_count_nat = 1;
    fixture->super_block.next_free_nid = 9000;

    fixture->fs_manager.super_blk_mem_ = &fixture->super_block;
    fixture->fs_manager.dev_ = &fixture->dev;
    fixture->fs_manager.cur_journal_ = &fixture->journal;

    journalContainerInit(&fixture->journal);

    nodeBlockCacheInit(&fixture->node_cache, &fixture->fs_manager, 8);
    fixture->fs_manager.node_cache_ = &fixture->node_cache;

    sitNatCacheInit(&fixture->sit_cache, &fixture->dev, 8);
    fixture->fs_manager.sit_cache_ = &fixture->sit_cache;

    sitNatCacheInit(&fixture->nat_cache, &fixture->dev, 8);
    fixture->fs_manager.nat_cache_ = &fixture->nat_cache;

    sit_entry = (SitNatCacheEntry *)malloc(sizeof(*sit_entry));
    TEST_ASSERT_NOT_NULL(sit_entry);
    sitNatCacheEntryInit(sit_entry, fixture->super_block.sit_blkaddr);
    memcpy(
        blockBufferGetPtr(&sit_entry->cache),
        &fixture->sit_block,
        sizeof(fixture->sit_block)
    );
    genericCacheManagerAdd(&fixture->sit_cache.cacheManager, fixture->super_block.sit_blkaddr, sit_entry);
    fixture->sit_cache.curSize++;

    nat_lpa = fixture->super_block.nat_blkaddr;
    nat_entry = (SitNatCacheEntry *)malloc(sizeof(*nat_entry));
    TEST_ASSERT_NOT_NULL(nat_entry);
    sitNatCacheEntryInit(nat_entry, nat_lpa);
    memcpy(
        blockBufferGetPtr(&nat_entry->cache),
        &fixture->nat_block,
        sizeof(fixture->nat_block)
    );
    genericCacheManagerAdd(&fixture->nat_cache.cacheManager, nat_lpa, nat_entry);
    fixture->nat_cache.curSize++;

    fixture->sp_manager = superManagerCreate(&fixture->fs_manager);
    fixture->fs_manager.sp_manager_ = fixture->sp_manager;

    cowReclaimRegistryInit(&fixture->fs_manager);
}

static void cowReclaimFixtureFini(CowReclaimFixture *fixture)
{
    cowReclaimRegistryDestroy();
    superManagerDestroy(fixture->sp_manager);
    fixture->sp_manager = NULL;
    sitNatCacheSetReadBlockHook(NULL);
    journalContainerDestroy(&fixture->journal);
    sitNatCacheDestroy(&fixture->nat_cache);
    sitNatCacheDestroy(&fixture->sit_cache);
    nodeBlockCacheDestroy(&fixture->node_cache);
}

RTFS_TEST(CowReclaimRegistryRegister_WhenRegistryIsInactive_ShouldReturnZero)
{
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryRegister(1, NULL, 0, NULL, 0, NULL, 0));
}

RTFS_TEST(CowReclaimRegistryRegisterAndDrain_WhenDataAndNodeLpasComplete_ShouldInvalidateSitEntries)
{
    CowReclaimFixture fixture;
    uint32_t data_lpas[2] = {512u, 1025u};
    uint32_t node_lpas[1] = {513u};
    struct RtfsSitEntry *seg1_entry;
    struct RtfsSitEntry *seg2_entry;

    cowReclaimFixtureInit(&fixture);

    cowReclaimFixtureMarkSitValid(&fixture, data_lpas[0]);
    cowReclaimFixtureMarkSitValid(&fixture, data_lpas[1]);
    cowReclaimFixtureMarkSitValid(&fixture, node_lpas[0]);

    seg1_entry = cowReclaimFixtureGetSitEntry(&fixture, data_lpas[0]);
    seg2_entry = cowReclaimFixtureGetSitEntry(&fixture, data_lpas[1]);

    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)GET_SIT_VBLOCKS(seg1_entry));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)GET_SIT_VBLOCKS(seg2_entry));
    free(seg1_entry);
    free(seg2_entry);

    TEST_ASSERT_EQUAL(
        0,
        cowReclaimRegistryRegister(77u, data_lpas, 2u, node_lpas, 1u, NULL, 0u)
    );
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    seg1_entry = cowReclaimFixtureGetSitEntry(&fixture, data_lpas[0]);
    seg2_entry = cowReclaimFixtureGetSitEntry(&fixture, data_lpas[1]);
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)GET_SIT_VBLOCKS(seg1_entry));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)GET_SIT_VBLOCKS(seg2_entry));
    free(seg1_entry);
    free(seg2_entry);

    cowReclaimRegistryOnTxComplete(77u);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    seg1_entry = cowReclaimFixtureGetSitEntry(&fixture, data_lpas[0]);
    seg2_entry = cowReclaimFixtureGetSitEntry(&fixture, data_lpas[1]);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)GET_SIT_VBLOCKS(seg1_entry));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)GET_SIT_VBLOCKS(seg2_entry));
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)kv_size(fixture.journal.sitJournal));
    free(seg1_entry);
    free(seg2_entry);

    cowReclaimFixtureFini(&fixture);
}

RTFS_TEST(CowReclaimRegistryOnTxComplete_WhenTxIdDoesNotMatch_ShouldLeavePendingRecordsUntouched)
{
    CowReclaimFixture fixture;
    uint32_t data_lpa = 512u;
    struct RtfsSitEntry *entry;

    cowReclaimFixtureInit(&fixture);
    cowReclaimFixtureMarkSitValid(&fixture, data_lpa);
    entry = cowReclaimFixtureGetSitEntry(&fixture, data_lpa);

    TEST_ASSERT_EQUAL(
        0,
        cowReclaimRegistryRegister(88u, &data_lpa, 1u, NULL, 0u, NULL, 0u)
    );

    cowReclaimRegistryOnTxComplete(99u);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    free(entry);
    entry = cowReclaimFixtureGetSitEntry(&fixture, data_lpa);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)GET_SIT_VBLOCKS(entry));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    free(entry);

    cowReclaimFixtureFini(&fixture);
}

RTFS_TEST(CowReclaimRegistryRegister_WhenDeletedNodeHandleCompletes_ShouldFreeNidAndInvalidateNodeLpa)
{
    CowReclaimFixture fixture;
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;
    uint32_t deleted_nid = 7002u;
    uint32_t deleted_lpa = 514u;
    struct RtfsSitEntry *entry;
    struct RtfsNatEntry *nat_entry;

    cowReclaimFixtureInit(&fixture);
    blockBufferInit(&buffer);
    memset(blockBufferGetPtr(&buffer), 0, BLOCK_BUFFER_SIZE);
    node = (struct RtfsNode *)blockBufferGetPtr(&buffer);
    node->footer.nid = deleted_nid;
    node->footer.ino = deleted_nid;
    node->footer.offset = 0;

    cowReclaimFixtureSetNatEntry(&fixture, deleted_nid, deleted_nid, INVALID_NID);
    cowReclaimFixtureMarkSitValid(&fixture, deleted_lpa);
    entry = cowReclaimFixtureGetSitEntry(&fixture, deleted_lpa);

    handle = nodeBlockCacheAdd(&fixture.node_cache, &buffer, deleted_nid, INVALID_NID, deleted_lpa);
    blockBufferDestroy(&buffer);
    TEST_ASSERT_NOT_NULL(handle.entry);
    TEST_ASSERT_EQUAL_UINT32(1u, handle.entry->refCount);

    TEST_ASSERT_EQUAL(
        0,
        cowReclaimRegistryRegister(123u, NULL, 0u, NULL, 0u, &handle, 1u)
    );
    nodeBlockCacheEntryHandleDestroy(&handle);

    cowReclaimRegistryOnTxComplete(123u);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    nat_entry = cowReclaimFixtureGetNatEntry(&fixture, deleted_nid);
    free(entry);
    entry = cowReclaimFixtureGetSitEntry(&fixture, deleted_lpa);
    TEST_ASSERT_EQUAL_UINT32(deleted_nid, fixture.super_block.next_free_nid);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, nat_entry->ino);
    TEST_ASSERT_EQUAL_UINT32(9000u, nat_entry->block_addr);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)GET_SIT_VBLOCKS(entry));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.superBlockJournal));
    free(nat_entry);
    free(entry);

    cowReclaimFixtureFini(&fixture);
}
