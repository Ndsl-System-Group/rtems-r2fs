#include "rtfs_test.h"

#include "cache/generic_cache_manager.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/dev.h"
#include "file_inode/file_inode.h"
#include "file_inode/file_inode_resolver.h"
#include "fs/cow_reclaim_registry.h"
#include "fs/super_manager.h"
#include "journal/journal_container.h"

#include <errno.h>
#include <memory.h>
#include <pthread.h>
#include <rtems/thread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
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

typedef struct FileResolverFixture
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
    struct RtfsNode inode_node;
    struct RtfsNode direct_node1;
    struct RtfsNode indirect_node1;
    struct RtfsNode double_indirect_root;
    struct RtfsNode double_indirect_level1;
    char data_block20[BLOCK_BUFFER_SIZE];
    char data_block21[BLOCK_BUFFER_SIZE];
    char data_block22[BLOCK_BUFFER_SIZE];
    char data_block23[BLOCK_BUFFER_SIZE];
    uint32_t fail_node_lpa;
    uint32_t fail_data_lpa;
    uint32_t node_read_count;
    uint32_t data_read_count;
    bool hook_enabled;
} FileResolverFixture;

static FileResolverFixture *g_file_resolver_fixture = NULL;
static uint32_t g_file_inode_write_lpa = INVALID_LPA;
static uint32_t g_file_inode_write_lpas[8];
static size_t g_file_inode_write_lpa_count = 0;
static char g_file_inode_written_block[BLOCK_BUFFER_SIZE];
static uint32_t g_file_inode_fail_write_lpa = INVALID_LPA;
static JournalContainer *g_file_inode_committed_journal = NULL;
static int g_file_inode_journal_commit_rc = 0;
static uint32_t g_node_cow_fail_write_lpa = INVALID_LPA;

static void fillDataBlock(char *buffer, char base)
{
    size_t i;

    for (i = 0; i < BLOCK_BUFFER_SIZE; ++i) {
        buffer[i] = (char)(base + (char)(i % 26));
    }
}

