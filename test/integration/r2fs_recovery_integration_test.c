#include "integration/r2fs_integration_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"
#include "fs/cow_reclaim_registry.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#define R2FS_ITEST_REC_DIR "/rec"
#define R2FS_ITEST_REC_FILE "/rec/file.bin"
#define R2FS_ITEST_REC_FILE_RENAMED "/rec/file-renamed.bin"

static const unsigned char *r2fsIntegrationRecoveryRawBlockPtr(
    const R2fsIntegrationFixture *fixture,
    uint32_t lpa
)
{
    const R2fsIntegrationBlockStore *store = r2fsIntegrationFixtureBlockStore(fixture);

    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_NOT_NULL(store->bytes);
    TEST_ASSERT_TRUE((uint64_t)lpa < store->lpa_count);
    return store->bytes + (uint64_t)lpa * BLOCK_BUFFER_SIZE;
}

static const struct RtfsSuperBlock *r2fsIntegrationRecoveryRawSuperBlock(
    const R2fsIntegrationFixture *fixture
)
{
    return (const struct RtfsSuperBlock *)r2fsIntegrationRecoveryRawBlockPtr(
        fixture,
        0u
    );
}

static bool r2fsIntegrationRecoveryIsSitBitValid(
    const R2fsIntegrationFixture *fixture,
    uint32_t lpa
)
{
    const struct RtfsSuperBlock *super_block;
    const struct RtfsSitBlock *sit_block;
    uint32_t seg_id;
    uint32_t seg_off;
    uint32_t sit_lpa;
    uint32_t sit_idx;
    uint32_t byte_idx;
    uint32_t bit_off;

    TEST_ASSERT_NOT_EQUAL_UINT32(INVALID_LPA, lpa);

    super_block = r2fsIntegrationRecoveryRawSuperBlock(fixture);
    seg_id = (lpa - super_block->segment0_blkaddr) / BLOCK_PER_SEGMENT;
    seg_off = (lpa - super_block->segment0_blkaddr) % BLOCK_PER_SEGMENT;
    sit_lpa = super_block->sit_blkaddr + seg_id / SIT_ENTRY_PER_BLOCK;
    sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    byte_idx = seg_off / 8u;
    bit_off = seg_off % 8u;

    sit_block = (const struct RtfsSitBlock *)r2fsIntegrationRecoveryRawBlockPtr(
        fixture,
        sit_lpa
    );
    return (sit_block->entries[sit_idx].valid_map[byte_idx] & (1u << bit_off)) != 0u;
}

