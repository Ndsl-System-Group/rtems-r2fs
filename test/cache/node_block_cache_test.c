#include "rtfs_test.h"

#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/dev.h"
#include "fs/fs_manager.h"
#include "fs/nat_utils.h"
#include "fs/super_manager.h"
#include "journal/journal_container.h"

#include <memory.h>

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

typedef struct NodeCowFixture
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
    uint32_t written_lpa;
    struct RtfsNode written_node;
    uint32_t fail_write_lpa;
    bool hook_enabled;
} NodeCowFixture;

static NodeCowFixture *g_node_cow_fixture = NULL;

static int nodeCowTestWriteBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    const void *buffer
)
{
    (void)dev;

    if (g_node_cow_fixture == NULL) {
        return EIO;
    }

    if (g_node_cow_fixture->fail_write_lpa != INVALID_LPA &&
        lpa == g_node_cow_fixture->fail_write_lpa) {
        return EIO;
    }

    g_node_cow_fixture->written_lpa = lpa;
    memcpy(&g_node_cow_fixture->written_node, buffer, sizeof(g_node_cow_fixture->written_node));
    return 0;
}

static void nodeCowFixtureInit(NodeCowFixture *fixture)
{
    SitNatCacheEntry *entry;
    SitNatCacheEntry *nat_entry;
    uint32_t nat_lpa_for_7002;

    memset(fixture, 0, sizeof(*fixture));
    fixture->super_block.sit_blkaddr = 200;
    fixture->super_block.nat_blkaddr = 100;
    fixture->super_block.segment_count_nat = 1;
    fixture->super_block.segment0_blkaddr = 0;
    fixture->super_block.current_node_segment_id = 1;
    fixture->super_block.current_node_segment_blkoff = 0;
    fixture->super_block.free_segment_count = 8;

    fixture->fs_manager.super_blk_mem_ = &fixture->super_block;
    fixture->fs_manager.dev_ = &fixture->dev;
    journalContainerInit(&fixture->journal);
    fixture->fs_manager.cur_journal_ = &fixture->journal;

    nodeBlockCacheInit(&fixture->node_cache, &fixture->fs_manager, 8);
    fixture->fs_manager.node_cache_ = &fixture->node_cache;

    sitNatCacheInit(&fixture->sit_cache, &fixture->dev, 8);
    fixture->fs_manager.sit_cache_ = &fixture->sit_cache;

    sitNatCacheInit(&fixture->nat_cache, &fixture->dev, 8);
    fixture->fs_manager.nat_cache_ = &fixture->nat_cache;

    entry = (SitNatCacheEntry *)malloc(sizeof(*entry));
    TEST_ASSERT_NOT_NULL(entry);
    sitNatCacheEntryInit(entry, fixture->super_block.sit_blkaddr);
    memset(blockBufferGetPtr(&entry->cache), 0, BLOCK_BUFFER_SIZE);
    genericCacheManagerAdd(&fixture->sit_cache.cacheManager, fixture->super_block.sit_blkaddr, entry);
    fixture->sit_cache.curSize++;

    nat_lpa_for_7002 = fixture->super_block.nat_blkaddr + (7002 / NAT_ENTRY_PER_BLOCK);
    nat_entry = (SitNatCacheEntry *)malloc(sizeof(*nat_entry));
    TEST_ASSERT_NOT_NULL(nat_entry);
    sitNatCacheEntryInit(nat_entry, nat_lpa_for_7002);
    memset(blockBufferGetPtr(&nat_entry->cache), 0, BLOCK_BUFFER_SIZE);
    genericCacheManagerAdd(&fixture->nat_cache.cacheManager, nat_lpa_for_7002, nat_entry);
    fixture->nat_cache.curSize++;

    fixture->sp_manager = superManagerCreate(&fixture->fs_manager);
    fixture->fs_manager.sp_manager_ = fixture->sp_manager;

    g_node_cow_fixture = fixture;
    fixture->hook_enabled = true;
    nodeBlockCacheSetWriteBlockHook(nodeCowTestWriteBlockHook);
}

