#include "rtfs_test.h"

#include <errno.h>
#include <pthread.h>
#include <rtems.h>
#include <rtems/thread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include "cache/block_buffer.h"
#include "cache/generic_cache_manager.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/comm_api.h"
#include "communication/dev.h"
#include "dir_inode/dir_inode.h"
#include "dir_inode/dir_handler.h"
#include "dir_inode/dir_inode_resolver.h"
#include "fs/cow_reclaim_registry.h"
#include "fs/fs.h"
#include "fs/fs_handler.h"
#include "fs/srmap_utils.h"
#include "fs/super_manager.h"
#include "file_inode/file_handler.h"
#include "journal/journal_container.h"
#include "journal/journal_process_env.h"
#include "uthash/utlist.h"

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

typedef struct FsHandlerWrittenBlock
{
    uint32_t lpa;
    char data[BLOCK_BUFFER_SIZE];
} FsHandlerWrittenBlock;

typedef struct FsHandlerFixture
{
    file_system_manager fs_manager;
    struct RtfsSuperBlock super_block;
    NodeBlockCache node_cache;
    SitNatCache sit_cache;
    SitNatCache nat_cache;
    super_manager *sp_manager;
    JournalContainer journal;
    comm_dev dev;
    struct RtfsNode parent_inode;
    struct RtfsNode other_parent_inode;
    struct RtfsNode target_inode;
    struct RtfsDentryBlock parent_block;
    struct RtfsDentryBlock other_parent_block;
    rtems_filesystem_mount_table_entry_t mt_entry;
    rtems_filesystem_location_info_t parentloc;
    rtems_filesystem_location_info_t other_parentloc;
    rtems_filesystem_location_info_t oldloc;
    RtfsRuntimeInodeView parent_view;
    RtfsRuntimeInodeView other_parent_view;
    RtfsRuntimeInodeView old_view;
    FsHandlerWrittenBlock written_blocks[8];
    size_t written_block_count;
    uint32_t fail_read_lpa;
    uint32_t fail_dir_write_lpa;
    uint32_t fail_node_write_lpa;
    bool hook_enabled;
} FsHandlerFixture;

typedef struct FsHandlerInitFixture
{
    comm_dev dev;
    rtems_disk_device disk;
    struct RtfsSuperBlock super_block;
    unsigned char *block_store;
    size_t block_store_size;
    rtems_filesystem_mount_table_entry_t mt_entry;
    rtems_filesystem_global_location_t root_gloc;
} FsHandlerInitFixture;

static FsHandlerFixture *g_fs_handler_fixture = NULL;
static FsHandlerInitFixture *g_fs_handler_init_fixture = NULL;
static uint64_t g_fs_handler_committed_tx_id = 0;
static bool g_fs_handler_journal_env_stub_inited = false;
static uint32_t g_fs_handler_init_super_read_count = 0;
static uint32_t g_fs_handler_init_recover_call_count = 0;
static int g_fs_handler_init_recover_result = 0;

static uint64_t fsHandlerGetLatestQueuedJournalTxId(void)
{
    JournalProcessEnv *env;
    JournalCommitNode *node;

    env = journalProcessEnvGetInstance();
    if (env == NULL || env->commitQueueHead == NULL) {
        return 0;
    }

    node = env->commitQueueHead;
    while (node->next != NULL) {
        node = node->next;
    }

    return journalContainerGetTxId(node->journal);
}

static uint64_t fsHandlerGetLatestObservedTxId(void)
{
    uint64_t queued_tx_id = fsHandlerGetLatestQueuedJournalTxId();

    return g_fs_handler_committed_tx_id > queued_tx_id
        ? g_fs_handler_committed_tx_id
        : queued_tx_id;
}

static int fsHandlerInitReadSuperBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    FsHandlerInitFixture *fixture = g_fs_handler_init_fixture;

    (void)dev;

    if (fixture == NULL || buffer == NULL) {
        return EIO;
    }

    TEST_ASSERT_EQUAL_UINT32(0u, lpa);
    TEST_ASSERT_EQUAL_UINT32(1u, g_fs_handler_init_recover_call_count);
    g_fs_handler_init_super_read_count++;
    memcpy(buffer, &fixture->super_block, sizeof(fixture->super_block));
    return 0;
}

static int fsHandlerInitRecoverHook(struct comm_dev *dev)
{
    FsHandlerInitFixture *fixture = g_fs_handler_init_fixture;

    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL_PTR(&fixture->dev, dev);
    TEST_ASSERT_EQUAL_UINT32(0u, g_fs_handler_init_super_read_count);
    g_fs_handler_init_recover_call_count++;
    return g_fs_handler_init_recover_result;
}

static int fsHandlerInitDiskIoctl(
    rtems_disk_device *dd,
    uint32_t req,
    void *argp
)
{
    FsHandlerInitFixture *fixture = g_fs_handler_init_fixture;
    rtems_blkdev_request *breq;
    size_t i;

    TEST_ASSERT_NOT_NULL(dd);
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL_PTR(&fixture->disk, dd);

    if (req != RTEMS_BLKIO_REQUEST || argp == NULL) {
        errno = EINVAL;
        return -1;
    }

    breq = (rtems_blkdev_request *)argp;
    for (i = 0; i < breq->bufnum; ++i) {
        rtems_blkdev_sg_buffer *sg = &breq->bufs[i];
        uint64_t byte_off = (uint64_t)sg->block * fixture->dev.blockSize;

        TEST_ASSERT_NOT_NULL(sg->buffer);
        TEST_ASSERT_NOT_NULL(fixture->block_store);
        TEST_ASSERT_TRUE(byte_off + sg->length <= fixture->block_store_size);

        if (breq->req == RTEMS_BLKDEV_REQ_READ) {
            memcpy(sg->buffer, fixture->block_store + byte_off, sg->length);
        } else if (breq->req == RTEMS_BLKDEV_REQ_WRITE) {
            memcpy(fixture->block_store + byte_off, sg->buffer, sg->length);
        } else if (breq->req != RTEMS_BLKDEV_REQ_SYNC) {
            rtems_blkdev_request_done(breq, RTEMS_IO_ERROR);
            errno = EINVAL;
            return -1;
        }
    }

    rtems_blkdev_request_done(breq, RTEMS_SUCCESSFUL);
    return 0;
}

static void fsHandlerInitFixtureReset(void)
{
    superCacheSetReadBlockHook(NULL);
    commSetTestFsRecoverHook(NULL);
    fileSystemManagerFini();
    g_fs_handler_init_fixture = NULL;
    g_fs_handler_fixture = NULL;
    g_fs_handler_init_super_read_count = 0;
    g_fs_handler_init_recover_call_count = 0;
    g_fs_handler_init_recover_result = 0;
}

static void fsHandlerInitFixtureInit(FsHandlerInitFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->super_block.srmap_blkaddr = 80;
    fixture->super_block.meta_journal_blkaddr = 100;
    fixture->super_block.segment_count_meta_journal = 2;
    fixture->super_block.meta_journal_end_blkoff = 3;
    fixture->super_block.root_ino = 1;
    fixture->super_block.nat_blkaddr = 200;
    fixture->super_block.sit_blkaddr = 300;
    fixture->super_block.segment0_blkaddr = 0;
    fixture->block_store_size = 8192u * 512u;
    fixture->block_store = (unsigned char *)calloc(1, fixture->block_store_size);
    TEST_ASSERT_NOT_NULL(fixture->block_store);
    fixture->disk.phys_dev = &fixture->disk;
    fixture->disk.ioctl = fsHandlerInitDiskIoctl;
    fixture->disk.block_size = 512;
    fixture->disk.media_block_size = 512;
    fixture->disk.block_count = 8192;

    TEST_ASSERT_EQUAL(
        0,
        commDevInit(
            &fixture->dev,
            &fixture->disk,
            512,
            8192,
            100,
            116
        )
    );

    fixture->mt_entry.mt_fs_root = &fixture->root_gloc;
    fixture->root_gloc.location.mt_entry = &fixture->mt_entry;

    g_fs_handler_init_fixture = fixture;
    superCacheSetReadBlockHook(fsHandlerInitReadSuperBlockHook);
    commSetTestFsRecoverHook(fsHandlerInitRecoverHook);
}

static void fsHandlerInitFixtureFini(FsHandlerInitFixture *fixture)
{
    fileSystemManagerFini();
    commDevDestroy(&fixture->dev);
    free(fixture->block_store);
    fixture->block_store = NULL;
    fixture->block_store_size = 0;
    fsHandlerInitFixtureReset();
}

static void fsHandlerSetBitmapBit(uint8_t *bitmap, size_t bit_index)
{
    bitmap[bit_index / 8] |= (uint8_t)(1u << (bit_index % 8));
}

static void fsHandlerWriteRegularName(
    struct RtfsDentryBlock *dentry_block,
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

        memcpy(dentry_block->filename[index + slot], name + offset, copy_len);
        offset += copy_len;
    }
}

