#include "rtfs_test.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <rtems/libio_.h>
#include <rtems/thread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cache/block_buffer.h"
#include "cache/generic_cache_manager.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/dev.h"
#include "file_inode/file_inode.h"
#include "fs/cow_reclaim_registry.h"
#include "fs/fs.h"
#include "fs/srmap_utils.h"
#include "fs/super_manager.h"
#include "file_inode/file_handler.h"
#include "inode/inode.h"
#include "journal/journal_container.h"

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

typedef struct FileHandlerFixture
{
    file_system_manager fs_manager;
    struct RtfsSuperBlock super_block;
    NodeBlockCache node_cache;
    SitNatCache sit_cache;
    SitNatCache nat_cache;
    super_manager *sp_manager;
    JournalContainer journal;
    comm_dev dev;
    rtems_filesystem_mount_table_entry_t mt_entry;
    rtems_filesystem_location_info_t location;
    RtfsRuntimeInodeView node_view;
    rtems_libio_t iop;
    struct RtfsSitBlock sit_block;
    struct RtfsNode inode_node;
    char data_block20[BLOCK_BUFFER_SIZE];
    char data_block21[BLOCK_BUFFER_SIZE];
    char written_block[BLOCK_BUFFER_SIZE];
    uint32_t data_read_count;
    uint32_t data_write_count;
    uint32_t node_write_count;
    uint32_t fail_data_lpa;
    uint32_t fail_write_lpa;
    uint32_t last_write_lpa;
    bool hook_enabled;
} FileHandlerFixture;

static FileHandlerFixture *g_file_handler_fixture = NULL;
static JournalContainer *g_file_handler_committed_journal = NULL;
static int g_file_handler_journal_commit_rc = 0;

static void fileHandlerFillBlock(char *buffer, char base)
{
    size_t i;

    for (i = 0; i < BLOCK_BUFFER_SIZE; ++i) {
        buffer[i] = (char)(base + (char)(i % 26));
    }
}

