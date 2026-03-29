#include "rtfs_test.h"

#include "fs/fs_manager.h"
#include "fs/srmap_utils.h"
#include "cache/sit_nat_cache.h"


static void fsManagerReset(void)
{
    FileSystemManagerFini();
}


RTFS_TEST(FsManagerGetInstance_BeforeSetup_ShouldBeNull)
{
    fsManagerReset();

    TEST_ASSERT_NULL(FileSystemManagerGetInstance());
}

RTFS_TEST(FsManagerSetup_ShouldCreateSingletonAndInitializeSubModules)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, FileSystemManagerSetup(NULL));

    file_system_manager *manager = FileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    SitNatCache *sit_cache = FileSystemManagerGetSitCache(manager);
    SitNatCache *nat_cache = FileSystemManagerGetNatCache(manager);
    SrmapUtils *srmap_utils = FileSystemManagerGetSrmapUtils(manager);

    TEST_ASSERT_NOT_NULL(sit_cache);
    TEST_ASSERT_NOT_NULL(nat_cache);
    TEST_ASSERT_NOT_NULL(srmap_utils);
    TEST_ASSERT(sit_cache != nat_cache);
    TEST_ASSERT_EQUAL_PTR(manager, srmap_utils->fsManager);

    TEST_ASSERT_NULL(sit_cache->dev);
    TEST_ASSERT_NULL(nat_cache->dev);
    TEST_ASSERT_EQUAL(100, sit_cache->expectSize);
    TEST_ASSERT_EQUAL(100, nat_cache->expectSize);
    TEST_ASSERT_EQUAL(0, sit_cache->curSize);
    TEST_ASSERT_EQUAL(0, nat_cache->curSize);
    TEST_ASSERT_NOT_NULL(srmap_utils->srmapCache);
    TEST_ASSERT_NOT_NULL(srmap_utils->dirtyBlks);

    fsManagerReset();
}

RTFS_TEST(FsManagerGetters_AfterSetup_ShouldReflectCurrentAssemblyState)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, FileSystemManagerSetup(NULL));

    file_system_manager *manager = FileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    TEST_ASSERT_NULL(FileSystemManagerGetSuperBlkMem(manager));
    TEST_ASSERT_NULL(FileSystemManagerGetSuperManager(manager));
    TEST_ASSERT_NULL(FileSystemManagerGetNodeCache(manager));
    TEST_ASSERT_NULL(FileSystemManagerGetDirDataCache(manager));
    TEST_ASSERT_NOT_NULL(FileSystemManagerGetSitCache(manager));
    TEST_ASSERT_NOT_NULL(FileSystemManagerGetNatCache(manager));
    TEST_ASSERT_NOT_NULL(FileSystemManagerGetSrmapUtils(manager));
    TEST_ASSERT_NULL(FileSystemManagerGetFdArray(manager));
    TEST_ASSERT_NULL(FileSystemManagerGetCurJournal(manager));

    fsManagerReset();
}

RTFS_TEST(FsManagerGetters_WhenManagerIsNull_ShouldReturnNull)
{
    TEST_ASSERT_NULL(FileSystemManagerGetSuperBlkMem(NULL));
    TEST_ASSERT_NULL(FileSystemManagerGetSuperManager(NULL));
    TEST_ASSERT_NULL(FileSystemManagerGetNodeCache(NULL));
    TEST_ASSERT_NULL(FileSystemManagerGetDirDataCache(NULL));
    TEST_ASSERT_NULL(FileSystemManagerGetSitCache(NULL));
    TEST_ASSERT_NULL(FileSystemManagerGetNatCache(NULL));
    TEST_ASSERT_NULL(FileSystemManagerGetSrmapUtils(NULL));
    TEST_ASSERT_NULL(FileSystemManagerGetFdArray(NULL));
    TEST_ASSERT_NULL(FileSystemManagerGetCurJournal(NULL));
}

RTFS_TEST(FsManagerSetup_WhenCalledTwice_ShouldRejectSecondInitialization)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, FileSystemManagerSetup(NULL));
    TEST_ASSERT_EQUAL(-1, FileSystemManagerSetup(NULL));
    TEST_ASSERT_NOT_NULL(FileSystemManagerGetInstance());

    fsManagerReset();
}

RTFS_TEST(FsManagerFini_AfterSetup_ShouldReleaseSingleton)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, FileSystemManagerSetup(NULL));
    TEST_ASSERT_NOT_NULL(FileSystemManagerGetInstance());

    FileSystemManagerFini();

    TEST_ASSERT_NULL(FileSystemManagerGetInstance());

    fsManagerReset();
}

RTFS_TEST(FsManagerFini_WhenCalledRepeatedly_ShouldRemainSafe)
{
    fsManagerReset();

    FileSystemManagerFini();
    TEST_ASSERT_NULL(FileSystemManagerGetInstance());

    TEST_ASSERT_EQUAL(0, FileSystemManagerSetup(NULL));
    TEST_ASSERT_NOT_NULL(FileSystemManagerGetInstance());

    FileSystemManagerFini();
    FileSystemManagerFini();

    TEST_ASSERT_NULL(FileSystemManagerGetInstance());
}

RTFS_TEST(FsManagerSetup_AfterFini_ShouldAllowRecreation)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, FileSystemManagerSetup(NULL));
    file_system_manager *first = FileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(first);

    FileSystemManagerFini();
    TEST_ASSERT_NULL(FileSystemManagerGetInstance());

    TEST_ASSERT_EQUAL(0, FileSystemManagerSetup(NULL));
    file_system_manager *second = FileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_NOT_NULL(FileSystemManagerGetSitCache(second));
    TEST_ASSERT_NOT_NULL(FileSystemManagerGetNatCache(second));
    TEST_ASSERT_NOT_NULL(FileSystemManagerGetSrmapUtils(second));

    fsManagerReset();
}

RTFS_TEST(FsManagerLockApis_WhenManagerIsNull_ShouldBeSafe)
{
    FileSystemManagerMetaLock(NULL);
    FileSystemManagerMetaUnlock(NULL);
    FileSystemManagerFreezeLock(NULL);
    FileSystemManagerFreezeUnLock(NULL);

    TEST_PASS();
}

RTFS_TEST(FsManagerMetaLock_ShouldSupportRecursiveLocking)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, FileSystemManagerSetup(NULL));

    file_system_manager *manager = FileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    FileSystemManagerMetaLock(manager);
    FileSystemManagerMetaLock(manager);
    FileSystemManagerMetaUnlock(manager);
    FileSystemManagerMetaUnlock(manager);

    fsManagerReset();
}

RTFS_TEST(FsManagerFreezeLock_ShouldSupportBasicLockUnlock)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, FileSystemManagerSetup(NULL));

    file_system_manager *manager = FileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    FileSystemManagerFreezeLock(manager);
    FileSystemManagerFreezeUnLock(manager);

    fsManagerReset();
}
