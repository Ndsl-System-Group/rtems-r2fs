#include "rtfs_test.h"

#include <errno.h>
#include <pthread.h>
#include <rtems/thread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cache/block_buffer.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/dev.h"
#include "file_inode/file_inode.h"
#include "fs/fs.h"
#include "fs/srmap_utils.h"
#include "fs/super_manager.h"
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

typedef struct FileInodeTestFixture
{
    file_system_manager fs_manager;
    comm_dev dev;
    NodeBlockCache node_cache;
    RtfsFileInodeCache *cache;
    NodeBlockCacheEntryHandle inode_handle;
    char block20[BLOCK_BUFFER_SIZE];
    char block21[BLOCK_BUFFER_SIZE];
    char block22[BLOCK_BUFFER_SIZE];
    uint32_t read_count;
    uint32_t fail_lpa;
    bool read_hook_enabled;
} FileInodeTestFixture;

static FileInodeTestFixture *g_file_inode_fixture = NULL;

static void fillBlockWithSequence(char *buffer, char base)
{
    size_t i;

    for (i = 0; i < BLOCK_BUFFER_SIZE; ++i) {
        buffer[i] = (char)(base + (char)(i % 26));
    }
}

static void prepareRegularFileNode(
    BlockBuffer *buffer,
    rtfs_ino ino,
    uint64_t size
)
{
    struct RtfsNode *node = (struct RtfsNode *)blockBufferGetPtr(buffer);

    memset(node, 0, sizeof(*node));
    node->i.i_type = RTFS_FT_REG_FILE;
    node->i.i_mode = 0644;
    node->i.i_nlink = 1;
    node->i.i_size = size;
    node->i.i_atime = 11;
    node->i.i_mtime = 22;
    node->footer.nid = ino;
    node->footer.ino = ino;
}

static void fileInodeFixtureInit(
    FileInodeTestFixture *fixture,
    rtfs_ino ino,
    uint64_t size
)
{
    BlockBuffer buffer;

    memset(fixture, 0, sizeof(*fixture));
    fixture->fail_lpa = INVALID_LPA;
    fixture->fs_manager.dev_ = &fixture->dev;
    fixture->fs_manager.node_cache_ = &fixture->node_cache;

    fillBlockWithSequence(fixture->block20, 'A');
    fillBlockWithSequence(fixture->block21, 'a');
    fillBlockWithSequence(fixture->block22, '0');

    nodeBlockCacheInit(&fixture->node_cache, &fixture->fs_manager, 8);
    fixture->cache = rtfsFileInodeCacheCreate(&fixture->node_cache);
    TEST_ASSERT_NOT_NULL(fixture->cache);

    blockBufferInit(&buffer);
    prepareRegularFileNode(&buffer, ino, size);
    fixture->inode_handle =
        nodeBlockCacheAdd(&fixture->node_cache, &buffer, ino, INVALID_NID, 10);
    blockBufferDestroy(&buffer);
}

static void fileInodeFixtureFini(FileInodeTestFixture *fixture)
{
    rtfsFileInodeSetReadBlockHook(NULL);
    g_file_inode_fixture = NULL;

    rtfsFileInodeCacheDestroy(fixture->cache);
    nodeBlockCacheEntryHandleDestroy(&fixture->inode_handle);
    nodeBlockCacheDestroy(&fixture->node_cache);
}

static struct RtfsNode *fileInodeFixtureGetNode(FileInodeTestFixture *fixture)
{
    return nodeBlockCacheEntryGetNodeBlockPtr(fixture->inode_handle.entry);
}

static RtfsFileInode *mustLoadFileInode(RtfsFileInodeCache *cache, rtfs_ino ino)
{
    RtfsFileInode *file_inode = NULL;

    TEST_ASSERT_EQUAL(0, rtfsFileInodeBuild(cache, ino, &file_inode));
    TEST_ASSERT_NOT_NULL(file_inode);
    return file_inode;
}