static void fileHandlerSetNatEntry(
    FileHandlerFixture *fixture,
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

static void fileHandlerMarkSitValid(FileHandlerFixture *fixture, uint32_t lpa)
{
    uint32_t seg_id = lpa / BLOCK_PER_SEGMENT;
    uint32_t seg_off = lpa % BLOCK_PER_SEGMENT;
    uint32_t sit_lpa = fixture->super_block.sit_blkaddr + (seg_id / SIT_ENTRY_PER_BLOCK);
    uint32_t sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    uint32_t bitmap_idx = seg_off / 8;
    uint32_t bitmap_off = seg_off % 8;
    SitNatCacheEntryHandle handle;
    struct RtfsSitBlock *sit_block;
    struct RtfsSitEntry *entry;

    handle = sitNatCacheGet(&fixture->sit_cache, sit_lpa);
    TEST_ASSERT_NOT_NULL(handle.entry);
    sit_block = sitNatCacheEntryHandleGetSitBlockPtr(&handle);
    entry = &sit_block->entries[sit_idx];
    if ((entry->valid_map[bitmap_idx] & (1u << bitmap_off)) == 0) {
        entry->valid_map[bitmap_idx] |= (uint8_t)(1u << bitmap_off);
        if (GET_SIT_VBLOCKS(entry) < 511) {
            entry->vblocks += 1;
        }
    }
    sitNatCacheEntryHandleDestroy(&handle);
}

static void fileHandlerAddCachedInode(FileHandlerFixture *fixture)
{
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;

    blockBufferInit(&buffer);
    blockBufferCopyContentFromBuf(&buffer, (const char *)&fixture->inode_node);
    handle = nodeBlockCacheAdd(
        &fixture->node_cache,
        &buffer,
        (uint32_t)fixture->node_view.ino,
        INVALID_NID,
        10
    );
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
}

static void fileHandlerRefreshCachedInode(FileHandlerFixture *fixture)
{
    NodeBlockCacheEntryHandle handle;

    handle = nodeBlockCacheGet(&fixture->node_cache, (uint32_t)fixture->node_view.ino);
    TEST_ASSERT_NOT_NULL(handle.entry);
    blockBufferCopyContentFromBuf(
        nodeBlockCacheEntryGetNodeBuffer(handle.entry),
        (const char *)&fixture->inode_node
    );
    nodeBlockCacheEntryHandleDestroy(&handle);
}

static int fileHandlerReadBlockHook(comm_dev *dev, uint32_t lpa, void *buffer)
{
    (void)dev;

    if (g_file_handler_fixture == NULL) {
        return EIO;
    }

    g_file_handler_fixture->data_read_count++;

    if (lpa == g_file_handler_fixture->fail_data_lpa) {
        return EIO;
    }

    if (lpa == 20) {
        memcpy(buffer, g_file_handler_fixture->data_block20, BLOCK_BUFFER_SIZE);
        return 0;
    }

    if (lpa == 21) {
        memcpy(buffer, g_file_handler_fixture->data_block21, BLOCK_BUFFER_SIZE);
        return 0;
    }

    return ENOENT;
}

static int fileHandlerWriteBlockHook(comm_dev *dev, uint32_t lpa, const void *buffer)
{
    (void)dev;

    if (g_file_handler_fixture == NULL) {
        return EIO;
    }

    if (lpa == g_file_handler_fixture->fail_write_lpa) {
        return EIO;
    }

    g_file_handler_fixture->data_write_count++;
    g_file_handler_fixture->last_write_lpa = lpa;
    memcpy(g_file_handler_fixture->written_block, buffer, BLOCK_BUFFER_SIZE);
    return 0;
}

static int fileHandlerNodeWriteBlockHook(comm_dev *dev, uint32_t lpa, const void *buffer)
{
    (void)dev;
    (void)lpa;
    (void)buffer;

    if (g_file_handler_fixture == NULL) {
        return EIO;
    }

    g_file_handler_fixture->node_write_count++;
    return 0;
}

static int fileHandlerJournalCommitHook(JournalContainer *journal)
{
    if (g_file_handler_journal_commit_rc != 0) {
        return g_file_handler_journal_commit_rc;
    }

    g_file_handler_committed_journal = journal;
    return 0;
}

static void fileHandlerReleaseCommittedJournal(void)
{
    if (g_file_handler_committed_journal != NULL) {
        journalContainerDestroy(g_file_handler_committed_journal);
        free(g_file_handler_committed_journal);
        g_file_handler_committed_journal = NULL;
    }
}

static void fileHandlerFixturePrepareRegularFile(
    FileHandlerFixture *fixture,
    rtfs_ino ino,
    rtfs_ino parent_ino
)
{
    SitNatCacheEntry *sit_entry;

    memset(fixture, 0, sizeof(*fixture));
    fixture->fail_data_lpa = INVALID_LPA;
    fixture->fail_write_lpa = INVALID_LPA;
    fixture->last_write_lpa = INVALID_LPA;

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

    sit_entry = (SitNatCacheEntry *)malloc(sizeof(*sit_entry));
    TEST_ASSERT_NOT_NULL(sit_entry);
    sitNatCacheEntryInit(sit_entry, fixture->super_block.sit_blkaddr);
    memset(blockBufferGetPtr(&sit_entry->cache), 0, BLOCK_BUFFER_SIZE);
    genericCacheManagerAdd(&fixture->sit_cache.cacheManager, fixture->super_block.sit_blkaddr, sit_entry);
    fixture->sit_cache.curSize++;

    fileHandlerMarkSitValid(fixture, 10);
    fileHandlerMarkSitValid(fixture, 20);
    fileHandlerMarkSitValid(fixture, 21);

    rtfsRuntimeInodeViewInit(&fixture->node_view, ino, parent_ino, RTFS_FT_REG_FILE);
    fixture->mt_entry.fs_info = &fixture->fs_manager;
    fixture->location.mt_entry = &fixture->mt_entry;
    fixture->location.node_access = &fixture->node_view;
    fixture->iop.pathinfo = fixture->location;

    memset(&fixture->inode_node, 0, sizeof(fixture->inode_node));
    fixture->inode_node.i.i_type = RTFS_FT_REG_FILE;
    fixture->inode_node.i.i_mode = 0644;
    fixture->inode_node.i.i_nlink = 2;
    fixture->inode_node.i.i_size = 2 * BLOCK_BUFFER_SIZE;
    fixture->inode_node.i.i_atime = 111;
    fixture->inode_node.i.i_mtime = 222;
    fixture->inode_node.i.i_addr[0] = 20;
    fixture->inode_node.i.i_addr[1] = 21;
    fixture->inode_node.footer.nid = (uint32_t)ino;
    fixture->inode_node.footer.ino = (uint32_t)ino;

    fileHandlerFillBlock(fixture->data_block20, 'A');
    fileHandlerFillBlock(fixture->data_block21, 'a');

    fileHandlerSetNatEntry(fixture, (uint32_t)ino, (uint32_t)ino, 10);
    fileHandlerSetNatEntry(fixture, 6000, INVALID_NID, 6001);
    fileHandlerSetNatEntry(fixture, 6001, INVALID_NID, 6002);
    fileHandlerSetNatEntry(fixture, 6002, INVALID_NID, INVALID_NID);
    fileHandlerAddCachedInode(fixture);

    g_file_handler_fixture = fixture;
    g_file_handler_journal_commit_rc = 0;
    g_file_handler_committed_journal = NULL;
    fixture->hook_enabled = true;
    rtfsFileInodeSetReadBlockHook(fileHandlerReadBlockHook);
    rtfsFileInodeSetWriteBlockHook(fileHandlerWriteBlockHook);
    rtfsFileInodeSetJournalCommitHook(fileHandlerJournalCommitHook);
    nodeBlockCacheSetWriteBlockHook(fileHandlerNodeWriteBlockHook);
}

static void fileHandlerFixtureFini(FileHandlerFixture *fixture)
{
    if (fixture->iop.data1 != NULL) {
        rtfsFilehandlers.close_h(&fixture->iop);
    }

    if (fixture->hook_enabled) {
        rtfsFileInodeSetReadBlockHook(NULL);
        rtfsFileInodeSetWriteBlockHook(NULL);
        rtfsFileInodeSetJournalCommitHook(NULL);
        nodeBlockCacheSetWriteBlockHook(NULL);
        fixture->hook_enabled = false;
        g_file_handler_fixture = NULL;
    }

    fileHandlerReleaseCommittedJournal();
    cowReclaimRegistryDestroy();
    superManagerDestroy(fixture->sp_manager);
    journalContainerDestroy(&fixture->journal);
    sitNatCacheDestroy(&fixture->sit_cache);
    sitNatCacheDestroy(&fixture->nat_cache);
    nodeBlockCacheDestroy(&fixture->node_cache);
}

RTFS_TEST(FileHandlerOpen_WhenNodeIsRegularFile_ShouldSucceed)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5100, 5099);

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY, 0));
    TEST_ASSERT_NOT_NULL(fixture.iop.data1);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerOpen_WhenNodeIsDirectory_ShouldReturnEISDIR)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5101, 5100);
    fixture.node_view.file_type = RTFS_FT_DIR;

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY, 0));
    TEST_ASSERT_EQUAL(EISDIR, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerOpen_WhenIopIsNull_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.open_h(NULL, "/file", O_RDONLY, 0));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(FileHandlerOpen_WhenHandleAlreadyExists_ShouldReturnEBUSY)
{
    FileHandlerFixture fixture;
    int dummy;

    fileHandlerFixturePrepareRegularFile(&fixture, 5102, 5101);
    fixture.iop.data1 = &dummy;

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY, 0));
    TEST_ASSERT_EQUAL(EBUSY, errno);

    fixture.iop.data1 = NULL;
    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerOpen_WhenTruncateRequestedReadOnly_ShouldReturnEACCES)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5103, 5102);

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY | O_TRUNC, 0));
    TEST_ASSERT_EQUAL(EACCES, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerOpen_WhenAppendRequested_ShouldStartAtEnd)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5104, 5103);

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_WRONLY | O_APPEND, 0));
    TEST_ASSERT_EQUAL((off_t)(2 * BLOCK_BUFFER_SIZE), fixture.iop.offset);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerOpen_WhenTruncateRequestedWritable_ShouldClearSizeAndCommit)
{
    FileHandlerFixture fixture;
    struct stat st;

    fileHandlerFixturePrepareRegularFile(&fixture, 5105, 5104);

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_WRONLY | O_TRUNC, 0));
    TEST_ASSERT_NOT_NULL(g_file_handler_committed_journal);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL((off_t)0, st.st_size);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerRead_WhenFileHasMappedBlocks_ShouldReadAndAdvanceOffset)
{
    FileHandlerFixture fixture;
    char buffer[8];
    ssize_t bytes_read;

    fileHandlerFixturePrepareRegularFile(&fixture, 5200, 5199);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY, 0));
    fixture.iop.offset = 4;

    bytes_read = rtfsFilehandlers.read_h(&fixture.iop, buffer, sizeof(buffer));

    TEST_ASSERT_EQUAL((ssize_t)sizeof(buffer), bytes_read);
    TEST_ASSERT_EQUAL_MEMORY(fixture.data_block20 + 4, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL((off_t)(4 + sizeof(buffer)), fixture.iop.offset);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.data_read_count);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerRead_WhenOpenedWriteOnly_ShouldReturnEBADF)
{
    FileHandlerFixture fixture;
    char buffer[8];

    fileHandlerFixturePrepareRegularFile(&fixture, 5201, 5200);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_WRONLY, 0));

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.read_h(&fixture.iop, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(EBADF, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerRead_WhenIopIsNull_ShouldReturnEINVAL)
{
    char buffer[8];

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.read_h(NULL, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(FileHandlerRead_WhenBufferIsNull_ShouldReturnEINVAL)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5202, 5201);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY, 0));

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.read_h(&fixture.iop, NULL, 1));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerWrite_WhenOpenedReadWrite_ShouldUpdateOffsetAndReadBack)
{
    FileHandlerFixture fixture;
    const char *text = "filewrite";
    char buffer[16];

    fileHandlerFixturePrepareRegularFile(&fixture, 5300, 5299);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDWR, 0));

    TEST_ASSERT_EQUAL(9, rtfsFilehandlers.write_h(&fixture.iop, text, strlen(text)));
    TEST_ASSERT_EQUAL((off_t)9, fixture.iop.offset);

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.lseek_h(&fixture.iop, 0, SEEK_SET));
    memset(buffer, 0, sizeof(buffer));
    TEST_ASSERT_EQUAL(9, rtfsFilehandlers.read_h(&fixture.iop, buffer, strlen(text)));
    TEST_ASSERT_EQUAL_MEMORY(text, buffer, strlen(text));

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerWrite_WhenOpenedReadOnly_ShouldReturnEBADF)
{
    FileHandlerFixture fixture;
    const char *text = "readonly";

    fileHandlerFixturePrepareRegularFile(&fixture, 5301, 5300);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY, 0));

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.write_h(&fixture.iop, text, strlen(text)));
    TEST_ASSERT_EQUAL(EBADF, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerWrite_WhenBufferIsNull_ShouldReturnEINVAL)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5302, 5301);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_WRONLY, 0));

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.write_h(&fixture.iop, NULL, 1));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerWrite_WhenAppendMode_ShouldAppendAtCurrentEnd)
{
    FileHandlerFixture fixture;
    const char *text = "tail";
    struct stat st;

    fileHandlerFixturePrepareRegularFile(&fixture, 5303, 5302);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_WRONLY | O_APPEND, 0));
    TEST_ASSERT_EQUAL(4, rtfsFilehandlers.write_h(&fixture.iop, text, strlen(text)));
    TEST_ASSERT_EQUAL((off_t)(2 * BLOCK_BUFFER_SIZE + 4), fixture.iop.offset);

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.fdatasync_h(&fixture.iop));
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL((off_t)(2 * BLOCK_BUFFER_SIZE + 4), st.st_size);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerLseek_WhenUsingSetCurAndEnd_ShouldUpdateOffset)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5400, 5399);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY, 0));

    TEST_ASSERT_EQUAL(5, rtfsFilehandlers.lseek_h(&fixture.iop, 5, SEEK_SET));
    TEST_ASSERT_EQUAL(8, rtfsFilehandlers.lseek_h(&fixture.iop, 3, SEEK_CUR));
    TEST_ASSERT_EQUAL((off_t)(2 * BLOCK_BUFFER_SIZE - 2), rtfsFilehandlers.lseek_h(&fixture.iop, -2, SEEK_END));

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerLseek_WhenArgumentsInvalid_ShouldReturnEINVAL)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5401, 5400);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY, 0));

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.lseek_h(&fixture.iop, -1, SEEK_SET));
    TEST_ASSERT_EQUAL(EINVAL, errno);
    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.lseek_h(&fixture.iop, 0, 999));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFtruncate_WhenWritable_ShouldUpdateSize)
{
    FileHandlerFixture fixture;
    struct stat st;

    fileHandlerFixturePrepareRegularFile(&fixture, 5500, 5499);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDWR, 0));

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.ftruncate_h(&fixture.iop, BLOCK_BUFFER_SIZE));
    TEST_ASSERT_EQUAL((off_t)BLOCK_BUFFER_SIZE, rtfsFilehandlers.lseek_h(&fixture.iop, 0, SEEK_END));
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.fdatasync_h(&fixture.iop));
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL((off_t)BLOCK_BUFFER_SIZE, st.st_size);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFtruncate_WhenReadOnly_ShouldReturnEBADF)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5501, 5500);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDONLY, 0));

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.ftruncate_h(&fixture.iop, 0));
    TEST_ASSERT_EQUAL(EBADF, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFtruncate_WhenLengthIsNegative_ShouldReturnEINVAL)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5502, 5501);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_WRONLY, 0));

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.ftruncate_h(&fixture.iop, -1));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFdatasync_WhenDirtyFileExists_ShouldCommitJournal)
{
    FileHandlerFixture fixture;
    const char *text = "sync";

    fileHandlerFixturePrepareRegularFile(&fixture, 5600, 5599);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDWR, 0));
    TEST_ASSERT_EQUAL(4, rtfsFilehandlers.write_h(&fixture.iop, text, strlen(text)));

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.fdatasync_h(&fixture.iop));
    TEST_ASSERT_NOT_NULL(g_file_handler_committed_journal);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.data_write_count);
    TEST_ASSERT_EQUAL_MEMORY(text, fixture.written_block, strlen(text));

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFdatasync_WhenJournalSubmitFails_ShouldReturnError)
{
    FileHandlerFixture fixture;
    const char *text = "fail";

    fileHandlerFixturePrepareRegularFile(&fixture, 5601, 5600);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDWR, 0));
    TEST_ASSERT_EQUAL(4, rtfsFilehandlers.write_h(&fixture.iop, text, strlen(text)));

    g_file_handler_journal_commit_rc = EBUSY;
    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.fdatasync_h(&fixture.iop));
    TEST_ASSERT_EQUAL(EBUSY, errno);
    TEST_ASSERT_NULL(g_file_handler_committed_journal);

    g_file_handler_journal_commit_rc = 0;
    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerClose_WhenDirtyFileExists_ShouldCommitAndClearHandle)
{
    FileHandlerFixture fixture;
    const char *text = "close";

    fileHandlerFixturePrepareRegularFile(&fixture, 5602, 5601);
    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.open_h(&fixture.iop, "/file", O_RDWR, 0));
    TEST_ASSERT_EQUAL(5, rtfsFilehandlers.write_h(&fixture.iop, text, strlen(text)));

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.close_h(&fixture.iop));
    TEST_ASSERT_NULL(fixture.iop.data1);
    TEST_ASSERT_NOT_NULL(g_file_handler_committed_journal);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerClose_WhenIopIsNull_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.close_h(NULL));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(FileHandlerClose_WhenHandleIsMissing_ShouldSucceed)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5603, 5602);

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.close_h(&fixture.iop));

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFstat_WhenDiskInodeExists_ShouldReportRealInodeMetadata)
{
    FileHandlerFixture fixture;
    struct stat st;

    fileHandlerFixturePrepareRegularFile(&fixture, 5700, 5699);

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(5700, st.st_ino);
    TEST_ASSERT_EQUAL((mode_t)(S_IFREG | 0644), st.st_mode);
    TEST_ASSERT_EQUAL(2, st.st_nlink);
    TEST_ASSERT_EQUAL((off_t)(2 * BLOCK_BUFFER_SIZE), st.st_size);
    TEST_ASSERT_EQUAL((blkcnt_t)2, st.st_blocks);
    TEST_ASSERT_EQUAL(4096, st.st_blksize);
    TEST_ASSERT_EQUAL((time_t)111, st.st_atime);
    TEST_ASSERT_EQUAL((time_t)222, st.st_mtime);
    TEST_ASSERT_EQUAL((time_t)222, st.st_ctime);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFstat_WhenDiskModeOrNlinkIsZero_ShouldUseFileDefaults)
{
    FileHandlerFixture fixture;
    struct stat st;

    fileHandlerFixturePrepareRegularFile(&fixture, 5701, 5700);
    fixture.inode_node.i.i_mode = 0;
    fixture.inode_node.i.i_nlink = 0;
    fileHandlerRefreshCachedInode(&fixture);

    TEST_ASSERT_EQUAL(0, rtfsFilehandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL((mode_t)(S_IFREG | 0644), st.st_mode);
    TEST_ASSERT_EQUAL(1, st.st_nlink);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFstat_WhenPathlocIsNull_ShouldReturnEINVAL)
{
    struct stat st;

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.fstat_h(NULL, &st));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(FileHandlerFstat_WhenStatBufferIsNull_ShouldReturnEINVAL)
{
    FileHandlerFixture fixture;

    fileHandlerFixturePrepareRegularFile(&fixture, 5702, 5701);

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.fstat_h(&fixture.location, NULL));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFstat_WhenLocationNodeAccessIsNull_ShouldReturnEINVAL)
{
    FileHandlerFixture fixture;
    struct stat st;

    fileHandlerFixturePrepareRegularFile(&fixture, 5703, 5702);
    fixture.location.node_access = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFstat_WhenMountEntryIsMissing_ShouldReturnEINVAL)
{
    FileHandlerFixture fixture;
    struct stat st;

    fileHandlerFixturePrepareRegularFile(&fixture, 5704, 5703);
    fixture.location.mt_entry = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFstat_WhenFsInfoIsMissing_ShouldReturnEINVAL)
{
    FileHandlerFixture fixture;
    struct stat st;

    fileHandlerFixturePrepareRegularFile(&fixture, 5705, 5704);
    fixture.mt_entry.fs_info = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fileHandlerFixtureFini(&fixture);
}

RTFS_TEST(FileHandlerFstat_WhenInodeIsNotCachedAndCannotBeLoaded_ShouldReturnENOENT)
{
    FileHandlerFixture fixture;
    struct stat st;

    fileHandlerFixturePrepareRegularFile(&fixture, 5706, 5705);
    rtfsRuntimeInodeViewInit(&fixture.node_view, 9998, 5705, RTFS_FT_REG_FILE);
    fixture.fs_manager.nat_cache_ = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsFilehandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(ENOENT, errno);

    fileHandlerFixtureFini(&fixture);
}
