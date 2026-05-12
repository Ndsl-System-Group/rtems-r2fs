#include "rtfs_test.h"

#include "cache/generic_cache_manager.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/dev.h"
#include "dir_inode/dir_inode_resolver.h"
#include "fs/cow_reclaim_registry.h"
#include "fs/super_manager.h"
#include "journal/journal_container.h"
#include "journal/journal_processor.h"

#include <errno.h>
#include <memory.h>
#include <pthread.h>
#include <rtems/thread.h>
#include <string.h>

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

typedef struct DirResolverFixture
{
    file_system_manager fs_manager;
    struct RtfsSuperBlock super_block;
    NodeBlockCache node_cache;
    SitNatCache sit_cache;
    SitNatCache nat_cache;
    super_manager *sp_manager;
    JournalContainer journal;
    comm_dev dev;
    struct RtfsNatBlock nat_block;
    struct RtfsSitBlock sit_block;
    struct RtfsNode inode_node;
    struct RtfsNode direct_node1;
    struct RtfsNode direct_node2;
    struct RtfsNode indirect_node1;
    struct RtfsNode indirect_node2;
    struct RtfsNode double_indirect_root;
    struct RtfsDentryBlock dentry_block1;
    struct RtfsDentryBlock dentry_block2;
    struct RtfsDentryBlock dentry_block3;
    struct RtfsDentryBlock dentry_block4;
    uint32_t fail_lpa;
    uint32_t read_count;
    bool hook_enabled;
} DirResolverFixture;

static DirResolverFixture *g_dir_resolver_fixture = NULL;
static uint32_t g_dir_inode_write_lpa = INVALID_LPA;
static struct RtfsDentryBlock g_dir_inode_written_block;
static JournalContainer *g_dir_inode_committed_journal = NULL;
static uint32_t g_dir_inode_fail_write_lpa = INVALID_LPA;
static int g_dir_inode_journal_commit_rc = 0;
static uint32_t g_node_cow_fail_write_lpa = INVALID_LPA;

static void dirResolverFixtureSetNatEntry(
    DirResolverFixture *fixture,
    uint32_t nid,
    uint32_t ino,
    uint32_t block_addr
)
{
    uint32_t nat_lpa = fixture->super_block.nat_blkaddr + (nid / NAT_ENTRY_PER_BLOCK);
    uint32_t nat_idx = nid % NAT_ENTRY_PER_BLOCK;
    SitNatCacheEntry *entry;
    SitNatCacheEntryHandle handle;

    entry = (SitNatCacheEntry *)genericCacheManagerGet(&fixture->nat_cache.cacheManager, nat_lpa, false);
    if (entry == NULL) {
        entry = (SitNatCacheEntry *)malloc(sizeof(*entry));
        TEST_ASSERT_NOT_NULL(entry);
        sitNatCacheEntryInit(entry, nat_lpa);
        memset(blockBufferGetPtr(&entry->cache), 0, BLOCK_BUFFER_SIZE);
        genericCacheManagerAdd(&fixture->nat_cache.cacheManager, nat_lpa, entry);
        fixture->nat_cache.curSize++;
    }

    handle = sitNatCacheGet(&fixture->nat_cache, nat_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    sitNatCacheEntryHandleGetNatBlockPtr(&handle)->entries[nat_idx].ino = ino;
    sitNatCacheEntryHandleGetNatBlockPtr(&handle)->entries[nat_idx].block_addr = block_addr;
    sitNatCacheEntryHandleDestroy(&handle);
}

static void dirResolverFixtureMarkSitValid(
    DirResolverFixture *fixture,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    uint32_t bitmap_idx = seg_off / 8;
    uint32_t bitmap_off = seg_off % 8;
    struct RtfsSitEntry *entry = &fixture->sit_block.entries[sit_idx];

    if ((entry->valid_map[bitmap_idx] & (1u << bitmap_off)) == 0) {
        entry->valid_map[bitmap_idx] |= (1u << bitmap_off);
        if (GET_SIT_VBLOCKS(entry) < 511) {
            entry->vblocks += 1;
        }
    }

    {
        uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);
        SitNatCacheEntryHandle handle = sitNatCacheGet(&fixture->sit_cache, sit_lpa);
        struct RtfsSitBlock *sit_block;
        struct RtfsSitEntry *cached_entry;

        TEST_ASSERT_NOT_NULL(handle.entry);
        sit_block = sitNatCacheEntryHandleGetSitBlockPtr(&handle);
        cached_entry = &sit_block->entries[sit_idx];
        if ((cached_entry->valid_map[bitmap_idx] & (1u << bitmap_off)) == 0) {
            cached_entry->valid_map[bitmap_idx] |= (1u << bitmap_off);
            if (GET_SIT_VBLOCKS(cached_entry) < 511) {
                cached_entry->vblocks += 1;
            }
        }
        sitNatCacheEntryHandleDestroy(&handle);
    }
}

static void dirResolverFixtureSyncCachedNode(
    DirResolverFixture *fixture,
    uint32_t nid,
    const struct RtfsNode *node
)
{
    NodeBlockCacheEntryHandle handle;

    handle = nodeBlockCacheGet(&fixture->node_cache, nid);
    TEST_ASSERT_NOT_NULL(handle.entry);
    blockBufferCopyContentFromBuf(
        nodeBlockCacheEntryGetNodeBuffer(handle.entry),
        (const char *)node
    );
    nodeBlockCacheEntryHandleDestroy(&handle);
}

static int dirResolverTestReadBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    (void)dev;

    if (g_dir_resolver_fixture == NULL) {
        return EIO;
    }

    g_dir_resolver_fixture->read_count++;

    if (g_dir_resolver_fixture->fail_lpa != 0 && lpa == g_dir_resolver_fixture->fail_lpa) {
        return EIO;
    }

    if (lpa == 10) {
        memcpy(buffer, &g_dir_resolver_fixture->inode_node, sizeof(g_dir_resolver_fixture->inode_node));
        return 0;
    }

    if (lpa == 20) {
        memcpy(buffer, &g_dir_resolver_fixture->dentry_block1, sizeof(g_dir_resolver_fixture->dentry_block1));
        return 0;
    }

    if (lpa == 21) {
        memcpy(buffer, &g_dir_resolver_fixture->dentry_block2, sizeof(g_dir_resolver_fixture->dentry_block2));
        return 0;
    }

    if (lpa == 30) {
        memcpy(buffer, &g_dir_resolver_fixture->direct_node1, sizeof(g_dir_resolver_fixture->direct_node1));
        return 0;
    }

    if (lpa == 31) {
        memcpy(buffer, &g_dir_resolver_fixture->direct_node2, sizeof(g_dir_resolver_fixture->direct_node2));
        return 0;
    }

    if (lpa == 40) {
        memcpy(buffer, &g_dir_resolver_fixture->indirect_node1, sizeof(g_dir_resolver_fixture->indirect_node1));
        return 0;
    }

    if (lpa == 41) {
        memcpy(buffer, &g_dir_resolver_fixture->indirect_node2, sizeof(g_dir_resolver_fixture->indirect_node2));
        return 0;
    }

    if (lpa == 50) {
        memcpy(buffer, &g_dir_resolver_fixture->double_indirect_root, sizeof(g_dir_resolver_fixture->double_indirect_root));
        return 0;
    }

    if (lpa == 22) {
        memcpy(buffer, &g_dir_resolver_fixture->dentry_block3, sizeof(g_dir_resolver_fixture->dentry_block3));
        return 0;
    }

    if (lpa == 23) {
        memcpy(buffer, &g_dir_resolver_fixture->dentry_block4, sizeof(g_dir_resolver_fixture->dentry_block4));
        return 0;
    }

    return ENOENT;
}

static int dirInodeTestWriteBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    const void *buffer
)
{
    (void)dev;

    if (g_dir_inode_fail_write_lpa != INVALID_LPA &&
        lpa == g_dir_inode_fail_write_lpa) {
        return EIO;
    }

    g_dir_inode_write_lpa = lpa;
    memcpy(&g_dir_inode_written_block, buffer, sizeof(g_dir_inode_written_block));
    return 0;
}

static int dirNodeCowTestWriteBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    const void *buffer
)
{
    (void)dev;
    (void)buffer;

    if (g_node_cow_fail_write_lpa != INVALID_LPA &&
        lpa == g_node_cow_fail_write_lpa) {
        return EIO;
    }

    return 0;
}

static int dirInodeTestJournalCommitHook(JournalContainer *journal)
{
    if (g_dir_inode_journal_commit_rc != 0) {
        return g_dir_inode_journal_commit_rc;
    }
    g_dir_inode_committed_journal = journal;
    return 0;
}

static void dirAssertSitJournalSegIds(
    JournalContainer *journal,
    const uint32_t *expected_seg_ids,
    size_t expected_count
)
{
    size_t i;

    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)expected_count,
        (uint32_t)kv_size(journal->sitJournal)
    );

    for (i = 0; i < expected_count; ++i) {
        TEST_ASSERT_EQUAL_UINT32(
            expected_seg_ids[i],
            kv_a(SitJournalEntry, journal->sitJournal, i).segID
        );
    }
}

static void dirAssertSitJournalEntryInvalidatesLpa(
    const SitJournalEntry *entry,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t bitmap_idx = seg_off / 8;
    uint32_t bitmap_off = seg_off % 8;

    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT32(seg_id, entry->segID);
    TEST_ASSERT_EQUAL_UINT32(
        0u,
        (uint32_t)(entry->newValue.valid_map[bitmap_idx] & (1u << bitmap_off))
    );
}

static void dirAssertSitJournalEntryVblocksDelta(
    const SitJournalEntry *entry,
    uint32_t old_vblocks,
    uint32_t expected_new_vblocks
)
{
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT32(old_vblocks, expected_new_vblocks + 1u);
    TEST_ASSERT_EQUAL_UINT32(
        expected_new_vblocks,
        (uint32_t)GET_SIT_VBLOCKS(&entry->newValue)
    );
}

