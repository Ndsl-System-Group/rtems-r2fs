#include "rtfs_test.h"

#include "cache/generic_cache_manager.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/dev.h"
#include "fs/fs_manager.h"
#include "journal/journal_container.h"

#include <memory.h>
#include <pthread.h>
#include <rtems/thread.h>

#include "inode/inode_loader.h"

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

typedef struct InodeLoaderFixture
{
    file_system_manager fs_manager;
    struct RtfsSuperBlock super_block;
    NodeBlockCache node_cache;
    SitNatCache nat_cache;
    JournalContainer journal;
    comm_dev dev;
    struct RtfsNode loaded_node;
    uint32_t read_count;
    uint32_t last_read_lpa;
    bool read_hook_enabled;
} InodeLoaderFixture;

static InodeLoaderFixture *g_inode_loader_fixture = NULL;
static struct RtfsNode g_inode_loader_alt_node;

static int inodeLoaderReadBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    (void)dev;

    if (g_inode_loader_fixture == NULL) {
        return EIO;
    }

    g_inode_loader_fixture->read_count++;
    g_inode_loader_fixture->last_read_lpa = lpa;
    memset(buffer, 0, BLOCK_BUFFER_SIZE);
    if (lpa == 500u) {
        memcpy(buffer, &g_inode_loader_fixture->loaded_node, sizeof(g_inode_loader_fixture->loaded_node));
        return 0;
    }

    if (lpa == 501u) {
        memcpy(buffer, &g_inode_loader_alt_node, sizeof(g_inode_loader_alt_node));
        return 0;
    }

    if (lpa != 500u && lpa != 501u) {
        return EIO;
    }

    return EIO;
}

static void inodeLoaderFixtureSetNatEntry(
    InodeLoaderFixture *fixture,
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

static void inodeLoaderFixtureInit(InodeLoaderFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));

    fixture->super_block.nat_blkaddr = 100;
    fixture->super_block.segment_count_nat = 1;
    fixture->fs_manager.super_blk_mem_ = &fixture->super_block;
    fixture->fs_manager.dev_ = &fixture->dev;
    fixture->fs_manager.cur_journal_ = &fixture->journal;

    journalContainerInit(&fixture->journal);
    nodeBlockCacheInit(&fixture->node_cache, &fixture->fs_manager, 8);
    fixture->fs_manager.node_cache_ = &fixture->node_cache;
    sitNatCacheInit(&fixture->nat_cache, &fixture->dev, 8);
    fixture->fs_manager.nat_cache_ = &fixture->nat_cache;

    fixture->loaded_node.footer.nid = 42;
    fixture->loaded_node.footer.ino = 42;
    fixture->loaded_node.footer.offset = 0;
    fixture->loaded_node.i.i_type = RTFS_FT_DIR;
    fixture->loaded_node.i.i_pino = 7;
    fixture->loaded_node.i.i_size = 4096;
    fixture->loaded_node.i.i_addr[0] = 1234;

    memset(&g_inode_loader_alt_node, 0, sizeof(g_inode_loader_alt_node));
    g_inode_loader_alt_node.footer.nid = 43;
    g_inode_loader_alt_node.footer.ino = 43;
    g_inode_loader_alt_node.footer.offset = 99;
    g_inode_loader_alt_node.i.i_type = RTFS_FT_REG_FILE;
    g_inode_loader_alt_node.i.i_pino = 8;
    g_inode_loader_alt_node.i.i_size = 8192;
    g_inode_loader_alt_node.i.i_addr[0] = 5678;
}

static void inodeLoaderFixtureEnableReadHook(InodeLoaderFixture *fixture)
{
    g_inode_loader_fixture = fixture;
    fixture->read_hook_enabled = true;
    nodeBlockCacheSetReadBlockHook(inodeLoaderReadBlockHook);
}

static void inodeLoaderFixtureFini(InodeLoaderFixture *fixture)
{
    if (fixture->read_hook_enabled) {
        nodeBlockCacheSetReadBlockHook(NULL);
        fixture->read_hook_enabled = false;
        g_inode_loader_fixture = NULL;
    }

    sitNatCacheDestroy(&fixture->nat_cache);
    nodeBlockCacheDestroy(&fixture->node_cache);
    journalContainerDestroy(&fixture->journal);
}

