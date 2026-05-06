#include "rtfs_test.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <rtems/libio_.h>
#include <rtems/thread.h>
#include <stdbool.h>
#include <string.h>

#include "cache/block_buffer.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/dev.h"
#include "dir_inode/dir_handler.h"
#include "dir_inode/dir_inode_resolver.h"
#include "fs/fs.h"
#include "fs/fs_manager.h"
#include "inode/inode.h"
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

typedef struct DirHandlerFixture
{
    file_system_manager fs_manager;
    NodeBlockCache node_cache;
    comm_dev dev;
    rtems_filesystem_mount_table_entry_t mt_entry;
    rtems_filesystem_location_info_t location;
    RtfsRuntimeInodeView node_view;
    rtems_libio_t iop;
    struct RtfsNode inode_node;
    struct RtfsDentryBlock block0;
    struct RtfsDentryBlock block1;
    bool read_hook_enabled;
    uint32_t read_count;
    uint32_t fail_lpa;
} DirHandlerFixture;

static DirHandlerFixture *g_dir_handler_fixture = NULL;

static void dirHandlerSetBitmapBit(uint8_t *bitmap, size_t bit_index)
{
    bitmap[bit_index / 8] |= (uint8_t)(1u << (bit_index % 8));
}

static void dirHandlerWriteRegularName(
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

static void dirHandlerAddRegularDentry(
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
        dirHandlerSetBitmapBit(dentry_block->dentry_bitmap, index + slot);
    }
    dirHandlerWriteRegularName(dentry_block, index, name);
}

static void dirHandlerFixturePrepareMultiBlockDirectory(
    DirHandlerFixture *fixture,
    rtfs_ino ino,
    rtfs_ino parent_ino
)
{
    BlockBuffer buffer;
    NodeBlockCacheEntryHandle handle;

    memset(fixture, 0, sizeof(*fixture));
    fixture->fail_lpa = INVALID_LPA;

    fixture->fs_manager.node_cache_ = &fixture->node_cache;
    fixture->fs_manager.dev_ = &fixture->dev;
    fixture->mt_entry.fs_info = &fixture->fs_manager;
    fixture->location.mt_entry = &fixture->mt_entry;
    fixture->location.node_access = &fixture->node_view;
    fixture->iop.pathinfo = fixture->location;

    rtfsRuntimeInodeViewInit(&fixture->node_view, ino, parent_ino, RTFS_FT_DIR);

    memset(&fixture->inode_node, 0, sizeof(fixture->inode_node));
    fixture->inode_node.i.i_type = RTFS_FT_DIR;
    fixture->inode_node.i.i_mode = 0700;
    fixture->inode_node.i.i_nlink = 3;
    fixture->inode_node.i.i_pino = parent_ino;
    fixture->inode_node.i.i_size = 2 * BLOCK_BUFFER_SIZE;
    fixture->inode_node.i.i_dentry_num = 2;
    fixture->inode_node.i.i_atime = 111;
    fixture->inode_node.i.i_mtime = 222;
    fixture->inode_node.i.i_addr[0] = 20;
    fixture->inode_node.i.i_addr[1] = 21;
    fixture->inode_node.footer.nid = (uint32_t)ino;
    fixture->inode_node.footer.ino = (uint32_t)ino;

    memset(&fixture->block0, 0, sizeof(fixture->block0));
    memset(&fixture->block1, 0, sizeof(fixture->block1));
    dirHandlerAddRegularDentry(&fixture->block0, 0, 5001, RTFS_FT_REG_FILE, "alpha");
    dirHandlerAddRegularDentry(&fixture->block1, 0, 5002, RTFS_FT_DIR, "beta");

    nodeBlockCacheInit(&fixture->node_cache, &fixture->fs_manager, 8);
    blockBufferInit(&buffer);
    blockBufferCopyContentFromBuf(&buffer, (const char *)&fixture->inode_node);
    handle = nodeBlockCacheAdd(&fixture->node_cache, &buffer, (uint32_t)ino, INVALID_NID, 10);
    nodeBlockCacheEntryHandleDestroy(&handle);
    blockBufferDestroy(&buffer);
}

