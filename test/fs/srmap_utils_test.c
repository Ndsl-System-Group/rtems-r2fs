#include "rtfs_test.h"

#include "fs/srmap_utils.h"

#include "cache/super_cache.h"
#include "fs/fs_manager.h"

#include <rtems/thread.h>


// XXX 同 nat_utils_test.c 的说明。
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


static void initFsForSrmap(file_system_manager *fs, struct RtfsSuperBlock *sb, uint32_t srmapStartLpa)
{
    memset(fs, 0, sizeof(*fs));
    memset(sb, 0, sizeof(*sb));

    sb->srmap_blkaddr = srmapStartLpa;

    fs->super_blk_mem_ = sb;
}


RTFS_TEST(SuInitTest)
{
    SrmapUtils su;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSrmap(&fs, &sb, 100);

    srmapUtilsInit(&su, &fs);

    TEST_ASSERT_EQUAL_PTR(&fs, su.fsManager);
    TEST_ASSERT_EQUAL_UINT32(100, su.srmapStartLpa);
    TEST_ASSERT_NOT_NULL(su.srmapCache);
    TEST_ASSERT_NOT_NULL(su.dirtyBlks);

    srmapUtilsDestroy(&su);
}

RTFS_TEST(SuWriteDataFirstEntryTest)
{
    SrmapUtils su;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSrmap(&fs, &sb, 200);
    srmapUtilsInit(&su, &fs);

    srmapUtilsWriteSrmapOfData(&su, 0, 11, 22);

    khiter_t k = kh_get(khsc, su.srmapCache, 200);
    TEST_ASSERT_TRUE(k != kh_end(su.srmapCache));

    BlockBuffer *blk = &kh_value(su.srmapCache, k);
    struct RtfsSummaryBlock *sum = (struct RtfsSummaryBlock *)blockBufferGetPtr(blk);

    TEST_ASSERT_EQUAL_UINT32(11, sum->entries[0].nid);
    TEST_ASSERT_EQUAL_UINT32(22, sum->entries[0].ofs_in_node);

    TEST_ASSERT_EQUAL_UINT32(1, kh_size(su.dirtyBlks));

    srmapUtilsDestroy(&su);
}

RTFS_TEST(SuWriteDataMiddleEntryTest)
{
    SrmapUtils su;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSrmap(&fs, &sb, 300);
    srmapUtilsInit(&su, &fs);

    uint32_t lpa = ENTRIES_IN_SUM * 3 + 17;
    uint32_t blkLpa = 303;
    uint32_t idx = 17;

    srmapUtilsWriteSrmapOfData(&su, lpa, 88, 99);

    khiter_t k = kh_get(khsc, su.srmapCache, blkLpa);
    TEST_ASSERT_TRUE(k != kh_end(su.srmapCache));

    BlockBuffer *blk = &kh_value(su.srmapCache, k);
    struct RtfsSummaryBlock *sum = (struct RtfsSummaryBlock *)blockBufferGetPtr(blk);

    TEST_ASSERT_EQUAL_UINT32(88, sum->entries[idx].nid);
    TEST_ASSERT_EQUAL_UINT32(99, sum->entries[idx].ofs_in_node);

    srmapUtilsDestroy(&su);
}

RTFS_TEST(SuWriteNodeTest)
{
    SrmapUtils su;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSrmap(&fs, &sb, 500);
    srmapUtilsInit(&su, &fs);

    uint32_t lpa = ENTRIES_IN_SUM + 9;
    uint32_t blkLpa = 501;
    uint32_t idx = 9;

    srmapUtilsWriteSrmapOfNode(&su, lpa, 1234);

    khiter_t k = kh_get(khsc, su.srmapCache, blkLpa);
    TEST_ASSERT_TRUE(k != kh_end(su.srmapCache));

    BlockBuffer *blk = &kh_value(su.srmapCache, k);
    struct RtfsSummaryBlock *sum = (struct RtfsSummaryBlock *)blockBufferGetPtr(blk);

    TEST_ASSERT_EQUAL_UINT32(1234, sum->entries[idx].nid);

    srmapUtilsDestroy(&su);
}

RTFS_TEST(SuWriteSameBlockDirtyOnlyOnceTest)
{
    SrmapUtils su;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSrmap(&fs, &sb, 600);
    srmapUtilsInit(&su, &fs);

    srmapUtilsWriteSrmapOfData(&su, 1, 1, 1);
    srmapUtilsWriteSrmapOfData(&su, 2, 2, 2);
    srmapUtilsWriteSrmapOfNode(&su, 3, 3);

    TEST_ASSERT_EQUAL_UINT32(1, kh_size(su.dirtyBlks));
    TEST_ASSERT_EQUAL_UINT32(1, kh_size(su.srmapCache));

    srmapUtilsDestroy(&su);
}

RTFS_TEST(SuWriteDifferentBlockTest)
{
    SrmapUtils su;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSrmap(&fs, &sb, 700);
    srmapUtilsInit(&su, &fs);

    srmapUtilsWriteSrmapOfData(&su, 0, 1, 1);
    srmapUtilsWriteSrmapOfData(&su, ENTRIES_IN_SUM, 2, 2);
    srmapUtilsWriteSrmapOfData(&su, ENTRIES_IN_SUM * 2, 3, 3);

    TEST_ASSERT_EQUAL_UINT32(3, kh_size(su.dirtyBlks));
    TEST_ASSERT_EQUAL_UINT32(3, kh_size(su.srmapCache));

    srmapUtilsDestroy(&su);
}

RTFS_TEST(SuClearCacheTest)
{
    SrmapUtils su;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSrmap(&fs, &sb, 800);
    srmapUtilsInit(&su, &fs);

    srmapUtilsWriteSrmapOfData(&su, 0, 1, 1);
    srmapUtilsWriteSrmapOfData(&su, ENTRIES_IN_SUM, 2, 2);

    srmapUtilsClearCache(&su);

    TEST_ASSERT_NULL(su.srmapCache);
    TEST_ASSERT_NULL(su.dirtyBlks);
}