static void fsHandlerAddRegularDentry(
    struct RtfsDentryBlock *dentry_block,
    size_t index,
    rtfs_ino ino,
    uint8_t file_type,
    const char *name
)
{
    size_t slot_count = GET_DENTRY_SLOTS(strlen(name));
    size_t slot;

    dentry_block->dentry[index].ino = ino;
    dentry_block->dentry[index].name_len = strlen(name);
    dentry_block->dentry[index].file_type = file_type;

    for (slot = 0; slot < slot_count; ++slot) {
        fsHandlerSetBitmapBit(dentry_block->dentry_bitmap, index + slot);
    }

    fsHandlerWriteRegularName(dentry_block, index, name);
}

static void fsHandlerFixtureSetNatEntry(
    FsHandlerFixture *fixture,
    uint32_t nid,
    uint32_t ino,
    uint32_t block_addr
)
{
    uint32_t nat_lpa = fixture->super_block.nat_blkaddr + (nid / NAT_ENTRY_PER_BLOCK);
    uint32_t nat_idx = nid % NAT_ENTRY_PER_BLOCK;
    SitNatCacheEntry *entry;
    SitNatCacheEntryHandle handle;

    entry = (SitNatCacheEntry *)genericCacheManagerGet(
        &fixture->nat_cache.cacheManager,
        nat_lpa,
        false
    );
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

static void fsHandlerFixtureMarkSitValid(
    FsHandlerFixture *fixture,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    uint32_t bitmap_idx = seg_off / 8u;
    uint32_t bitmap_off = seg_off % 8u;
    uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);
    SitNatCacheEntry *entry;
    SitNatCacheEntryHandle handle;
    struct RtfsSitBlock *sit_block;
    struct RtfsSitEntry *sit_entry;

    entry = (SitNatCacheEntry *)genericCacheManagerGet(
        &fixture->sit_cache.cacheManager,
        sit_lpa,
        false
    );
    if (entry == NULL) {
        entry = (SitNatCacheEntry *)malloc(sizeof(*entry));
        TEST_ASSERT_NOT_NULL(entry);
        sitNatCacheEntryInit(entry, sit_lpa);
        memset(blockBufferGetPtr(&entry->cache), 0, BLOCK_BUFFER_SIZE);
        genericCacheManagerAdd(&fixture->sit_cache.cacheManager, sit_lpa, entry);
        fixture->sit_cache.curSize++;
    }

    handle = sitNatCacheGet(&fixture->sit_cache, sit_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    sit_block = sitNatCacheEntryHandleGetSitBlockPtr(&handle);
    sit_entry = &sit_block->entries[sit_idx];

    if ((sit_entry->valid_map[bitmap_idx] & (1u << bitmap_off)) == 0) {
        sit_entry->valid_map[bitmap_idx] |= (uint8_t)(1u << bitmap_off);
        if (GET_SIT_VBLOCKS(sit_entry) < 511u) {
            sit_entry->vblocks += 1u;
        }
    }

    sitNatCacheEntryHandleDestroy(&handle);
}

static void fsHandlerFixtureStoreWrittenBlock(
    FsHandlerFixture *fixture,
    uint32_t lpa,
    const void *buffer
)
{
    size_t i;

    for (i = 0; i < fixture->written_block_count; ++i) {
        if (fixture->written_blocks[i].lpa == lpa) {
            memcpy(fixture->written_blocks[i].data, buffer, BLOCK_BUFFER_SIZE);
            return;
        }
    }

    TEST_ASSERT_LESS_OR_EQUAL_size_t(
        sizeof(fixture->written_blocks) / sizeof(fixture->written_blocks[0]),
        fixture->written_block_count + 1
    );
    fixture->written_blocks[fixture->written_block_count].lpa = lpa;
    memcpy(
        fixture->written_blocks[fixture->written_block_count].data,
        buffer,
        BLOCK_BUFFER_SIZE
    );
    fixture->written_block_count++;
}

static void fsHandlerFixtureSyncCachedNode(
    FsHandlerFixture *fixture,
    uint32_t nid,
    const struct RtfsNode *node
)
{
    NodeBlockCacheEntryHandle handle;

    handle = nodeBlockCacheGet(&fixture->node_cache, nid);
    TEST_ASSERT_NOT_NULL(handle.entry);
    blockBufferCopyContentFromBuf(nodeBlockCacheEntryGetNodeBuffer(handle.entry), (const char *)node);
    nodeBlockCacheEntryHandleDestroy(&handle);
}

static bool fsHandlerFixtureIsSitBitValid(
    const FsHandlerFixture *fixture,
    uint32_t lpa
)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    uint32_t bitmap_idx = seg_off / 8u;
    uint32_t bitmap_off = seg_off % 8u;
    SitNatCacheEntryHandle handle;
    struct RtfsSitBlock *sit_block;
    bool is_valid = false;
    uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);

    handle = sitNatCacheGet((SitNatCache *)&fixture->sit_cache, sit_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    sit_block = sitNatCacheEntryHandleGetSitBlockPtr(&handle);
    is_valid =
        (sit_block->entries[sit_idx].valid_map[bitmap_idx] & (1u << bitmap_off)) != 0;
    sitNatCacheEntryHandleDestroy(&handle);
    return is_valid;
}

static int fsHandlerTestReadBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    size_t i;

    (void)dev;

    if (g_fs_handler_fixture == NULL) {
        return EIO;
    }

    if (g_fs_handler_fixture->fail_read_lpa != INVALID_LPA &&
        lpa == g_fs_handler_fixture->fail_read_lpa) {
        return EIO;
    }

    for (i = 0; i < g_fs_handler_fixture->written_block_count; ++i) {
        if (g_fs_handler_fixture->written_blocks[i].lpa == lpa) {
            memcpy(buffer, g_fs_handler_fixture->written_blocks[i].data, BLOCK_BUFFER_SIZE);
            return 0;
        }
    }

    if (lpa == 10) {
        memcpy(buffer, &g_fs_handler_fixture->parent_inode, sizeof(g_fs_handler_fixture->parent_inode));
        return 0;
    }

    if (lpa == 11) {
        memcpy(buffer, &g_fs_handler_fixture->other_parent_inode, sizeof(g_fs_handler_fixture->other_parent_inode));
        return 0;
    }

    if (lpa == 20) {
        memcpy(buffer, &g_fs_handler_fixture->parent_block, sizeof(g_fs_handler_fixture->parent_block));
        return 0;
    }

    if (lpa == 21) {
        memcpy(buffer, &g_fs_handler_fixture->other_parent_block, sizeof(g_fs_handler_fixture->other_parent_block));
        return 0;
    }

    if (lpa == 30) {
        memcpy(buffer, &g_fs_handler_fixture->target_inode, sizeof(g_fs_handler_fixture->target_inode));
        return 0;
    }

    return ENOENT;
}

static int fsHandlerTestDirWriteBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    const void *buffer
)
{
    (void)dev;

    if (g_fs_handler_fixture == NULL) {
        return EIO;
    }

    if (g_fs_handler_fixture->fail_dir_write_lpa != INVALID_LPA &&
        lpa == g_fs_handler_fixture->fail_dir_write_lpa) {
        return EIO;
    }

    fsHandlerFixtureStoreWrittenBlock(g_fs_handler_fixture, lpa, buffer);
    return 0;
}

static int fsHandlerTestNodeWriteBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    const void *buffer
)
{
    (void)dev;
    (void)buffer;

    if (g_fs_handler_fixture == NULL) {
        return EIO;
    }

    if (g_fs_handler_fixture->fail_node_write_lpa != INVALID_LPA &&
        lpa == g_fs_handler_fixture->fail_node_write_lpa) {
        return EIO;
    }

    return 0;
}

static int fsHandlerTestJournalCommitHook(JournalContainer *journal)
{
    g_fs_handler_committed_tx_id = journalContainerGetTxId(journal);
    (void)journal;
    return 0;
}

static void fsHandlerTestInitJournalEnvStub(void)
{
    JournalProcessEnv *env;

    if (g_fs_handler_journal_env_stub_inited) {
        return;
    }

    env = journalProcessEnvGetInstance();
    memset(env, 0, sizeof(*env));
    rtfsMutexInit(&env->mtx);
    rtfsCondInit(&env->cond);
    atomic_init(&env->txIdToAlloc, 1);
    env->commitQueueHead = NULL;
    env->exitReq = false;
    g_fs_handler_journal_env_stub_inited = true;
}

static void fsHandlerTestFiniJournalEnvStub(void)
{
    JournalProcessEnv *env;
    JournalCommitNode *node;
    JournalCommitNode *tmp;

    if (!g_fs_handler_journal_env_stub_inited) {
        return;
    }

    env = journalProcessEnvGetInstance();
    DL_FOREACH_SAFE(env->commitQueueHead, node, tmp) {
        DL_DELETE(env->commitQueueHead, node);
        free(node);
    }
    env->commitQueueHead = NULL;
    rtfsCondDestroy(&env->cond);
    rtfsMutexDestroy(&env->mtx);
    memset(env, 0, sizeof(*env));
    g_fs_handler_journal_env_stub_inited = false;
}

