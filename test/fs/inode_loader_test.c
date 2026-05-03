#include "rtfs_test.h"

#include "fs/inode/inode_loader.h"


RTFS_TEST(InodeLoaderEnsureCached_WhenFsManagerIsNull_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(EINVAL, rtfsInodeLoaderEnsureCached(NULL, 1));
}

RTFS_TEST(InodeLoaderEnsureCached_WhenInoIsInvalid_ShouldReturnEINVAL)
{
    TEST_ASSERT_EQUAL(EINVAL, rtfsInodeLoaderEnsureCached(NULL, INVALID_NID));
}