static void dirAssertSitJournalDoesNotInvalidateLpa(
    const JournalContainer *journal,
    uint32_t lpa
)
{
    size_t i;
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t bitmap_idx = seg_off / 8u;
    uint32_t bitmap_off = seg_off % 8u;

    TEST_ASSERT_NOT_NULL(journal);

    for (i = 0; i < kv_size(journal->sitJournal); ++i) {
        const SitJournalEntry *entry = &kv_A(journal->sitJournal, i);

        if (entry->segID != seg_id) {
            continue;
        }

        TEST_ASSERT_TRUE(
            (entry->newValue.valid_map[bitmap_idx] & (1u << bitmap_off)) != 0
        );
    }
}

static void dirAssertSitJournalEntryDoesNotInvalidateLpa(
    const SitJournalEntry *entry,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t bitmap_idx = seg_off / 8u;
    uint32_t bitmap_off = seg_off % 8u;

    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT32(seg_id, entry->segID);
    TEST_ASSERT_TRUE(
        (entry->newValue.valid_map[bitmap_idx] & (1u << bitmap_off)) != 0
    );
}

static void dirAssertSitJournalEntryInvalidatesOnlyExpectedLpas(
    const SitJournalEntry *entry,
    const uint32_t *expected_lpas,
    size_t expected_count
)
{
    uint32_t seg_id;
    uint32_t seg_off;
    size_t i;

    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_TRUE(expected_count > 0);

    seg_id = expected_lpas[0] / BLOCK_PER_SEGMENT;
    TEST_ASSERT_EQUAL_UINT32(seg_id, entry->segID);

    for (seg_off = 0; seg_off < BLOCK_PER_SEGMENT; ++seg_off) {
        uint32_t lpa = seg_id * BLOCK_PER_SEGMENT + seg_off;
        bool should_invalidate = false;

        for (i = 0; i < expected_count; ++i) {
            if (expected_lpas[i] == lpa) {
                should_invalidate = true;
                break;
            }
        }

        if (should_invalidate) {
            dirAssertSitJournalEntryInvalidatesLpa(entry, lpa);
        } else {
            dirAssertSitJournalEntryDoesNotInvalidateLpa(entry, lpa);
        }
    }
}