static void fsHandlerFixtureInit(FsHandlerFixture *fixture)
{
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;

    memset(fixture, 0, sizeof(*fixture));
    fixture->fail_read_lpa = INVALID_LPA;
    fixture->fail_dir_write_lpa = INVALID_LPA;
    fixture->fail_node_write_lpa = INVALID_LPA;

    fixture->super_block.root_ino = 2000;
    fixture->super_block.block_count = 4096;
    fixture->super_block.nat_blkaddr = 100;
    fixture->super_block.sit_blkaddr = 200;
    fixture->super_block.segment_count_nat = 1;
    fixture->super_block.segment_count_sit = 1;
    fixture->super_block.current_data_segment_id = 1;
    fixture->super_block.current_data_segment_blkoff = 0;
    fixture->super_block.current_node_segment_id = 2;
    fixture->super_block.current_node_segment_blkoff = 0;
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
    fsHandlerTestInitJournalEnvStub();

    fsHandlerFixtureSetNatEntry(fixture, 2000, 2000, 10);
    fsHandlerFixtureSetNatEntry(fixture, 2001, 2001, 11);
    fsHandlerFixtureSetNatEntry(fixture, 3001, 3001, 30);
    fsHandlerFixtureSetNatEntry(fixture, 6000, INVALID_NID, 6001);
    fsHandlerFixtureSetNatEntry(fixture, 6001, INVALID_NID, INVALID_NID);

    fsHandlerFixtureMarkSitValid(fixture, 10);
    fsHandlerFixtureMarkSitValid(fixture, 11);
    fsHandlerFixtureMarkSitValid(fixture, 20);
    fsHandlerFixtureMarkSitValid(fixture, 21);
    fsHandlerFixtureMarkSitValid(fixture, 30);

    memset(&fixture->parent_inode, 0, sizeof(fixture->parent_inode));
    fixture->parent_inode.i.i_type = RTFS_FT_DIR;
    fixture->parent_inode.i.i_mode = 0755;
    fixture->parent_inode.i.i_nlink = 2;
    fixture->parent_inode.i.i_pino = 1999;
    fixture->parent_inode.i.i_size = BLOCK_BUFFER_SIZE;
    fixture->parent_inode.i.i_dentry_num = 1;
    fixture->parent_inode.i.i_addr[0] = 20;
    fixture->parent_inode.footer.nid = 2000;
    fixture->parent_inode.footer.ino = 2000;

    memset(&fixture->other_parent_inode, 0, sizeof(fixture->other_parent_inode));
    fixture->other_parent_inode.i.i_type = RTFS_FT_DIR;
    fixture->other_parent_inode.i.i_mode = 0755;
    fixture->other_parent_inode.i.i_nlink = 2;
    fixture->other_parent_inode.i.i_pino = 1999;
    fixture->other_parent_inode.i.i_size = BLOCK_BUFFER_SIZE;
    fixture->other_parent_inode.i.i_dentry_num = 0;
    fixture->other_parent_inode.i.i_addr[0] = 21;
    fixture->other_parent_inode.footer.nid = 2001;
    fixture->other_parent_inode.footer.ino = 2001;

    memset(&fixture->target_inode, 0, sizeof(fixture->target_inode));
    fixture->target_inode.i.i_type = RTFS_FT_REG_FILE;
    fixture->target_inode.i.i_mode = 0644;
    fixture->target_inode.i.i_nlink = 1;
    fixture->target_inode.i.i_pino = 2000;
    fixture->target_inode.i.i_namelen = 5;
    memcpy(fixture->target_inode.i.i_name, "alpha", 5);
    fixture->target_inode.footer.nid = 3001;
    fixture->target_inode.footer.ino = 3001;

    memset(&fixture->parent_block, 0, sizeof(fixture->parent_block));
    fsHandlerAddRegularDentry(
        &fixture->parent_block,
        0,
        3001,
        RTFS_FT_REG_FILE,
        "alpha"
    );

    blockBufferInit(&buffer);
    blockBufferCopyContentFromBuf(&buffer, (const char *)&fixture->parent_inode);
    handle = nodeBlockCacheAdd(&fixture->node_cache, &buffer, 2000, INVALID_NID, 10);
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferCopyContentFromBuf(&buffer, (const char *)&fixture->other_parent_inode);
    handle = nodeBlockCacheAdd(&fixture->node_cache, &buffer, 2001, INVALID_NID, 11);
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferCopyContentFromBuf(&buffer, (const char *)&fixture->target_inode);
    handle = nodeBlockCacheAdd(&fixture->node_cache, &buffer, 3001, INVALID_NID, 30);
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);

    fixture->mt_entry.fs_info = &fixture->fs_manager;
    fixture->parentloc.mt_entry = &fixture->mt_entry;
    fixture->other_parentloc.mt_entry = &fixture->mt_entry;
    fixture->oldloc.mt_entry = &fixture->mt_entry;

    rtfsRuntimeInodeViewInit(&fixture->parent_view, 2000, 1999, RTFS_FT_DIR);
    rtfsRuntimeInodeViewInit(&fixture->other_parent_view, 2001, 1999, RTFS_FT_DIR);
    rtfsRuntimeInodeViewInit(&fixture->old_view, 3001, 2000, RTFS_FT_REG_FILE);
    fixture->parentloc.node_access = &fixture->parent_view;
    fixture->other_parentloc.node_access = &fixture->other_parent_view;
    fixture->oldloc.node_access = &fixture->old_view;

    g_fs_handler_fixture = fixture;
    g_fs_handler_committed_tx_id = 0;
    fixture->hook_enabled = true;
    rtfsDirResolverSetReadBlockHook(fsHandlerTestReadBlockHook);
    rtfsDirInodeSetWriteBlockHook(fsHandlerTestDirWriteBlockHook);
    rtfsDirInodeSetJournalCommitHook(fsHandlerTestJournalCommitHook);
    nodeBlockCacheSetWriteBlockHook(fsHandlerTestNodeWriteBlockHook);
}

static void fsHandlerFixtureFini(FsHandlerFixture *fixture)
{
    free(fixture->oldloc.node_access_2);
    fixture->oldloc.node_access_2 = NULL;

    if (fixture->hook_enabled) {
        rtfsDirResolverSetReadBlockHook(NULL);
        rtfsDirInodeSetWriteBlockHook(NULL);
        rtfsDirInodeSetJournalCommitHook(NULL);
        nodeBlockCacheSetWriteBlockHook(NULL);
        fixture->hook_enabled = false;
        g_fs_handler_fixture = NULL;
        g_fs_handler_committed_tx_id = 0;
    }

    cowReclaimRegistryDestroy();
    fsHandlerTestFiniJournalEnvStub();
    superManagerDestroy(fixture->sp_manager);
    fixture->sp_manager = NULL;
    journalContainerDestroy(&fixture->journal);
    sitNatCacheDestroy(&fixture->sit_cache);
    sitNatCacheDestroy(&fixture->nat_cache);
    nodeBlockCacheDestroy(&fixture->node_cache);
}

static int fsHandlerLookupInParent(
    FsHandlerFixture *fixture,
    const char *name,
    RtfsDirLookupResult *result
)
{
    RtfsDirInodeBuildRequest request = {
        .ino = fixture->parent_view.ino,
        .mode = RTFS_DIR_BUILD_ON_DEMAND
    };
    RtfsDirInode *dir_inode = NULL;
    int ret;

    ret = rtfsDirInodeResolve(&fixture->fs_manager, NULL, &request, &dir_inode);
    TEST_ASSERT_EQUAL(0, ret);

    do {
        ret = rtfsDirInodeLookup(dir_inode, name, strlen(name), result);
        if (ret != ENOENT || rtfsDirInodeIsFullyLoaded(dir_inode)) {
            break;
        }

        ret = rtfsDirInodeResolveNext(&fixture->fs_manager, fixture->parent_view.ino, dir_inode);
        if (ret != 0) {
            rtfsDirInodePut(dir_inode);
            return ret;
        }
    } while (true);

    rtfsDirInodePut(dir_inode);
    return ret;
}

RTFS_TEST(FsHandlerCloneNode_WhenNodeAndNameExist_ShouldDuplicateContext)
{
    rtems_filesystem_location_info_t loc;
    RtfsRuntimeInodeView *original_view;
    char *original_name;

    memset(&loc, 0, sizeof(loc));
    original_view = rtfsRuntimeInodeViewCreate(3100, 2000, RTFS_FT_REG_FILE);
    TEST_ASSERT_NOT_NULL(original_view);
    original_name = strdup("alpha");
    TEST_ASSERT_NOT_NULL(original_name);

    loc.node_access = original_view;
    loc.node_access_2 = original_name;

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.clonenod_h(&loc));
    TEST_ASSERT_NOT_NULL(loc.node_access);
    TEST_ASSERT_NOT_EQUAL(original_view, loc.node_access);
    TEST_ASSERT_NOT_NULL(loc.node_access_2);
    TEST_ASSERT_NOT_EQUAL(original_name, loc.node_access_2);
    TEST_ASSERT_EQUAL_STRING("alpha", (const char *)loc.node_access_2);
    TEST_ASSERT_EQUAL_PTR(&rtfsFilehandlers, loc.handlers);

    free(original_view);
    free(original_name);
    r2fsFsHandler.freenod_h(&loc);
}

