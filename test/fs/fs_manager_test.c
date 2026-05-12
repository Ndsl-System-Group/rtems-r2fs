#include "rtfs_test.h"

#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "communication/dev.h"
#include "fs/fs_manager.h"
#include "fs/srmap_utils.h"
#include "fs/super_manager.h"
#include "journal/journal_container.h"
#include "journal/journal_process_env.h"

#include <pthread.h>
#include <rtems/thread.h>
#include <string.h>

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

typedef struct FsManagerFixture
{
    comm_dev dev;
    rtems_disk_device disk;
    struct RtfsSuperBlock super_block;
    uint32_t read_count;
    uint32_t last_read_lpa;
} FsManagerFixture;

static FsManagerFixture *g_fs_manager_fixture = NULL;

static int fsManagerReadSuperBlockHook(
    struct comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    (void)dev;

    if (g_fs_manager_fixture == NULL || buffer == NULL) {
        return EIO;
    }

    g_fs_manager_fixture->read_count++;
    g_fs_manager_fixture->last_read_lpa = lpa;
    memcpy(buffer, &g_fs_manager_fixture->super_block, sizeof(g_fs_manager_fixture->super_block));
    return 0;
}

static void fsManagerFixtureInit(FsManagerFixture *fixture)
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
}

static void fsManagerFixtureFini(FsManagerFixture *fixture)
{
    if (fixture != NULL) {
        commDevDestroy(&fixture->dev);
    }
}


static void fsManagerReset(void)
{
    fileSystemManagerSetSetupFailureStepForTest(0);
    superCacheSetReadBlockHook(NULL);
    g_fs_manager_fixture = NULL;
    fileSystemManagerFini();
}


RTFS_TEST(FsManagerGetInstance_BeforeSetup_ShouldBeNull)
{
    fsManagerReset();

    TEST_ASSERT_NULL(fileSystemManagerGetInstance());
}

