#include "rtfs_test.h"

#include "inode/inode.h"

#include <stdlib.h>
#include <string.h>


RTFS_TEST(RuntimeInodeViewInit_ShouldPopulateAllFields)
{
    RtfsRuntimeInodeView view;

    memset(&view, 0, sizeof(view));
    rtfsRuntimeInodeViewInit(&view, 123, 45, RTFS_FT_DIR);

    TEST_ASSERT_EQUAL_UINT64(123u, view.ino);
    TEST_ASSERT_EQUAL_UINT64(45u, view.parent_ino);
    TEST_ASSERT_EQUAL_UINT8(RTFS_FT_DIR, view.file_type);
}

RTFS_TEST(RuntimeInodeViewCreate_ShouldAllocateAndPopulate)
{
    RtfsRuntimeInodeView *view = rtfsRuntimeInodeViewCreate(321, 54, RTFS_FT_REG_FILE);

    TEST_ASSERT_NOT_NULL(view);
    TEST_ASSERT_EQUAL_UINT64(321u, view->ino);
    TEST_ASSERT_EQUAL_UINT64(54u, view->parent_ino);
    TEST_ASSERT_EQUAL_UINT8(RTFS_FT_REG_FILE, view->file_type);

    free(view);
}

RTFS_TEST(RuntimeInodeViewClone_ShouldReturnIndependentCopy)
{
    RtfsRuntimeInodeView source;
    RtfsRuntimeInodeView *clone;

    rtfsRuntimeInodeViewInit(&source, 222, 111, RTFS_FT_DIR);
    clone = rtfsRuntimeInodeViewClone(&source);

    TEST_ASSERT_NOT_NULL(clone);
    TEST_ASSERT_EQUAL_UINT64(source.ino, clone->ino);
    TEST_ASSERT_EQUAL_UINT64(source.parent_ino, clone->parent_ino);
    TEST_ASSERT_EQUAL_UINT8(source.file_type, clone->file_type);

    source.ino = 999;
    source.parent_ino = 888;
    source.file_type = RTFS_FT_REG_FILE;

    TEST_ASSERT_EQUAL_UINT64(222u, clone->ino);
    TEST_ASSERT_EQUAL_UINT64(111u, clone->parent_ino);
    TEST_ASSERT_EQUAL_UINT8(RTFS_FT_DIR, clone->file_type);

    free(clone);
}

RTFS_TEST(RuntimeInodeViewClone_WhenSourceIsNull_ShouldReturnNull)
{
    TEST_ASSERT_NULL(rtfsRuntimeInodeViewClone(NULL));
}

RTFS_TEST(RuntimeInodeIsDirectoryType_ShouldMatchDirectoryFlag)
{
    TEST_ASSERT_TRUE(rtfsInodeIsDirectoryType(RTFS_FT_DIR));
    TEST_ASSERT_FALSE(rtfsInodeIsDirectoryType(RTFS_FT_REG_FILE));
}