static void nodeCowFixtureFini(NodeCowFixture *fixture)
{
    if (fixture->hook_enabled) {
        nodeBlockCacheSetWriteBlockHook(NULL);
        fixture->hook_enabled = false;
        g_node_cow_fixture = NULL;
    }

    superManagerDestroy(fixture->sp_manager);
    fixture->sp_manager = NULL;
    journalContainerDestroy(&fixture->journal);
    sitNatCacheDestroy(&fixture->nat_cache);
    sitNatCacheDestroy(&fixture->sit_cache);
    nodeBlockCacheDestroy(&fixture->node_cache);
}


RTFS_TEST(NbceInitTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    memset(&entry, 0xAA, sizeof(entry));

    nodeBlockCacheEntryInit(&entry, &buffer, 100, 200, 300);


    TEST_ASSERT_EQUAL_UINT32(100, entry.nid);
    TEST_ASSERT_EQUAL_UINT32(200, entry.parentNid);
    TEST_ASSERT_EQUAL_UINT32(300, entry.lpa);
    TEST_ASSERT_EQUAL_UINT32(0, entry.refCount);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, entry.state);


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceDestroyTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 10, 20, 30);

    nodeBlockCacheEntryDestroy(&entry);


    TEST_ASSERT_EQUAL_UINT32(0, entry.nid);
    TEST_ASSERT_EQUAL_UINT32(0, entry.parentNid);
    TEST_ASSERT_EQUAL_UINT32(0, entry.lpa);
    TEST_ASSERT_EQUAL_UINT32(0, entry.refCount);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_DELETED, entry.state);


    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceLpaTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);


    TEST_ASSERT_EQUAL_UINT32(3, nodeBlockCacheEntryGetLpa(&entry));

    nodeBlockCacheEntrySetLpa(&entry, 999);

    TEST_ASSERT_EQUAL_UINT32(999, nodeBlockCacheEntryGetLpa(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceStateTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);


    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, nodeBlockCacheEntryGetState(&entry));

    nodeBlockCacheEntrySetState(&entry, NODE_BLOCK_CACHE_ENTRY_DIRTY);

    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_DIRTY, nodeBlockCacheEntryGetState(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceBufferTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);


    TEST_ASSERT_EQUAL_PTR(&entry.node, nodeBlockCacheEntryGetNodeBuffer(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceNidTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 777, 2, 3);


    TEST_ASSERT_EQUAL_UINT32(777, nodeBlockCacheEntryGetNid(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceNodeBlockPtrTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);


    TEST_ASSERT_EQUAL_PTR(blockBufferGetPtr(&entry.node), nodeBlockCacheEntryGetNodeBlockPtr(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcehInitTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 16);

    NodeBlockCacheEntry entry;
    NodeBlockCacheEntryHandle handle;

    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);
    nodeBlockCacheEntryHandleInit(&handle, &cache, &entry);


    TEST_ASSERT_EQUAL_PTR(&cache, handle.cache);
    TEST_ASSERT_EQUAL_PTR(&entry, handle.entry);


    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheEntryDestroy(&entry);
    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcehIsEmptyTrueTest)
{
    NodeBlockCacheEntryHandle handle;

    nodeBlockCacheEntryHandleInit(&handle, NULL, NULL);

    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&handle));

    nodeBlockCacheEntryHandleDestroy(&handle);
}

RTFS_TEST(NbcehIsEmptyFalseTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 16);

    NodeBlockCacheEntry entry;
    NodeBlockCacheEntryHandle handle;

    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);
    nodeBlockCacheEntryHandleInit(&handle, &cache, &entry);


    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));


    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheEntryDestroy(&entry);
    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcehDestroyEmptyTest)
{
    NodeBlockCacheEntryHandle handle;

    nodeBlockCacheEntryHandleInit(&handle, NULL, NULL);
    nodeBlockCacheEntryHandleDestroy(&handle);

    TEST_PASS();
}

RTFS_TEST(NbcehCopyEmptyTest)
{
    NodeBlockCacheEntryHandle src;
    NodeBlockCacheEntryHandle dst;

    nodeBlockCacheEntryHandleInit(&src, NULL, NULL);
    memset(&dst, 0xAA, sizeof(dst));

    nodeBlockCacheEntryHandleCopy(&dst, &src);


    TEST_ASSERT_EQUAL_PTR(NULL, dst.cache);
    TEST_ASSERT_EQUAL_PTR(NULL, dst.entry);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&dst));


    nodeBlockCacheEntryHandleDestroy(&src);
    nodeBlockCacheEntryHandleDestroy(&dst);
}