RTFS_TEST(FsHandlerInitialize_WhenRecoverSucceeds_ShouldRecoverBeforeSetupAndCreateRoot)
{
    FsHandlerInitFixture fixture;

    fsHandlerInitFixtureReset();
    fsHandlerInitFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, r2fsInitialize(&fixture.mt_entry, &fixture.dev));
    TEST_ASSERT_EQUAL_UINT32(1u, g_fs_handler_init_recover_call_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_fs_handler_init_super_read_count);
    TEST_ASSERT_NOT_NULL(fixture.mt_entry.fs_info);
    TEST_ASSERT_NOT_NULL(fixture.mt_entry.mt_fs_root->location.node_access);
    TEST_ASSERT_EQUAL_PTR(&rtfsDirhandlers, fixture.mt_entry.mt_fs_root->location.handlers);

    r2fsFsHandler.fsunmount_me_h(&fixture.mt_entry);
    fixture.mt_entry.mt_fs_root->location.node_access = NULL;
    fixture.mt_entry.mt_fs_root->location.node_access_2 = NULL;
    fixture.mt_entry.mt_fs_root->location.handlers = NULL;
    fsHandlerInitFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerInitialize_WhenRecoverFails_ShouldAbortBeforeSetup)
{
    FsHandlerInitFixture fixture;

    fsHandlerInitFixtureReset();
    fsHandlerInitFixtureInit(&fixture);
    g_fs_handler_init_recover_result = EIO;

    TEST_ASSERT_EQUAL(-1, r2fsInitialize(&fixture.mt_entry, &fixture.dev));
    TEST_ASSERT_EQUAL(EBUSY, errno);
    TEST_ASSERT_EQUAL_UINT32(1u, g_fs_handler_init_recover_call_count);
    TEST_ASSERT_EQUAL_UINT32(0u, g_fs_handler_init_super_read_count);
    TEST_ASSERT_NULL(fileSystemManagerGetInstance());
    TEST_ASSERT_NULL(fixture.mt_entry.fs_info);

    fsHandlerInitFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerCloneNode_WhenNodeViewIsMissing_ShouldReturnEINVAL)
{
    rtems_filesystem_location_info_t loc;

    memset(&loc, 0, sizeof(loc));

    TEST_ASSERT_EQUAL(-1, r2fsFsHandler.clonenod_h(&loc));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(FsHandlerFchmod_WhenTargetInodeExists_ShouldUpdateModeAndCommit)
{
    FsHandlerFixture fixture;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *inode;

    fsHandlerFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.fchmod_h(&fixture.oldloc, 0600));

    handle = nodeBlockCacheGet(&fixture.node_cache, 3001);
    TEST_ASSERT_NOT_NULL(handle.entry);
    inode = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    TEST_ASSERT_EQUAL_UINT32(0600u, inode->i.i_mode);
    TEST_ASSERT_EQUAL_UINT32(1024u, handle.entry->lpa);
    nodeBlockCacheEntryHandleDestroy(&handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerFchmod_WhenLocationIsInvalid_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(-1, r2fsFsHandler.fchmod_h(NULL, 0600));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(FsHandlerUtimens_WhenTargetInodeExists_ShouldUpdateTimesAndCommit)
{
    FsHandlerFixture fixture;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *inode;
    struct timespec times[2];

    fsHandlerFixtureInit(&fixture);
    times[0].tv_sec = 1111;
    times[0].tv_nsec = 222;
    times[1].tv_sec = 3333;
    times[1].tv_nsec = 444;

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.utimens_h(&fixture.oldloc, times));

    handle = nodeBlockCacheGet(&fixture.node_cache, 3001);
    TEST_ASSERT_NOT_NULL(handle.entry);
    inode = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    TEST_ASSERT_EQUAL_UINT64(1111u, inode->i.i_atime);
    TEST_ASSERT_EQUAL_UINT32(222u, inode->i.i_atime_nsec);
    TEST_ASSERT_EQUAL_UINT64(3333u, inode->i.i_mtime);
    TEST_ASSERT_EQUAL_UINT32(444u, inode->i.i_mtime_nsec);
    TEST_ASSERT_EQUAL_UINT32(1024u, handle.entry->lpa);
    nodeBlockCacheEntryHandleDestroy(&handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerUtimens_WhenTimesAreNull_ShouldReturnEINVAL)
{
    FsHandlerFixture fixture;

    fsHandlerFixtureInit(&fixture);
    TEST_ASSERT_EQUAL(-1, r2fsFsHandler.utimens_h(&fixture.oldloc, NULL));
    TEST_ASSERT_EQUAL(EINVAL, errno);
    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerStatvfs_WhenSuperBlockExists_ShouldReportCapacity)
{
    FsHandlerFixture fixture;
    struct statvfs stat;
    fsfilcnt_t expected_total_files;
    fsfilcnt_t expected_free_files;

    fsHandlerFixtureInit(&fixture);
    expected_total_files =
        (fsfilcnt_t)fixture.super_block.segment_count_nat *
        (fsfilcnt_t)BLOCK_PER_SEGMENT *
        (fsfilcnt_t)NAT_ENTRY_PER_BLOCK - 1u;
    expected_free_files = 2u;

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.statvfs_h(&fixture.parentloc, &stat));
    TEST_ASSERT_EQUAL_UINT32(4096u, (uint32_t)stat.f_bsize);
    TEST_ASSERT_EQUAL_UINT32(4096u, (uint32_t)stat.f_frsize);
    TEST_ASSERT_EQUAL_UINT32(RTFS_NAME_LEN, (uint32_t)stat.f_namemax);
    TEST_ASSERT_EQUAL_UINT32(fixture.super_block.block_count, (uint32_t)stat.f_blocks);
    TEST_ASSERT_EQUAL_UINT32(
        fixture.super_block.free_segment_count * BLOCK_PER_SEGMENT,
        (uint32_t)stat.f_bfree
    );
    TEST_ASSERT_EQUAL_UINT32((uint32_t)stat.f_bfree, (uint32_t)stat.f_bavail);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)expected_total_files, (uint32_t)stat.f_files);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)expected_free_files, (uint32_t)stat.f_ffree);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)expected_free_files, (uint32_t)stat.f_favail);
    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerStatvfs_WhenArgumentsAreNull_ShouldReturnEINVAL)
{
    struct statvfs stat;

    TEST_ASSERT_EQUAL(-1, r2fsFsHandler.statvfs_h(NULL, &stat));
    TEST_ASSERT_EQUAL(EINVAL, errno);
    TEST_ASSERT_EQUAL(-1, r2fsFsHandler.statvfs_h((const rtems_filesystem_location_info_t *)&stat, NULL));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(FsHandlerStatvfs_WhenMknodConsumesNid_ShouldReduceFreeFileSlots)
{
    FsHandlerFixture fixture;
    struct statvfs before_stat;
    struct statvfs after_stat;

    fsHandlerFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.statvfs_h(&fixture.parentloc, &before_stat));
    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "omega",
            strlen("omega"),
            S_IFREG | 0644,
            0
        )
    );
    TEST_ASSERT_EQUAL(0, r2fsFsHandler.statvfs_h(&fixture.parentloc, &after_stat));

    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_stat.f_files, (uint32_t)after_stat.f_files);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(before_stat.f_ffree - 1u), (uint32_t)after_stat.f_ffree);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)after_stat.f_ffree, (uint32_t)after_stat.f_favail);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerStatvfs_WhenRmnodReclaimsNidAfterTxComplete_ShouldRestoreFreeFileSlots)
{
    FsHandlerFixture fixture;
    struct statvfs before_stat;
    struct statvfs after_unlink_stat;
    struct statvfs after_reclaim_stat;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.statvfs_h(&fixture.parentloc, &before_stat));
    TEST_ASSERT_EQUAL(0, r2fsFsHandler.rmnod_h(&fixture.parentloc, &fixture.oldloc));
    TEST_ASSERT_EQUAL(0, r2fsFsHandler.statvfs_h(&fixture.parentloc, &after_unlink_stat));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_stat.f_ffree, (uint32_t)after_unlink_stat.f_ffree);

    {
        uint64_t tx_id = fsHandlerGetLatestObservedTxId();
        TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
        cowReclaimRegistryOnTxComplete(tx_id);
    }
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL(0, r2fsFsHandler.statvfs_h(&fixture.parentloc, &after_reclaim_stat));

    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_stat.f_files, (uint32_t)after_reclaim_stat.f_files);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(before_stat.f_ffree + 1u), (uint32_t)after_reclaim_stat.f_ffree);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)after_reclaim_stat.f_ffree, (uint32_t)after_reclaim_stat.f_favail);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerMknod_WhenTargetDoesNotExist_ShouldCreateEntryAndInode)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *created_inode;

    fsHandlerFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "omega",
            strlen("omega"),
            S_IFREG | 0644,
            0
        )
    );

    TEST_ASSERT_EQUAL(0, fsHandlerLookupInParent(&fixture, "omega", &result));
    TEST_ASSERT_EQUAL_UINT32(6000u, (uint32_t)result.inode_view.ino);
    TEST_ASSERT_EQUAL_UINT32(RTFS_FT_REG_FILE, (uint32_t)result.inode_view.file_type);
    TEST_ASSERT_EQUAL(0, fsHandlerLookupInParent(&fixture, "alpha", &result));
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);
    TEST_ASSERT_EQUAL_UINT32(6001u, fixture.super_block.next_free_nid);

    handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(handle.entry);
    created_inode = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    TEST_ASSERT_EQUAL_UINT32(6000u, created_inode->footer.ino);
    TEST_ASSERT_EQUAL_UINT32(6000u, created_inode->footer.nid);
    TEST_ASSERT_EQUAL_UINT32(2000u, created_inode->i.i_pino);
    TEST_ASSERT_EQUAL_UINT32(RTFS_FT_REG_FILE, created_inode->i.i_type);
    TEST_ASSERT_EQUAL_UINT32(0644u, created_inode->i.i_mode);
    TEST_ASSERT_EQUAL_UINT32(5u, created_inode->i.i_namelen);
    TEST_ASSERT_EQUAL_MEMORY("omega", created_inode->i.i_name, 5);
    nodeBlockCacheEntryHandleDestroy(&handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerMknod_WhenCreatingDirectory_ShouldInitializeDirectoryNlinks)
{
    FsHandlerFixture fixture;
    NodeBlockCacheEntryHandle handle;
    NodeBlockCacheEntryHandle parent_handle;
    struct RtfsNode *created_inode;
    struct RtfsNode *parent_inode;

    fsHandlerFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "omega",
            strlen("omega"),
            S_IFDIR | 0755,
            0
        )
    );

    handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(handle.entry);
    created_inode = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    TEST_ASSERT_EQUAL_UINT32(RTFS_FT_DIR, created_inode->i.i_type);
    TEST_ASSERT_EQUAL_UINT32(2u, created_inode->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&handle);

    parent_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(parent_handle.entry);
    parent_inode = nodeBlockCacheEntryGetNodeBlockPtr(parent_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(3u, parent_inode->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&parent_handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerMknod_WhenTargetAlreadyExists_ShouldReturnEEXIST)
{
    FsHandlerFixture fixture;

    fsHandlerFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(
        -1,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "alpha",
            strlen("alpha"),
            S_IFREG | 0644,
            0
        )
    );
    TEST_ASSERT_EQUAL(EEXIST, errno);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenTargetDoesNotExist_ShouldMoveEntryAndUpdateCachedName)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.parentloc,
            "omega",
            strlen("omega")
        )
    );

    TEST_ASSERT_EQUAL_STRING("omega", (const char *)fixture.oldloc.node_access_2);
    TEST_ASSERT_EQUAL(ENOENT, fsHandlerLookupInParent(&fixture, "alpha", &result));
    TEST_ASSERT_EQUAL(0, fsHandlerLookupInParent(&fixture, "omega", &result));
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);
    TEST_ASSERT_EQUAL_UINT32(RTFS_FT_REG_FILE, (uint32_t)result.inode_view.file_type);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenRegularTargetExists_ShouldNotReturnEexistAndShouldReplaceIt)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "omega",
            strlen("omega"),
            S_IFREG | 0600,
            0
        )
    );

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.parentloc,
            "omega",
            strlen("omega")
        )
    );

    TEST_ASSERT_EQUAL_STRING("omega", (const char *)fixture.oldloc.node_access_2);
    TEST_ASSERT_EQUAL(ENOENT, fsHandlerLookupInParent(&fixture, "alpha", &result));
    TEST_ASSERT_EQUAL(0, fsHandlerLookupInParent(&fixture, "omega", &result));
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenRegularTargetExists_ShouldReplaceIt)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle old_target_handle;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "omega",
            strlen("omega"),
            S_IFREG | 0600,
            0
        )
    );

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.parentloc,
            "omega",
            strlen("omega")
        )
    );

    TEST_ASSERT_EQUAL_STRING("omega", (const char *)fixture.oldloc.node_access_2);
    TEST_ASSERT_EQUAL(ENOENT, fsHandlerLookupInParent(&fixture, "alpha", &result));
    TEST_ASSERT_EQUAL(0, fsHandlerLookupInParent(&fixture, "omega", &result));
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);

    old_target_handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(old_target_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(0u, nodeBlockCacheEntryGetNodeBlockPtr(old_target_handle.entry)->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&old_target_handle);

    {
        uint64_t tx_id = fsHandlerGetLatestObservedTxId();
        TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
        cowReclaimRegistryOnTxComplete(tx_id);
    }
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    old_target_handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&old_target_handle));
    nodeBlockCacheEntryHandleDestroy(&old_target_handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenOldAndNewNamesMatch_ShouldReturnSuccessWithoutMutation)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.parentloc,
            "alpha",
            strlen("alpha")
        )
    );

    TEST_ASSERT_EQUAL_STRING("alpha", (const char *)fixture.oldloc.node_access_2);
    TEST_ASSERT_EQUAL(0, fsHandlerLookupInParent(&fixture, "alpha", &result));
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenDirectoryTargetExistsButIsNotEmpty_ShouldReturnEnotempty)
{
    FsHandlerFixture fixture;
    rtems_filesystem_location_info_t target_loc;
    RtfsRuntimeInodeView target_view;
    struct RtfsNode target_dir_node;
    BlockBuffer node_buffer;
    NodeBlockCacheEntryHandle handle;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);
    fixture.old_view.file_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_nlink = 2;
    fixture.target_inode.i.i_dentry_num = 0;
    fixture.parent_block.dentry[0].file_type = RTFS_FT_DIR;
    fsHandlerFixtureSyncCachedNode(&fixture, 3001, &fixture.target_inode);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "omega",
            strlen("omega"),
            S_IFDIR | 0755,
            0
        )
    );

    handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(handle.entry);
    target_dir_node = *nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    target_dir_node.i.i_type = RTFS_FT_DIR;
    target_dir_node.i.i_nlink = 2;
    target_dir_node.i.i_dentry_num = 1;
    nodeBlockCacheEntryHandleDestroy(&handle);

    blockBufferInit(&node_buffer);
    blockBufferCopyContentFromBuf(&node_buffer, (const char *)&target_dir_node);
    handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(handle.entry);
    blockBufferCopyContentFromBuf(nodeBlockCacheEntryGetNodeBuffer(handle.entry), (const char *)&target_dir_node);
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&node_buffer);

    memset(&target_loc, 0, sizeof(target_loc));
    rtfsRuntimeInodeViewInit(&target_view, 6000, 2000, RTFS_FT_DIR);
    target_loc.mt_entry = &fixture.mt_entry;
    target_loc.node_access = &target_view;

    TEST_ASSERT_EQUAL(
        -1,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.parentloc,
            "omega",
            strlen("omega")
        )
    );
    TEST_ASSERT_EQUAL(ENOTEMPTY, errno);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenTargetTypeConflicts_ShouldReturnTypeError)
{
    FsHandlerFixture fixture;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "omega",
            strlen("omega"),
            S_IFDIR | 0755,
            0
        )
    );

    TEST_ASSERT_EQUAL(
        -1,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.parentloc,
            "omega",
            strlen("omega")
        )
    );
    TEST_ASSERT_EQUAL(EISDIR, errno);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenDirectoryReplacesRegularTarget_ShouldReturnEnotdir)
{
    FsHandlerFixture fixture;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);
    fixture.old_view.file_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_nlink = 2;
    fixture.parent_block.dentry[0].file_type = RTFS_FT_DIR;
    fsHandlerFixtureSyncCachedNode(&fixture, 3001, &fixture.target_inode);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "omega",
            strlen("omega"),
            S_IFREG | 0600,
            0
        )
    );

    TEST_ASSERT_EQUAL(
        -1,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.parentloc,
            "omega",
            strlen("omega")
        )
    );
    TEST_ASSERT_EQUAL(ENOTDIR, errno);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenEmptyDirectoryTargetExists_ShouldReplaceItAndDecrementParentNlink)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle old_target_handle;
    NodeBlockCacheEntryHandle parent_handle;
    struct RtfsNode *parent_inode;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);
    fixture.old_view.file_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_nlink = 2;
    fixture.parent_block.dentry[0].file_type = RTFS_FT_DIR;
    fsHandlerFixtureSyncCachedNode(&fixture, 3001, &fixture.target_inode);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.parentloc,
            "omega",
            strlen("omega"),
            S_IFDIR | 0755,
            0
        )
    );

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.parentloc,
            "omega",
            strlen("omega")
        )
    );

    TEST_ASSERT_EQUAL_STRING("omega", (const char *)fixture.oldloc.node_access_2);
    TEST_ASSERT_EQUAL(ENOENT, fsHandlerLookupInParent(&fixture, "alpha", &result));
    TEST_ASSERT_EQUAL(0, fsHandlerLookupInParent(&fixture, "omega", &result));
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);
    TEST_ASSERT_EQUAL_UINT32(RTFS_FT_DIR, (uint32_t)result.inode_view.file_type);

    old_target_handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(old_target_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(0u, nodeBlockCacheEntryGetNodeBlockPtr(old_target_handle.entry)->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&old_target_handle);

    parent_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(parent_handle.entry);
    parent_inode = nodeBlockCacheEntryGetNodeBlockPtr(parent_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(2u, parent_inode->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&parent_handle);

    {
        uint64_t tx_id = fsHandlerGetLatestObservedTxId();
        TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
        cowReclaimRegistryOnTxComplete(tx_id);
    }
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    old_target_handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&old_target_handle));
    nodeBlockCacheEntryHandleDestroy(&old_target_handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenRegularTargetMovesAcrossParentsWithoutReplacement_ShouldSucceed)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *inode;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.other_parentloc,
            "omega",
            strlen("omega")
        )
    );

    TEST_ASSERT_EQUAL_STRING("omega", (const char *)fixture.oldloc.node_access_2);
    TEST_ASSERT_EQUAL(ENOENT, fsHandlerLookupInParent(&fixture, "alpha", &result));

    {
        RtfsDirInodeBuildRequest request = {
            .ino = fixture.other_parent_view.ino,
            .mode = RTFS_DIR_BUILD_ON_DEMAND
        };
        RtfsDirInode *dir_inode = NULL;

        TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
        TEST_ASSERT_EQUAL(
            0,
            rtfsDirInodeLookup(dir_inode, "omega", strlen("omega"), &result)
        );
        rtfsDirInodePut(dir_inode);
    }
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);

    handle = nodeBlockCacheGet(&fixture.node_cache, 3001);
    TEST_ASSERT_NOT_NULL(handle.entry);
    inode = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    TEST_ASSERT_EQUAL_UINT32(2001u, inode->i.i_pino);
    TEST_ASSERT_EQUAL_UINT32(5u, inode->i.i_namelen);
    TEST_ASSERT_EQUAL_MEMORY("omega", inode->i.i_name, 5);
    nodeBlockCacheEntryHandleDestroy(&handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenDirectoryMovesAcrossParentsWithoutReplacement_ShouldSucceedAndUpdateParentNlinks)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle source_handle;
    NodeBlockCacheEntryHandle old_parent_handle;
    NodeBlockCacheEntryHandle new_parent_handle;
    struct RtfsNode *source_inode;
    struct RtfsNode *old_parent_inode;
    struct RtfsNode *new_parent_inode;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);
    fixture.old_view.file_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_nlink = 2;
    fixture.parent_block.dentry[0].file_type = RTFS_FT_DIR;
    fsHandlerFixtureSyncCachedNode(&fixture, 3001, &fixture.target_inode);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.other_parentloc,
            "omega",
            strlen("omega")
        )
    );

    TEST_ASSERT_EQUAL_STRING("omega", (const char *)fixture.oldloc.node_access_2);
    TEST_ASSERT_EQUAL(ENOENT, fsHandlerLookupInParent(&fixture, "alpha", &result));

    {
        RtfsDirInodeBuildRequest request = {
            .ino = fixture.other_parent_view.ino,
            .mode = RTFS_DIR_BUILD_ON_DEMAND
        };
        RtfsDirInode *dir_inode = NULL;

        TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
        TEST_ASSERT_EQUAL(
            0,
            rtfsDirInodeLookup(dir_inode, "omega", strlen("omega"), &result)
        );
        rtfsDirInodePut(dir_inode);
    }
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);
    TEST_ASSERT_EQUAL_UINT32(RTFS_FT_DIR, (uint32_t)result.inode_view.file_type);

    source_handle = nodeBlockCacheGet(&fixture.node_cache, 3001);
    TEST_ASSERT_NOT_NULL(source_handle.entry);
    source_inode = nodeBlockCacheEntryGetNodeBlockPtr(source_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(2001u, source_inode->i.i_pino);
    TEST_ASSERT_EQUAL_UINT32(5u, source_inode->i.i_namelen);
    TEST_ASSERT_EQUAL_MEMORY("omega", source_inode->i.i_name, 5);
    nodeBlockCacheEntryHandleDestroy(&source_handle);

    old_parent_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(old_parent_handle.entry);
    old_parent_inode = nodeBlockCacheEntryGetNodeBlockPtr(old_parent_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(1u, old_parent_inode->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&old_parent_handle);

    new_parent_handle = nodeBlockCacheGet(&fixture.node_cache, 2001);
    TEST_ASSERT_NOT_NULL(new_parent_handle.entry);
    new_parent_inode = nodeBlockCacheEntryGetNodeBlockPtr(new_parent_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(3u, new_parent_inode->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&new_parent_handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenRegularTargetMovesAcrossParentsWithReplacement_ShouldReplaceIt)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle source_handle;
    NodeBlockCacheEntryHandle old_target_handle;
    struct RtfsNode *source_inode;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.other_parentloc,
            "omega",
            strlen("omega"),
            S_IFREG | 0600,
            0
        )
    );

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.other_parentloc,
            "omega",
            strlen("omega")
        )
    );

    TEST_ASSERT_EQUAL_STRING("omega", (const char *)fixture.oldloc.node_access_2);
    TEST_ASSERT_EQUAL(ENOENT, fsHandlerLookupInParent(&fixture, "alpha", &result));

    {
        RtfsDirInodeBuildRequest request = {
            .ino = fixture.other_parent_view.ino,
            .mode = RTFS_DIR_BUILD_ON_DEMAND
        };
        RtfsDirInode *dir_inode = NULL;

        TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
        TEST_ASSERT_EQUAL(
            0,
            rtfsDirInodeLookup(dir_inode, "omega", strlen("omega"), &result)
        );
        rtfsDirInodePut(dir_inode);
    }
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);

    source_handle = nodeBlockCacheGet(&fixture.node_cache, 3001);
    TEST_ASSERT_NOT_NULL(source_handle.entry);
    source_inode = nodeBlockCacheEntryGetNodeBlockPtr(source_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(2001u, source_inode->i.i_pino);
    TEST_ASSERT_EQUAL_UINT32(5u, source_inode->i.i_namelen);
    TEST_ASSERT_EQUAL_MEMORY("omega", source_inode->i.i_name, 5);
    nodeBlockCacheEntryHandleDestroy(&source_handle);

    old_target_handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(old_target_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(0u, nodeBlockCacheEntryGetNodeBlockPtr(old_target_handle.entry)->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&old_target_handle);

    {
        uint64_t tx_id = fsHandlerGetLatestObservedTxId();
        TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
        cowReclaimRegistryOnTxComplete(tx_id);
    }
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    old_target_handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&old_target_handle));
    nodeBlockCacheEntryHandleDestroy(&old_target_handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRename_WhenDirectoryMovesAcrossParentsWithReplacement_ShouldReplaceItAndUpdateParentNlinks)
{
    FsHandlerFixture fixture;
    RtfsDirLookupResult result;
    NodeBlockCacheEntryHandle source_handle;
    NodeBlockCacheEntryHandle old_parent_handle;
    NodeBlockCacheEntryHandle new_parent_handle;
    NodeBlockCacheEntryHandle old_target_handle;
    struct RtfsNode *source_inode;
    struct RtfsNode *old_parent_inode;
    struct RtfsNode *new_parent_inode;
    struct RtfsNode target_dir_node;
    BlockBuffer node_buffer;
    NodeBlockCacheEntryHandle handle;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);
    fixture.old_view.file_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_nlink = 2;
    fixture.parent_block.dentry[0].file_type = RTFS_FT_DIR;
    fsHandlerFixtureSyncCachedNode(&fixture, 3001, &fixture.target_inode);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.mknod_h(
            &fixture.other_parentloc,
            "omega",
            strlen("omega"),
            S_IFDIR | 0755,
            0
        )
    );

    handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(handle.entry);
    target_dir_node = *nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    target_dir_node.i.i_type = RTFS_FT_DIR;
    target_dir_node.i.i_nlink = 2;
    target_dir_node.i.i_dentry_num = 0;
    nodeBlockCacheEntryHandleDestroy(&handle);

    blockBufferInit(&node_buffer);
    blockBufferCopyContentFromBuf(&node_buffer, (const char *)&target_dir_node);
    handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(handle.entry);
    blockBufferCopyContentFromBuf(nodeBlockCacheEntryGetNodeBuffer(handle.entry), (const char *)&target_dir_node);
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&node_buffer);

    TEST_ASSERT_EQUAL(
        0,
        r2fsFsHandler.rename_h(
            &fixture.parentloc,
            &fixture.oldloc,
            &fixture.other_parentloc,
            "omega",
            strlen("omega")
        )
    );

    TEST_ASSERT_EQUAL_STRING("omega", (const char *)fixture.oldloc.node_access_2);
    TEST_ASSERT_EQUAL(ENOENT, fsHandlerLookupInParent(&fixture, "alpha", &result));

    {
        RtfsDirInodeBuildRequest request = {
            .ino = fixture.other_parent_view.ino,
            .mode = RTFS_DIR_BUILD_ON_DEMAND
        };
        RtfsDirInode *dir_inode = NULL;

        TEST_ASSERT_EQUAL(0, rtfsDirInodeResolve(&fixture.fs_manager, NULL, &request, &dir_inode));
        TEST_ASSERT_EQUAL(
            0,
            rtfsDirInodeLookup(dir_inode, "omega", strlen("omega"), &result)
        );
        rtfsDirInodePut(dir_inode);
    }
    TEST_ASSERT_EQUAL_UINT32(3001u, (uint32_t)result.inode_view.ino);
    TEST_ASSERT_EQUAL_UINT32(RTFS_FT_DIR, (uint32_t)result.inode_view.file_type);

    source_handle = nodeBlockCacheGet(&fixture.node_cache, 3001);
    TEST_ASSERT_NOT_NULL(source_handle.entry);
    source_inode = nodeBlockCacheEntryGetNodeBlockPtr(source_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(2001u, source_inode->i.i_pino);
    TEST_ASSERT_EQUAL_UINT32(5u, source_inode->i.i_namelen);
    TEST_ASSERT_EQUAL_MEMORY("omega", source_inode->i.i_name, 5);
    nodeBlockCacheEntryHandleDestroy(&source_handle);

    old_parent_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(old_parent_handle.entry);
    old_parent_inode = nodeBlockCacheEntryGetNodeBlockPtr(old_parent_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(1u, old_parent_inode->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&old_parent_handle);

    new_parent_handle = nodeBlockCacheGet(&fixture.node_cache, 2001);
    TEST_ASSERT_NOT_NULL(new_parent_handle.entry);
    new_parent_inode = nodeBlockCacheEntryGetNodeBlockPtr(new_parent_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(3u, new_parent_inode->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&new_parent_handle);

    old_target_handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_NOT_NULL(old_target_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(0u, nodeBlockCacheEntryGetNodeBlockPtr(old_target_handle.entry)->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&old_target_handle);

    {
        uint64_t tx_id = fsHandlerGetLatestObservedTxId();
        TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
        cowReclaimRegistryOnTxComplete(tx_id);
    }
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    old_target_handle = nodeBlockCacheGet(&fixture.node_cache, 6000);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&old_target_handle));
    nodeBlockCacheEntryHandleDestroy(&old_target_handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRmnod_WhenTargetNameIsUnavailable_ShouldReturnEINVAL)
{
    FsHandlerFixture fixture;

    fsHandlerFixtureInit(&fixture);

    TEST_ASSERT_EQUAL(-1, r2fsFsHandler.rmnod_h(&fixture.parentloc, &fixture.oldloc));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRmnod_WhenTargetIsEmptyAndRemovable_ShouldUnlinkThenReclaimAfterTxComplete)
{
    FsHandlerFixture fixture;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *target_inode;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.rmnod_h(&fixture.parentloc, &fixture.oldloc));
    {
        RtfsDirLookupResult result;
        TEST_ASSERT_EQUAL(ENOENT, fsHandlerLookupInParent(&fixture, "alpha", &result));
    }
    handle = nodeBlockCacheGet(&fixture.node_cache, 3001);
    TEST_ASSERT_NOT_NULL(handle.entry);
    target_inode = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    TEST_ASSERT_EQUAL_UINT32(0u, target_inode->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&handle);
    {
        uint64_t tx_id = fsHandlerGetLatestObservedTxId();
        TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
        cowReclaimRegistryOnTxComplete(tx_id);
    }
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    handle = nodeBlockCacheGet(&fixture.node_cache, 3001);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&handle));
    nodeBlockCacheEntryHandleDestroy(&handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRmnod_WhenTargetIsEmptyDirectory_ShouldDecrementParentNlinkInSameTransaction)
{
    FsHandlerFixture fixture;
    NodeBlockCacheEntryHandle parent_handle;
    struct RtfsNode *parent_inode;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);
    fixture.old_view.file_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_nlink = 2;
    fixture.parent_block.dentry[0].file_type = RTFS_FT_DIR;
    fsHandlerFixtureSyncCachedNode(&fixture, 3001, &fixture.target_inode);

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.rmnod_h(&fixture.parentloc, &fixture.oldloc));

    parent_handle = nodeBlockCacheGet(&fixture.node_cache, 2000);
    TEST_ASSERT_NOT_NULL(parent_handle.entry);
    parent_inode = nodeBlockCacheEntryGetNodeBlockPtr(parent_handle.entry);
    TEST_ASSERT_EQUAL_UINT32(1u, parent_inode->i.i_nlink);
    nodeBlockCacheEntryHandleDestroy(&parent_handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRmnod_WhenRegularFileUsesDirectAndDirectNodeMappings_ShouldDeferAllReferencedLpasUntilTxComplete)
{
    FsHandlerFixture fixture;
    NodeBlockCacheEntryHandle child_node_handle;
    NodeBlockCacheEntryHandle lookup_handle;
    struct RtfsNode child_direct_node;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    fixture.target_inode.i.i_size = (uint64_t)(DEF_ADDRS_PER_INODE + 1u) * BLOCK_BUFFER_SIZE;
    fixture.target_inode.i.i_addr[0] = 31;
    fixture.target_inode.i.i_nid[0] = 3002;
    fixture.target_inode.i.i_nlink = 1;
    fsHandlerFixtureMarkSitValid(&fixture, 31);

    memset(&child_direct_node, 0, sizeof(child_direct_node));
    child_direct_node.dn.addr[0] = 32;
    child_direct_node.footer.nid = 3002;
    child_direct_node.footer.ino = 3001;
    child_direct_node.footer.offset = DEF_ADDRS_PER_INODE + 1u;
    fsHandlerFixtureMarkSitValid(&fixture, 32);
    fsHandlerFixtureSetNatEntry(&fixture, 3002, 3001, 40);
    fsHandlerFixtureMarkSitValid(&fixture, 40);

    fsHandlerFixtureSyncCachedNode(&fixture, 3001, &fixture.target_inode);
    {
        BlockBuffer node_buffer;
        blockBufferInit(&node_buffer);
        blockBufferCopyContentFromBuf(&node_buffer, (const char *)&child_direct_node);
        child_node_handle = nodeBlockCacheAdd(&fixture.node_cache, &node_buffer, 3002, 3001, 40);
        nodeBlockCacheEntryHandleDestroy(&child_node_handle);
        blockBufferDestroy(&node_buffer);
    }

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.rmnod_h(&fixture.parentloc, &fixture.oldloc));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(&fixture, 31));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(&fixture, 32));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(&fixture, 40));

    {
        uint64_t tx_id = fsHandlerGetLatestObservedTxId();
        TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
        cowReclaimRegistryOnTxComplete(tx_id);
    }
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(&fixture, 31));
    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(&fixture, 32));
    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(&fixture, 40));
    lookup_handle = nodeBlockCacheGet(&fixture.node_cache, 3002);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&lookup_handle));
    nodeBlockCacheEntryHandleDestroy(&lookup_handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRmnod_WhenRegularFileUsesSingleIndirectMappings_ShouldDeferAllReferencedLpasUntilTxComplete)
{
    FsHandlerFixture fixture;
    NodeBlockCacheEntryHandle indirect_handle;
    NodeBlockCacheEntryHandle direct_handle;
    NodeBlockCacheEntryHandle lookup_handle;
    struct RtfsNode indirect_node;
    struct RtfsNode direct_node;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);

    fixture.target_inode.i.i_size =
        (uint64_t)(DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK + 1u) * BLOCK_BUFFER_SIZE;
    fixture.target_inode.i.i_addr[0] = 31;
    fixture.target_inode.i.i_nid[2] = 3003;
    fixture.target_inode.i.i_nlink = 1;
    fsHandlerFixtureMarkSitValid(&fixture, 31);

    memset(&indirect_node, 0, sizeof(indirect_node));
    indirect_node.in.nid[0] = 3004;
    indirect_node.footer.nid = 3003;
    indirect_node.footer.ino = 3001;
    indirect_node.footer.offset = NODE_IND1_BLOCK;
    fsHandlerFixtureSetNatEntry(&fixture, 3003, 3001, 41);
    fsHandlerFixtureMarkSitValid(&fixture, 41);

    memset(&direct_node, 0, sizeof(direct_node));
    direct_node.dn.addr[0] = 32;
    direct_node.footer.nid = 3004;
    direct_node.footer.ino = 3001;
    direct_node.footer.offset = DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK;
    fsHandlerFixtureSetNatEntry(&fixture, 3004, 3001, 42);
    fsHandlerFixtureMarkSitValid(&fixture, 32);
    fsHandlerFixtureMarkSitValid(&fixture, 42);

    fsHandlerFixtureSyncCachedNode(&fixture, 3001, &fixture.target_inode);
    {
        BlockBuffer node_buffer;

        blockBufferInit(&node_buffer);
        blockBufferCopyContentFromBuf(&node_buffer, (const char *)&indirect_node);
        indirect_handle = nodeBlockCacheAdd(&fixture.node_cache, &node_buffer, 3003, 3001, 41);
        nodeBlockCacheEntryHandleDestroy(&indirect_handle);

        blockBufferCopyContentFromBuf(&node_buffer, (const char *)&direct_node);
        direct_handle = nodeBlockCacheAdd(&fixture.node_cache, &node_buffer, 3004, 3003, 42);
        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        blockBufferDestroy(&node_buffer);
    }

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.rmnod_h(&fixture.parentloc, &fixture.oldloc));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(&fixture, 31));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(&fixture, 32));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(&fixture, 41));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(&fixture, 42));

    {
        uint64_t tx_id = fsHandlerGetLatestObservedTxId();
        TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
        cowReclaimRegistryOnTxComplete(tx_id);
    }
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(&fixture, 31));
    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(&fixture, 32));
    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(&fixture, 41));
    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(&fixture, 42));

    lookup_handle = nodeBlockCacheGet(&fixture.node_cache, 3003);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&lookup_handle));
    nodeBlockCacheEntryHandleDestroy(&lookup_handle);
    lookup_handle = nodeBlockCacheGet(&fixture.node_cache, 3004);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&lookup_handle));
    nodeBlockCacheEntryHandleDestroy(&lookup_handle);

    fsHandlerFixtureFini(&fixture);
}

