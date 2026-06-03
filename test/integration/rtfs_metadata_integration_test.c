#include "integration/rtfs_integration_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"
#include "fs/cow_reclaim_registry.h"
#include "journal/journal_process_env.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define RTFS_ITEST_META_PARENT "/meta"
#define RTFS_ITEST_META_FILE "/meta/a.txt"

static const unsigned char *rtfsIntegrationRawBlockPtr(
    const RtfsIntegrationFixture *fixture,
    uint32_t lpa)
{
    const RtfsIntegrationBlockStore *store = rtfsIntegrationFixtureBlockStore(fixture);

    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_NOT_NULL(store->bytes);
    TEST_ASSERT_TRUE((uint64_t)lpa < store->lpa_count);
    return store->bytes + (uint64_t)lpa * BLOCK_BUFFER_SIZE;
}

static const struct RtfsSuperBlock *rtfsIntegrationRawSuperBlock(
    const RtfsIntegrationFixture *fixture)
{
    return (const struct RtfsSuperBlock *)rtfsIntegrationRawBlockPtr(fixture, 0);
}

static struct RtfsNatEntry rtfsIntegrationReadNatEntry(
    const RtfsIntegrationFixture *fixture,
    uint32_t nid)
{
    const struct RtfsSuperBlock *super_block;
    const struct RtfsNatBlock *nat_block;
    uint32_t nat_lpa;
    uint32_t nat_idx;

    super_block = rtfsIntegrationRawSuperBlock(fixture);
    nat_lpa = super_block->nat_blkaddr + nid / NAT_ENTRY_PER_BLOCK;
    nat_idx = nid % NAT_ENTRY_PER_BLOCK;
    nat_block = (const struct RtfsNatBlock *)rtfsIntegrationRawBlockPtr(
        fixture,
        nat_lpa);
    return nat_block->entries[nat_idx];
}

static const struct RtfsNode *rtfsIntegrationReadNodeAtLpa(
    const RtfsIntegrationFixture *fixture,
    uint32_t lpa)
{
    TEST_ASSERT_NOT_EQUAL_UINT32(INVALID_LPA, lpa);
    return (const struct RtfsNode *)rtfsIntegrationRawBlockPtr(fixture, lpa);
}

static struct RtfsSummary rtfsIntegrationReadSrmapEntry(
    const RtfsIntegrationFixture *fixture,
    uint32_t lpa)
{
    const struct RtfsSuperBlock *super_block;
    const struct RtfsSummaryBlock *summary_block;
    uint32_t srmap_lpa;
    uint32_t srmap_idx;

    super_block = rtfsIntegrationRawSuperBlock(fixture);
    srmap_lpa = super_block->srmap_blkaddr + lpa / ENTRIES_IN_SUM;
    srmap_idx = lpa % ENTRIES_IN_SUM;
    summary_block = (const struct RtfsSummaryBlock *)rtfsIntegrationRawBlockPtr(
        fixture,
        srmap_lpa);
    return summary_block->entries[srmap_idx];
}

static bool rtfsIntegrationIsSitBitValid(
    const RtfsIntegrationFixture *fixture,
    uint32_t lpa)
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

    super_block = rtfsIntegrationRawSuperBlock(fixture);
    seg_id = (lpa - super_block->segment0_blkaddr) / BLOCK_PER_SEGMENT;
    seg_off = (lpa - super_block->segment0_blkaddr) % BLOCK_PER_SEGMENT;
    sit_lpa = super_block->sit_blkaddr + seg_id / SIT_ENTRY_PER_BLOCK;
    sit_idx = seg_id % SIT_ENTRY_PER_BLOCK;
    byte_idx = seg_off / 8u;
    bit_off = seg_off % 8u;

    sit_block = (const struct RtfsSitBlock *)rtfsIntegrationRawBlockPtr(
        fixture,
        sit_lpa);
    return (sit_block->entries[sit_idx].valid_map[byte_idx] & (1u << bit_off)) != 0u;
}

static uint32_t rtfsIntegrationReadFirstDataLpaForPath(
    RtfsIntegrationFixture *fixture,
    const char *path,
    uint32_t *out_ino,
    uint32_t *out_inode_lpa)
{
    struct stat st;
    struct RtfsNatEntry nat_entry;
    const struct RtfsNode *inode_node;

    TEST_ASSERT_EQUAL(0, rtfsIntegrationStatPath(fixture, path, &st));
    nat_entry = rtfsIntegrationReadNatEntry(fixture, (uint32_t)st.st_ino);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)st.st_ino, nat_entry.ino);
    TEST_ASSERT_NOT_EQUAL_UINT32(INVALID_LPA, nat_entry.block_addr);

    inode_node = rtfsIntegrationReadNodeAtLpa(fixture, nat_entry.block_addr);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)st.st_ino, inode_node->footer.nid);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)st.st_ino, inode_node->footer.ino);
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));

    if (out_ino != NULL)
    {
        *out_ino = (uint32_t)st.st_ino;
    }
    if (out_inode_lpa != NULL)
    {
        *out_inode_lpa = nat_entry.block_addr;
    }
    return inode_node->i.i_addr[0];
}