RTFS_TEST(NbcehCopyBasicTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 16);

    NodeBlockCacheEntry entry;
    NodeBlockCacheEntryHandle src;
    NodeBlockCacheEntryHandle dst;

    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);

    entry.refCount = 0;

    nodeBlockCacheEntryHandleInit(&src, &cache, &entry);
    nodeBlockCacheEntryHandleCopy(&dst, &src);


    TEST_ASSERT_EQUAL_PTR(&cache, dst.cache);
    TEST_ASSERT_EQUAL_PTR(&entry, dst.entry);
    TEST_ASSERT_EQUAL_UINT32(1, entry.refCount);


    nodeBlockCacheEntryHandleDestroy(&dst);
    nodeBlockCacheEntryHandleDestroy(&src);

    nodeBlockCacheEntryDestroy(&entry);
    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcInitTest)
{
    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    TEST_ASSERT_EQUAL_UINT32(8, cache.expectSize);
    TEST_ASSERT_EQUAL_UINT32(0, cache.curSize);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.fsManager);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.dirtyListHead);
    TEST_ASSERT_NOT_NULL(cache.dirtyPos);


    nodeBlockCacheDestroy(&cache);
}

RTFS_TEST(NbcDestroyTest)
{
    NodeBlockCache cache;

    nodeBlockCacheInit(&cache, NULL, 8);
    nodeBlockCacheDestroy(&cache);

    TEST_ASSERT_EQUAL_UINT32(0, cache.expectSize);
    TEST_ASSERT_EQUAL_UINT32(0, cache.curSize);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.fsManager);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.dirtyListHead);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.dirtyPos);
}

RTFS_TEST(NbcAddTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    NodeBlockCacheEntryHandle handle = nodeBlockCacheAdd(&cache, &buffer, 100, INVALID_NID, 200);

    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));
    TEST_ASSERT_EQUAL_UINT32(1, cache.curSize);

    TEST_ASSERT_EQUAL_UINT32(100, handle.entry->nid);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, handle.entry->parentNid);
    TEST_ASSERT_EQUAL_UINT32(200, handle.entry->lpa);
    TEST_ASSERT_EQUAL_UINT32(1, handle.entry->refCount);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, handle.entry->state);


    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcGetHitTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    NodeBlockCacheEntryHandle h1 = nodeBlockCacheAdd(&cache, &buffer, 123, INVALID_NID, 456);

    NodeBlockCacheEntryHandle h2 = nodeBlockCacheGet(&cache, 123);

    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&h2));
    TEST_ASSERT_EQUAL_PTR(h1.entry, h2.entry);
    TEST_ASSERT_EQUAL_UINT32(2, h1.entry->refCount);


    nodeBlockCacheEntryHandleDestroy(&h1);
    nodeBlockCacheEntryHandleDestroy(&h2);

    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcGetMissTest)
{
    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    NodeBlockCacheEntryHandle handle = nodeBlockCacheGet(&cache, 999);

    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&handle));


    nodeBlockCacheDestroy(&cache);
}

RTFS_TEST(NbcForceReplaceNoopTest)
{
    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    nodeBlockCacheForceReplace(&cache);

    TEST_ASSERT_EQUAL_UINT32(0, cache.curSize);


    nodeBlockCacheDestroy(&cache);
}

RTFS_TEST(NbcGetAndClearDirtyListEmptyTest)
{
    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    NodeBlockCacheDirtyNode *list = nodeBlockCacheGetAndClearDirtyList(&cache);

    TEST_ASSERT_EQUAL_PTR(NULL, list);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.dirtyListHead);


    nodeBlockCacheDestroy(&cache);
}