static void fileResolverFixtureSetNatEntry(
    FileResolverFixture *fixture,
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

static void fileResolverFixtureMarkSitValid(
    FileResolverFixture *fixture,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    uint32_t bitmap_idx = seg_off / 8;
    uint32_t bitmap_off = seg_off % 8;
    struct RtfsSitEntry *entry = &fixture->sit_block.entries[sit_idx];
    uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);
    SitNatCacheEntryHandle handle;
    struct RtfsSitBlock *sit_block;
    struct RtfsSitEntry *cached_entry;

    if ((entry->valid_map[bitmap_idx] & (1u << bitmap_off)) == 0) {
        entry->valid_map[bitmap_idx] |= (uint8_t)(1u << bitmap_off);
        if (GET_SIT_VBLOCKS(entry) < 511) {
            entry->vblocks += 1;
        }
    }

    handle = sitNatCacheGet(&fixture->sit_cache, sit_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    sit_block = sitNatCacheEntryHandleGetSitBlockPtr(&handle);
    cached_entry = &sit_block->entries[sit_idx];
    if ((cached_entry->valid_map[bitmap_idx] & (1u << bitmap_off)) == 0) {
        cached_entry->valid_map[bitmap_idx] |= (uint8_t)(1u << bitmap_off);
        if (GET_SIT_VBLOCKS(cached_entry) < 511) {
            cached_entry->vblocks += 1;
        }
    }
    sitNatCacheEntryHandleDestroy(&handle);
}

static void fileResolverFixtureAddCachedNode(
    FileResolverFixture *fixture,
    uint32_t nid,
    uint32_t parent_nid,
    uint32_t lpa,
    const struct RtfsNode *node
)
{
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;

    blockBufferInit(&buffer);
    blockBufferCopyContentFromBuf(&buffer, (const char *)node);
    handle = nodeBlockCacheAdd(&fixture->node_cache, &buffer, nid, parent_nid, lpa);
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
}

static void fileResolverFixtureCacheDefaultNodes(FileResolverFixture *fixture)
{
    fileResolverFixtureAddCachedNode(
        fixture,
        2000,
        INVALID_NID,
        10,
        &fixture->inode_node
    );
    fileResolverFixtureAddCachedNode(
        fixture,
        3000,
        2000,
        30,
        &fixture->direct_node1
    );
    fileResolverFixtureAddCachedNode(
        fixture,
        4000,
        2000,
        40,
        &fixture->indirect_node1
    );
    fileResolverFixtureAddCachedNode(
        fixture,
        5000,
        2000,
        50,
        &fixture->double_indirect_root
    );
    fileResolverFixtureAddCachedNode(
        fixture,
        5001,
        5000,
        51,
        &fixture->double_indirect_level1
    );
}

static int fileResolverNodeReadBlockHook(
    comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    (void)dev;

    if (g_file_resolver_fixture == NULL) {
        return EIO;
    }

    g_file_resolver_fixture->node_read_count++;

    if (lpa == g_file_resolver_fixture->fail_node_lpa) {
        return EIO;
    }

    if (lpa == 10) {
        memcpy(buffer, &g_file_resolver_fixture->inode_node, sizeof(g_file_resolver_fixture->inode_node));
        return 0;
    }

    if (lpa == 30) {
        memcpy(buffer, &g_file_resolver_fixture->direct_node1, sizeof(g_file_resolver_fixture->direct_node1));
        return 0;
    }

    if (lpa == 40) {
        memcpy(buffer, &g_file_resolver_fixture->indirect_node1, sizeof(g_file_resolver_fixture->indirect_node1));
        return 0;
    }

    if (lpa == 50) {
        memcpy(buffer, &g_file_resolver_fixture->double_indirect_root, sizeof(g_file_resolver_fixture->double_indirect_root));
        return 0;
    }

    if (lpa == 51) {
        memcpy(buffer, &g_file_resolver_fixture->double_indirect_level1, sizeof(g_file_resolver_fixture->double_indirect_level1));
        return 0;
    }

    return ENOENT;
}

static int fileResolverDataReadBlockHook(
    comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    (void)dev;

    if (g_file_resolver_fixture == NULL) {
        return EIO;
    }

    g_file_resolver_fixture->data_read_count++;

    if (lpa == g_file_resolver_fixture->fail_data_lpa) {
        return EIO;
    }

    if (lpa == 20) {
        memcpy(buffer, g_file_resolver_fixture->data_block20, BLOCK_BUFFER_SIZE);
        return 0;
    }

    if (lpa == 21) {
        memcpy(buffer, g_file_resolver_fixture->data_block21, BLOCK_BUFFER_SIZE);
        return 0;
    }

    if (lpa == 22) {
        memcpy(buffer, g_file_resolver_fixture->data_block22, BLOCK_BUFFER_SIZE);
        return 0;
    }

    if (lpa == 23) {
        memcpy(buffer, g_file_resolver_fixture->data_block23, BLOCK_BUFFER_SIZE);
        return 0;
    }

    return ENOENT;
}

static int fileResolverWriteBlockHook(
    comm_dev *dev,
    uint32_t lpa,
    const void *buffer
)
{
    (void)dev;

    if (g_file_inode_fail_write_lpa != INVALID_LPA &&
        lpa == g_file_inode_fail_write_lpa) {
        return EIO;
    }

    g_file_inode_write_lpa = lpa;
    if (g_file_inode_write_lpa_count <
        sizeof(g_file_inode_write_lpas) / sizeof(g_file_inode_write_lpas[0])) {
        g_file_inode_write_lpas[g_file_inode_write_lpa_count++] = lpa;
    }
    memcpy(g_file_inode_written_block, buffer, BLOCK_BUFFER_SIZE);
    return 0;
}

static int fileResolverNodeCowWriteBlockHook(
    comm_dev *dev,
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

static int fileResolverJournalCommitHook(JournalContainer *journal)
{
    if (g_file_inode_journal_commit_rc != 0) {
        return g_file_inode_journal_commit_rc;
    }

    g_file_inode_committed_journal = journal;
    return 0;
}

static void fileResolverReleaseCommittedJournal(void)
{
    if (g_file_inode_committed_journal != NULL) {
        journalContainerDestroy(g_file_inode_committed_journal);
        free(g_file_inode_committed_journal);
        g_file_inode_committed_journal = NULL;
    }
}

static void fileResolverAssertCurrentSitBitValid(
    FileResolverFixture *fixture,
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
    TEST_ASSERT_NOT_EQUAL_UINT32(
        0u,
        (uint32_t)(sit_block->entries[sit_idx].valid_map[bitmap_idx] & (1u << bitmap_off))
    );
    sitNatCacheEntryHandleDestroy(&handle);
}

static void fileResolverAssertCurrentSitBitInvalid(
    FileResolverFixture *fixture,
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

static void fileResolverFixtureInit(FileResolverFixture *fixture)
{
    SitNatCacheEntry *entry;

    memset(fixture, 0, sizeof(*fixture));
    fixture->fail_node_lpa = INVALID_LPA;
    fixture->fail_data_lpa = INVALID_LPA;

    fixture->super_block.nat_blkaddr = 100;
    fixture->super_block.sit_blkaddr = 200;
    fixture->super_block.segment_count = 16;
    fixture->super_block.segment_count_nat = 1;
    fixture->super_block.segment_count_sit = 1;
    fixture->super_block.segment0_blkaddr = 0;
    fixture->super_block.current_data_segment_id = 1;
    fixture->super_block.current_data_segment_blkoff = 0;
    fixture->super_block.current_node_segment_id = 2;
    fixture->super_block.current_node_segment_blkoff = 0;
    fixture->super_block.first_free_segment_id = 3;
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

    entry = (SitNatCacheEntry *)malloc(sizeof(*entry));
    TEST_ASSERT_NOT_NULL(entry);
    sitNatCacheEntryInit(entry, fixture->super_block.sit_blkaddr);
    memset(blockBufferGetPtr(&entry->cache), 0, BLOCK_BUFFER_SIZE);
    genericCacheManagerAdd(&fixture->sit_cache.cacheManager, fixture->super_block.sit_blkaddr, entry);
    fixture->sit_cache.curSize++;

    memset(&fixture->sit_block, 0, sizeof(fixture->sit_block));
    fileResolverFixtureMarkSitValid(fixture, 10);
    fileResolverFixtureMarkSitValid(fixture, 20);
    fileResolverFixtureMarkSitValid(fixture, 21);
    fileResolverFixtureMarkSitValid(fixture, 22);
    fileResolverFixtureMarkSitValid(fixture, 23);
    fileResolverFixtureMarkSitValid(fixture, 30);
    fileResolverFixtureMarkSitValid(fixture, 40);
    fileResolverFixtureMarkSitValid(fixture, 50);
    fileResolverFixtureMarkSitValid(fixture, 51);

    memset(&fixture->inode_node, 0, sizeof(fixture->inode_node));
    fixture->inode_node.i.i_type = RTFS_FT_REG_FILE;
    fixture->inode_node.i.i_mode = 0644;
    fixture->inode_node.i.i_nlink = 1;
    fixture->inode_node.i.i_size = 2 * BLOCK_BUFFER_SIZE;
    fixture->inode_node.i.i_atime = 111;
    fixture->inode_node.i.i_mtime = 222;
    fixture->inode_node.i.i_addr[0] = 20;
    fixture->inode_node.i.i_addr[1] = 21;
    fixture->inode_node.footer.nid = 2000;
    fixture->inode_node.footer.ino = 2000;

    memset(&fixture->direct_node1, 0, sizeof(fixture->direct_node1));
    fixture->direct_node1.dn.addr[0] = 22;
    fixture->direct_node1.footer.nid = 3000;
    fixture->direct_node1.footer.ino = 2000;

    memset(&fixture->indirect_node1, 0, sizeof(fixture->indirect_node1));
    fixture->indirect_node1.in.nid[0] = 3000;
    fixture->indirect_node1.footer.nid = 4000;
    fixture->indirect_node1.footer.ino = 2000;

    memset(&fixture->double_indirect_root, 0, sizeof(fixture->double_indirect_root));
    fixture->double_indirect_root.in.nid[0] = 5001;
    fixture->double_indirect_root.footer.nid = 5000;
    fixture->double_indirect_root.footer.ino = 2000;

    memset(&fixture->double_indirect_level1, 0, sizeof(fixture->double_indirect_level1));
    fixture->double_indirect_level1.in.nid[0] = 3000;
    fixture->double_indirect_level1.footer.nid = 5001;
    fixture->double_indirect_level1.footer.ino = 2000;

    fillDataBlock(fixture->data_block20, 'A');
    fillDataBlock(fixture->data_block21, 'a');
    fillDataBlock(fixture->data_block22, '0');
    fillDataBlock(fixture->data_block23, 'm');

    fileResolverFixtureSetNatEntry(fixture, 2000, 2000, 10);
    fileResolverFixtureSetNatEntry(fixture, 3000, 2000, 30);
    fileResolverFixtureSetNatEntry(fixture, 4000, 2000, 40);
    fileResolverFixtureSetNatEntry(fixture, 5000, 2000, 50);
    fileResolverFixtureSetNatEntry(fixture, 5001, 2000, 51);
    fileResolverFixtureSetNatEntry(fixture, 6000, INVALID_NID, 6001);
    fileResolverFixtureSetNatEntry(fixture, 6001, INVALID_NID, 6002);
    fileResolverFixtureSetNatEntry(fixture, 6002, INVALID_NID, INVALID_NID);

    g_file_resolver_fixture = fixture;
    fixture->hook_enabled = true;
    nodeBlockCacheSetReadBlockHook(fileResolverNodeReadBlockHook);
    nodeBlockCacheSetWriteBlockHook(fileResolverNodeCowWriteBlockHook);
    rtfsFileInodeSetReadBlockHook(fileResolverDataReadBlockHook);
    rtfsFileInodeSetWriteBlockHook(fileResolverWriteBlockHook);
    rtfsFileInodeSetJournalCommitHook(fileResolverJournalCommitHook);
    g_file_inode_write_lpa = INVALID_LPA;
    g_file_inode_write_lpa_count = 0;
    g_file_inode_fail_write_lpa = INVALID_LPA;
    g_file_inode_journal_commit_rc = 0;
    g_node_cow_fail_write_lpa = INVALID_LPA;
    memset(g_file_inode_written_block, 0, sizeof(g_file_inode_written_block));
    memset(g_file_inode_write_lpas, 0, sizeof(g_file_inode_write_lpas));
    g_file_inode_committed_journal = NULL;
}

static void fileResolverFixtureFini(FileResolverFixture *fixture)
{
    if (fixture->hook_enabled) {
        nodeBlockCacheSetReadBlockHook(NULL);
        nodeBlockCacheSetWriteBlockHook(NULL);
        rtfsFileInodeSetReadBlockHook(NULL);
        rtfsFileInodeSetWriteBlockHook(NULL);
        rtfsFileInodeSetJournalCommitHook(NULL);
        fixture->hook_enabled = false;
        g_file_resolver_fixture = NULL;
    }

    if (g_file_inode_committed_journal != NULL) {
        journalContainerDestroy(g_file_inode_committed_journal);
        free(g_file_inode_committed_journal);
        g_file_inode_committed_journal = NULL;
    }

    cowReclaimRegistryDestroy();
    superManagerDestroy(fixture->sp_manager);
    fixture->sp_manager = NULL;
    journalContainerDestroy(&fixture->journal);
    sitNatCacheDestroy(&fixture->sit_cache);
    sitNatCacheDestroy(&fixture->nat_cache);
    nodeBlockCacheDestroy(&fixture->node_cache);
}

RTFS_TEST(FileInodeResolve_WhenRequestIsNull_ShouldReturnEINVAL)
{
    RtfsFileInode *file_inode = NULL;

    TEST_ASSERT_EQUAL(EINVAL, rtfsFileInodeResolve(NULL, NULL, NULL, &file_inode));
    TEST_ASSERT_NULL(file_inode);
}

RTFS_TEST(FileInodeResolve_WhenOutParamIsNull_ShouldReturnEINVAL)
{
    RtfsFileInodeBuildRequest request = {
        .ino = 1,
        .mode = RTFS_FILE_BUILD_METADATA_ONLY
    };

    TEST_ASSERT_EQUAL(EINVAL, rtfsFileInodeResolve(NULL, NULL, &request, NULL));
}

RTFS_TEST(FileInodeResolve_WhenModeUnsupported_ShouldReturnENOSYS)
{
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 1,
        .mode = (RtfsFileInodeBuildMode)99
    };

    TEST_ASSERT_EQUAL(ENOSYS, rtfsFileInodeResolve(NULL, NULL, &request, &file_inode));
    TEST_ASSERT_NULL(file_inode);
}

RTFS_TEST(FileInodeResolve_WhenLoaderFails_ShouldPropagateError)
{
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 1,
        .mode = RTFS_FILE_BUILD_METADATA_ONLY
    };

    TEST_ASSERT_EQUAL(EINVAL, rtfsFileInodeResolve(NULL, NULL, &request, &file_inode));
    TEST_ASSERT_NULL(file_inode);
}

RTFS_TEST(FileInodeResolve_WhenFsManagerAssemblyIsIncomplete_ShouldReturnEINVAL)
{
    file_system_manager fs_manager;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 1,
        .mode = RTFS_FILE_BUILD_METADATA_ONLY
    };

    memset(&fs_manager, 0, sizeof(fs_manager));

    TEST_ASSERT_EQUAL(EINVAL, rtfsFileInodeResolve(&fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NULL(file_inode);
}

RTFS_TEST(FileInodeResolve_WhenInodeAlreadyCached_ShouldBuildFileInode)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_METADATA_ONLY
    };

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL_UINT64(2u * BLOCK_BUFFER_SIZE, rtfsFileInodeGetSize(file_inode));
    TEST_ASSERT_EQUAL_UINT32(0u, fixture.node_read_count);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeResolve_WhenInodeCanBeLoadedFromNat_ShouldPopulateNodeCache)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle handle;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_METADATA_ONLY
    };

    fileResolverFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.node_read_count);

    handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));
    TEST_ASSERT_EQUAL_UINT32(2000u, nodeBlockCacheEntryGetNodeBlockPtr(handle.entry)->footer.nid);
    TEST_ASSERT_EQUAL_UINT32(10u, nodeBlockCacheEntryGetLpa(handle.entry));
    nodeBlockCacheEntryHandleDestroy(&handle);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeResolve_WithExplicitCache_ShouldUseProvidedCache)
{
    FileResolverFixture fixture;
    RtfsFileInodeCache *cache;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);
    cache = rtfsFileInodeCacheCreate(&fixture.node_cache);
    TEST_ASSERT_NOT_NULL(cache);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, cache, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL_UINT64(2u * BLOCK_BUFFER_SIZE, rtfsFileInodeGetSize(file_inode));

    rtfsFileInodePut(file_inode);
    rtfsFileInodeCacheDestroy(cache);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeResolve_WhenCachedNodeIsDirectory_ShouldReturnEINVAL)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_METADATA_ONLY
    };

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_type = RTFS_FT_DIR;
    fileResolverFixtureAddCachedNode(
        &fixture,
        2000,
        INVALID_NID,
        10,
        &fixture.inode_node
    );

    TEST_ASSERT_EQUAL(EINVAL, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NULL(file_inode);

    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_AfterResolve_ShouldReadDirectInodeBlock)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    char buffer[8];
    off_t offset = 4;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(8, rtfsFileInodeRead(&fixture.fs_manager, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(fixture.data_block20 + 4, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.data_read_count);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_AfterResolve_ShouldReadDirectNodeBlock)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    uint32_t block_index = DEF_ADDRS_PER_INODE;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    char buffer[8];
    off_t offset = (off_t)((uint64_t)block_index * BLOCK_BUFFER_SIZE);

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[0] = 3000;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(8, rtfsFileInodeRead(&fixture.fs_manager, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(fixture.data_block22, buffer, sizeof(buffer));

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_AfterResolve_ShouldReadSingleIndirectBlock)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    uint32_t block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    char buffer[8];
    off_t offset = (off_t)((uint64_t)block_index * BLOCK_BUFFER_SIZE);

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[2] = 4000;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(8, rtfsFileInodeRead(&fixture.fs_manager, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(fixture.data_block22, buffer, sizeof(buffer));

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_AfterResolve_ShouldReadDoubleIndirectBlock)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    uint32_t block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    char buffer[8];
    off_t offset = (off_t)((uint64_t)block_index * BLOCK_BUFFER_SIZE);

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[4] = 5000;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(8, rtfsFileInodeRead(&fixture.fs_manager, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(fixture.data_block22, buffer, sizeof(buffer));

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_WhenDirectNodePathIsMissing_ShouldReturnENOENT)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    uint32_t block_index = DEF_ADDRS_PER_INODE;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    char buffer[8];
    off_t offset = (off_t)((uint64_t)block_index * BLOCK_BUFFER_SIZE);

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[0] = 3999;
    fileResolverFixtureAddCachedNode(
        &fixture,
        2000,
        INVALID_NID,
        10,
        &fixture.inode_node
    );

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeRead(NULL, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(ENOENT, errno);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeWritebackContentCow_WhenDirtyPageExists_ShouldWriteNewDataLpa)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle inode_handle;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "cow";
    off_t offset = 0;
    uint32_t old_lpas[1];
    size_t old_lpa_count = 0;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(3, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));

    TEST_ASSERT_EQUAL(0, rtfsFileInodeWritebackContentCow(&fixture.fs_manager, file_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_file_inode_write_lpa);
    TEST_ASSERT_NOT_EQUAL(20u, g_file_inode_write_lpa);
    TEST_ASSERT_EQUAL_MEMORY(text, g_file_inode_written_block, strlen(text));

    TEST_ASSERT_EQUAL(
        0,
        rtfsFileInodeCollectPendingDataCowOldLpas(
            file_inode,
            old_lpas,
            sizeof(old_lpas) / sizeof(old_lpas[0]),
            &old_lpa_count
        )
    );
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)old_lpa_count);
    TEST_ASSERT_EQUAL_UINT32(20u, old_lpas[0]);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeApplyPendingCowRelocations(&fixture.fs_manager, file_inode));
    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(
        g_file_inode_write_lpa,
        nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry)->i.i_addr[0]
    );
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeWritebackContentCow_WhenWriteFails_ShouldReturnEIOAndKeepMapping)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle inode_handle;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "fail";
    off_t offset = 0;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(4, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));

    g_file_inode_fail_write_lpa = 512;
    TEST_ASSERT_EQUAL(EIO, rtfsFileInodeWritebackContentCow(&fixture.fs_manager, file_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(20u, nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry)->i.i_addr[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeApplyPendingCowRelocations_WhenDirectNodeBlockRelocated_ShouldSwitchMappedLpa)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;
    uint32_t block_index = DEF_ADDRS_PER_INODE;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "dn";
    off_t offset = (off_t)((uint64_t)block_index * BLOCK_BUFFER_SIZE);

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.inode_node.i.i_nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(2, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeWritebackContentCow(&fixture.fs_manager, file_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_file_inode_write_lpa);

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeApplyPendingCowRelocations(&fixture.fs_manager, file_inode));
    TEST_ASSERT_EQUAL_UINT32(g_file_inode_write_lpa, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeApplyPendingCowRelocations_WhenSingleIndirectBlockRelocated_ShouldSwitchMappedLpa)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;
    uint32_t block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "si";
    off_t offset = (off_t)((uint64_t)block_index * BLOCK_BUFFER_SIZE);

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[2] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(2, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeWritebackContentCow(&fixture.fs_manager, file_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_file_inode_write_lpa);

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeApplyPendingCowRelocations(&fixture.fs_manager, file_inode));
    TEST_ASSERT_EQUAL_UINT32(g_file_inode_write_lpa, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeApplyPendingCowRelocations_WhenDoubleIndirectBlockRelocated_ShouldSwitchMappedLpa)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;
    uint32_t block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "di";
    off_t offset = (off_t)((uint64_t)block_index * BLOCK_BUFFER_SIZE);

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[4] = 5000;
    fixture.double_indirect_root.in.nid[0] = 5001;
    fixture.double_indirect_level1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(2, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeWritebackContentCow(&fixture.fs_manager, file_inode));
    TEST_ASSERT_NOT_EQUAL(INVALID_LPA, g_file_inode_write_lpa);

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(22u, cached_direct_node->dn.addr[0]);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeApplyPendingCowRelocations(&fixture.fs_manager, file_inode));
    TEST_ASSERT_EQUAL_UINT32(g_file_inode_write_lpa, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeApplyPendingCowRelocations_WhenMultiplePendingExist_ShouldApplyAllMappings)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle inode_handle;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_inode;
    struct RtfsNode *cached_direct_node;
    uint32_t block_index = DEF_ADDRS_PER_INODE;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *first = "aa";
    const char *second = "bb";
    off_t first_offset = 0;
    off_t second_offset = (off_t)((uint64_t)block_index * BLOCK_BUFFER_SIZE);

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(2, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &first_offset, first, strlen(first)));
    TEST_ASSERT_EQUAL(2, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &second_offset, second, strlen(second)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeWritebackContentCow(&fixture.fs_manager, file_inode));
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)g_file_inode_write_lpa_count);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeApplyPendingCowRelocations(&fixture.fs_manager, file_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_inode = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(g_file_inode_write_lpas[0], cached_inode->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(g_file_inode_write_lpas[1], cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCommitCowWriteback_WhenDirtyFileExists_ShouldSubmitJournalAndClearCurrentJournal)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "commit";
    off_t offset = 0;
    uint64_t tx_id = 0;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(6, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));

    TEST_ASSERT_NULL(g_file_inode_committed_journal);
    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);
    TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
    TEST_ASSERT_TRUE(journalContainerIsEmpty(&fixture.journal));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(g_file_inode_write_lpa, cached_inode->i.i_addr[0]);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, inode_handle.entry->state);
    TEST_ASSERT_FALSE(inode_handle.entry->hasPendingCowRelocation);
    TEST_ASSERT_NOT_EQUAL_UINT32(10u, inode_handle.entry->lpa);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCommitCowWriteback_WhenNodeCowWriteFails_ShouldReturnEIOAndKeepJournal)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "nodefail";
    off_t offset = 0;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(8, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));

    g_node_cow_fail_write_lpa = 1024;
    TEST_ASSERT_EQUAL(EIO, rtfsFileInodeCommitCowWriteback(&fixture.fs_manager, file_inode));
    TEST_ASSERT_NULL(g_file_inode_committed_journal);
    TEST_ASSERT_FALSE(journalContainerIsEmpty(&fixture.journal));

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCommitCowWriteback_WhenJournalSubmitFails_ShouldReturnErrorAndKeepCurrentJournal)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "journalfail";
    off_t offset = 0;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(11, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));

    g_file_inode_journal_commit_rc = EBUSY;
    TEST_ASSERT_EQUAL(EBUSY, rtfsFileInodeCommitCowWriteback(&fixture.fs_manager, file_inode));
    TEST_ASSERT_NULL(g_file_inode_committed_journal);
    TEST_ASSERT_FALSE(journalContainerIsEmpty(&fixture.journal));

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCommitCowWriteback_WhenTxCompletes_ShouldReclaimOldDataAndNodeLpas)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle inode_handle;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "reclaim";
    off_t offset = 0;
    uint64_t tx_id = 0;
    uint32_t old_node_lpa = 10;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(7, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    TEST_ASSERT_NOT_EQUAL_UINT32(old_node_lpa, inode_handle.entry->lpa);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    fileResolverAssertCurrentSitBitInvalid(&fixture, 20);
    fileResolverAssertCurrentSitBitInvalid(&fixture, old_node_lpa);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeWritebackContentCow_WhenWriteFailureIsCleared_ShouldRetryAndCommit)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "recover";
    off_t offset = 0;
    uint64_t tx_id = 0;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(7, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));

    g_file_inode_fail_write_lpa = 512;
    TEST_ASSERT_EQUAL(EIO, rtfsFileInodeWritebackContentCow(&fixture.fs_manager, file_inode));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(20u, cached_inode->i.i_addr[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    g_file_inode_fail_write_lpa = INVALID_LPA;
    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);
    TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(g_file_inode_write_lpa, cached_inode->i.i_addr[0]);
    TEST_ASSERT_NOT_EQUAL_UINT32(20u, cached_inode->i.i_addr[0]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCommitCowWriteback_WhenJournalSubmitFailureIsCleared_ShouldRetryAndClearCurrentJournal)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "retryjournal";
    off_t offset = 0;
    uint64_t tx_id = 0;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(12, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));

    g_file_inode_journal_commit_rc = EBUSY;
    TEST_ASSERT_EQUAL(EBUSY, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NULL(g_file_inode_committed_journal);
    TEST_ASSERT_FALSE(journalContainerIsEmpty(&fixture.journal));
    fileResolverAssertCurrentSitBitValid(&fixture, 20);
    fileResolverAssertCurrentSitBitValid(&fixture, 10);
    fileResolverAssertCurrentSitBitValid(&fixture, g_file_inode_write_lpa);

    g_file_inode_journal_commit_rc = 0;
    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);
    TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
    TEST_ASSERT_TRUE(journalContainerIsEmpty(&fixture.journal));

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCommitCowWriteback_WhenTxNotCompleted_ShouldNotReclaimOldLpasEarly)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "pending";
    off_t offset = 0;
    uint64_t tx_id = 0;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(7, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)kv_size(fixture.journal.sitJournal));
    fileResolverAssertCurrentSitBitValid(&fixture, 20);
    fileResolverAssertCurrentSitBitValid(&fixture, 10);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCommitCowWriteback_WhenTxCompleteAndDrainAreRepeated_ShouldNotDoubleReclaimOldLpas)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *text = "repeat";
    off_t offset = 0;
    uint64_t tx_id = 0;
    size_t sit_after_first_reclaim;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(6, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &offset, text, strlen(text)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);
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

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCommitCowWriteback_WhenMultipleTxExist_ShouldOnlyReclaimCompletedTx)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle inode_handle;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    const char *first = "tx1";
    const char *second = "tx2";
    off_t first_offset = 0;
    off_t second_offset = BLOCK_BUFFER_SIZE;
    uint64_t tx1 = 0;
    uint64_t tx2 = 0;
    uint32_t tx2_old_node_lpa;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);

    TEST_ASSERT_EQUAL(3, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &first_offset, first, strlen(first)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx1));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    tx2_old_node_lpa = inode_handle.entry->lpa;
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    fileResolverReleaseCommittedJournal();
    TEST_ASSERT_EQUAL(3, rtfsFileInodeWrite(&fixture.fs_manager, file_inode, &second_offset, second, strlen(second)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx2));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);
    TEST_ASSERT_NOT_EQUAL_UINT64(tx1, tx2);

    cowReclaimRegistryOnTxComplete(tx1);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)kv_size(fixture.journal.sitJournal));
    fileResolverAssertCurrentSitBitInvalid(&fixture, 20);
    fileResolverAssertCurrentSitBitInvalid(&fixture, 10);
    fileResolverAssertCurrentSitBitValid(&fixture, 21);
    fileResolverAssertCurrentSitBitValid(&fixture, tx2_old_node_lpa);

    cowReclaimRegistryOnTxComplete(tx2);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)kv_size(fixture.journal.sitJournal));
    fileResolverAssertCurrentSitBitInvalid(&fixture, 21);
    fileResolverAssertCurrentSitBitInvalid(&fixture, tx2_old_node_lpa);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeTruncateCommit_WhenDirectInodeBlockIsDropped_ShouldReclaimOldDataLpa)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle inode_handle;
    struct RtfsNode *cached_inode;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    uint64_t tx_id = 0;

    fileResolverFixtureInit(&fixture);
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(0, rtfsFileInodeTruncate(&fixture.fs_manager, file_inode, BLOCK_BUFFER_SIZE));

    inode_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(inode_handle.entry);
    cached_inode = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, cached_inode->i.i_addr[1]);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);
    fileResolverAssertCurrentSitBitValid(&fixture, 21);

    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    fileResolverAssertCurrentSitBitInvalid(&fixture, 21);
    fileResolverAssertCurrentSitBitInvalid(&fixture, 10);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeTruncateCommit_WhenDirectNodeBlockIsDropped_ShouldReclaimOldDataLpa)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;
    uint32_t block_index = DEF_ADDRS_PER_INODE;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    uint64_t tx_id = 0;

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(
        0,
        rtfsFileInodeTruncate(
            &fixture.fs_manager,
            file_inode,
            (uint64_t)block_index * BLOCK_BUFFER_SIZE
        )
    );

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);
    fileResolverAssertCurrentSitBitValid(&fixture, 22);

    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    fileResolverAssertCurrentSitBitInvalid(&fixture, 22);
    fileResolverAssertCurrentSitBitInvalid(&fixture, 30);
    fileResolverAssertCurrentSitBitInvalid(&fixture, 10);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeTruncateCommit_WhenSingleIndirectBlockIsDropped_ShouldReclaimOldDataLpa)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;
    uint32_t block_index = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    uint64_t tx_id = 0;

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[2] = 4000;
    fixture.indirect_node1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(
        0,
        rtfsFileInodeTruncate(
            &fixture.fs_manager,
            file_inode,
            (uint64_t)block_index * BLOCK_BUFFER_SIZE
        )
    );

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);
    fileResolverAssertCurrentSitBitValid(&fixture, 22);

    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    fileResolverAssertCurrentSitBitInvalid(&fixture, 22);
    fileResolverAssertCurrentSitBitInvalid(&fixture, 30);
    fileResolverAssertCurrentSitBitInvalid(&fixture, 10);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}