static void rtfsIntegrationWaitForCommittedJournal(void)
{
    JournalProcessEnv *env = journalProcessEnvGetInstance();
    uint64_t next_tx_id;

    TEST_ASSERT_NOT_NULL(env);
    next_tx_id = atomic_load_explicit(&env->txIdToAlloc, memory_order_relaxed);
    TEST_ASSERT_NOT_EQUAL_UINT64(0u, next_tx_id);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(2u, next_tx_id);
    cowReclaimRegistryOnTxComplete(next_tx_id - 1u);
}

RTFS_TEST(IntegrationMetadata_StatvfsCreateRemoveReclaim_ShouldTrackFreeFileSlots)
{
    RtfsIntegrationFixture fixture;
    struct statvfs before_stvfs;
    struct statvfs after_create_stvfs;
    struct statvfs after_unlink_stvfs;
    struct statvfs after_reclaim_stvfs;
    struct stat st;
    char payload[] = "meta-payload";

    /*
     * 这条用例验证 statvfs 空闲 inode/文件槽位统计的精确语义：
     * 创建目录与文件后计数应按对象数下降；unlink 之后回收前
     * 计数不应立即恢复；journal 提交并完成 reclaim 后，计数
     * 才应精确回升。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationFixtureFormatAndMount(&fixture, RTFS_ITEST_DISK_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsIntegrationStatvfsRoot(&fixture, &before_stvfs));
    TEST_ASSERT_EQUAL(0, rtfsIntegrationMkdir(&fixture, RTFS_ITEST_META_PARENT, 0755));
    TEST_ASSERT_EQUAL(0, rtfsIntegrationCreateFile(&fixture, RTFS_ITEST_META_FILE, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationWriteFile(
            &fixture,
            RTFS_ITEST_META_FILE,
            payload,
            sizeof(payload)));
    TEST_ASSERT_EQUAL(0, rtfsIntegrationStatPath(&fixture, RTFS_ITEST_META_FILE, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    TEST_ASSERT_EQUAL(0, rtfsIntegrationStatvfsRoot(&fixture, &after_create_stvfs));

    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(before_stvfs.f_ffree - 2u),
        (uint32_t)after_create_stvfs.f_ffree);

    TEST_ASSERT_EQUAL(0, rtfsIntegrationRemove(&fixture, RTFS_ITEST_META_FILE));
    TEST_ASSERT_EQUAL(ENOENT, rtfsIntegrationStatPath(&fixture, RTFS_ITEST_META_FILE, &st));
    TEST_ASSERT_EQUAL(0, rtfsIntegrationStatvfsRoot(&fixture, &after_unlink_stvfs));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)after_create_stvfs.f_ffree,
        (uint32_t)after_unlink_stvfs.f_ffree);

    rtfsIntegrationWaitForCommittedJournal();
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL(0, rtfsIntegrationStatvfsRoot(&fixture, &after_reclaim_stvfs));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(after_unlink_stvfs.f_ffree + 1u),
        (uint32_t)after_reclaim_stvfs.f_ffree);

    rtfsIntegrationFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationMetadata_SingleWriteNatSitSrmap_ShouldStayConsistent)
{
    RtfsIntegrationFixture fixture;
    uint32_t ino = 0;
    uint32_t inode_lpa = INVALID_LPA;
    uint32_t data_lpa;
    struct RtfsSummary node_srmap;
    struct RtfsSummary data_srmap;
    char payload[BLOCK_BUFFER_SIZE];

    /*
     * 这条用例验证单次文件写入后的元数据联动一致性：
     * 路径对应 inode 的 NAT 映射、inode/data block 的 SIT 有效位、
     * 以及 SRMAP 反向映射都应指向同一组真实对象。
     */

    memset(payload, 'M', sizeof(payload));

    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationFixtureFormatAndMount(&fixture, RTFS_ITEST_DISK_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsIntegrationMkdir(&fixture, RTFS_ITEST_META_PARENT, 0755));
    TEST_ASSERT_EQUAL(0, rtfsIntegrationCreateFile(&fixture, RTFS_ITEST_META_FILE, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationWriteFile(
            &fixture,
            RTFS_ITEST_META_FILE,
            payload,
            sizeof(payload)));
    TEST_ASSERT_EQUAL(0, rtfsIntegrationFlushMetadataToStore(&fixture));

    data_lpa = rtfsIntegrationReadFirstDataLpaForPath(
        &fixture,
        RTFS_ITEST_META_FILE,
        &ino,
        &inode_lpa);
    TEST_ASSERT_NOT_EQUAL_UINT32(INVALID_LPA, data_lpa);

    TEST_ASSERT_TRUE(rtfsIntegrationIsSitBitValid(&fixture, inode_lpa));
    TEST_ASSERT_TRUE(rtfsIntegrationIsSitBitValid(&fixture, data_lpa));

    node_srmap = rtfsIntegrationReadSrmapEntry(&fixture, inode_lpa);
    TEST_ASSERT_EQUAL_UINT32(ino, node_srmap.nid);

    data_srmap = rtfsIntegrationReadSrmapEntry(&fixture, data_lpa);
    TEST_ASSERT_EQUAL_UINT32(ino, data_srmap.nid);
    TEST_ASSERT_EQUAL_UINT32(0u, data_srmap.ofs_in_node);

    rtfsIntegrationFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationMetadata_CowOverwriteBeforeReclaim_ShouldKeepOldLpaValidUntilTxComplete)
{
    RtfsIntegrationFixture fixture;
    uint32_t ino = 0;
    uint32_t old_inode_lpa = INVALID_LPA;
    uint32_t old_data_lpa;
    uint32_t new_inode_lpa = INVALID_LPA;
    uint32_t new_data_lpa;
    char initial[BLOCK_BUFFER_SIZE];
    char overwrite[BLOCK_BUFFER_SIZE];

    /*
     * 这条用例验证 COW 覆盖后的回收时序语义：
     * 新 inode/data LPA 生成之后，旧 LPA 在事务完成前必须继续有效；
     * journal 提交并完成 reclaim 后，旧 LPA 才应失效，新 LPA
     * 则应继续保持有效。
     */

    memset(initial, 'A', sizeof(initial));
    memset(overwrite, 'B', sizeof(overwrite));

    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationFixtureFormatAndMount(&fixture, RTFS_ITEST_DISK_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsIntegrationMkdir(&fixture, RTFS_ITEST_META_PARENT, 0755));
    TEST_ASSERT_EQUAL(0, rtfsIntegrationCreateFile(&fixture, RTFS_ITEST_META_FILE, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationWriteFile(
            &fixture,
            RTFS_ITEST_META_FILE,
            initial,
            sizeof(initial)));

    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationReadCurrentFileMapping(
            &fixture,
            RTFS_ITEST_META_FILE,
            &ino,
            &old_inode_lpa,
            &old_data_lpa));
    TEST_ASSERT_NOT_EQUAL_UINT32(INVALID_LPA, old_data_lpa);

    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationWriteAt(
            &fixture,
            RTFS_ITEST_META_FILE,
            0,
            overwrite,
            sizeof(overwrite)));
    TEST_ASSERT_EQUAL(0, rtfsIntegrationFlushMetadataToStore(&fixture));

    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationReadCurrentFileMapping(
            &fixture,
            RTFS_ITEST_META_FILE,
            NULL,
            &new_inode_lpa,
            &new_data_lpa));
    TEST_ASSERT_NOT_EQUAL_UINT32(old_data_lpa, new_data_lpa);
    TEST_ASSERT_NOT_EQUAL_UINT32(old_inode_lpa, new_inode_lpa);

    TEST_ASSERT_TRUE(rtfsIntegrationIsSitBitValid(&fixture, old_data_lpa));
    TEST_ASSERT_TRUE(rtfsIntegrationIsSitBitValid(&fixture, old_inode_lpa));
    TEST_ASSERT_TRUE(rtfsIntegrationIsSitBitValid(&fixture, new_data_lpa));
    TEST_ASSERT_TRUE(rtfsIntegrationIsSitBitValid(&fixture, new_inode_lpa));

    rtfsIntegrationWaitForCommittedJournal();
    TEST_ASSERT_EQUAL(0, cowReclaimRegistryDrainCompleted());
    TEST_ASSERT_EQUAL(0, rtfsIntegrationFlushMetadataToStore(&fixture));

    TEST_ASSERT_FALSE(rtfsIntegrationIsSitBitValid(&fixture, old_data_lpa));
    TEST_ASSERT_FALSE(rtfsIntegrationIsSitBitValid(&fixture, old_inode_lpa));
    TEST_ASSERT_TRUE(rtfsIntegrationIsSitBitValid(&fixture, new_data_lpa));
    TEST_ASSERT_TRUE(rtfsIntegrationIsSitBitValid(&fixture, new_inode_lpa));
    TEST_ASSERT_EQUAL_UINT32(
        ino,
        rtfsIntegrationReadSrmapEntry(&fixture, new_data_lpa).nid);

    rtfsIntegrationFixtureDestroy(&fixture);
}