static int fileInodeReadBlockHook(
    comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    (void)dev;

    if (g_file_inode_fixture == NULL ||
        !g_file_inode_fixture->read_hook_enabled) {
        return EIO;
    }

    g_file_inode_fixture->read_count++;

    if (lpa == g_file_inode_fixture->fail_lpa) {
        return EIO;
    }

    if (lpa == 20) {
        memcpy(buffer, g_file_inode_fixture->block20, BLOCK_BUFFER_SIZE);
        return 0;
    }

    if (lpa == 21) {
        memcpy(buffer, g_file_inode_fixture->block21, BLOCK_BUFFER_SIZE);
        return 0;
    }

    if (lpa == 22) {
        memcpy(buffer, g_file_inode_fixture->block22, BLOCK_BUFFER_SIZE);
        return 0;
    }

    return ENOENT;
}

RTFS_TEST(FileInodeBuild_WithRegularFileNode_ShouldLoadSize)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;

    fileInodeFixtureInit(&fixture, 2000, 12345);

    file_inode = mustLoadFileInode(fixture.cache, 2000);

    TEST_ASSERT_EQUAL_UINT64(12345u, rtfsFileInodeGetSize(file_inode));

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeBuild_WithCacheMiss_ShouldReturnENOENT)
{
    NodeBlockCache node_cache;
    RtfsFileInodeCache *cache;
    RtfsFileInode *file_inode = NULL;

    nodeBlockCacheInit(&node_cache, NULL, 8);
    cache = rtfsFileInodeCacheCreate(&node_cache);

    TEST_ASSERT_NOT_NULL(cache);
    TEST_ASSERT_EQUAL(ENOENT, rtfsFileInodeBuild(cache, 404, &file_inode));
    TEST_ASSERT_NULL(file_inode);

    rtfsFileInodeCacheDestroy(cache);
    nodeBlockCacheDestroy(&node_cache);
}

RTFS_TEST(FileInodeBuild_WhenOutParamIsNull_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(EINVAL, rtfsFileInodeBuild(NULL, 1, NULL));
}

RTFS_TEST(FileInodeBuild_WhenCacheIsNull_ShouldReturnEINVAL)
{
    RtfsFileInode *file_inode = NULL;

    TEST_ASSERT_EQUAL(EINVAL, rtfsFileInodeBuild(NULL, 1, &file_inode));
    TEST_ASSERT_NULL(file_inode);
}

RTFS_TEST(FileInodeBuild_WhenNodeIsNotRegularFile_ShouldReturnEINVAL)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode = NULL;
    struct RtfsNode *node;

    fileInodeFixtureInit(&fixture, 2100, 0);
    node = fileInodeFixtureGetNode(&fixture);
    node->i.i_type = RTFS_FT_DIR;

    TEST_ASSERT_EQUAL(EINVAL, rtfsFileInodeBuild(fixture.cache, 2100, &file_inode));
    TEST_ASSERT_NULL(file_inode);

    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodePut_WhenNull_ShouldBeSafe)
{
    rtfsFileInodePut(NULL);
    TEST_PASS();
}

RTFS_TEST(FileInodeGetSize_WhenNull_ShouldReturnZero)
{
    TEST_ASSERT_EQUAL_UINT64(0u, rtfsFileInodeGetSize(NULL));
}

