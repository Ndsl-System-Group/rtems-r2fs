#include "integration/r2fs_rtems_mount_fixture.h"
#include "rtfs_test.h"

#include "fs/fs.h"
#include "fs/fs_manager.h"

#include <sys/stat.h>

RTFS_TEST(IntegrationLifecycle_CleanRemount_ShouldPreserveRootViewAndSuperblockState)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat before_st;
    struct stat after_st;
    struct statvfs before_stvfs;
    struct statvfs after_stvfs;
    file_system_manager *fs_manager;
    RtfsSuperBlock *super_block;
    uint32_t before_free_segment_count;
    uint32_t before_next_free_nid;
    uint16_t before_journal_end_blkoff;

    /*
     * 这条用例验证 clean unmount + remount 的稳定性：
     * 在没有额外文件系统写入的前提下，根目录的外部可见视图
     * 以及 superblock 中的关键持久状态在重挂载前后应保持一致。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    fs_manager = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(fs_manager);
    super_block = fileSystemManagerGetSuperBlkMem(fs_manager);
    TEST_ASSERT_NOT_NULL(super_block);

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatRoot(&before_st));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatvfsRoot(&before_stvfs));

    before_free_segment_count = super_block->free_segment_count;
    before_next_free_nid = super_block->next_free_nid;
    before_journal_end_blkoff = super_block->meta_journal_end_blkoff;

    r2fsRtemsMountFixtureUnmount(&fixture);

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRemount(&fixture));

    fs_manager = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(fs_manager);
    super_block = fileSystemManagerGetSuperBlkMem(fs_manager);
    TEST_ASSERT_NOT_NULL(super_block);

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatRoot(&after_st));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatvfsRoot(&after_stvfs));

    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_st.st_ino, (uint32_t)after_st.st_ino);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_stvfs.f_blocks, (uint32_t)after_stvfs.f_blocks);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_stvfs.f_bfree, (uint32_t)after_stvfs.f_bfree);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_stvfs.f_ffree, (uint32_t)after_stvfs.f_ffree);
    TEST_ASSERT_EQUAL_UINT32(before_free_segment_count, super_block->free_segment_count);
    TEST_ASSERT_EQUAL_UINT32(before_next_free_nid, super_block->next_free_nid);
    TEST_ASSERT_EQUAL_UINT16(before_journal_end_blkoff, super_block->meta_journal_end_blkoff);

    r2fsRtemsMountFixtureDestroy(&fixture);
}