RTFS_TEST(NbcWritebackDirtyContentCow_WhenNoDirtyNodeExists_ShouldBeStableNoOp)
{
    NodeCowFixture fixture;
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;

    nodeCowFixtureInit(&fixture);
    blockBufferInit(&buffer);

    memset(blockBufferGetPtr(&buffer), 0, BLOCK_BUFFER_SIZE);
    handle = nodeBlockCacheAdd(&fixture.node_cache, &buffer, 6999, INVALID_NID, 66);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));

    TEST_ASSERT_EQUAL(INVALID_LPA, fixture.written_lpa);
    TEST_ASSERT_EQUAL(0, nodeBlockCacheWritebackDirtyContentCow(&fixture.node_cache));
    TEST_ASSERT_EQUAL(INVALID_LPA, fixture.written_lpa);
    TEST_ASSERT_FALSE(handle.entry->hasPendingCowRelocation);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, handle.entry->cowNewLpa);
    TEST_ASSERT_EQUAL_UINT32(66u, handle.entry->lpa);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, handle.entry->state);
    TEST_ASSERT_NULL(fixture.node_cache.dirtyListHead);

    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
    nodeCowFixtureFini(&fixture);
}

RTFS_TEST(NbcWritebackDirtyContentCow_WhenDirtyNodeExists_ShouldWriteNewVersionToNewLpa)
{
    NodeCowFixture fixture;
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;

    nodeCowFixtureInit(&fixture);
    blockBufferInit(&buffer);

    memset(blockBufferGetPtr(&buffer), 0, BLOCK_BUFFER_SIZE);
    handle = nodeBlockCacheAdd(&fixture.node_cache, &buffer, 7000, INVALID_NID, 77);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));

    node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    node->footer.nid = 7000;
    node->footer.ino = 7000;
    node->footer.offset = 123;
    nodeBlockCacheEntryHandleMarkDirty(&handle);

    TEST_ASSERT_EQUAL(INVALID_LPA, fixture.written_lpa);
    TEST_ASSERT_EQUAL(0, nodeBlockCacheWritebackDirtyContentCow(&fixture.node_cache));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, fixture.written_lpa);
    TEST_ASSERT_EQUAL_UINT32(7000u, fixture.written_node.footer.nid);
    TEST_ASSERT_EQUAL_UINT32(7000u, fixture.written_node.footer.ino);
    TEST_ASSERT_EQUAL_UINT32(123u, fixture.written_node.footer.offset);
    TEST_ASSERT_TRUE(handle.entry->hasPendingCowRelocation);
    TEST_ASSERT_EQUAL_UINT32(fixture.written_lpa, handle.entry->cowNewLpa);

    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
    nodeCowFixtureFini(&fixture);
}

RTFS_TEST(NbcWritebackDirtyContentCow_WhenWriteFails_ShouldReturnEioAndKeepDirtyState)
{
    NodeCowFixture fixture;
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;

    nodeCowFixtureInit(&fixture);
    blockBufferInit(&buffer);

    memset(blockBufferGetPtr(&buffer), 0, BLOCK_BUFFER_SIZE);
    handle = nodeBlockCacheAdd(&fixture.node_cache, &buffer, 7003, INVALID_NID, 111);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));

    node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    node->footer.nid = 7003;
    node->footer.ino = 7003;
    nodeBlockCacheEntryHandleMarkDirty(&handle);

    fixture.fail_write_lpa = 512;
    TEST_ASSERT_EQUAL(EIO, nodeBlockCacheWritebackDirtyContentCow(&fixture.node_cache));
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, fixture.written_lpa);
    TEST_ASSERT_TRUE(handle.entry->refCount >= 1);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_DIRTY, handle.entry->state);
    TEST_ASSERT_FALSE(handle.entry->hasPendingCowRelocation);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, handle.entry->cowNewLpa);
    TEST_ASSERT_EQUAL_UINT32(111u, handle.entry->lpa);
    TEST_ASSERT_NOT_NULL(fixture.node_cache.dirtyListHead);

    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
    nodeCowFixtureFini(&fixture);
}