RTFS_TEST(IntegrationRecovery_CreateCrashRemount_ShouldKeepCommittedFileVisible)
{
    R2fsIntegrationFixture fixture;
    struct stat st;
    char expected[] = "recovery-create";
    char actual[sizeof(expected)];

    /*
     * 这条用例验证已提交创建事务的恢复语义：
     * crash remount 之后，已创建并写入完成的文件应继续可见，
     * 且文件大小与内容都应保持正确。
     */

    memset(actual, 0, sizeof(actual));

    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationFixtureFormatAndMount(&fixture, R2FS_ITEST_DISK_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsIntegrationMkdir(&fixture, R2FS_ITEST_REC_DIR, 0755));
    TEST_ASSERT_EQUAL(0, r2fsIntegrationCreateFile(&fixture, R2FS_ITEST_REC_FILE, 0644));
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationWriteFile(
            &fixture,
            R2FS_ITEST_REC_FILE,
            expected,
            sizeof(expected)
        )
    );

    r2fsIntegrationFixtureCrash(&fixture);
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureRemount(&fixture));

    TEST_ASSERT_EQUAL(0, r2fsIntegrationStatPath(&fixture, R2FS_ITEST_REC_FILE, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(expected), (uint32_t)st.st_size);
    TEST_ASSERT_EQUAL_INT(
        (int)sizeof(actual),
        (int)r2fsIntegrationReadAt(
            &fixture,
            R2FS_ITEST_REC_FILE,
            0,
            actual,
            sizeof(actual)
        )
    );
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(expected));

    r2fsIntegrationFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationRecovery_RenameCrashRemount_ShouldKeepCommittedResultVisible)
{
    R2fsIntegrationFixture fixture;
    struct stat before_st;
    struct stat after_st;

    /*
     * 这条用例验证已提交 rename 事务的恢复语义：
     * crash remount 之后，旧路径应保持不可见，新路径应继续可见，
     * 且对象 inode 身份不应发生变化。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationFixtureFormatAndMount(&fixture, R2FS_ITEST_DISK_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsIntegrationMkdir(&fixture, R2FS_ITEST_REC_DIR, 0755));
    TEST_ASSERT_EQUAL(0, r2fsIntegrationCreateFile(&fixture, R2FS_ITEST_REC_FILE, 0644));
    TEST_ASSERT_EQUAL(0, r2fsIntegrationStatPath(&fixture, R2FS_ITEST_REC_FILE, &before_st));

    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationRename(
            &fixture,
            R2FS_ITEST_REC_FILE,
            R2FS_ITEST_REC_FILE_RENAMED
        )
    );
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationStatPath(&fixture, R2FS_ITEST_REC_FILE_RENAMED, &after_st)
    );
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)before_st.st_ino,
        (uint32_t)after_st.st_ino
    );

    r2fsIntegrationFixtureCrash(&fixture);
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureRemount(&fixture));

    TEST_ASSERT_EQUAL(
        ENOENT,
        r2fsIntegrationStatPath(&fixture, R2FS_ITEST_REC_FILE, &after_st)
    );
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationStatPath(&fixture, R2FS_ITEST_REC_FILE_RENAMED, &after_st)
    );
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)before_st.st_ino,
        (uint32_t)after_st.st_ino
    );

    r2fsIntegrationFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationRecovery_IncompleteTransactionCrashRemount_ShouldHideUncommittedFile)
{
    R2fsIntegrationFixture fixture;
    struct stat st;

    /*
     * 这条用例验证未完整提交事务的恢复语义：
     * 若盘上的 journal 缺少事务 END 标记，则 replay 不应把该事务
     * 暴露到可见状态。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationFixtureFormatAndMount(&fixture, R2FS_ITEST_DISK_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsIntegrationMkdir(&fixture, R2FS_ITEST_REC_DIR, 0755));
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationFlushMetadataToStore(&fixture)
    );
    r2fsIntegrationFixtureUnmount(&fixture);
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureRemount(&fixture));

    TEST_ASSERT_EQUAL(0, r2fsIntegrationCreateFile(&fixture, R2FS_ITEST_REC_FILE, 0644));
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureCorruptLatestJournalEndEntry(&fixture));

    r2fsIntegrationFixtureCrash(&fixture);
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureRemount(&fixture));
    TEST_ASSERT_EQUAL(0, r2fsIntegrationStatPath(&fixture, R2FS_ITEST_REC_DIR, &st));
    TEST_ASSERT_EQUAL(
        ENOENT,
        r2fsIntegrationStatPath(&fixture, R2FS_ITEST_REC_FILE, &st)
    );

    r2fsIntegrationFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationRecovery_ReclaimAfterCrashRemount_ShouldReleaseOldCowLpas)
{
    R2fsIntegrationFixture fixture;
    uint32_t old_inode_lpa = INVALID_LPA;
    uint32_t old_data_lpa = INVALID_LPA;
    uint32_t new_inode_lpa = INVALID_LPA;
    uint32_t new_data_lpa = INVALID_LPA;
    char initial[BLOCK_BUFFER_SIZE];
    char overwrite[BLOCK_BUFFER_SIZE];

    /*
     * 这条用例验证 recovery 之后的 reclaim 语义：
     * COW 覆盖生成的新 LPA 在 crash remount 后应继续保留，
     * 旧 LPA 则应在 replay 与 reclaim 完成后被释放。
     */

    memset(initial, 'A', sizeof(initial));
    memset(overwrite, 'B', sizeof(overwrite));

    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationFixtureFormatAndMount(&fixture, R2FS_ITEST_DISK_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsIntegrationMkdir(&fixture, R2FS_ITEST_REC_DIR, 0755));
    TEST_ASSERT_EQUAL(0, r2fsIntegrationCreateFile(&fixture, R2FS_ITEST_REC_FILE, 0644));
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationWriteFile(
            &fixture,
            R2FS_ITEST_REC_FILE,
            initial,
            sizeof(initial)
        )
    );
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationReadCurrentFileMapping(
            &fixture,
            R2FS_ITEST_REC_FILE,
            NULL,
            &old_inode_lpa,
            &old_data_lpa
        )
    );
    TEST_ASSERT_NOT_EQUAL_UINT32(INVALID_LPA, old_data_lpa);

    TEST_ASSERT_EQUAL(0, r2fsIntegrationFlushMetadataToStore(&fixture));
    r2fsIntegrationFixtureUnmount(&fixture);
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureRemount(&fixture));

    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationWriteAt(
            &fixture,
            R2FS_ITEST_REC_FILE,
            0,
            overwrite,
            sizeof(overwrite)
        )
    );
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationReadCurrentFileMapping(
            &fixture,
            R2FS_ITEST_REC_FILE,
            NULL,
            &new_inode_lpa,
            &new_data_lpa
        )
    );
    TEST_ASSERT_NOT_EQUAL_UINT32(old_inode_lpa, new_inode_lpa);
    TEST_ASSERT_NOT_EQUAL_UINT32(old_data_lpa, new_data_lpa);

    r2fsIntegrationFixtureCrash(&fixture);
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureRemount(&fixture));

    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFlushMetadataToStore(&fixture));

    TEST_ASSERT_TRUE(r2fsIntegrationRecoveryIsSitBitValid(&fixture, new_inode_lpa));
    TEST_ASSERT_TRUE(r2fsIntegrationRecoveryIsSitBitValid(&fixture, new_data_lpa));
    TEST_ASSERT_FALSE(r2fsIntegrationRecoveryIsSitBitValid(&fixture, old_inode_lpa));
    TEST_ASSERT_FALSE(r2fsIntegrationRecoveryIsSitBitValid(&fixture, old_data_lpa));

    r2fsIntegrationFixtureDestroy(&fixture);
}
