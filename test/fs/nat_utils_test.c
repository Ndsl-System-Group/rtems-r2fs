#include "rtfs_test.h"

#include "fs/nat_utils.h"

#include "cache/super_cache.h"
#include "fs/fs_manager.h"

#include <rtems/thread.h>


// XXX 这里 Mock 的 file_system_manager 要和 src 里面的定义完全一致，否则会导致 src 中定义的 fileSystemManagerGetSuperBlkMem 遇到两个 file_system_manager 的定义，会编译错误。
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


static void initFsForNat(file_system_manager *fs, struct RtfsSuperBlock *sb, uint32_t natBlkaddr, uint32_t segmentCountNat)
{
    memset(fs, 0, sizeof(*fs));
    memset(sb, 0, sizeof(*sb));

    sb->nat_blkaddr = natBlkaddr;
    sb->segment_count_nat = segmentCountNat;

    fs->super_blk_mem_ = sb;
}


RTFS_TEST(NuInitTest)
{
    NatLpaMapping nlm;
    file_system_manager fs;
    struct RtfsSuperBlock sb;


    initFsForNat(&fs, &sb, 1, 2);
    natLpaMappingInit(&nlm, &fs);

    TEST_ASSERT_EQUAL_UINT32(1, nlm.natStartLpa);
    TEST_ASSERT_EQUAL_UINT32(2, nlm.natSegmentCnt);
    TEST_ASSERT_EQUAL_PTR(&fs, nlm.fsManager);
}

RTFS_TEST(NuGetNidPosZeroTest)
{
    NatLpaMapping nlm;
    file_system_manager fs;
    struct RtfsSuperBlock sb;


    initFsForNat(&fs, &sb, 100, 4);
    natLpaMappingInit(&nlm, &fs);

    NatNidPos pos = natGetNidPos(&nlm, 0);

    TEST_ASSERT_EQUAL_UINT32(100, pos.lpa);
    TEST_ASSERT_EQUAL_UINT32(0, pos.idx);
}

RTFS_TEST(NuGetNidPosFirstBlockLastEntryTest)
{
    NatLpaMapping nlm;
    file_system_manager fs;
    struct RtfsSuperBlock sb;


    initFsForNat(&fs, &sb, 200, 4);
    natLpaMappingInit(&nlm, &fs);

    NatNidPos pos = natGetNidPos(&nlm, NAT_ENTRY_PER_BLOCK - 1);

    TEST_ASSERT_EQUAL_UINT32(200, pos.lpa);
    TEST_ASSERT_EQUAL_UINT32(NAT_ENTRY_PER_BLOCK - 1, pos.idx);
}

RTFS_TEST(NuGetNidPosSecondBlockFirstEntryTest)
{
    NatLpaMapping nlm;
    file_system_manager fs;
    struct RtfsSuperBlock sb;


    initFsForNat(&fs, &sb, 300, 4);
    natLpaMappingInit(&nlm, &fs);

    NatNidPos pos = natGetNidPos(&nlm, NAT_ENTRY_PER_BLOCK);

    TEST_ASSERT_EQUAL_UINT32(301, pos.lpa);
    TEST_ASSERT_EQUAL_UINT32(0, pos.idx);
}

RTFS_TEST(NuGetNidPosMiddleEntryTest)
{
    NatLpaMapping nlm;
    file_system_manager fs;
    struct RtfsSuperBlock sb;


    initFsForNat(&fs, &sb, 500, 16);
    natLpaMappingInit(&nlm, &fs);

    uint32_t nid = NAT_ENTRY_PER_BLOCK * 3 + 17;
    NatNidPos pos = natGetNidPos(&nlm, nid);

    TEST_ASSERT_EQUAL_UINT32(503, pos.lpa);
    TEST_ASSERT_EQUAL_UINT32(17, pos.idx);
}

RTFS_TEST(NuGetNidPosLastValidNidTest)
{
    NatLpaMapping nlm;
    file_system_manager fs;
    struct RtfsSuperBlock sb;


    initFsForNat(&fs, &sb, 800, 2);
    natLpaMappingInit(&nlm, &fs);

    uint32_t totalBlockCnt = nlm.natSegmentCnt * BLOCK_PER_SEGMENT;
    uint32_t lastNid = totalBlockCnt * NAT_ENTRY_PER_BLOCK - 1;

    NatNidPos pos = natGetNidPos(&nlm, lastNid);

    TEST_ASSERT_EQUAL_UINT32(nlm.natStartLpa + totalBlockCnt - 1, pos.lpa);
    TEST_ASSERT_EQUAL_UINT32(NAT_ENTRY_PER_BLOCK - 1, pos.idx);
}