RTFS_TEST(NbcCollectPendingCowRelocations_WhenPendingExists_ShouldCollectIt)
{
    NodeCowFixture fixture;
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;
    NodeBlockCacheCowRelocation relocations[2];
    size_t out_count = 0;

    nodeCowFixtureInit(&fixture);
    blockBufferInit(&buffer);

    memset(blockBufferGetPtr(&buffer), 0, BLOCK_BUFFER_SIZE);
    handle = nodeBlockCacheAdd(&fixture.node_cache, &buffer, 7001, INVALID_NID, 88);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));

    node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    node->footer.nid = 7001;
    node->footer.ino = 7001;
    node->footer.offset = 456;
    nodeBlockCacheEntryHandleMarkDirty(&handle);

    TEST_ASSERT_EQUAL(0, nodeBlockCacheWritebackDirtyContentCow(&fixture.node_cache));
    TEST_ASSERT_EQUAL(0, nodeBlockCacheCollectPendingCowRelocations(
        &fixture.node_cache,
        relocations,
        2,
        &out_count
    ));

    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)out_count);
    TEST_ASSERT_EQUAL_UINT32(7001u, relocations[0].nid);
    TEST_ASSERT_EQUAL_UINT32(88u, relocations[0].oldLpa);
    TEST_ASSERT_EQUAL_UINT32(handle.entry->cowNewLpa, relocations[0].newLpa);

    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
    nodeCowFixtureFini(&fixture);
}

RTFS_TEST(NbcCollectPendingCowRelocations_WhenNoPendingExists_ShouldReturnZeroAndEmpty)
{
    NodeCowFixture fixture;
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;
    NodeBlockCacheCowRelocation relocations[1];
    size_t out_count = 99;

    nodeCowFixtureInit(&fixture);
    blockBufferInit(&buffer);

    memset(blockBufferGetPtr(&buffer), 0, BLOCK_BUFFER_SIZE);
    handle = nodeBlockCacheAdd(&fixture.node_cache, &buffer, 7005, INVALID_NID, 133);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));

    TEST_ASSERT_EQUAL(0, nodeBlockCacheCollectPendingCowRelocations(
        &fixture.node_cache,
        relocations,
        1,
        &out_count
    ));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)out_count);

    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
    nodeCowFixtureFini(&fixture);
}

RTFS_TEST(NbcCollectPendingCowRelocations_WhenCapacityTooSmall_ShouldReturnEnospc)
{
    NodeCowFixture fixture;
    BlockBuffer buffer1;
    BlockBuffer buffer2;
    NodeBlockCacheEntryHandle handle1;
    NodeBlockCacheEntryHandle handle2;
    struct RtfsNode *node;
    NodeBlockCacheCowRelocation relocations[1];
    size_t out_count = 0;

    nodeCowFixtureInit(&fixture);
    blockBufferInit(&buffer1);
    blockBufferInit(&buffer2);

    memset(blockBufferGetPtr(&buffer1), 0, BLOCK_BUFFER_SIZE);
    handle1 = nodeBlockCacheAdd(&fixture.node_cache, &buffer1, 7006, INVALID_NID, 144);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle1));
    node = nodeBlockCacheEntryGetNodeBlockPtr(handle1.entry);
    node->footer.nid = 7006;
    node->footer.ino = 7006;
    nodeBlockCacheEntryHandleMarkDirty(&handle1);

    memset(blockBufferGetPtr(&buffer2), 0, BLOCK_BUFFER_SIZE);
    handle2 = nodeBlockCacheAdd(&fixture.node_cache, &buffer2, 7007, INVALID_NID, 155);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle2));
    node = nodeBlockCacheEntryGetNodeBlockPtr(handle2.entry);
    node->footer.nid = 7007;
    node->footer.ino = 7007;
    nodeBlockCacheEntryHandleMarkDirty(&handle2);

    TEST_ASSERT_EQUAL(0, nodeBlockCacheWritebackDirtyContentCow(&fixture.node_cache));
    TEST_ASSERT_EQUAL(ENOSPC, nodeBlockCacheCollectPendingCowRelocations(
        &fixture.node_cache,
        relocations,
        1,
        &out_count
    ));

    TEST_ASSERT_TRUE(handle1.entry->hasPendingCowRelocation);
    TEST_ASSERT_TRUE(handle2.entry->hasPendingCowRelocation);

    nodeBlockCacheEntryHandleDestroy(&handle2);
    nodeBlockCacheEntryHandleDestroy(&handle1);
    blockBufferDestroy(&buffer2);
    blockBufferDestroy(&buffer1);
    nodeCowFixtureFini(&fixture);
}