static uint32_t dirFixtureGetCurrentSitVblocks(
    DirResolverFixture *fixture,
    uint32_t seg_id
)
{
    uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    SitNatCacheEntryHandle handle;
    uint32_t vblocks;

    handle = sitNatCacheGet(&fixture->sit_cache, sit_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    vblocks = (uint32_t)GET_SIT_VBLOCKS(
        &sitNatCacheEntryHandleGetSitBlockPtr(&handle)->entries[sit_idx]
    );
    sitNatCacheEntryHandleDestroy(&handle);

    return vblocks;
}

static void dirAssertCurrentSitBitValid(
    DirResolverFixture *fixture,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    uint32_t bitmap_idx = seg_off / 8u;
    uint32_t bitmap_off = seg_off % 8u;
    SitNatCacheEntryHandle handle;
    struct RtfsSitBlock *sit_block;

    handle = sitNatCacheGet(&fixture->sit_cache, sit_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    sit_block = sitNatCacheEntryHandleGetSitBlockPtr(&handle);
    TEST_ASSERT_TRUE((sit_block->entries[sit_idx].valid_map[bitmap_idx] & (1u << bitmap_off)) != 0);
    sitNatCacheEntryHandleDestroy(&handle);
}

static void dirAssertCurrentSitBitInvalid(
    DirResolverFixture *fixture,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    uint32_t bitmap_idx = seg_off / 8u;
    uint32_t bitmap_off = seg_off % 8u;
    SitNatCacheEntryHandle handle;
    struct RtfsSitBlock *sit_block;

    handle = sitNatCacheGet(&fixture->sit_cache, sit_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    sit_block = sitNatCacheEntryHandleGetSitBlockPtr(&handle);
    TEST_ASSERT_EQUAL_UINT32(
        0u,
        (uint32_t)(sit_block->entries[sit_idx].valid_map[bitmap_idx] & (1u << bitmap_off))
    );
    sitNatCacheEntryHandleDestroy(&handle);
}

static void dirResolverFixtureInit(DirResolverFixture *fixture)
{
    SitNatCacheEntry *entry;
    BlockBuffer node_buffer;
    NodeBlockCacheEntryHandle node_handle;
    uint32_t nat_lpa_for_2000;

    memset(fixture, 0, sizeof(*fixture));
    memset(&fixture->super_block, 0, sizeof(fixture->super_block));
    fixture->super_block.nat_blkaddr = 100;
    fixture->super_block.sit_blkaddr = 200;
    fixture->super_block.segment_count_nat = 1;
    fixture->super_block.segment_count_sit = 1;
    fixture->super_block.segment0_blkaddr = 0;
    fixture->super_block.current_node_segment_id = 2;
    fixture->super_block.current_node_segment_blkoff = 0;
    fixture->super_block.current_data_segment_id = 1;
    fixture->super_block.current_data_segment_blkoff = 0;
    fixture->super_block.free_segment_count = 8;
    fixture->super_block.next_free_nid = 6000;

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
    fixture->sp_manager = superManagerCreate(&fixture->fs_manager);
    fixture->fs_manager.sp_manager_ = fixture->sp_manager;
    cowReclaimRegistryInit(&fixture->fs_manager);

    nat_lpa_for_2000 = fixture->super_block.nat_blkaddr + (2000 / NAT_ENTRY_PER_BLOCK);
    entry = (SitNatCacheEntry *)malloc(sizeof(*entry));
    TEST_ASSERT_NOT_NULL(entry);
    sitNatCacheEntryInit(entry, nat_lpa_for_2000);
    memset(blockBufferGetPtr(&entry->cache), 0, BLOCK_BUFFER_SIZE);
    genericCacheManagerAdd(&fixture->nat_cache.cacheManager, nat_lpa_for_2000, entry);
    fixture->nat_cache.curSize++;

    dirResolverFixtureSetNatEntry(fixture, 2000, 2000, 10);
    dirResolverFixtureSetNatEntry(fixture, 3000, 3000, 30);
    dirResolverFixtureSetNatEntry(fixture, 3001, 3001, 31);
    dirResolverFixtureSetNatEntry(fixture, 4000, 4000, 40);
    dirResolverFixtureSetNatEntry(fixture, 4001, 4001, 41);
    dirResolverFixtureSetNatEntry(fixture, 5000, 5000, 50);
    dirResolverFixtureSetNatEntry(fixture, 6000, INVALID_NID, 6001);
    dirResolverFixtureSetNatEntry(fixture, 6001, INVALID_NID, 6002);
    dirResolverFixtureSetNatEntry(fixture, 6002, INVALID_NID, 6003);
    dirResolverFixtureSetNatEntry(fixture, 6003, INVALID_NID, 6004);
    dirResolverFixtureSetNatEntry(fixture, 6004, INVALID_NID, 6005);
    dirResolverFixtureSetNatEntry(fixture, 6005, INVALID_NID, INVALID_NID);

    entry = (SitNatCacheEntry *)malloc(sizeof(*entry));
    TEST_ASSERT_NOT_NULL(entry);
    sitNatCacheEntryInit(entry, fixture->super_block.sit_blkaddr);
    memset(blockBufferGetPtr(&entry->cache), 0, BLOCK_BUFFER_SIZE);
    genericCacheManagerAdd(&fixture->sit_cache.cacheManager, fixture->super_block.sit_blkaddr, entry);
    fixture->sit_cache.curSize++;

    memset(&fixture->sit_block, 0, sizeof(fixture->sit_block));
    dirResolverFixtureMarkSitValid(fixture, 10);
    dirResolverFixtureMarkSitValid(fixture, 20);
    dirResolverFixtureMarkSitValid(fixture, 21);
    dirResolverFixtureMarkSitValid(fixture, 30);
    dirResolverFixtureMarkSitValid(fixture, 31);
    dirResolverFixtureMarkSitValid(fixture, 40);
    dirResolverFixtureMarkSitValid(fixture, 41);
    dirResolverFixtureMarkSitValid(fixture, 50);
    memcpy(blockBufferGetPtr(&entry->cache), &fixture->sit_block, sizeof(fixture->sit_block));

    memset(&fixture->inode_node, 0, sizeof(fixture->inode_node));
    fixture->inode_node.i.i_type = RTFS_FT_DIR;
    fixture->inode_node.i.i_pino = 1999;
    fixture->inode_node.i.i_size = 2 * BLOCK_BUFFER_SIZE;
    fixture->inode_node.i.i_dentry_num = 2;
    fixture->inode_node.i.i_addr[0] = 20;
    fixture->inode_node.i.i_addr[1] = 21;
    fixture->inode_node.footer.nid = 2000;
    fixture->inode_node.footer.ino = 2000;

    memset(&fixture->dentry_block1, 0, sizeof(fixture->dentry_block1));
    fixture->dentry_block1.dentry_bitmap[0] |= 1u;
    fixture->dentry_block1.dentry[0].ino = 3001;
    fixture->dentry_block1.dentry[0].name_len = 5;
    fixture->dentry_block1.dentry[0].file_type = RTFS_FT_REG_FILE;
    memcpy(fixture->dentry_block1.filename[0], "alpha", 5);

    memset(&fixture->dentry_block2, 0, sizeof(fixture->dentry_block2));
    fixture->dentry_block2.dentry_bitmap[0] |= 1u;
    fixture->dentry_block2.dentry[0].ino = 3002;
    fixture->dentry_block2.dentry[0].name_len = 4;
    fixture->dentry_block2.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(fixture->dentry_block2.filename[0], "beta", 4);

    memset(&fixture->dentry_block3, 0, sizeof(fixture->dentry_block3));
    fixture->dentry_block3.dentry_bitmap[0] |= 1u;
    fixture->dentry_block3.dentry[0].ino = 3003;
    fixture->dentry_block3.dentry[0].name_len = 5;
    fixture->dentry_block3.dentry[0].file_type = RTFS_FT_REG_FILE;
    memcpy(fixture->dentry_block3.filename[0], "gamma", 5);

    memset(&fixture->dentry_block4, 0, sizeof(fixture->dentry_block4));
    fixture->dentry_block4.dentry_bitmap[0] |= 1u;
    fixture->dentry_block4.dentry[0].ino = 3004;
    fixture->dentry_block4.dentry[0].name_len = 5;
    fixture->dentry_block4.dentry[0].file_type = RTFS_FT_DIR;
    memcpy(fixture->dentry_block4.filename[0], "delta", 5);

    memset(&fixture->direct_node1, 0, sizeof(fixture->direct_node1));
    fixture->direct_node1.dn.addr[0] = 22;
    fixture->direct_node1.footer.nid = 3000;
    fixture->direct_node1.footer.ino = 2000;

    memset(&fixture->direct_node2, 0, sizeof(fixture->direct_node2));
    fixture->direct_node2.dn.addr[0] = 23;
    fixture->direct_node2.footer.nid = 3001;
    fixture->direct_node2.footer.ino = 2000;

    memset(&fixture->indirect_node1, 0, sizeof(fixture->indirect_node1));
    fixture->indirect_node1.in.nid[0] = 3000;
    fixture->indirect_node1.footer.nid = 4000;
    fixture->indirect_node1.footer.ino = 2000;

    memset(&fixture->indirect_node2, 0, sizeof(fixture->indirect_node2));
    fixture->indirect_node2.in.nid[0] = 3001;
    fixture->indirect_node2.footer.nid = 4001;
    fixture->indirect_node2.footer.ino = 2000;

    memset(&fixture->double_indirect_root, 0, sizeof(fixture->double_indirect_root));
    fixture->double_indirect_root.in.nid[0] = 4001;
    fixture->double_indirect_root.footer.nid = 5000;
    fixture->double_indirect_root.footer.ino = 2000;

    blockBufferInit(&node_buffer);

    blockBufferCopyContentFromBuf(&node_buffer, (const char *)&fixture->inode_node);
    node_handle = nodeBlockCacheAdd(&fixture->node_cache, &node_buffer, 2000, INVALID_NID, 10);
    nodeBlockCacheEntryHandleDestroy(&node_handle);

    blockBufferCopyContentFromBuf(&node_buffer, (const char *)&fixture->direct_node1);
    node_handle = nodeBlockCacheAdd(&fixture->node_cache, &node_buffer, 3000, 2000, 30);
    nodeBlockCacheEntryHandleDestroy(&node_handle);

    blockBufferCopyContentFromBuf(&node_buffer, (const char *)&fixture->direct_node2);
    node_handle = nodeBlockCacheAdd(&fixture->node_cache, &node_buffer, 3001, 2000, 31);
    nodeBlockCacheEntryHandleDestroy(&node_handle);

    blockBufferCopyContentFromBuf(&node_buffer, (const char *)&fixture->indirect_node1);
    node_handle = nodeBlockCacheAdd(&fixture->node_cache, &node_buffer, 4000, 2000, 40);
    nodeBlockCacheEntryHandleDestroy(&node_handle);

    blockBufferCopyContentFromBuf(&node_buffer, (const char *)&fixture->indirect_node2);
    node_handle = nodeBlockCacheAdd(&fixture->node_cache, &node_buffer, 4001, 2000, 41);
    nodeBlockCacheEntryHandleDestroy(&node_handle);

    blockBufferCopyContentFromBuf(&node_buffer, (const char *)&fixture->double_indirect_root);
    node_handle = nodeBlockCacheAdd(&fixture->node_cache, &node_buffer, 5000, 2000, 50);
    nodeBlockCacheEntryHandleDestroy(&node_handle);

    blockBufferDestroy(&node_buffer);

    g_dir_resolver_fixture = fixture;
    fixture->hook_enabled = true;
    rtfsDirResolverSetReadBlockHook(dirResolverTestReadBlockHook);
    rtfsDirInodeSetWriteBlockHook(dirInodeTestWriteBlockHook);
    rtfsDirInodeSetJournalCommitHook(dirInodeTestJournalCommitHook);
    nodeBlockCacheSetWriteBlockHook(dirNodeCowTestWriteBlockHook);
    g_dir_inode_write_lpa = INVALID_LPA;
    g_dir_inode_fail_write_lpa = INVALID_LPA;
    g_dir_inode_journal_commit_rc = 0;
    g_node_cow_fail_write_lpa = INVALID_LPA;
    memset(&g_dir_inode_written_block, 0, sizeof(g_dir_inode_written_block));
    g_dir_inode_committed_journal = NULL;
}

static void dirResolverFixtureFini(DirResolverFixture *fixture)
{
    if (fixture->hook_enabled) {
        rtfsDirResolverSetReadBlockHook(NULL);
        rtfsDirInodeSetWriteBlockHook(NULL);
        rtfsDirInodeSetJournalCommitHook(NULL);
        nodeBlockCacheSetWriteBlockHook(NULL);
        fixture->hook_enabled = false;
        g_dir_resolver_fixture = NULL;
    }

    if (g_dir_inode_committed_journal != NULL) {
        journalContainerDestroy(g_dir_inode_committed_journal);
        free(g_dir_inode_committed_journal);
        g_dir_inode_committed_journal = NULL;
    }

    cowReclaimRegistryDestroy();
    superManagerDestroy(fixture->sp_manager);
    fixture->sp_manager = NULL;
    journalContainerDestroy(&fixture->journal);
    sitNatCacheDestroy(&fixture->sit_cache);
    sitNatCacheDestroy(&fixture->nat_cache);
    nodeBlockCacheDestroy(&fixture->node_cache);
}

static void writeInlineDentryName(
    struct RtfsInlineDentry *inline_dentry,
    size_t index,
    const char *name
)
{
    size_t name_len = strlen(name);
    size_t slot_count = GET_DENTRY_SLOTS(name_len);
    size_t slot;
    size_t offset = 0;

    for (slot = 0; slot < slot_count; ++slot) {
        size_t copy_len = RTFS_SLOT_LEN;
        if (offset + copy_len > name_len) {
            copy_len = name_len - offset;
        }

        memcpy(inline_dentry->filename[index + slot], name + offset, copy_len);
        offset += copy_len;
    }
}

static void addInlineDentry(
    struct RtfsInlineDentry *inline_dentry,
    size_t index,
    rtfs_ino ino,
    uint8_t file_type,
    const char *name
)
{
    size_t slot_count = GET_DENTRY_SLOTS(strlen(name));
    size_t slot;

    inline_dentry->dentry[index].ino = ino;
    inline_dentry->dentry[index].name_len = strlen(name);
    inline_dentry->dentry[index].file_type = file_type;
    for (slot = 0; slot < slot_count; ++slot) {
        inline_dentry->dentry_bitmap[(index + slot) / 8] |=
            (uint8_t)(1u << ((index + slot) % 8));
    }
    writeInlineDentryName(inline_dentry, index, name);
}

RTFS_TEST(DirInodeResolve_WhenRequestIsNull_ShouldReturnEINVAL)
{
    RtfsDirInode *dir_inode = NULL;

    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeResolve(NULL, NULL, NULL, &dir_inode));
    TEST_ASSERT_NULL(dir_inode);
}

RTFS_TEST(DirInodeResolve_WhenOutParamIsNull_ShouldReturnEINVAL)
{
    RtfsDirInodeBuildRequest request = {
        .ino = 1,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };

    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeResolve(NULL, NULL, &request, NULL));
}

RTFS_TEST(DirInodeResolve_WhenEagerModeIsNotImplemented_ShouldReturnENOSYS)
{
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 1,
        .mode = RTFS_DIR_BUILD_EAGER
    };

    TEST_ASSERT_EQUAL(ENOSYS, rtfsDirInodeResolve(NULL, NULL, &request, &dir_inode));
    TEST_ASSERT_NULL(dir_inode);
}

RTFS_TEST(DirInodeResolve_WhenLoaderFails_ShouldPropagateError)
{
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 1,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };

    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeResolve(NULL, NULL, &request, &dir_inode));
    TEST_ASSERT_NULL(dir_inode);
}

RTFS_TEST(DirInodeResolve_WhenFsManagerAssemblyIsIncomplete_ShouldReturnEINVAL)
{
    file_system_manager fs_manager;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 1,
        .mode = RTFS_DIR_BUILD_INLINE_IF_POSSIBLE
    };

    memset(&fs_manager, 0, sizeof(fs_manager));

    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeResolve(&fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NULL(dir_inode);
}

RTFS_TEST(DirInodeResolveNext_WhenArgumentsAreInvalid_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeResolveNext(NULL, 1, NULL));
}

