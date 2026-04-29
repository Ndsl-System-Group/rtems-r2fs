#include "rtfs_test.h"

#include "fs/sit_utils.h"

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
    dir_data_block_cache *dir_data_cache_;

    SrmapUtils *srmap_utils_;
    SitNatCache *sit_cache_;
    SitNatCache *nat_cache_;

    comm_dev *dev_;
    fd_array *fd_arr_;

    JournalContainer *cur_journal_;
    bool is_unrecoverable_;
} file_system_manager;


static void initFsForSit(file_system_manager *fs, struct RtfsSuperBlock *sb, uint32_t seg0StartLpa, uint32_t segCount, uint32_t sitStartLpa, uint32_t sitSegmentCnt)
{
    memset(fs, 0, sizeof(*fs));
    memset(sb, 0, sizeof(*sb));

    sb->segment0_blkaddr = seg0StartLpa;
    sb->segment_count = segCount;
    sb->sit_blkaddr = sitStartLpa;
    sb->segment_count_sit = sitSegmentCnt;

    fs->super_blk_mem_ = sb;
}


RTFS_TEST(SoInitTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);

    sitOperatorInit(&so, &fs);

    TEST_ASSERT_EQUAL_UINT32(1000, so.seg0StartLpa);
    TEST_ASSERT_EQUAL_UINT32(64, so.segCount);
    TEST_ASSERT_EQUAL_UINT32(200, so.sitStartLpa);
    TEST_ASSERT_EQUAL_UINT32(4, so.sitSegmentCnt);
    TEST_ASSERT_EQUAL_PTR(&fs, so.fsManager);
}

RTFS_TEST(SoGetSegPosFirstLpaTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SegPos pos = sitGetSegPosOfLpa(&so, 1000);

    TEST_ASSERT_EQUAL_UINT32(0, pos.segId);
    TEST_ASSERT_EQUAL_UINT32(0, pos.offset);
}

RTFS_TEST(SoGetSegPosLastBlockInFirstSegTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SegPos pos = sitGetSegPosOfLpa(&so, 1000 + BLOCK_PER_SEGMENT - 1);

    TEST_ASSERT_EQUAL_UINT32(0, pos.segId);
    TEST_ASSERT_EQUAL_UINT32(BLOCK_PER_SEGMENT - 1, pos.offset);
}

RTFS_TEST(SoGetSegPosNextSegFirstBlockTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SegPos pos = sitGetSegPosOfLpa(&so, 1000 + BLOCK_PER_SEGMENT);

    TEST_ASSERT_EQUAL_UINT32(1, pos.segId);
    TEST_ASSERT_EQUAL_UINT32(0, pos.offset);
}

RTFS_TEST(SoGetSegPosMiddleTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 5000, 128, 300, 8);
    sitOperatorInit(&so, &fs);

    uint32_t lpa = 5000 + BLOCK_PER_SEGMENT * 3 + 17;

    SegPos pos = sitGetSegPosOfLpa(&so, lpa);

    TEST_ASSERT_EQUAL_UINT32(3, pos.segId);
    TEST_ASSERT_EQUAL_UINT32(17, pos.offset);
}

RTFS_TEST(SoGetSegIdPosFirstEntryTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SitPos pos = sitGetSegIdPosInSit(&so, 0);

    TEST_ASSERT_EQUAL_UINT32(200, pos.sitLpa);
    TEST_ASSERT_EQUAL_UINT32(0, pos.idx);
}

RTFS_TEST(SoGetSegIdPosLastEntryFirstBlockTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SitPos pos = sitGetSegIdPosInSit(&so, SIT_ENTRY_PER_BLOCK - 1);

    TEST_ASSERT_EQUAL_UINT32(200, pos.sitLpa);
    TEST_ASSERT_EQUAL_UINT32(SIT_ENTRY_PER_BLOCK - 1, pos.idx);
}

RTFS_TEST(SoGetSegIdPosSecondBlockFirstEntryTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    SitPos pos = sitGetSegIdPosInSit(&so, SIT_ENTRY_PER_BLOCK);

    TEST_ASSERT_EQUAL_UINT32(201, pos.sitLpa);
    TEST_ASSERT_EQUAL_UINT32(0, pos.idx);
}

RTFS_TEST(SoGetSegIdPosMiddleTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 256, 400, 8);
    sitOperatorInit(&so, &fs);

    uint32_t segId = SIT_ENTRY_PER_BLOCK * 3 + 29;

    SitPos pos = sitGetSegIdPosInSit(&so, segId);

    TEST_ASSERT_EQUAL_UINT32(403, pos.sitLpa);
    TEST_ASSERT_EQUAL_UINT32(29, pos.idx);
}

RTFS_TEST(SoGetFirstLpaOfSegId0Test)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    TEST_ASSERT_EQUAL_UINT32(1000, sitGetFirstLpaOfSegId(&so, 0));
}

RTFS_TEST(SoGetFirstLpaOfSegId1Test)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    TEST_ASSERT_EQUAL_UINT32(1000 + BLOCK_PER_SEGMENT, sitGetFirstLpaOfSegId(&so, 1));
}

RTFS_TEST(SoGetFirstLpaOfSegIdMiddleTest)
{
    SitOperator so;
    file_system_manager fs;
    struct RtfsSuperBlock sb;
    uint32_t segId = 7;

    initFsForSit(&fs, &sb, 1000, 64, 200, 4);
    sitOperatorInit(&so, &fs);

    TEST_ASSERT_EQUAL_UINT32(1000 + segId * BLOCK_PER_SEGMENT, sitGetFirstLpaOfSegId(&so, segId));
}