RTFS_TEST(NbcApplyPendingCowRelocations_WhenNoPendingExists_ShouldBeStableNoOp)
{
    NodeCowFixture fixture;
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;
    NatLpaMapping nat_mapping;
    NatNidPos pos;
    SitNatCacheEntryHandle nat_handle;
    struct RtfsNatBlock *nat_block;

    nodeCowFixtureInit(&fixture);
    blockBufferInit(&buffer);

    memset(blockBufferGetPtr(&buffer), 0, BLOCK_BUFFER_SIZE);
    handle = nodeBlockCacheAdd(&fixture.node_cache, &buffer, 7004, INVALID_NID, 122);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));

    natLpaMappingInit(&nat_mapping, &fixture.fs_manager);
    pos = natGetNidPos(&nat_mapping, 7004);
    nat_handle = sitNatCacheGet(&fixture.nat_cache, pos.lpa);
    nat_block = sitNatCacheEntryHandleGetNatBlockPtr(&nat_handle);

    TEST_ASSERT_EQUAL(0, nodeBlockCacheApplyPendingCowRelocations(&fixture.node_cache));
    TEST_ASSERT_EQUAL_UINT32(0u, nat_block->entries[pos.idx].block_addr);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.natJournal));
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, handle.entry->state);
    TEST_ASSERT_FALSE(handle.entry->hasPendingCowRelocation);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, handle.entry->cowNewLpa);
    TEST_ASSERT_EQUAL_UINT32(122u, handle.entry->lpa);
    TEST_ASSERT_NULL(fixture.node_cache.dirtyListHead);

    sitNatCacheEntryHandleDestroy(&nat_handle);
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
    nodeCowFixtureFini(&fixture);
}

RTFS_TEST(NbcApplyPendingCowRelocations_WhenMultiplePendingExist_ShouldUpdateAllNatAndClearDirtyList)
{
    NodeCowFixture fixture;
    BlockBuffer buffer1;
    BlockBuffer buffer2;
    NodeBlockCacheEntryHandle handle1;
    NodeBlockCacheEntryHandle handle2;
    struct RtfsNode *node;
    NatLpaMapping nat_mapping;
    NatNidPos pos1;
    NatNidPos pos2;
    SitNatCacheEntryHandle nat_handle1;
    SitNatCacheEntryHandle nat_handle2;
    struct RtfsNatBlock *nat_block1;
    struct RtfsNatBlock *nat_block2;
    uint32_t new_lpa1;
    uint32_t new_lpa2;

    nodeCowFixtureInit(&fixture);
    blockBufferInit(&buffer1);
    blockBufferInit(&buffer2);

    memset(blockBufferGetPtr(&buffer1), 0, BLOCK_BUFFER_SIZE);
    handle1 = nodeBlockCacheAdd(&fixture.node_cache, &buffer1, 7008, INVALID_NID, 166);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle1));
    node = nodeBlockCacheEntryGetNodeBlockPtr(handle1.entry);
    node->footer.nid = 7008;
    node->footer.ino = 7008;
    nodeBlockCacheEntryHandleMarkDirty(&handle1);

    memset(blockBufferGetPtr(&buffer2), 0, BLOCK_BUFFER_SIZE);
    handle2 = nodeBlockCacheAdd(&fixture.node_cache, &buffer2, 7009, INVALID_NID, 177);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle2));
    node = nodeBlockCacheEntryGetNodeBlockPtr(handle2.entry);
    node->footer.nid = 7009;
    node->footer.ino = 7009;
    nodeBlockCacheEntryHandleMarkDirty(&handle2);

    TEST_ASSERT_EQUAL(0, nodeBlockCacheWritebackDirtyContentCow(&fixture.node_cache));
    TEST_ASSERT_TRUE(handle1.entry->hasPendingCowRelocation);
    TEST_ASSERT_TRUE(handle2.entry->hasPendingCowRelocation);
    new_lpa1 = handle1.entry->cowNewLpa;
    new_lpa2 = handle2.entry->cowNewLpa;

    natLpaMappingInit(&nat_mapping, &fixture.fs_manager);
    pos1 = natGetNidPos(&nat_mapping, 7008);
    pos2 = natGetNidPos(&nat_mapping, 7009);
    nat_handle1 = sitNatCacheGet(&fixture.nat_cache, pos1.lpa);
    nat_handle2 = sitNatCacheGet(&fixture.nat_cache, pos2.lpa);
    nat_block1 = sitNatCacheEntryHandleGetNatBlockPtr(&nat_handle1);
    nat_block2 = sitNatCacheEntryHandleGetNatBlockPtr(&nat_handle2);

    TEST_ASSERT_EQUAL(0, nodeBlockCacheApplyPendingCowRelocations(&fixture.node_cache));

    TEST_ASSERT_FALSE(handle1.entry->hasPendingCowRelocation);
    TEST_ASSERT_FALSE(handle2.entry->hasPendingCowRelocation);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, handle1.entry->cowNewLpa);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, handle2.entry->cowNewLpa);
    TEST_ASSERT_EQUAL_UINT32(new_lpa1, handle1.entry->lpa);
    TEST_ASSERT_EQUAL_UINT32(new_lpa2, handle2.entry->lpa);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, handle1.entry->state);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, handle2.entry->state);
    TEST_ASSERT_NULL(fixture.node_cache.dirtyListHead);
    TEST_ASSERT_EQUAL_UINT32(new_lpa1, nat_block1->entries[pos1.idx].block_addr);
    TEST_ASSERT_EQUAL_UINT32(new_lpa2, nat_block2->entries[pos2.idx].block_addr);
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)kv_size(fixture.journal.natJournal));

    sitNatCacheEntryHandleDestroy(&nat_handle2);
    sitNatCacheEntryHandleDestroy(&nat_handle1);
    nodeBlockCacheEntryHandleDestroy(&handle2);
    nodeBlockCacheEntryHandleDestroy(&handle1);
    blockBufferDestroy(&buffer2);
    blockBufferDestroy(&buffer1);
    nodeCowFixtureFini(&fixture);
}

