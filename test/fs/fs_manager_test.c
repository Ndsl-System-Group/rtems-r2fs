#include "rtfs_test.h"

#include "fs/fs_manager.h"
#include "fs/srmap_utils.h"
#include "cache/sit_nat_cache.h"


static void fsManagerReset(void)
{
    fileSystemManagerFini();
}


RTFS_TEST(FsManagerGetInstance_BeforeSetup_ShouldBeNull)
{
    fsManagerReset();

    TEST_ASSERT_NULL(fileSystemManagerGetInstance());
}

RTFS_TEST(FsManagerSetup_ShouldCreateSingletonAndInitializeSubModules)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(NULL));

    file_system_manager *manager = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    SitNatCache *sit_cache = fileSystemManagerGetSitCache(manager);
    SitNatCache *nat_cache = fileSystemManagerGetNatCache(manager);
    SrmapUtils *srmap_utils = fileSystemManagerGetSrmapUtils(manager);

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

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(NULL));

    file_system_manager *manager = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    TEST_ASSERT_NULL(fileSystemManagerGetSuperBlkMem(manager));
    TEST_ASSERT_NULL(fileSystemManagerGetSuperManager(manager));
    TEST_ASSERT_NULL(fileSystemManagerGetNodeCache(manager));
    TEST_ASSERT_NULL(fileSystemManagerGetDirDataCache(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSitCache(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetNatCache(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSrmapUtils(manager));
    TEST_ASSERT_NULL(fileSystemManagerGetFdArray(manager));
    TEST_ASSERT_NULL(fileSystemManagerGetCurJournal(manager));

    fsManagerReset();
}

RTFS_TEST(FsManagerGetters_WhenManagerIsNull_ShouldReturnNull)
{
    TEST_ASSERT_NULL(fileSystemManagerGetSuperBlkMem(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetSuperManager(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetNodeCache(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetDirDataCache(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetSitCache(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetNatCache(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetSrmapUtils(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetFdArray(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetCurJournal(NULL));
}

RTFS_TEST(FsManagerSetup_WhenCalledTwice_ShouldRejectSecondInitialization)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(NULL));
    TEST_ASSERT_EQUAL(-1, fileSystemManagerSetup(NULL));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetInstance());

    fsManagerReset();
}

RTFS_TEST(FsManagerFini_AfterSetup_ShouldReleaseSingleton)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(NULL));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetInstance());

    fileSystemManagerFini();

    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    fsManagerReset();
}

RTFS_TEST(FsManagerFini_WhenCalledRepeatedly_ShouldRemainSafe)
{
    fsManagerReset();

    fileSystemManagerFini();
    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(NULL));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetInstance());

    fileSystemManagerFini();
    fileSystemManagerFini();

    TEST_ASSERT_NULL(fileSystemManagerGetInstance());
}

RTFS_TEST(FsManagerSetup_AfterFini_ShouldAllowRecreation)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(NULL));
    file_system_manager *first = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(first);

    fileSystemManagerFini();
    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(NULL));
    file_system_manager *second = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSitCache(second));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetNatCache(second));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSrmapUtils(second));

    fsManagerReset();
}

RTFS_TEST(FsManagerLockApis_WhenManagerIsNull_ShouldBeSafe)
{
    fileSystemManagerMetaLock(NULL);
    fileSystemManagerMetaUnlock(NULL);
    fileSystemManagerFreezeLock(NULL);
    fileSystemManagerFreezeUnLock(NULL);

    TEST_PASS();
}

RTFS_TEST(FsManagerMetaLock_ShouldSupportRecursiveLocking)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(NULL));

    file_system_manager *manager = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    fileSystemManagerMetaLock(manager);
    fileSystemManagerMetaLock(manager);
    fileSystemManagerMetaUnlock(manager);
    fileSystemManagerMetaUnlock(manager);

    fsManagerReset();
}

RTFS_TEST(FsManagerFreezeLock_ShouldSupportBasicLockUnlock)
{
    fsManagerReset();

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(NULL));

    file_system_manager *manager = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    fileSystemManagerFreezeLock(manager);
    fileSystemManagerFreezeUnLock(manager);

    fsManagerReset();
}