RTFS_TEST(InodeLoaderEnsureCached_WhenFsManagerIsNull_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(EINVAL, rtfsInodeLoaderEnsureCached(NULL, 1));
}

RTFS_TEST(InodeLoaderEnsureCached_WhenInoIsInvalid_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(EINVAL, rtfsInodeLoaderEnsureCached(NULL, INVALID_NID));
}

RTFS_TEST(InodeLoaderEnsureCached_WhenNodeCacheIsMissing_ShouldReturnEINVAL)
{
    InodeLoaderFixture fixture;

    inodeLoaderFixtureInit(&fixture);
    fixture.fs_manager.node_cache_ = NULL;

    TEST_ASSERT_EQUAL(EINVAL, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 42));

    inodeLoaderFixtureFini(&fixture);
}

RTFS_TEST(InodeLoaderEnsureCached_WhenNodeAlreadyInCache_ShouldReturnZero)
{
    InodeLoaderFixture fixture;
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;

    inodeLoaderFixtureInit(&fixture);
    blockBufferInit(&buffer);
    memset(blockBufferGetPtr(&buffer), 0, BLOCK_BUFFER_SIZE);
    ((struct RtfsNode *)blockBufferGetPtr(&buffer))->footer.nid = 42;
    ((struct RtfsNode *)blockBufferGetPtr(&buffer))->footer.ino = 42;

    handle = nodeBlockCacheAdd(&fixture.node_cache, &buffer, 42, INVALID_NID, 700);
    fixture.fs_manager.nat_cache_ = NULL;
    fixture.fs_manager.dev_ = NULL;

    TEST_ASSERT_EQUAL(0, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 42));
    TEST_ASSERT_EQUAL_UINT32(0u, fixture.read_count);

    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
    inodeLoaderFixtureFini(&fixture);
}

RTFS_TEST(InodeLoaderEnsureCached_WhenCacheMissAndHelperLoadsNode_ShouldReturnZeroAndPopulateCache)
{
    InodeLoaderFixture fixture;
    NodeBlockCacheEntryHandle handle;

    inodeLoaderFixtureInit(&fixture);
    inodeLoaderFixtureSetNatEntry(&fixture, 42, 42, 500);
    inodeLoaderFixtureEnableReadHook(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 42));
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.read_count);
    TEST_ASSERT_EQUAL_UINT32(500u, fixture.last_read_lpa);

    handle = nodeBlockCacheGet(&fixture.node_cache, 42);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));
    TEST_ASSERT_EQUAL_UINT32(42u, nodeBlockCacheEntryGetNodeBlockPtr(handle.entry)->footer.nid);
    TEST_ASSERT_EQUAL_UINT32(42u, nodeBlockCacheEntryGetNodeBlockPtr(handle.entry)->footer.ino);
    TEST_ASSERT_EQUAL_UINT32(500u, nodeBlockCacheEntryGetLpa(handle.entry));
    nodeBlockCacheEntryHandleDestroy(&handle);

    inodeLoaderFixtureFini(&fixture);
}

RTFS_TEST(InodeLoaderEnsureCached_WhenNodeWasLoadedPreviously_ShouldReuseCacheWithoutExtraRead)
{
    InodeLoaderFixture fixture;

    inodeLoaderFixtureInit(&fixture);
    inodeLoaderFixtureSetNatEntry(&fixture, 42, 42, 500);
    inodeLoaderFixtureEnableReadHook(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 42));
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.read_count);

    fixture.fs_manager.nat_cache_ = NULL;
    fixture.fs_manager.dev_ = NULL;

    TEST_ASSERT_EQUAL(0, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 42));
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.read_count);

    inodeLoaderFixtureFini(&fixture);
}