RTFS_TEST(NbcApplyPendingCowRelocations_WhenPendingExists_ShouldUpdateNatAndClearPending)
{
    NodeCowFixture fixture;
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;
    NatLpaMapping nat_mapping;
    NatNidPos pos;
    SitNatCacheEntryHandle nat_handle;
    struct RtfsNatBlock *nat_block;

    nodeCowFixtureInit(&fixture);
    blockBufferInit(&buffer);

    memset(blockBufferGetPtr(&buffer), 0, BLOCK_BUFFER_SIZE);
    handle = nodeBlockCacheAdd(&fixture.node_cache, &buffer, 7002, INVALID_NID, 99);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));

    node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    node->footer.nid = 7002;
    node->footer.ino = 7002;
    node->footer.offset = 789;
    nodeBlockCacheEntryHandleMarkDirty(&handle);

    TEST_ASSERT_EQUAL(0, nodeBlockCacheWritebackDirtyContentCow(&fixture.node_cache));
    TEST_ASSERT_TRUE(handle.entry->hasPendingCowRelocation);

    natLpaMappingInit(&nat_mapping, &fixture.fs_manager);
    pos = natGetNidPos(&nat_mapping, 7002);
    nat_handle = sitNatCacheGet(&fixture.nat_cache, pos.lpa);
    nat_block = sitNatCacheEntryHandleGetNatBlockPtr(&nat_handle);
    TEST_ASSERT_EQUAL_UINT32(0u, nat_block->entries[pos.idx].block_addr);

    TEST_ASSERT_EQUAL(0, nodeBlockCacheApplyPendingCowRelocations(&fixture.node_cache));

    TEST_ASSERT_FALSE(handle.entry->hasPendingCowRelocation);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, handle.entry->cowNewLpa);
    TEST_ASSERT_EQUAL_UINT32(fixture.written_lpa, handle.entry->lpa);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, handle.entry->state);
    TEST_ASSERT_NULL(fixture.node_cache.dirtyListHead);
    TEST_ASSERT_EQUAL_UINT32(fixture.written_lpa, nat_block->entries[pos.idx].block_addr);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(fixture.journal.natJournal));
    TEST_ASSERT_EQUAL_UINT32(7002u, kv_a(NatJournalEntry, fixture.journal.natJournal, 0).nid);
    TEST_ASSERT_EQUAL_UINT32(
        fixture.written_lpa,
        kv_a(NatJournalEntry, fixture.journal.natJournal, 0).newValue.block_addr
    );

    sitNatCacheEntryHandleDestroy(&nat_handle);
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
    nodeCowFixtureFini(&fixture);
}