static int dirHandlerReadBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    (void)dev;

    if (g_dir_handler_fixture == NULL || !g_dir_handler_fixture->read_hook_enabled) {
        return EIO;
    }

    g_dir_handler_fixture->read_count++;

    if (lpa == g_dir_handler_fixture->fail_lpa) {
        return EIO;
    }

    if (lpa == 20) {
        memcpy(buffer, &g_dir_handler_fixture->block0, sizeof(g_dir_handler_fixture->block0));
        return 0;
    }

    if (lpa == 21) {
        memcpy(buffer, &g_dir_handler_fixture->block1, sizeof(g_dir_handler_fixture->block1));
        return 0;
    }

    return ENOENT;
}

static struct dirent *dirHandlerDirentAt(void *buffer, size_t index)
{
    return (struct dirent *)((char *)buffer + index * sizeof(struct dirent));
}

RTFS_TEST(DirHandlerOpen_WhenNodeIsDirectoryAndReadOnly_ShouldSucceed)
{
    DirHandlerFixture fixture;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4100, 4099);

    TEST_ASSERT_EQUAL(0, rtfsDirhandlers.open_h(&fixture.iop, "/", O_RDONLY, 0));

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerOpen_WhenNodeIsNotDirectory_ShouldReturnENOTDIR)
{
    DirHandlerFixture fixture;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4101, 4100);
    fixture.node_view.file_type = RTFS_FT_REG_FILE;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.open_h(&fixture.iop, "/", O_RDONLY, 0));
    TEST_ASSERT_EQUAL(ENOTDIR, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerOpen_WhenAccessModeIsNotReadOnly_ShouldReturnEISDIR)
{
    DirHandlerFixture fixture;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4102, 4101);

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.open_h(&fixture.iop, "/", O_WRONLY, 0));
    TEST_ASSERT_EQUAL(EISDIR, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerOpen_WhenIopIsNull_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.open_h(NULL, "/", O_RDONLY, 0));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(DirHandlerRead_WhenDirectorySpansMultipleBlocks_ShouldContinueResolvingUntilBufferIsFull)
{
    DirHandlerFixture fixture;
    struct dirent entries[4];
    ssize_t bytes_read;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4200, 4199);
    g_dir_handler_fixture = &fixture;
    fixture.read_hook_enabled = true;
    rtfsDirResolverSetReadBlockHook(dirHandlerReadBlockHook);

    memset(entries, 0, sizeof(entries));
    bytes_read = rtfsDirhandlers.read_h(&fixture.iop, entries, sizeof(entries));

    TEST_ASSERT_EQUAL(4 * (ssize_t)sizeof(struct dirent), bytes_read);
    TEST_ASSERT_EQUAL(2u, fixture.read_count);
    TEST_ASSERT_EQUAL_STRING(".", dirHandlerDirentAt(entries, 0)->d_name);
    TEST_ASSERT_EQUAL_STRING("..", dirHandlerDirentAt(entries, 1)->d_name);
    TEST_ASSERT_EQUAL_STRING("alpha", dirHandlerDirentAt(entries, 2)->d_name);
    TEST_ASSERT_EQUAL_STRING("beta", dirHandlerDirentAt(entries, 3)->d_name);
    TEST_ASSERT_EQUAL(4 * (off_t)sizeof(struct dirent), fixture.iop.offset);

    rtfsDirResolverSetReadBlockHook(NULL);
    g_dir_handler_fixture = NULL;
    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerRead_WhenBufferFitsOnlyOneDirent_ShouldAdvanceOffsetIncrementally)
{
    DirHandlerFixture fixture;
    struct dirent entry;
    ssize_t bytes_read;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4201, 4200);
    g_dir_handler_fixture = &fixture;
    fixture.read_hook_enabled = true;
    rtfsDirResolverSetReadBlockHook(dirHandlerReadBlockHook);

    memset(&entry, 0, sizeof(entry));
    bytes_read = rtfsDirhandlers.read_h(&fixture.iop, &entry, sizeof(entry));
    TEST_ASSERT_EQUAL((ssize_t)sizeof(struct dirent), bytes_read);
    TEST_ASSERT_EQUAL_STRING(".", entry.d_name);
    TEST_ASSERT_EQUAL((off_t)sizeof(struct dirent), fixture.iop.offset);

    memset(&entry, 0, sizeof(entry));
    bytes_read = rtfsDirhandlers.read_h(&fixture.iop, &entry, sizeof(entry));
    TEST_ASSERT_EQUAL((ssize_t)sizeof(struct dirent), bytes_read);
    TEST_ASSERT_EQUAL_STRING("..", entry.d_name);
    TEST_ASSERT_EQUAL(2 * (off_t)sizeof(struct dirent), fixture.iop.offset);

    rtfsDirResolverSetReadBlockHook(NULL);
    g_dir_handler_fixture = NULL;
    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerRead_WhenNodeIsNotDirectory_ShouldReturnENOTDIR)
{
    DirHandlerFixture fixture;
    struct dirent entry;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4209, 4208);
    fixture.node_view.file_type = RTFS_FT_REG_FILE;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.read_h(&fixture.iop, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(ENOTDIR, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerFstat_WhenDiskInodeExists_ShouldReportRealInodeMetadata)
{
    DirHandlerFixture fixture;
    struct stat st;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4300, 4299);

    TEST_ASSERT_EQUAL(0, rtfsDirhandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(4300, st.st_ino);
    TEST_ASSERT_EQUAL((mode_t)(S_IFDIR | 0700), st.st_mode);
    TEST_ASSERT_EQUAL(3, st.st_nlink);
    TEST_ASSERT_EQUAL((off_t)(2 * BLOCK_BUFFER_SIZE), st.st_size);
    TEST_ASSERT_EQUAL((blkcnt_t)2, st.st_blocks);
    TEST_ASSERT_EQUAL((time_t)111, st.st_atime);
    TEST_ASSERT_EQUAL((time_t)222, st.st_mtime);
    TEST_ASSERT_EQUAL((time_t)222, st.st_ctime);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerRead_WhenIopIsNull_ShouldReturnEINVAL)
{
    struct dirent entry;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.read_h(NULL, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(DirHandlerRead_WhenBufferIsNull_ShouldReturnEINVAL)
{
    DirHandlerFixture fixture;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4202, 4201);

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.read_h(&fixture.iop, NULL, sizeof(struct dirent)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerRead_WhenMountEntryIsMissing_ShouldReturnEINVAL)
{
    DirHandlerFixture fixture;
    struct dirent entry;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4203, 4202);
    fixture.iop.pathinfo.mt_entry = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.read_h(&fixture.iop, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerRead_WhenFsInfoIsMissing_ShouldReturnEINVAL)
{
    DirHandlerFixture fixture;
    struct dirent entry;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4204, 4203);
    fixture.mt_entry.fs_info = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.read_h(&fixture.iop, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerRead_WhenNodeCacheIsMissingAndInodeIsNotCached_ShouldReturnEINVAL)
{
    DirHandlerFixture fixture;
    NodeBlockCache original_cache;
    struct dirent entry;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4205, 4204);
    original_cache = fixture.node_cache;
    memset(&fixture.node_cache, 0, sizeof(fixture.node_cache));
    fixture.fs_manager.node_cache_ = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.read_h(&fixture.iop, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fixture.node_cache = original_cache;
    fixture.fs_manager.node_cache_ = &fixture.node_cache;
    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerRead_WhenInodeIsNotCachedAndCannotBeLoaded_ShouldReturnENOENT)
{
    DirHandlerFixture fixture;
    struct dirent entry;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4208, 4207);
    rtfsRuntimeInodeViewInit(&fixture.node_view, 9999, 4207, RTFS_FT_DIR);

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.read_h(&fixture.iop, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(ENOENT, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerRead_WhenReadHookFailsWhileResolvingNextBlock_ShouldReturnEIO)
{
    DirHandlerFixture fixture;
    struct dirent entries[4];
    ssize_t bytes_read;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4206, 4205);
    g_dir_handler_fixture = &fixture;
    fixture.read_hook_enabled = true;
    fixture.fail_lpa = 21;
    rtfsDirResolverSetReadBlockHook(dirHandlerReadBlockHook);

    memset(entries, 0, sizeof(entries));
    bytes_read = rtfsDirhandlers.read_h(&fixture.iop, entries, sizeof(entries));

    TEST_ASSERT_EQUAL(-1, bytes_read);
    TEST_ASSERT_EQUAL(EIO, errno);
    TEST_ASSERT_EQUAL(0, fixture.iop.offset);
    TEST_ASSERT_EQUAL(2u, fixture.read_count);

    rtfsDirResolverSetReadBlockHook(NULL);
    g_dir_handler_fixture = NULL;
    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerRead_WhenDirectoryIsEmptyRegular_ShouldReturnDotAndDotDotOnly)
{
    DirHandlerFixture fixture;
    struct dirent entries[4];
    ssize_t bytes_read;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4207, 4206);
    fixture.inode_node.i.i_size = 0;
    fixture.inode_node.i.i_addr[0] = INVALID_LPA;
    fixture.inode_node.i.i_addr[1] = INVALID_LPA;

    {
        NodeBlockCacheEntryHandle handle = nodeBlockCacheGet(&fixture.node_cache, 4207);
        TEST_ASSERT_NOT_NULL(handle.entry);
        blockBufferCopyContentFromBuf(
            nodeBlockCacheEntryGetNodeBuffer(handle.entry),
            (const char *)&fixture.inode_node
        );
        nodeBlockCacheEntryHandleDestroy(&handle);
    }

    memset(entries, 0, sizeof(entries));
    bytes_read = rtfsDirhandlers.read_h(&fixture.iop, entries, sizeof(entries));

    TEST_ASSERT_EQUAL(2 * (ssize_t)sizeof(struct dirent), bytes_read);
    TEST_ASSERT_EQUAL_STRING(".", dirHandlerDirentAt(entries, 0)->d_name);
    TEST_ASSERT_EQUAL_STRING("..", dirHandlerDirentAt(entries, 1)->d_name);
    TEST_ASSERT_EQUAL(2 * (off_t)sizeof(struct dirent), fixture.iop.offset);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerFstat_WhenDiskModeOrNlinkIsZero_ShouldUseDirectoryDefaults)
{
    DirHandlerFixture fixture;
    struct stat st;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4301, 4300);
    fixture.inode_node.i.i_mode = 0;
    fixture.inode_node.i.i_nlink = 0;

    {
        NodeBlockCacheEntryHandle handle = nodeBlockCacheGet(&fixture.node_cache, 4301);
        TEST_ASSERT_NOT_NULL(handle.entry);
        blockBufferCopyContentFromBuf(
            nodeBlockCacheEntryGetNodeBuffer(handle.entry),
            (const char *)&fixture.inode_node
        );
        nodeBlockCacheEntryHandleDestroy(&handle);
    }

    TEST_ASSERT_EQUAL(0, rtfsDirhandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL((mode_t)(S_IFDIR | 0755), st.st_mode);
    TEST_ASSERT_EQUAL(1, st.st_nlink);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerFstat_WhenPathlocIsNull_ShouldReturnEINVAL)
{
    struct stat st;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.fstat_h(NULL, &st));
    TEST_ASSERT_EQUAL(EINVAL, errno);
}

RTFS_TEST(DirHandlerFstat_WhenStatBufferIsNull_ShouldReturnEINVAL)
{
    DirHandlerFixture fixture;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4302, 4301);

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.fstat_h(&fixture.location, NULL));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerFstat_WhenLocationNodeAccessIsNull_ShouldReturnENOTDIR)
{
    DirHandlerFixture fixture;
    struct stat st;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4303, 4302);
    fixture.location.node_access = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(ENOTDIR, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerFstat_WhenMountEntryIsMissing_ShouldReturnEINVAL)
{
    DirHandlerFixture fixture;
    struct stat st;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4304, 4303);
    fixture.location.mt_entry = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerFstat_WhenFsInfoIsMissing_ShouldReturnEINVAL)
{
    DirHandlerFixture fixture;
    struct stat st;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4305, 4304);
    fixture.mt_entry.fs_info = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerFstat_WhenNodeCacheIsMissingAndInodeIsNotCached_ShouldReturnEINVAL)
{
    DirHandlerFixture fixture;
    NodeBlockCache original_cache;
    struct stat st;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4306, 4305);
    original_cache = fixture.node_cache;
    memset(&fixture.node_cache, 0, sizeof(fixture.node_cache));
    fixture.fs_manager.node_cache_ = NULL;

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    fixture.node_cache = original_cache;
    fixture.fs_manager.node_cache_ = &fixture.node_cache;
    nodeBlockCacheDestroy(&fixture.node_cache);
}

RTFS_TEST(DirHandlerFstat_WhenInodeIsNotCachedAndCannotBeLoaded_ShouldReturnENOENT)
{
    DirHandlerFixture fixture;
    struct stat st;

    dirHandlerFixturePrepareMultiBlockDirectory(&fixture, 4307, 4306);
    rtfsRuntimeInodeViewInit(&fixture.node_view, 9998, 4306, RTFS_FT_DIR);

    TEST_ASSERT_EQUAL(-1, rtfsDirhandlers.fstat_h(&fixture.location, &st));
    TEST_ASSERT_EQUAL(ENOENT, errno);

    nodeBlockCacheDestroy(&fixture.node_cache);
}