RTFS_TEST(FileInodeRead_WhenOffsetAtEnd_ShouldReturnZeroAndKeepOffset)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    char buffer[8];
    off_t offset = 10;

    fileInodeFixtureInit(&fixture, 2200, 10);
    file_inode = mustLoadFileInode(fixture.cache, 2200);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeRead(NULL, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(10, offset);

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_WhenReadingMappedBlock_ShouldUseReadHookAndAdvanceOffset)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    struct RtfsNode *node;
    char buffer[8];
    off_t offset = 5;

    fileInodeFixtureInit(&fixture, 2300, 32);
    node = fileInodeFixtureGetNode(&fixture);
    node->i.i_addr[0] = 20;
    file_inode = mustLoadFileInode(fixture.cache, 2300);

    g_file_inode_fixture = &fixture;
    fixture.read_hook_enabled = true;
    rtfsFileInodeSetReadBlockHook(fileInodeReadBlockHook);

    TEST_ASSERT_EQUAL(8, rtfsFileInodeRead(&fixture.fs_manager, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(fixture.block20 + 5, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(13, offset);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.read_count);

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_WhenReadingHole_ShouldReturnZeroFilledData)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    char buffer[8];
    uint8_t zero[8] = {0};
    off_t offset = 0;

    fileInodeFixtureInit(&fixture, 2400, sizeof(buffer));
    file_inode = mustLoadFileInode(fixture.cache, 2400);

    memset(buffer, 0x5a, sizeof(buffer));
    TEST_ASSERT_EQUAL(8, rtfsFileInodeRead(NULL, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(zero, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(8, offset);

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_WhenCrossingBlockBoundary_ShouldReadMultipleBlocks)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    struct RtfsNode *node;
    char buffer[6];
    char expected[6];
    off_t offset = BLOCK_BUFFER_SIZE - 2;

    fileInodeFixtureInit(&fixture, 2500, BLOCK_BUFFER_SIZE + 4);
    node = fileInodeFixtureGetNode(&fixture);
    node->i.i_addr[0] = 20;
    node->i.i_addr[1] = 21;
    file_inode = mustLoadFileInode(fixture.cache, 2500);

    g_file_inode_fixture = &fixture;
    fixture.read_hook_enabled = true;
    rtfsFileInodeSetReadBlockHook(fileInodeReadBlockHook);

    memcpy(expected, fixture.block20 + BLOCK_BUFFER_SIZE - 2, 2);
    memcpy(expected + 2, fixture.block21, 4);

    TEST_ASSERT_EQUAL(6, rtfsFileInodeRead(&fixture.fs_manager, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(expected, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL((off_t)(BLOCK_BUFFER_SIZE + 4), offset);
    TEST_ASSERT_EQUAL_UINT32(2u, fixture.read_count);

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_WhenReadHookFails_ShouldReturnMinusOneAndSetErrno)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    struct RtfsNode *node;
    char buffer[8];
    off_t offset = 0;

    fileInodeFixtureInit(&fixture, 2600, sizeof(buffer));
    node = fileInodeFixtureGetNode(&fixture);
    node->i.i_addr[0] = 20;
    file_inode = mustLoadFileInode(fixture.cache, 2600);

    g_file_inode_fixture = &fixture;
    fixture.read_hook_enabled = true;
    fixture.fail_lpa = 20;
    rtfsFileInodeSetReadBlockHook(fileInodeReadBlockHook);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeRead(&fixture.fs_manager, file_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(EIO, errno);
    TEST_ASSERT_EQUAL(0, offset);

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeRead_WhenArgumentsAreInvalid_ShouldReturnMinusOneAndSetErrno)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    char buffer[8];
    off_t offset = 0;
    off_t negative_offset = -1;

    fileInodeFixtureInit(&fixture, 2700, sizeof(buffer));
    file_inode = mustLoadFileInode(fixture.cache, 2700);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeRead(NULL, NULL, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeRead(NULL, file_inode, NULL, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeRead(NULL, file_inode, &offset, NULL, sizeof(buffer)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeRead(NULL, file_inode, &negative_offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeRead(NULL, file_inode, &offset, NULL, 0));

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeWrite_WhenWritingEmptyFile_ShouldUpdateSizeOffsetAndReadableData)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    const char *text = "hello";
    char buffer[8];
    off_t offset = 0;
    off_t read_offset = 0;

    fileInodeFixtureInit(&fixture, 2800, 0);
    file_inode = mustLoadFileInode(fixture.cache, 2800);

    TEST_ASSERT_EQUAL(5, rtfsFileInodeWrite(NULL, file_inode, &offset, text, strlen(text)));
    TEST_ASSERT_EQUAL(5, offset);
    TEST_ASSERT_EQUAL_UINT64(5u, rtfsFileInodeGetSize(file_inode));

    memset(buffer, 0, sizeof(buffer));
    TEST_ASSERT_EQUAL(5, rtfsFileInodeRead(NULL, file_inode, &read_offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(text, buffer, strlen(text));

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeWrite_WhenPartialOverwrite_ShouldPreserveUnwrittenBytes)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    const char *initial = "abcdef";
    const char *patch = "XY";
    char buffer[8];
    off_t offset = 0;
    off_t overwrite_offset = 2;
    off_t read_offset = 0;

    fileInodeFixtureInit(&fixture, 2900, 0);
    file_inode = mustLoadFileInode(fixture.cache, 2900);

    TEST_ASSERT_EQUAL(6, rtfsFileInodeWrite(NULL, file_inode, &offset, initial, strlen(initial)));
    TEST_ASSERT_EQUAL(2, rtfsFileInodeWrite(NULL, file_inode, &overwrite_offset, patch, strlen(patch)));

    memset(buffer, 0, sizeof(buffer));
    TEST_ASSERT_EQUAL(6, rtfsFileInodeRead(NULL, file_inode, &read_offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY("abXYef", buffer, 6);
    TEST_ASSERT_EQUAL_UINT64(6u, rtfsFileInodeGetSize(file_inode));

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeWrite_WhenWritingAcrossBlockBoundary_ShouldAdvanceOffsetAndReadBack)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    const char *text = "ABCDEF";
    char buffer[6];
    off_t offset = BLOCK_BUFFER_SIZE - 2;
    off_t read_offset = BLOCK_BUFFER_SIZE - 2;

    fileInodeFixtureInit(&fixture, 3000, 0);
    file_inode = mustLoadFileInode(fixture.cache, 3000);

    TEST_ASSERT_EQUAL(6, rtfsFileInodeWrite(NULL, file_inode, &offset, text, strlen(text)));
    TEST_ASSERT_EQUAL((off_t)(BLOCK_BUFFER_SIZE + 4), offset);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)BLOCK_BUFFER_SIZE + 4u, rtfsFileInodeGetSize(file_inode));

    memset(buffer, 0, sizeof(buffer));
    TEST_ASSERT_EQUAL(6, rtfsFileInodeRead(NULL, file_inode, &read_offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(text, buffer, sizeof(buffer));

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeWrite_WhenArgumentsAreInvalid_ShouldReturnMinusOneAndSetErrno)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    const char *text = "x";
    off_t offset = 0;
    off_t negative_offset = -1;

    fileInodeFixtureInit(&fixture, 3100, 0);
    file_inode = mustLoadFileInode(fixture.cache, 3100);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeWrite(NULL, NULL, &offset, text, 1));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeWrite(NULL, file_inode, NULL, text, 1));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeWrite(NULL, file_inode, &offset, NULL, 1));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsFileInodeWrite(NULL, file_inode, &negative_offset, text, 1));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeWrite(NULL, file_inode, &offset, NULL, 0));

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeTruncate_WhenExtending_ShouldOnlyUpdateSize)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;

    fileInodeFixtureInit(&fixture, 3200, 4);
    file_inode = mustLoadFileInode(fixture.cache, 3200);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeTruncate(NULL, file_inode, 10));
    TEST_ASSERT_EQUAL_UINT64(10u, rtfsFileInodeGetSize(file_inode));

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeTruncate_WhenShrinkingInsideBlock_ShouldZeroTail)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    const char *text = "abcdefgh";
    char buffer[8];
    uint8_t expected[8] = {'a', 'b', 'c', 0, 0, 0, 0, 0};
    off_t offset = 0;
    off_t read_offset = 0;

    fileInodeFixtureInit(&fixture, 3210, 0);
    file_inode = mustLoadFileInode(fixture.cache, 3210);

    TEST_ASSERT_EQUAL(8, rtfsFileInodeWrite(NULL, file_inode, &offset, text, strlen(text)));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeTruncate(NULL, file_inode, 3));
    TEST_ASSERT_EQUAL_UINT64(3u, rtfsFileInodeGetSize(file_inode));
    TEST_ASSERT_EQUAL(0, rtfsFileInodeTruncate(NULL, file_inode, 8));
    TEST_ASSERT_EQUAL_UINT64(8u, rtfsFileInodeGetSize(file_inode));

    memset(buffer, 0x5a, sizeof(buffer));
    TEST_ASSERT_EQUAL(8, rtfsFileInodeRead(NULL, file_inode, &read_offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(expected, buffer, sizeof(buffer));

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeTruncate_WhenShrinkingDirectBlocks_ShouldClearMappingAndCollectOldLpa)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    struct RtfsNode *node;
    uint32_t old_lpas[2];
    size_t old_lpa_count = 0;

    fileInodeFixtureInit(&fixture, 3300, 2 * BLOCK_BUFFER_SIZE);
    node = fileInodeFixtureGetNode(&fixture);
    node->i.i_addr[0] = 20;
    node->i.i_addr[1] = 21;
    file_inode = mustLoadFileInode(fixture.cache, 3300);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeTruncate(NULL, file_inode, BLOCK_BUFFER_SIZE));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)BLOCK_BUFFER_SIZE, rtfsFileInodeGetSize(file_inode));
    TEST_ASSERT_EQUAL_UINT32(20u, node->i.i_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, node->i.i_addr[1]);

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
    TEST_ASSERT_EQUAL_UINT32(21u, old_lpas[0]);

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCollectPendingDataCowOldLpas_WhenCapacityTooSmall_ShouldReturnENOSPC)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    struct RtfsNode *node;
    uint32_t old_lpas[3];
    size_t old_lpa_count = 0;

    fileInodeFixtureInit(&fixture, 3400, 3 * BLOCK_BUFFER_SIZE);
    node = fileInodeFixtureGetNode(&fixture);
    node->i.i_addr[0] = 20;
    node->i.i_addr[1] = 21;
    node->i.i_addr[2] = 22;
    file_inode = mustLoadFileInode(fixture.cache, 3400);

    TEST_ASSERT_EQUAL(0, rtfsFileInodeTruncate(NULL, file_inode, 0));
    TEST_ASSERT_EQUAL(
        ENOSPC,
        rtfsFileInodeCollectPendingDataCowOldLpas(
            file_inode,
            old_lpas,
            2,
            &old_lpa_count
        )
    );

    TEST_ASSERT_EQUAL(
        0,
        rtfsFileInodeCollectPendingDataCowOldLpas(
            file_inode,
            old_lpas,
            sizeof(old_lpas) / sizeof(old_lpas[0]),
            &old_lpa_count
        )
    );
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)old_lpa_count);
    TEST_ASSERT_EQUAL_UINT32(20u, old_lpas[0]);
    TEST_ASSERT_EQUAL_UINT32(21u, old_lpas[1]);
    TEST_ASSERT_EQUAL_UINT32(22u, old_lpas[2]);

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeCollectPendingDataCowOldLpas_WhenArgumentsInvalid_ShouldReturnEINVAL)
{
    FileInodeTestFixture fixture;
    RtfsFileInode *file_inode;
    uint32_t old_lpas[1];
    size_t old_lpa_count = 0;

    fileInodeFixtureInit(&fixture, 3500, 0);
    file_inode = mustLoadFileInode(fixture.cache, 3500);

    TEST_ASSERT_EQUAL(
        EINVAL,
        rtfsFileInodeCollectPendingDataCowOldLpas(
            NULL,
            old_lpas,
            sizeof(old_lpas) / sizeof(old_lpas[0]),
            &old_lpa_count
        )
    );
    TEST_ASSERT_EQUAL(
        EINVAL,
        rtfsFileInodeCollectPendingDataCowOldLpas(
            file_inode,
            NULL,
            sizeof(old_lpas) / sizeof(old_lpas[0]),
            &old_lpa_count
        )
    );
    TEST_ASSERT_EQUAL(
        EINVAL,
        rtfsFileInodeCollectPendingDataCowOldLpas(
            file_inode,
            old_lpas,
            sizeof(old_lpas) / sizeof(old_lpas[0]),
            NULL
        )
    );

    rtfsFileInodePut(file_inode);
    fileInodeFixtureFini(&fixture);
}

RTFS_TEST(FileInodeTruncate_WhenArgumentsInvalid_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(EINVAL, rtfsFileInodeTruncate(NULL, NULL, 0));
}