RTFS_TEST(DirInodeResolveNext_WhenFsManagerAssemblyIsIncomplete_ShouldReturnEINVAL)
{
    file_system_manager fs_manager;
    RtfsDirInode *dir_inode = NULL;

    memset(&fs_manager, 0, sizeof(fs_manager));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeBuild(NULL, 42, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeResolveNext(&fs_manager, 42, dir_inode));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeResolveAndResolveNext_WhenNonInlineDirectoryExists_ShouldLoadEntriesIncrementally)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsDirLookupResult result;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(1u, rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_EQUAL(2u, rtfsDirInodeGetTotalBlockCount(dir_inode));
    TEST_ASSERT_FALSE(rtfsDirInodeIsFullyLoaded(dir_inode));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "alpha", 5, &result));
    TEST_ASSERT_EQUAL(3001u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(2000u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_REG_FILE, result.inode_view.file_type);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "beta", 4, &result));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));
    TEST_ASSERT_EQUAL(2u, rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_TRUE(rtfsDirInodeIsFullyLoaded(dir_inode));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "beta", 4, &result));
    TEST_ASSERT_EQUAL(3002u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(2000u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_DIR, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeResolveNext_WhenRegularDirectoryIsAlreadyFullyLoaded_ShouldNotReadAgainOrAdvanceProgress)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    uint32_t read_count_after_full_load;
    size_t loaded_count_after_full_load;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));
    TEST_ASSERT_TRUE(rtfsDirInodeIsFullyLoaded(dir_inode));

    read_count_after_full_load = fixture.read_count;
    loaded_count_after_full_load = rtfsDirInodeGetLoadedBlockCount(dir_inode);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));
    TEST_ASSERT_EQUAL(read_count_after_full_load, fixture.read_count);
    TEST_ASSERT_EQUAL(
        (uint32_t)loaded_count_after_full_load,
        (uint32_t)rtfsDirInodeGetLoadedBlockCount(dir_inode)
    );
    TEST_ASSERT_TRUE(rtfsDirInodeIsFullyLoaded(dir_inode));

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeResolveNext_WhenSecondBlockReadFails_ShouldReturnEioAndKeepProgress)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsDirLookupResult result;

    dirResolverFixtureInit(&fixture);
    fixture.fail_lpa = 21;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(1u, rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_FALSE(rtfsDirInodeIsFullyLoaded(dir_inode));

    TEST_ASSERT_EQUAL(EIO, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));
    TEST_ASSERT_EQUAL(1u, rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_FALSE(rtfsDirInodeIsFullyLoaded(dir_inode));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "beta", 4, &result));

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeResolveNext_WhenReadFailureIsCleared_ShouldResumeFromSameProgressAndSucceed)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsDirLookupResult result;
    uint32_t read_count_after_failure;

    dirResolverFixtureInit(&fixture);
    fixture.fail_lpa = 21;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(1u, rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_FALSE(rtfsDirInodeIsFullyLoaded(dir_inode));

    TEST_ASSERT_EQUAL(EIO, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));
    TEST_ASSERT_EQUAL(1u, rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_FALSE(rtfsDirInodeIsFullyLoaded(dir_inode));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "beta", 4, &result));
    read_count_after_failure = fixture.read_count;

    fixture.fail_lpa = 0;
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));
    TEST_ASSERT_EQUAL(2u, rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_TRUE(rtfsDirInodeIsFullyLoaded(dir_inode));
    TEST_ASSERT_EQUAL(read_count_after_failure + 1u, fixture.read_count);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "beta", 4, &result));
    TEST_ASSERT_EQUAL(3002u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(2000u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_DIR, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeResolveAndResolveNext_WhenInlineDirectoryIsAlreadyLoaded_ShouldNotReadAgain)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    struct RtfsInlineDentry *inline_dentry;
    uint32_t read_count_after_resolve;
    RtfsDirLookupResult result;

    dirResolverFixtureInit(&fixture);

    memset(&fixture.inode_node, 0, sizeof(fixture.inode_node));
    fixture.inode_node.i.i_inline = RTFS_INLINE_DENTRY;
    fixture.inode_node.i.i_type = RTFS_FT_DIR;
    fixture.inode_node.i.i_pino = 1999;
    fixture.inode_node.i.i_size = BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 1;
    fixture.inode_node.footer.nid = 2000;
    fixture.inode_node.footer.ino = 2000;

    inline_dentry = (struct RtfsInlineDentry *)fixture.inode_node.i.i_addr;
    addInlineDentry(inline_dentry, 0, 3003, RTFS_FT_REG_FILE, "solo");
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_TRUE(rtfsDirInodeIsFullyLoaded(dir_inode));
    TEST_ASSERT_EQUAL(1u, rtfsDirInodeGetLoadedBlockCount(dir_inode));
    TEST_ASSERT_EQUAL(1u, rtfsDirInodeGetTotalBlockCount(dir_inode));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "solo", 4, &result));
    read_count_after_resolve = fixture.read_count;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));
    TEST_ASSERT_EQUAL(read_count_after_resolve, fixture.read_count);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeResolveNext_WhenSingleIndirectPathIsNeeded_ShouldLoadTargetEntry)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsDirLookupResult result;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = (DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK + 1U) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[2] = 4000;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, target_block_index);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "gamma", 5, &result));
    TEST_ASSERT_EQUAL(3003u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(2000u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_REG_FILE, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeResolveNext_WhenDoubleIndirectPathIsNeeded_ShouldLoadTargetEntry)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsDirLookupResult result;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size =
        ((uint64_t)DEF_ADDRS_PER_INODE +
         2ULL * DEF_ADDRS_PER_BLOCK +
         2ULL * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK +
         1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[4] = 5000;
    memset(&fixture.double_indirect_root, 0, sizeof(fixture.double_indirect_root));
    fixture.double_indirect_root.in.nid[0] = 4000;
    fixture.double_indirect_root.footer.nid = 5000;
    fixture.double_indirect_root.footer.ino = 2000;
    memset(&fixture.indirect_node1, 0, sizeof(fixture.indirect_node1));
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.indirect_node1.footer.nid = 4000;
    fixture.indirect_node1.footer.ino = 2000;
    memset(&fixture.direct_node1, 0, sizeof(fixture.direct_node1));
    fixture.direct_node1.dn.addr[0] = 23;
    fixture.direct_node1.footer.nid = 3000;
    fixture.direct_node1.footer.ino = 2000;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 5000, &fixture.double_indirect_root);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, target_block_index);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));
    TEST_ASSERT_EQUAL(target_block_index + 1, rtfsDirInodeGetLoadedBlockCount(dir_inode));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "delta", 5, &result));
    TEST_ASSERT_EQUAL(3004u, result.inode_view.ino);
    TEST_ASSERT_EQUAL(2000u, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(RTFS_FT_DIR, result.inode_view.file_type);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenRegularBlockIsFull_ShouldGrowDirectBlock)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;
    uint32_t new_lpa;

    dirResolverFixtureInit(&fixture);

    fixture.inode_node.i.i_size = BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_dentry_num = 1;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3010, 2000, RTFS_FT_DIR);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "grow", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "grow", 4, &result));
    TEST_ASSERT_EQUAL(3010u, result.inode_view.ino);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    new_lpa = cached_inode_node->i.i_addr[1];
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, new_lpa);
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)SIZE_TO_BLOCK(cached_inode_node->i.i_size));
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenInlineCapacityIsExhausted_ShouldConvertToRegularAndKeepEntriesMutable)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;
    struct RtfsInlineDentry *inline_dentry;
    char existing_name[9];
    int i;

    dirResolverFixtureInit(&fixture);

    memset(&fixture.inode_node, 0, sizeof(fixture.inode_node));
    fixture.inode_node.i.i_inline = RTFS_INLINE_DENTRY;
    fixture.inode_node.i.i_type = RTFS_FT_DIR;
    fixture.inode_node.i.i_pino = 1999;
    fixture.inode_node.i.i_size = BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = NR_INLINE_DENTRY;
    fixture.inode_node.i.i_mtime = 777;
    fixture.inode_node.footer.nid = 2000;
    fixture.inode_node.footer.ino = 2000;
    inline_dentry = (struct RtfsInlineDentry *)fixture.inode_node.i.i_addr;

    for (i = 0; i < NR_INLINE_DENTRY; ++i) {
        char name[9];

        snprintf(name, sizeof(name), "e%03d", i);
        addInlineDentry(
            inline_dentry,
            (size_t)i,
            (rtfs_ino)(6000 + i),
            RTFS_FT_REG_FILE,
            name
        );
    }
    snprintf(existing_name, sizeof(existing_name), "e%03d", 0);

    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3600, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "z", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "z", 1, &result));
    TEST_ASSERT_EQUAL(3600u, result.inode_view.ino);

    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeLookup(dir_inode, existing_name, strlen(existing_name), &result)
    );
    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, existing_name));
    TEST_ASSERT_EQUAL(
        ENOENT,
        rtfsDirInodeLookup(dir_inode, existing_name, strlen(existing_name), &result)
    );
    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, "z"));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "z", 1, &result));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_FALSE((cached_inode_node->i.i_inline & RTFS_INLINE_DENTRY) != 0);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT64(BLOCK_BUFFER_SIZE, cached_inode_node->i.i_size);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenGrowDirectBlockCannotAllocateDataLpa_ShouldReturnEnospcAndKeepMappingStable)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);

    fixture.inode_node.i.i_size = BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_dentry_num = 1;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    fixture.super_block.current_data_segment_blkoff = BLOCK_PER_SEGMENT;
    fixture.super_block.free_segment_count = 0;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3011, 2000, RTFS_FT_DIR);
    TEST_ASSERT_EQUAL(ENOSPC, rtfsDirInodeAddEntry(dir_inode, "grf1", &child_view));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "grf1", 4, &result));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(20u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, cached_inode_node->i.i_addr[1]);
    TEST_ASSERT_EQUAL_UINT32(BLOCK_BUFFER_SIZE, (uint32_t)cached_inode_node->i.i_size);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenDirectBlocksAreFull_ShouldGrowIntoDirectNode)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = (uint64_t)DEF_ADDRS_PER_INODE * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 0;
    fixture.inode_node.i.i_nid[0] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3020, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "ndir", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "ndir", 4, &result));
    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_NOT_EQUAL(INVALID_NID, cached_inode_node->i.i_nid[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenGrowIntoDirectNodeCannotAllocateNid_ShouldReturnEnospcAndKeepMappingStable)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;
    size_t old_cur_size;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = (uint64_t)DEF_ADDRS_PER_INODE * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 0;
    fixture.inode_node.i.i_nid[0] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    fixture.super_block.next_free_nid = INVALID_NID;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    old_cur_size = fixture.node_cache.curSize;

    rtfsRuntimeInodeViewInit(&child_view, 3021, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(ENOSPC, rtfsDirInodeAddEntry(dir_inode, "ndrf", &child_view));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "ndrf", 4, &result));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)old_cur_size, (uint32_t)fixture.node_cache.curSize);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_inode_node->i.i_nid[0]);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)((uint64_t)DEF_ADDRS_PER_INODE * BLOCK_BUFFER_SIZE),
        (uint32_t)cached_inode_node->i.i_size
    );
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenDirectNodesAreFull_ShouldGrowIntoSingleIndirect)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size =
        ((uint64_t)DEF_ADDRS_PER_INODE + 2ULL * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 0;
    fixture.inode_node.i.i_nid[2] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3030, 2000, RTFS_FT_DIR);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "nind", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "nind", 4, &result));
    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_NOT_EQUAL(INVALID_NID, cached_inode_node->i.i_nid[2]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenGrowIntoSingleIndirectCannotAllocateNid_ShouldReturnEnospcAndKeepMappingStable)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;
    size_t old_cur_size;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size =
        ((uint64_t)DEF_ADDRS_PER_INODE + 2ULL * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 0;
    fixture.inode_node.i.i_nid[2] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    fixture.super_block.next_free_nid = INVALID_NID;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    old_cur_size = fixture.node_cache.curSize;

    rtfsRuntimeInodeViewInit(&child_view, 3031, 2000, RTFS_FT_DIR);
    TEST_ASSERT_EQUAL(ENOSPC, rtfsDirInodeAddEntry(dir_inode, "snif", &child_view));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "snif", 4, &result));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)old_cur_size, (uint32_t)fixture.node_cache.curSize);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_inode_node->i.i_nid[2]);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)((uint64_t)DEF_ADDRS_PER_INODE + 2ULL * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE,
        (uint32_t)cached_inode_node->i.i_size
    );
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenGrowIntoSingleIndirectChildCreationFails_ShouldNotLeaveHalfAttachedIndirectRoot)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle leaked_root_handle;
    struct RtfsNode *cached_inode_node;
    size_t old_cur_size;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size =
        ((uint64_t)DEF_ADDRS_PER_INODE + 2ULL * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 0;
    fixture.inode_node.i.i_nid[2] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    fixture.super_block.next_free_nid = 6005;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    old_cur_size = fixture.node_cache.curSize;

    rtfsRuntimeInodeViewInit(&child_view, 3032, 2000, RTFS_FT_DIR);
    TEST_ASSERT_EQUAL(ENOSPC, rtfsDirInodeAddEntry(dir_inode, "snih", &child_view));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "snih", 4, &result));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)old_cur_size, (uint32_t)fixture.node_cache.curSize);
    dirAssertCurrentSitBitInvalid(&fixture, 512);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_inode_node->i.i_nid[2]);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)((uint64_t)DEF_ADDRS_PER_INODE + 2ULL * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE,
        (uint32_t)cached_inode_node->i.i_size
    );
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    leaked_root_handle = nodeBlockCacheGet(&fixture.node_cache, 6005);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&leaked_root_handle));
    nodeBlockCacheEntryHandleDestroy(&leaked_root_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenSingleIndirectIsFull_ShouldGrowIntoDoubleIndirect)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size =
        ((uint64_t)DEF_ADDRS_PER_INODE +
         2ULL * DEF_ADDRS_PER_BLOCK +
         2ULL * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 0;
    fixture.inode_node.i.i_nid[4] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3040, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "ndind", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "ndind", 5, &result));
    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_NOT_EQUAL(INVALID_NID, cached_inode_node->i.i_nid[4]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenGrowIntoDoubleIndirectCannotAllocateNid_ShouldReturnEnospcAndKeepMappingStable)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;
    size_t old_cur_size;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size =
        ((uint64_t)DEF_ADDRS_PER_INODE +
         2ULL * DEF_ADDRS_PER_BLOCK +
         2ULL * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 0;
    fixture.inode_node.i.i_nid[4] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    fixture.super_block.next_free_nid = INVALID_NID;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    old_cur_size = fixture.node_cache.curSize;

    rtfsRuntimeInodeViewInit(&child_view, 3041, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(ENOSPC, rtfsDirInodeAddEntry(dir_inode, "dnif", &child_view));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "dnif", 4, &result));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)old_cur_size, (uint32_t)fixture.node_cache.curSize);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_inode_node->i.i_nid[4]);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)((uint64_t)DEF_ADDRS_PER_INODE +
                   2ULL * DEF_ADDRS_PER_BLOCK +
                   2ULL * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE,
        (uint32_t)cached_inode_node->i.i_size
    );
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeAddEntry_WhenGrowIntoDoubleIndirectChildCreationFails_ShouldNotLeaveHalfAttachedDoubleIndirectRoot)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle leaked_root_handle;
    struct RtfsNode *cached_inode_node;
    size_t old_cur_size;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size =
        ((uint64_t)DEF_ADDRS_PER_INODE +
         2ULL * DEF_ADDRS_PER_BLOCK +
         2ULL * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 0;
    fixture.inode_node.i.i_nid[4] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);

    fixture.super_block.next_free_nid = 6005;

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    old_cur_size = fixture.node_cache.curSize;

    rtfsRuntimeInodeViewInit(&child_view, 3042, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(ENOSPC, rtfsDirInodeAddEntry(dir_inode, "dnih", &child_view));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "dnih", 4, &result));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)old_cur_size, (uint32_t)fixture.node_cache.curSize);
    dirAssertCurrentSitBitInvalid(&fixture, 512);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_inode_node->i.i_nid[4]);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)((uint64_t)DEF_ADDRS_PER_INODE +
                   2ULL * DEF_ADDRS_PER_BLOCK +
                   2ULL * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK) * BLOCK_BUFFER_SIZE,
        (uint32_t)cached_inode_node->i.i_size
    );
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    leaked_root_handle = nodeBlockCacheGet(&fixture.node_cache, 6005);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&leaked_root_handle));
    nodeBlockCacheEntryHandleDestroy(&leaked_root_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeWritebackContentCow_WhenRegularBlockIsDirty_ShouldWriteNewVersionToNewLpa)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    RtfsDirLookupResult result;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3333, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeLookup(dir_inode, "cow", 3, &result));

    TEST_ASSERT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);
    TEST_ASSERT_EQUAL(3333u, g_dir_inode_written_block.dentry[1].ino);
    TEST_ASSERT_EQUAL(3u, g_dir_inode_written_block.dentry[1].name_len);
    TEST_ASSERT_EQUAL(RTFS_FT_REG_FILE, g_dir_inode_written_block.dentry[1].file_type);
    TEST_ASSERT_EQUAL_MEMORY("cow", g_dir_inode_written_block.filename[1], 3);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDirectBlockRelocated_ShouldSwitchMappedLpa)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3334, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow2", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(20u, cached_inode_node->i.i_addr[0]);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_inode_node->i.i_addr[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDirectNodeBlockRelocated_ShouldSwitchMappedLpa)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)DEF_ADDRS_PER_INODE + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[0] = 3000;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, DEF_ADDRS_PER_INODE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    rtfsRuntimeInodeViewInit(&child_view, 3335, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow3", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDirectNodePathIsMissing_ShouldReturnEnoentAndNotAdvanceMapping)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_inode_node;
    struct RtfsNode *cached_direct_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)DEF_ADDRS_PER_INODE + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[0] = 3000;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, DEF_ADDRS_PER_INODE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    rtfsRuntimeInodeViewInit(&child_view, 3338, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow6", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(3000u, cached_inode_node->i.i_nid[0]);
    cached_inode_node->i.i_nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_inode_node->i.i_nid[0]);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);
    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDirectNodePathIsRestored_ShouldResumeAndSucceed)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_inode_node;
    struct RtfsNode *cached_direct_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)DEF_ADDRS_PER_INODE + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[0] = 3000;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, DEF_ADDRS_PER_INODE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    rtfsRuntimeInodeViewInit(&child_view, 3339, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow7", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    cached_inode_node->i.i_nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    cached_inode_node->i.i_nid[0] = 3000;
    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_direct_node->dn.addr[0]);

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);
    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenSingleIndirectBlockRelocated_ShouldSwitchMappedLpa)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[2] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, target_block_index);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    rtfsRuntimeInodeViewInit(&child_view, 3336, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow4", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenSingleIndirectPathIsMissing_ShouldReturnEnoentAndNotAdvanceMapping)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle indirect_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_indirect_node;
    struct RtfsNode *cached_direct_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[2] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, target_block_index);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    rtfsRuntimeInodeViewInit(&child_view, 3342, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cw10", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    indirect_handle = nodeBlockCacheGet(&fixture.node_cache, 4000);
    TEST_ASSERT_NOT_NULL(indirect_handle.entry);
    cached_indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
    cached_indirect_node->in.nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_indirect_node->in.nid[0]);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&indirect_handle);
    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenSingleIndirectPathIsRestored_ShouldResumeAndSucceed)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle indirect_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_indirect_node;
    struct RtfsNode *cached_direct_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[2] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, target_block_index);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    rtfsRuntimeInodeViewInit(&child_view, 3340, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow8", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    indirect_handle = nodeBlockCacheGet(&fixture.node_cache, 4000);
    TEST_ASSERT_NOT_NULL(indirect_handle.entry);
    cached_indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
    cached_indirect_node->in.nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    cached_indirect_node->in.nid[0] = 3000;
    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_direct_node->dn.addr[0]);

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&indirect_handle);
    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDoubleIndirectBlockRelocated_ShouldSwitchMappedLpa)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[4] = 5000;
    fixture.double_indirect_root.in.nid[0] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 23;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 5000, &fixture.double_indirect_root);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, target_block_index);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    rtfsRuntimeInodeViewInit(&child_view, 3337, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow5", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(23u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDoubleIndirectPathIsMissing_ShouldReturnEnoentAndNotAdvanceMapping)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle level1_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_level1_node;
    struct RtfsNode *cached_direct_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[4] = 5000;
    fixture.double_indirect_root.in.nid[0] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 23;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 5000, &fixture.double_indirect_root);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, target_block_index);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    rtfsRuntimeInodeViewInit(&child_view, 3343, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cw11", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    level1_handle = nodeBlockCacheGet(&fixture.node_cache, 4000);
    TEST_ASSERT_NOT_NULL(level1_handle.entry);
    cached_level1_node = nodeBlockCacheEntryGetNodeBlockPtr(level1_handle.entry);
    cached_level1_node->in.nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(23u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_level1_node->in.nid[0]);
    TEST_ASSERT_EQUAL_UINT32(23u, cached_direct_node->dn.addr[0]);

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&level1_handle);
    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDoubleIndirectPathIsRestored_ShouldResumeAndSucceed)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle level1_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_level1_node;
    struct RtfsNode *cached_direct_node;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[4] = 5000;
    fixture.double_indirect_root.in.nid[0] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 23;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 5000, &fixture.double_indirect_root);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    rtfsDirInodeSetLoadedBlockCount(dir_inode, target_block_index);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolveNext(&fixture.fs_manager, 2000, dir_inode));

    rtfsRuntimeInodeViewInit(&child_view, 3341, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow9", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);

    level1_handle = nodeBlockCacheGet(&fixture.node_cache, 4000);
    TEST_ASSERT_NOT_NULL(level1_handle.entry);
    cached_level1_node = nodeBlockCacheEntryGetNodeBlockPtr(level1_handle.entry);
    cached_level1_node->in.nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(23u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(23u, cached_direct_node->dn.addr[0]);

    cached_level1_node->in.nid[0] = 3000;
    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_direct_node->dn.addr[0]);

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&level1_handle);
    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenMultiplePendingExistAndLaterPathIsMissing_ShouldNotPartiallyAdvanceAnyMapping)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_inode_node;
    struct RtfsNode *cached_direct_node;
    size_t i;
    char name[16];

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)DEF_ADDRS_PER_INODE + 2ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block1, 0, 20));
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block3, DEF_ADDRS_PER_INODE, 22)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 3201, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 3202, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "f%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "mblk1", &child_view2));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    cached_inode_node->i.i_nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));

    TEST_ASSERT_EQUAL_UINT32(20u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, cached_inode_node->i.i_addr[1]);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_inode_node->i.i_nid[0]);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenMultiplePendingLaterPathIsRestored_ShouldResumeAndApplyAllMappings)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_inode_node;
    struct RtfsNode *cached_direct_node;
    uint32_t direct_new_lpa;
    uint32_t direct_node_new_lpa;
    size_t i;
    char name[16];

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)DEF_ADDRS_PER_INODE + 2ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block1, 0, 20));
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block3, DEF_ADDRS_PER_INODE, 22)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 3211, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 3212, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "g%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "mblk2", &child_view2));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    cached_inode_node->i.i_nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);

    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, DEF_ADDRS_PER_INODE));
    direct_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0);
    direct_node_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, DEF_ADDRS_PER_INODE);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, direct_new_lpa);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, direct_node_new_lpa);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(20u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, DEF_ADDRS_PER_INODE));
    TEST_ASSERT_EQUAL_UINT32(20u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(22u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, DEF_ADDRS_PER_INODE));
    TEST_ASSERT_EQUAL_UINT32(direct_new_lpa, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(
        direct_node_new_lpa,
        rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, DEF_ADDRS_PER_INODE)
    );

    cached_inode_node->i.i_nid[0] = 3000;
    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(512u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(513u, cached_direct_node->dn.addr[0]);
    TEST_ASSERT_FALSE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_FALSE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, DEF_ADDRS_PER_INODE));
    TEST_ASSERT_EQUAL_UINT32(direct_new_lpa, rtfsDirInodeGetLoadedBlockLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(
        direct_node_new_lpa,
        rtfsDirInodeGetLoadedBlockLpa(dir_inode, DEF_ADDRS_PER_INODE)
    );
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(
        INVALID_LPA,
        rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, DEF_ADDRS_PER_INODE)
    );

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDirectAndSingleIndirectPendingExistAndLaterPathIsMissing_ShouldNotPartiallyAdvanceAnyMapping)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle indirect_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_inode_node;
    struct RtfsNode *cached_indirect_node;
    struct RtfsNode *cached_direct_node;
    size_t i;
    char name[16];

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 2ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[2] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block1, 0, 20));
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block3, target_block_index, 22)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 3221, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 3222, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "h%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "msi1", &child_view2));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);

    indirect_handle = nodeBlockCacheGet(&fixture.node_cache, 4000);
    TEST_ASSERT_NOT_NULL(indirect_handle.entry);
    cached_indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
    cached_indirect_node->in.nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(20u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, cached_inode_node->i.i_addr[1]);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_indirect_node->in.nid[0]);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&indirect_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDirectAndSingleIndirectLaterPathIsRestored_ShouldResumeAndApplyAllMappings)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle indirect_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_inode_node;
    struct RtfsNode *cached_indirect_node;
    struct RtfsNode *cached_direct_node;
    uint32_t direct_new_lpa;
    uint32_t single_new_lpa;
    size_t i;
    char name[16];

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 2ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[2] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block1, 0, 20));
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block3, target_block_index, 22)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 3231, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 3232, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "j%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "msi2", &child_view2));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);

    indirect_handle = nodeBlockCacheGet(&fixture.node_cache, 4000);
    TEST_ASSERT_NOT_NULL(indirect_handle.entry);
    cached_indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
    cached_indirect_node->in.nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);

    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, target_block_index));
    direct_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0);
    single_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, target_block_index);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, direct_new_lpa);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, single_new_lpa);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(20u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(20u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(22u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(direct_new_lpa, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(single_new_lpa, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, target_block_index));

    cached_indirect_node->in.nid[0] = 3000;
    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(512u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(513u, cached_direct_node->dn.addr[0]);
    TEST_ASSERT_FALSE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_FALSE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(direct_new_lpa, rtfsDirInodeGetLoadedBlockLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(single_new_lpa, rtfsDirInodeGetLoadedBlockLpa(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, target_block_index));

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&indirect_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDirectAndDoubleIndirectPendingExistAndLaterPathIsMissing_ShouldNotPartiallyAdvanceAnyMapping)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle level1_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_inode_node;
    struct RtfsNode *cached_level1_node;
    struct RtfsNode *cached_direct_node;
    size_t i;
    char name[16];

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 2ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[4] = 5000;
    fixture.double_indirect_root.in.nid[0] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 23;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 5000, &fixture.double_indirect_root);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block1, 0, 20));
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block4, target_block_index, 23)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 3251, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 3252, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "p%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "mdd1", &child_view2));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);

    level1_handle = nodeBlockCacheGet(&fixture.node_cache, 4000);
    TEST_ASSERT_NOT_NULL(level1_handle.entry);
    cached_level1_node = nodeBlockCacheEntryGetNodeBlockPtr(level1_handle.entry);
    cached_level1_node->in.nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(20u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, cached_inode_node->i.i_addr[1]);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, cached_level1_node->in.nid[0]);
    TEST_ASSERT_EQUAL_UINT32(23u, cached_direct_node->dn.addr[0]);

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&level1_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenDirectAndDoubleIndirectLaterPathIsRestored_ShouldResumeAndApplyAllMappings)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle level1_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_inode_node;
    struct RtfsNode *cached_level1_node;
    struct RtfsNode *cached_direct_node;
    uint32_t direct_new_lpa;
    uint32_t double_new_lpa;
    size_t i;
    char name[16];

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 2ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[4] = 5000;
    fixture.double_indirect_root.in.nid[0] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 23;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 5000, &fixture.double_indirect_root);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block1, 0, 20));
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block4, target_block_index, 23)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 3261, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 3262, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "q%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "mdd2", &child_view2));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);

    level1_handle = nodeBlockCacheGet(&fixture.node_cache, 4000);
    TEST_ASSERT_NOT_NULL(level1_handle.entry);
    cached_level1_node = nodeBlockCacheEntryGetNodeBlockPtr(level1_handle.entry);
    cached_level1_node->in.nid[0] = INVALID_NID;

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);

    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, target_block_index));
    direct_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0);
    double_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, target_block_index);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, direct_new_lpa);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, double_new_lpa);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(20u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(23u, cached_direct_node->dn.addr[0]);
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(20u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(23u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(direct_new_lpa, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(double_new_lpa, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, target_block_index));

    cached_level1_node->in.nid[0] = 3000;
    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_EQUAL_UINT32(512u, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(513u, cached_direct_node->dn.addr[0]);
    TEST_ASSERT_FALSE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_FALSE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(direct_new_lpa, rtfsDirInodeGetLoadedBlockLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(double_new_lpa, rtfsDirInodeGetLoadedBlockLpa(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, target_block_index));

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&level1_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenSingleIndirectAndDoubleIndirectPendingExistAndLaterPathIsMissing_ShouldNotPartiallyAdvanceAnyMapping)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t single_block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    size_t double_block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    uint32_t single_new_lpa;
    uint32_t double_new_lpa;
    size_t i;
    char name[16];

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)double_block_index + 2ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_nid[2] = 4001;
    fixture.inode_node.i.i_nid[4] = 5000;
    fixture.indirect_node2.in.nid[0] = 3001;
    fixture.double_indirect_root.in.nid[0] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node2.dn.addr[0] = 24;
    fixture.direct_node1.dn.addr[0] = 23;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 4001, &fixture.indirect_node2);
    dirResolverFixtureSyncCachedNode(&fixture, 3001, &fixture.direct_node2);
    dirResolverFixtureSyncCachedNode(&fixture, 5000, &fixture.double_indirect_root);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block3, single_block_index, 24)
    );
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block4, double_block_index, 23)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 3271, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 3272, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "r%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "msd1", &child_view2));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));

    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, single_block_index));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, double_block_index));
    TEST_ASSERT_EQUAL_UINT32(24u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, single_block_index));
    TEST_ASSERT_EQUAL_UINT32(23u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, double_block_index));
    single_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, single_block_index);
    double_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, double_block_index);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, single_new_lpa);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, double_new_lpa);

    fixture.indirect_node1.in.nid[0] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, single_block_index));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, double_block_index));
    TEST_ASSERT_EQUAL_UINT32(24u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, single_block_index));
    TEST_ASSERT_EQUAL_UINT32(23u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, double_block_index));
    TEST_ASSERT_EQUAL_UINT32(single_new_lpa, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, single_block_index));
    TEST_ASSERT_EQUAL_UINT32(double_new_lpa, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, double_block_index));

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenSingleIndirectAndDoubleIndirectLaterPathIsRestored_ShouldResumeAndApplyAllMappings)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t single_block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    size_t double_block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    uint32_t single_new_lpa;
    uint32_t double_new_lpa;
    size_t i;
    char name[16];

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)double_block_index + 2ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_nid[2] = 4001;
    fixture.inode_node.i.i_nid[4] = 5000;
    fixture.indirect_node2.in.nid[0] = 3001;
    fixture.double_indirect_root.in.nid[0] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node2.dn.addr[0] = 24;
    fixture.direct_node1.dn.addr[0] = 23;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 4001, &fixture.indirect_node2);
    dirResolverFixtureSyncCachedNode(&fixture, 3001, &fixture.direct_node2);
    dirResolverFixtureSyncCachedNode(&fixture, 5000, &fixture.double_indirect_root);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block3, single_block_index, 24)
    );
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block4, double_block_index, 23)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 3281, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 3282, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "s%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "msd2", &child_view2));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));

    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, single_block_index));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, double_block_index));
    single_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, single_block_index);
    double_new_lpa = rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, double_block_index);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, single_new_lpa);
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, double_new_lpa);

    fixture.indirect_node1.in.nid[0] = INVALID_NID;
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, single_block_index));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, double_block_index));
    TEST_ASSERT_EQUAL_UINT32(24u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, single_block_index));
    TEST_ASSERT_EQUAL_UINT32(23u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, double_block_index));

    fixture.indirect_node1.in.nid[0] = 3000;
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_FALSE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, single_block_index));
    TEST_ASSERT_FALSE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, double_block_index));
    TEST_ASSERT_EQUAL_UINT32(single_new_lpa, rtfsDirInodeGetLoadedBlockLpa(dir_inode, single_block_index));
    TEST_ASSERT_EQUAL_UINT32(double_new_lpa, rtfsDirInodeGetLoadedBlockLpa(dir_inode, double_block_index));
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, single_block_index));
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, double_block_index));

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeApplyPendingCowRelocations_WhenBatchApplyFails_ShouldKeepAllPendingStatesIntact)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    NodeBlockCacheEntryHandle indirect_handle;
    struct RtfsNode *cached_indirect_node;
    size_t i;
    char name[16];

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 2ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[2] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 4000, &fixture.indirect_node1);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block1, 0, 20));
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block3, target_block_index, 22)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 3241, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 3242, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "k%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "msi3", &child_view2));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeWritebackContentCow(&fixture.fs_manager, dir_inode));

    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(20u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(22u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(512u, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(513u, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, target_block_index));

    indirect_handle = nodeBlockCacheGet(&fixture.node_cache, 4000);
    TEST_ASSERT_NOT_NULL(indirect_handle.entry);
    cached_indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
    cached_indirect_node->in.nid[0] = INVALID_NID;

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeApplyPendingCowRelocations(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, 0));
    TEST_ASSERT_TRUE(rtfsDirInodeLoadedBlockHasPendingCowRelocation(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(20u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(22u, rtfsDirInodeGetLoadedBlockLpa(dir_inode, target_block_index));
    TEST_ASSERT_EQUAL_UINT32(512u, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, 0));
    TEST_ASSERT_EQUAL_UINT32(513u, rtfsDirInodeGetLoadedBlockCowNewLpa(dir_inode, target_block_index));

    nodeBlockCacheEntryHandleDestroy(&indirect_handle);
    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenDirtyDirectoryExists_ShouldSubmitJournalAndClearCurrentJournal)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4444, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cmt", &child_view));

    TEST_ASSERT_NULL(g_dir_inode_committed_journal);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)kv_size(g_dir_inode_committed_journal->natJournal));
    TEST_ASSERT_TRUE(journalContainerIsEmpty(&fixture.journal));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_inode_node->i.i_addr[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenCommitSucceeds_ShouldReachCommittedButNotReclaimedState)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4447, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cmt2", &child_view));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);

    TEST_ASSERT_TRUE(journalContainerIsEmpty(&fixture.journal));
    TEST_ASSERT_FALSE(rtfsDirInodeIsFullyLoaded(dir_inode));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)rtfsDirInodeGetLoadedBlockCount(dir_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, inode_handle.entry->state);
    TEST_ASSERT_FALSE(inode_handle.entry->hasPendingCowRelocation);
    TEST_ASSERT_EQUAL_UINT32(1024u, inode_handle.entry->lpa);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, inode_handle.entry->cowNewLpa);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenDirectoryIsAlreadyCommitted_ShouldBeStableNoOp)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;
    uint32_t first_data_lpa;
    uint32_t first_node_lpa;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4448, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "noop", &child_view));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);

    first_data_lpa = g_dir_inode_write_lpa;
    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    first_node_lpa = inode_handle.entry->lpa;
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(first_data_lpa, cached_inode_node->i.i_addr[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    journalContainerDestroy(g_dir_inode_committed_journal);
    free(g_dir_inode_committed_journal);
    g_dir_inode_committed_journal = NULL;

    TEST_ASSERT_TRUE(journalContainerIsEmpty(&fixture.journal));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NULL(g_dir_inode_committed_journal);
    TEST_ASSERT_TRUE(journalContainerIsEmpty(&fixture.journal));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL_UINT32(first_data_lpa, g_dir_inode_write_lpa);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(first_node_lpa, inode_handle.entry->lpa);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, inode_handle.entry->state);
    TEST_ASSERT_FALSE(inode_handle.entry->hasPendingCowRelocation);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, inode_handle.entry->cowNewLpa);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(first_data_lpa, cached_inode_node->i.i_addr[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenNodeCowWriteFails_ShouldReturnEioAndKeepJournal)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4445, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cf1", &child_view));

    g_node_cow_fail_write_lpa = 1024;
    TEST_ASSERT_EQUAL(EIO, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NULL(g_dir_inode_committed_journal);
    TEST_ASSERT_FALSE(journalContainerIsEmpty(&fixture.journal));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_inode_node->i.i_addr[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenJournalSubmitFails_ShouldReturnErrorAndKeepCurrentJournal)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4446, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cf2", &child_view));

    g_dir_inode_journal_commit_rc = EBUSY;
    TEST_ASSERT_EQUAL(EBUSY, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NULL(g_dir_inode_committed_journal);
    TEST_ASSERT_FALSE(journalContainerIsEmpty(&fixture.journal));

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenJournalSubmitFails_ShouldPreservePreparedStateAndNotTriggerReclaim)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4452, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "jr1", &child_view));

    g_dir_inode_journal_commit_rc = EBUSY;
    TEST_ASSERT_EQUAL(EBUSY, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NULL(g_dir_inode_committed_journal);
    TEST_ASSERT_FALSE(journalContainerIsEmpty(&fixture.journal));
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)kv_size(fixture.journal.sitJournal));
    dirAssertSitJournalDoesNotInvalidateLpa(&fixture.journal, 20);
    dirAssertSitJournalDoesNotInvalidateLpa(&fixture.journal, 10);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(g_dir_inode_write_lpa, cached_inode_node->i.i_addr[0]);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, inode_handle.entry->state);
    TEST_ASSERT_FALSE(inode_handle.entry->hasPendingCowRelocation);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)kv_size(fixture.journal.sitJournal));
    dirAssertSitJournalDoesNotInvalidateLpa(&fixture.journal, 20);
    dirAssertSitJournalDoesNotInvalidateLpa(&fixture.journal, 10);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenJournalSubmitFails_ShouldKeepBothOldAndNewLpasValid)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    uint32_t new_data_lpa;
    uint32_t new_node_lpa;
    struct RtfsNode *cached_inode_node;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4453, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "jr2", &child_view));

    g_dir_inode_journal_commit_rc = EBUSY;
    TEST_ASSERT_EQUAL(EBUSY, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));

    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);
    new_data_lpa = g_dir_inode_write_lpa;

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    new_node_lpa = inode_handle.entry->lpa;
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, new_node_lpa);
    TEST_ASSERT_EQUAL_UINT32(new_data_lpa, cached_inode_node->i.i_addr[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    dirAssertCurrentSitBitValid(&fixture, 20);
    dirAssertCurrentSitBitValid(&fixture, 10);
    dirAssertCurrentSitBitValid(&fixture, new_data_lpa);
    dirAssertCurrentSitBitValid(&fixture, new_node_lpa);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenTxCompletes_ShouldQueueOldLpaReclaimIntoSitJournal)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    uint64_t tx_id;
    uint32_t expected_seg_ids[] = {0u, 0u};
    uint32_t seg0_old_vblocks;

    dirResolverFixtureInit(&fixture);
    seg0_old_vblocks = dirFixtureGetCurrentSitVblocks(&fixture, 0);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4450, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "rcm", &child_view));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);

    tx_id = journalContainerGetTxId(g_dir_inode_committed_journal);

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    dirAssertSitJournalSegIds(
        &fixture.journal,
        expected_seg_ids,
        sizeof(expected_seg_ids) / sizeof(expected_seg_ids[0])
    );
    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 0),
        20
    );
    dirAssertSitJournalEntryVblocksDelta(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 0),
        seg0_old_vblocks,
        seg0_old_vblocks - 1u
    );
    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 1),
        10
    );
    dirAssertSitJournalEntryVblocksDelta(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 1),
        seg0_old_vblocks - 1u,
        seg0_old_vblocks - 2u
    );

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenTxCompletes_ShouldNotInvalidateNeighborLpasInSameSegment)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    uint64_t tx_id;

    dirResolverFixtureInit(&fixture);
    dirResolverFixtureMarkSitValid(&fixture, 11);
    dirResolverFixtureMarkSitValid(&fixture, 21);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3333, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));

    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);
    tx_id = journalContainerGetTxId(g_dir_inode_committed_journal);
    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 0),
        20
    );
    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 1),
        10
    );
    dirAssertCurrentSitBitValid(&fixture, 11);
    dirAssertCurrentSitBitValid(&fixture, 21);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenTxCompletes_ShouldKeepNewDataAndNodeLpasValid)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    NodeBlockCacheEntryHandle inode_handle;
    uint64_t tx_id;
    uint32_t new_data_lpa;
    uint32_t new_node_lpa;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 3334, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "cow2", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));

    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_dir_inode_write_lpa);
    new_data_lpa = g_dir_inode_write_lpa;

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    new_node_lpa = inode_handle.entry->lpa;
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, new_node_lpa);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);
    tx_id = journalContainerGetTxId(g_dir_inode_committed_journal);
    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    dirAssertCurrentSitBitValid(&fixture, new_data_lpa);
    dirAssertCurrentSitBitValid(&fixture, new_node_lpa);

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenTxNotCompleted_ShouldNotReclaimOldLpasEarly)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4451, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "nr1", &child_view));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(CowReclaimRegistry_WhenOnTxCompleteUsesUnknownTxId_ShouldBeStableNoOp)
{
    DirResolverFixture fixture;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    cowReclaimRegistryOnTxComplete(999999u);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));

    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(CowReclaimRegistry_WhenCompletedListIsAlreadyEmpty_ShouldDrainAsStableNoOp)
{
    DirResolverFixture fixture;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));

    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenTxCompleteAndDrainAreRepeated_ShouldNotDoubleReclaimOldLpas)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    uint64_t tx_id;
    size_t sit_after_first_reclaim;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);

    rtfsRuntimeInodeViewInit(&child_view, 4453, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "idr", &child_view));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);
    tx_id = journalContainerGetTxId(g_dir_inode_committed_journal);

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));

    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    sit_after_first_reclaim = kv_size(fixture.journal.sitJournal);
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)sit_after_first_reclaim);

    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)sit_after_first_reclaim,
        (uint32_t)kv_size(fixture.journal.sitJournal)
    );

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenMultipleTxExist_ShouldOnlyReclaimCompletedTx)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode1 = NULL;
    RtfsDirInode *dir_inode2 = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    uint64_t tx1;
    uint64_t tx2;
    size_t sit_before;
    size_t sit_after_one;
    uint32_t expected_tx1_seg_ids[] = {0u, 0u};
    uint32_t expected_tx2_seg_ids[] = {1u, 2u};
    uint32_t seg0_old_vblocks;
    uint32_t seg1_old_vblocks;
    uint32_t seg2_old_vblocks;

    dirResolverFixtureInit(&fixture);
    seg0_old_vblocks = dirFixtureGetCurrentSitVblocks(&fixture, 0);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode1));
    TEST_ASSERT_NOT_NULL(dir_inode1);
    rtfsRuntimeInodeViewInit(&child_view, 4460, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode1, "m1", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode1));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);
    tx1 = journalContainerGetTxId(g_dir_inode_committed_journal);

    journalContainerDestroy(g_dir_inode_committed_journal);
    free(g_dir_inode_committed_journal);
    g_dir_inode_committed_journal = NULL;

    dir_inode2 = dir_inode1;
    dir_inode1 = NULL;
    rtfsRuntimeInodeViewInit(&child_view, 4461, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode2, "m2", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode2));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);
    tx2 = journalContainerGetTxId(g_dir_inode_committed_journal);
    TEST_ASSERT_TRUE(tx2 > tx1);

    sit_before = kv_size(fixture.journal.sitJournal);
    cowReclaimRegistryOnTxComplete(tx1);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    sit_after_one = kv_size(fixture.journal.sitJournal);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)sit_before);
    dirAssertSitJournalSegIds(
        &fixture.journal,
        expected_tx1_seg_ids,
        sizeof(expected_tx1_seg_ids) / sizeof(expected_tx1_seg_ids[0])
    );
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)sit_after_one);
    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 0),
        20
    );
    dirAssertSitJournalEntryVblocksDelta(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 0),
        seg0_old_vblocks,
        seg0_old_vblocks - 1u
    );
    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 1),
        10
    );
    dirAssertSitJournalEntryVblocksDelta(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 1),
        seg0_old_vblocks - 1u,
        seg0_old_vblocks - 2u
    );

    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sit_after_one, (uint32_t)kv_size(fixture.journal.sitJournal));

    seg1_old_vblocks = dirFixtureGetCurrentSitVblocks(&fixture, 1);
    seg2_old_vblocks = dirFixtureGetCurrentSitVblocks(&fixture, 2);
    cowReclaimRegistryOnTxComplete(tx2);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL_UINT32(
        expected_tx2_seg_ids[0],
        kv_a(SitJournalEntry, fixture.journal.sitJournal, 2).segID
    );
    TEST_ASSERT_EQUAL_UINT32(
        expected_tx2_seg_ids[1],
        kv_a(SitJournalEntry, fixture.journal.sitJournal, 3).segID
    );
    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 2),
        512
    );
    dirAssertSitJournalEntryVblocksDelta(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 2),
        seg1_old_vblocks,
        seg1_old_vblocks - 1u
    );
    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 3),
        1024
    );
    dirAssertSitJournalEntryVblocksDelta(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 3),
        seg2_old_vblocks,
        seg2_old_vblocks - 1u
    );

    rtfsDirInodePut(dir_inode1);
    rtfsDirInodePut(dir_inode2);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenSingleTxHasMultipleOldObjects_ShouldReclaimExpectedSetWithoutNeighborDamage)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode = NULL;
    size_t target_block_index = DEF_ADDRS_PER_INODE;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_METADATA_ONLY
    };
    RtfsRuntimeInodeView child_view1;
    RtfsRuntimeInodeView child_view2;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;
    uint64_t tx_id;
    uint32_t seg0_old_vblocks;
    uint32_t expected_old_lpas[] = {20u, 22u, 10u, 30u};
    char name[16];
    size_t i;

    dirResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)target_block_index + 1ULL) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_dentry_num = 2;
    fixture.inode_node.i.i_addr[0] = 20;
    fixture.inode_node.i.i_nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    dirResolverFixtureSyncCachedNode(&fixture, 2000, &fixture.inode_node);
    dirResolverFixtureSyncCachedNode(&fixture, 3000, &fixture.direct_node1);
    dirResolverFixtureMarkSitValid(&fixture, 22u);
    seg0_old_vblocks = dirFixtureGetCurrentSitVblocks(&fixture, 0);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block1, 0, 20));
    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeAppendDentryBlockAt(dir_inode, &fixture.dentry_block3, target_block_index, 22)
    );
    rtfsDirInodeSetLoadedBlockCount(dir_inode, 2);

    rtfsRuntimeInodeViewInit(&child_view1, 4501, 2000, RTFS_FT_REG_FILE);
    rtfsRuntimeInodeViewInit(&child_view2, 4502, 2000, RTFS_FT_DIR);

    for (i = 0; i < (size_t)(NR_DENTRY_IN_BLOCK - 1U); ++i) {
        snprintf(name, sizeof(name), "u%u", (unsigned)i);
        TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, name, &child_view1));
    }
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "mdn1", &child_view2));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);
    tx_id = journalContainerGetTxId(g_dir_inode_committed_journal);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    TEST_ASSERT_NOT_EQUAL_UINT32(10u, inode_handle.entry->lpa);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_NOT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)kv_size(fixture.journal.sitJournal));
    dirAssertCurrentSitBitInvalid(&fixture, 20);
    dirAssertCurrentSitBitInvalid(&fixture, 22);
    dirAssertCurrentSitBitInvalid(&fixture, 10);
    dirAssertCurrentSitBitInvalid(&fixture, 30);
    dirAssertSitJournalEntryVblocksDelta(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 0),
        seg0_old_vblocks,
        seg0_old_vblocks - 1u
    );

    rtfsDirInodePut(dir_inode);
    dirResolverFixtureFini(&fixture);
}