RTFS_TEST(FileInodeTruncateCommit_WhenDoubleIndirectBlockIsDropped_ShouldReclaimOldDataLpa)
{
    FileResolverFixture fixture;
    RtfsFileInode *file_inode = NULL;
    NodeBlockCacheEntryHandle direct_handle;
    struct RtfsNode *cached_direct_node;
    uint32_t block_index =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    RtfsFileInodeBuildRequest request = {
        .ino = 2000,
        .mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE
    };
    uint64_t tx_id = 0;

    fileResolverFixtureInit(&fixture);
    fixture.inode_node.i.i_size = ((uint64_t)block_index + 1u) * BLOCK_BUFFER_SIZE;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;
    fixture.inode_node.i.i_nid[4] = 5000;
    fixture.double_indirect_root.in.nid[0] = 5001;
    fixture.double_indirect_level1.in.nid[0] = 3000;
    fixture.direct_node1.dn.addr[0] = 22;
    fileResolverFixtureCacheDefaultNodes(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeResolve(&fixture.fs_manager, NULL, &request, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    TEST_ASSERT_EQUAL(
        0,
        rtfsFileInodeTruncate(
            &fixture.fs_manager,
            file_inode,
            (uint64_t)block_index * BLOCK_BUFFER_SIZE
        )
    );

    direct_handle = nodeBlockCacheGet(&fixture.node_cache, 3000);
    TEST_ASSERT_NOT_NULL(direct_handle.entry);
    cached_direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, cached_direct_node->dn.addr[0]);
    nodeBlockCacheEntryHandleDestroy(&direct_handle);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeCommitCowWritebackWithTxId(&fixture.fs_manager, file_inode, &tx_id));
    TEST_ASSERT_NOT_NULL(g_file_inode_committed_journal);
    fileResolverAssertCurrentSitBitValid(&fixture, 22);

    cowReclaimRegistryOnTxComplete(tx_id);
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    fileResolverAssertCurrentSitBitInvalid(&fixture, 22);
    fileResolverAssertCurrentSitBitInvalid(&fixture, 30);
    fileResolverAssertCurrentSitBitInvalid(&fixture, 10);

    rtfsFileInodePut(file_inode);
    fileResolverFixtureFini(&fixture);
}