RTFS_TEST(FsManagerSetup_ShouldCreateSingletonAndInitializeSubModules)
{
    FsManagerFixture fixture;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&fixture.dev));

    file_system_manager *manager = fileSystemManagerGetInstance();
    SitNatCache *sit_cache = fileSystemManagerGetSitCache(manager);
    SitNatCache *nat_cache = fileSystemManagerGetNatCache(manager);
    SrmapUtils *srmap_utils = fileSystemManagerGetSrmapUtils(manager);

    TEST_ASSERT_NOT_NULL(manager);
    TEST_ASSERT_NOT_NULL(sit_cache);
    TEST_ASSERT_NOT_NULL(nat_cache);
    TEST_ASSERT_NOT_NULL(srmap_utils);
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetNodeCache(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSuperManager(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetCurJournal(manager));
    TEST_ASSERT_EQUAL_PTR(manager, srmap_utils->fsManager);

    TEST_ASSERT_EQUAL_PTR(&fixture.dev, sit_cache->dev);
    TEST_ASSERT_EQUAL_PTR(&fixture.dev, nat_cache->dev);
    TEST_ASSERT_EQUAL(100, sit_cache->expectSize);
    TEST_ASSERT_EQUAL(100, nat_cache->expectSize);
    TEST_ASSERT_EQUAL(0, sit_cache->curSize);
    TEST_ASSERT_EQUAL(0, nat_cache->curSize);
    TEST_ASSERT_NOT_NULL(srmap_utils->srmapCache);
    TEST_ASSERT_NOT_NULL(srmap_utils->dirtyBlks);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.read_count);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.last_read_lpa);

    fsManagerReset();
    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerGetters_AfterSetup_ShouldReflectCurrentAssemblyState)
{
    FsManagerFixture fixture;
    file_system_manager *manager;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&fixture.dev));

    manager = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    TEST_ASSERT_EQUAL_PTR(&fixture.dev, fileSystemManagerGetDevice(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSuperBlkMem(manager));
    TEST_ASSERT_EQUAL_UINT32(
        fixture.super_block.meta_journal_blkaddr,
        fileSystemManagerGetSuperBlkMem(manager)->meta_journal_blkaddr
    );
    TEST_ASSERT_EQUAL_UINT32(
        fixture.super_block.srmap_blkaddr,
        fileSystemManagerGetSrmapUtils(manager)->srmapStartLpa
    );
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSuperManager(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetNodeCache(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSitCache(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetNatCache(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSrmapUtils(manager));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetCurJournal(manager));

    fsManagerReset();
    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerGetters_WhenManagerIsNull_ShouldReturnNull)
{
    TEST_ASSERT_NULL(fileSystemManagerGetSuperBlkMem(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetSuperManager(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetNodeCache(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetSitCache(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetNatCache(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetSrmapUtils(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetCurJournal(NULL));
    TEST_ASSERT_NULL(fileSystemManagerGetDevice(NULL));
}

RTFS_TEST(FsManagerSetup_WhenCalledTwice_ShouldRejectSecondInitialization)
{
    FsManagerFixture fixture;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&fixture.dev));
    TEST_ASSERT_EQUAL(-1, fileSystemManagerSetup(&fixture.dev));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetInstance());
    TEST_ASSERT_EQUAL_UINT32(1, fixture.read_count);

    fsManagerReset();
    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerFini_AfterSetup_ShouldReleaseSingleton)
{
    FsManagerFixture fixture;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&fixture.dev));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetInstance());

    fileSystemManagerFini();
    superCacheSetReadBlockHook(NULL);
    g_fs_manager_fixture = NULL;

    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerFini_WhenCalledRepeatedly_ShouldRemainSafe)
{
    FsManagerFixture fixture;

    fsManagerReset();
    fileSystemManagerFini();
    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&fixture.dev));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetInstance());

    fileSystemManagerFini();
    fileSystemManagerFini();
    superCacheSetReadBlockHook(NULL);
    g_fs_manager_fixture = NULL;

    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerSetup_AfterFini_ShouldAllowRecreation)
{
    FsManagerFixture first_fixture;
    FsManagerFixture second_fixture;
    file_system_manager *first;
    file_system_manager *second;

    fsManagerReset();

    fsManagerFixtureInit(&first_fixture);
    g_fs_manager_fixture = &first_fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&first_fixture.dev));
    first = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(first);

    fileSystemManagerFini();
    TEST_ASSERT_NULL(fileSystemManagerGetInstance());
    fsManagerFixtureFini(&first_fixture);

    fsManagerFixtureInit(&second_fixture);
    g_fs_manager_fixture = &second_fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&second_fixture.dev));
    second = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSitCache(second));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetNatCache(second));
    TEST_ASSERT_NOT_NULL(fileSystemManagerGetSrmapUtils(second));

    fsManagerReset();
    fsManagerFixtureFini(&second_fixture);
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
    FsManagerFixture fixture;
    file_system_manager *manager;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&fixture.dev));

    manager = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    fileSystemManagerMetaLock(manager);
    fileSystemManagerMetaLock(manager);
    fileSystemManagerMetaUnlock(manager);
    fileSystemManagerMetaUnlock(manager);

    fsManagerReset();
    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerFreezeLock_ShouldSupportBasicLockUnlock)
{
    FsManagerFixture fixture;
    file_system_manager *manager;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&fixture.dev));

    manager = fileSystemManagerGetInstance();
    TEST_ASSERT_NOT_NULL(manager);

    fileSystemManagerFreezeLock(manager);
    fileSystemManagerFreezeUnLock(manager);

    fsManagerReset();
    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerSetup_WhenSitCacheAssemblyFails_ShouldRollbackAndLeaveSingletonNull)
{
    FsManagerFixture fixture;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);
    fileSystemManagerSetSetupFailureStepForTest(2);

    TEST_ASSERT_EQUAL(-ENOMEM, fileSystemManagerSetup(&fixture.dev));
    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    fsManagerReset();
    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerSetup_WhenSuperManagerAssemblyFails_ShouldRollbackAndLeaveSingletonNull)
{
    FsManagerFixture fixture;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);
    fileSystemManagerSetSetupFailureStepForTest(6);

    TEST_ASSERT_EQUAL(-ENOMEM, fileSystemManagerSetup(&fixture.dev));
    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    fsManagerReset();
    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerSetup_WhenJournalEnvAssemblyFails_ShouldRollbackAndLeaveSingletonNull)
{
    FsManagerFixture fixture;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);
    fileSystemManagerSetSetupFailureStepForTest(8);

    TEST_ASSERT_EQUAL(-ENOMEM, fileSystemManagerSetup(&fixture.dev));
    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    fsManagerReset();
    fsManagerFixtureFini(&fixture);
}

RTFS_TEST(FsManagerFini_AfterSetup_ShouldResetJournalEnvAndLeaveCommitQueueEmpty)
{
    FsManagerFixture fixture;
    JournalProcessEnv *env;

    fsManagerReset();
    fsManagerFixtureInit(&fixture);
    g_fs_manager_fixture = &fixture;
    superCacheSetReadBlockHook(fsManagerReadSuperBlockHook);

    TEST_ASSERT_EQUAL(0, fileSystemManagerSetup(&fixture.dev));

    env = journalProcessEnvGetInstance();
    TEST_ASSERT_NOT_NULL(env);
    TEST_ASSERT_TRUE(journalProcessEnvIsCommitQueueEmpty(env));
    TEST_ASSERT_FALSE(journalProcessEnvIsExitRequested(env));

    fileSystemManagerFini();
    superCacheSetReadBlockHook(NULL);
    g_fs_manager_fixture = NULL;

    TEST_ASSERT_TRUE(journalProcessEnvIsCommitQueueEmpty(env));
    TEST_ASSERT_FALSE(journalProcessEnvIsExitRequested(env));
    TEST_ASSERT_NULL(fileSystemManagerGetInstance());

    fsManagerFixtureFini(&fixture);
}