RTFS_TEST(InodeLoaderEnsureCached_WhenHelperLoadsNode_ShouldPreserveFooterAndKeyFields)
{
    InodeLoaderFixture fixture;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node;

    inodeLoaderFixtureInit(&fixture);
    inodeLoaderFixtureSetNatEntry(&fixture, 42, 42, 500);
    inodeLoaderFixtureEnableReadHook(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 42));

    handle = nodeBlockCacheGet(&fixture.node_cache, 42);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));
    node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);

    TEST_ASSERT_EQUAL_UINT32(42u, node->footer.nid);
    TEST_ASSERT_EQUAL_UINT32(42u, node->footer.ino);
    TEST_ASSERT_EQUAL_UINT32(0u, node->footer.offset);
    TEST_ASSERT_EQUAL_UINT32(RTFS_FT_DIR, (uint32_t)node->i.i_type);
    TEST_ASSERT_EQUAL_UINT32(7u, node->i.i_pino);
    TEST_ASSERT_EQUAL_UINT64(4096u, node->i.i_size);
    TEST_ASSERT_EQUAL_UINT32(1234u, node->i.i_addr[0]);

    nodeBlockCacheEntryHandleDestroy(&handle);
    inodeLoaderFixtureFini(&fixture);
}

RTFS_TEST(InodeLoaderEnsureCached_WhenDifferentInosAreLoadedSequentially_ShouldKeepCacheKeysIsolated)
{
    InodeLoaderFixture fixture;
    NodeBlockCacheEntryHandle handle42;
    NodeBlockCacheEntryHandle handle43;
    struct RtfsNode *node42;
    struct RtfsNode *node43;

    inodeLoaderFixtureInit(&fixture);
    inodeLoaderFixtureSetNatEntry(&fixture, 42, 42, 500);
    inodeLoaderFixtureSetNatEntry(&fixture, 43, 43, 501);
    inodeLoaderFixtureEnableReadHook(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 42));
    TEST_ASSERT_EQUAL(0, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 43));
    TEST_ASSERT_EQUAL_UINT32(2u, fixture.read_count);

    handle42 = nodeBlockCacheGet(&fixture.node_cache, 42);
    handle43 = nodeBlockCacheGet(&fixture.node_cache, 43);
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle42));
    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle43));

    node42 = nodeBlockCacheEntryGetNodeBlockPtr(handle42.entry);
    node43 = nodeBlockCacheEntryGetNodeBlockPtr(handle43.entry);

    TEST_ASSERT_EQUAL_UINT32(42u, node42->footer.nid);
    TEST_ASSERT_EQUAL_UINT32(42u, node42->footer.ino);
    TEST_ASSERT_EQUAL_UINT32(1234u, node42->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(500u, nodeBlockCacheEntryGetLpa(handle42.entry));

    TEST_ASSERT_EQUAL_UINT32(43u, node43->footer.nid);
    TEST_ASSERT_EQUAL_UINT32(43u, node43->footer.ino);
    TEST_ASSERT_EQUAL_UINT32(99u, node43->footer.offset);
    TEST_ASSERT_EQUAL_UINT32(5678u, node43->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(501u, nodeBlockCacheEntryGetLpa(handle43.entry));

    TEST_ASSERT_TRUE(handle42.entry != handle43.entry);

    nodeBlockCacheEntryHandleDestroy(&handle42);
    nodeBlockCacheEntryHandleDestroy(&handle43);
    inodeLoaderFixtureFini(&fixture);
}

RTFS_TEST(InodeLoaderEnsureCached_WhenCacheMissAndNatCacheMissing_ShouldReturnEnoent)
{
    InodeLoaderFixture fixture;

    inodeLoaderFixtureInit(&fixture);
    fixture.fs_manager.nat_cache_ = NULL;

    TEST_ASSERT_EQUAL(ENOENT, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 42));
    TEST_ASSERT_EQUAL_UINT32(0u, fixture.read_count);

    inodeLoaderFixtureFini(&fixture);
}

RTFS_TEST(InodeLoaderEnsureCached_WhenCacheMissAndDeviceMissing_ShouldReturnEnoent)
{
    InodeLoaderFixture fixture;

    inodeLoaderFixtureInit(&fixture);
    fixture.fs_manager.dev_ = NULL;

    TEST_ASSERT_EQUAL(ENOENT, rtfsInodeLoaderEnsureCached(&fixture.fs_manager, 42));
    TEST_ASSERT_EQUAL_UINT32(0u, fixture.read_count);

    inodeLoaderFixtureFini(&fixture);
}