RTFS_TEST(FsHandlerRmnod_DoubleIndirect_ShouldDeferReclaimUntilTxComplete)
{
    FsHandlerFixture *fixture;
    NodeBlockCacheEntryHandle dind_handle;
    NodeBlockCacheEntryHandle indirect_handle;
    NodeBlockCacheEntryHandle direct_handle;
    NodeBlockCacheEntryHandle lookup_handle;
    struct RtfsNode *dind_node;
    struct RtfsNode *indirect_node;
    struct RtfsNode *direct_node;

    fixture = (FsHandlerFixture *)malloc(sizeof(*fixture));
    TEST_ASSERT_NOT_NULL(fixture);
    fsHandlerFixtureInit(fixture);
    fixture->oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture->oldloc.node_access_2);
    dind_node = (struct RtfsNode *)malloc(sizeof(*dind_node));
    indirect_node = (struct RtfsNode *)malloc(sizeof(*indirect_node));
    direct_node = (struct RtfsNode *)malloc(sizeof(*direct_node));
    TEST_ASSERT_NOT_NULL(dind_node);
    TEST_ASSERT_NOT_NULL(indirect_node);
    TEST_ASSERT_NOT_NULL(direct_node);

    fixture->target_inode.i.i_size =
        (uint64_t)(
            DEF_ADDRS_PER_INODE +
            2U * DEF_ADDRS_PER_BLOCK +
            2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK +
            1u
        ) * BLOCK_BUFFER_SIZE;
    fixture->target_inode.i.i_addr[0] = 31;
    fixture->target_inode.i.i_nid[4] = 3005;
    fixture->target_inode.i.i_nlink = 1;
    fsHandlerFixtureMarkSitValid(fixture, 31);

    memset(dind_node, 0, sizeof(*dind_node));
    dind_node->in.nid[0] = 3006;
    dind_node->footer.nid = 3005;
    dind_node->footer.ino = 3001;
    dind_node->footer.offset = NODE_DIND_BLOCK;
    fsHandlerFixtureSetNatEntry(fixture, 3005, 3001, 43);
    fsHandlerFixtureMarkSitValid(fixture, 43);

    memset(indirect_node, 0, sizeof(*indirect_node));
    indirect_node->in.nid[0] = 3007;
    indirect_node->footer.nid = 3006;
    indirect_node->footer.ino = 3001;
    indirect_node->footer.offset = NODE_DIND_BLOCK + 1u;
    fsHandlerFixtureSetNatEntry(fixture, 3006, 3001, 44);
    fsHandlerFixtureMarkSitValid(fixture, 44);

    memset(direct_node, 0, sizeof(*direct_node));
    direct_node->dn.addr[0] = 32;
    direct_node->footer.nid = 3007;
    direct_node->footer.ino = 3001;
    direct_node->footer.offset =
        DEF_ADDRS_PER_INODE +
        2U * DEF_ADDRS_PER_BLOCK +
        2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
    fsHandlerFixtureSetNatEntry(fixture, 3007, 3001, 45);
    fsHandlerFixtureMarkSitValid(fixture, 32);
    fsHandlerFixtureMarkSitValid(fixture, 45);

    fsHandlerFixtureSyncCachedNode(fixture, 3001, &fixture->target_inode);
    {
        BlockBuffer node_buffer;

        blockBufferInit(&node_buffer);
        blockBufferCopyContentFromBuf(&node_buffer, (const char *)dind_node);
        dind_handle = nodeBlockCacheAdd(&fixture->node_cache, &node_buffer, 3005, 3001, 43);
        nodeBlockCacheEntryHandleDestroy(&dind_handle);

        blockBufferCopyContentFromBuf(&node_buffer, (const char *)indirect_node);
        indirect_handle = nodeBlockCacheAdd(&fixture->node_cache, &node_buffer, 3006, 3005, 44);
        nodeBlockCacheEntryHandleDestroy(&indirect_handle);

        blockBufferCopyContentFromBuf(&node_buffer, (const char *)direct_node);
        direct_handle = nodeBlockCacheAdd(&fixture->node_cache, &node_buffer, 3007, 3006, 45);
        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        blockBufferDestroy(&node_buffer);
    }

    TEST_ASSERT_EQUAL(0, r2fsFsHandler.rmnod_h(&fixture->parentloc, &fixture->oldloc));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(fixture, 31));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(fixture, 32));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(fixture, 43));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(fixture, 44));
    TEST_ASSERT_TRUE(fsHandlerFixtureIsSitBitValid(fixture, 45));

    {
        uint64_t tx_id = fsHandlerGetLatestObservedTxId();
        TEST_ASSERT_NOT_EQUAL_UINT64(0u, tx_id);
        cowReclaimRegistryOnTxComplete(tx_id);
    }
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());

    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(fixture, 31));
    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(fixture, 32));
    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(fixture, 43));
    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(fixture, 44));
    TEST_ASSERT_FALSE(fsHandlerFixtureIsSitBitValid(fixture, 45));

    lookup_handle = nodeBlockCacheGet(&fixture->node_cache, 3005);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&lookup_handle));
    nodeBlockCacheEntryHandleDestroy(&lookup_handle);
    lookup_handle = nodeBlockCacheGet(&fixture->node_cache, 3006);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&lookup_handle));
    nodeBlockCacheEntryHandleDestroy(&lookup_handle);
    lookup_handle = nodeBlockCacheGet(&fixture->node_cache, 3007);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&lookup_handle));
    nodeBlockCacheEntryHandleDestroy(&lookup_handle);
    free(dind_node);
    free(indirect_node);
    free(direct_node);

    fsHandlerFixtureFini(fixture);
    free(fixture);
}

RTFS_TEST(FsHandlerRmnod_WhenTargetDirectoryIsNotEmpty_ShouldReturnEnotempty)
{
    FsHandlerFixture fixture;

    fsHandlerFixtureInit(&fixture);
    fixture.oldloc.node_access_2 = strdup("alpha");
    TEST_ASSERT_NOT_NULL(fixture.oldloc.node_access_2);
    fixture.old_view.file_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_type = RTFS_FT_DIR;
    fixture.target_inode.i.i_dentry_num = 1;
    fixture.parent_block.dentry[0].file_type = RTFS_FT_DIR;
    fsHandlerFixtureSyncCachedNode(&fixture, 3001, &fixture.target_inode);

    TEST_ASSERT_EQUAL(-1, r2fsFsHandler.rmnod_h(&fixture.parentloc, &fixture.oldloc));
    TEST_ASSERT_EQUAL(ENOTEMPTY, errno);

    fsHandlerFixtureFini(&fixture);
}