RTFS_TEST(DirInodeCommitCowWriteback_WhenFirstOfTwoTxCompletes_ShouldNotTouchSecondTxOldLpas)
{
    DirResolverFixture fixture;
    RtfsDirInode *dir_inode1 = NULL;
    RtfsDirInode *dir_inode2 = NULL;
    RtfsDirInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsRuntimeInodeView child_view;
    uint64_t tx1;
    uint64_t tx2;

    dirResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode1));
    TEST_ASSERT_NOT_NULL(dir_inode1);
    rtfsRuntimeInodeViewInit(&child_view, 4601, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode1, "t1", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode1));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);
    tx1 = journalContainerGetTxId(g_dir_inode_committed_journal);

    journalContainerDestroy(g_dir_inode_committed_journal);
    free(g_dir_inode_committed_journal);
    g_dir_inode_committed_journal = NULL;

    dir_inode2 = dir_inode1;
    dir_inode1 = NULL;
    rtfsRuntimeInodeViewInit(&child_view, 4602, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode2, "t2", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeCommitCowWriteback(&fixture.fs_manager, dir_inode2));
    TEST_ASSERT_NOT_NULL(g_dir_inode_committed_journal);
    tx2 = journalContainerGetTxId(g_dir_inode_committed_journal);
    TEST_ASSERT_TRUE(tx2 > tx1);

    cowReclaimRegistryOnTxComplete(tx1);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)kv_size(fixture.journal.sitJournal));
    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 0),
        20u
    );
    dirAssertSitJournalEntryInvalidatesLpa(
        &kv_a(SitJournalEntry, fixture.journal.sitJournal, 1),
        10u
    );

    dirAssertSitJournalDoesNotInvalidateLpa(&fixture.journal, 512u);
    dirAssertSitJournalDoesNotInvalidateLpa(&fixture.journal, 1024u);
    dirAssertCurrentSitBitValid(&fixture, 512u);
    dirAssertCurrentSitBitValid(&fixture, 1024u);
    dirAssertCurrentSitBitInvalid(&fixture, 20u);
    dirAssertCurrentSitBitInvalid(&fixture, 10u);

    rtfsDirInodePut(dir_inode1);
    rtfsDirInodePut(dir_inode2);
    dirResolverFixtureFini(&fixture);
}
