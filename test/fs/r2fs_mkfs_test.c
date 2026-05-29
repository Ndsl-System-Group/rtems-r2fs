#include "rtfs_test.h"

#include "fs/r2fs_mkfs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct R2fsMkfsTestDisk
{
    unsigned char *blocks;
    uint64_t block_count;
    uint32_t write_count;
    uint32_t fail_lpa;
} R2fsMkfsTestDisk;

static int r2fsMkfsTestWriteBlock(
    void *ctx,
    uint32_t lpa,
    const void *block
)
{
    R2fsMkfsTestDisk *disk = (R2fsMkfsTestDisk *)ctx;

    if (disk == NULL || block == NULL || lpa >= disk->block_count) {
        return EINVAL;
    }

    if (lpa == disk->fail_lpa) {
        return EIO;
    }

    memcpy(disk->blocks + (uint64_t)lpa * 4096U, block, 4096U);
    disk->write_count++;
    return 0;
}

static void r2fsMkfsTestDiskInit(
    R2fsMkfsTestDisk *disk,
    uint64_t block_count
)
{
    memset(disk, 0, sizeof(*disk));
    disk->block_count = block_count;
    disk->fail_lpa = UINT32_MAX;
    disk->blocks = (unsigned char *)calloc((size_t)block_count, 4096U);
    TEST_ASSERT_NOT_NULL(disk->blocks);
}

static void r2fsMkfsTestDiskDestroy(R2fsMkfsTestDisk *disk)
{
    free(disk->blocks);
    memset(disk, 0, sizeof(*disk));
}

static void *r2fsMkfsTestBlockPtr(
    R2fsMkfsTestDisk *disk,
    uint32_t lpa
)
{
    TEST_ASSERT_TRUE((uint64_t)lpa < disk->block_count);
    return disk->blocks + (uint64_t)lpa * 4096U;
}

RTFS_TEST(R2fsMkfsCalculateLayout_WhenDiskIsLargeEnough_ShouldPlaceAreasOnSegmentBoundaries)
{
    R2fsMkfsLayout layout;

    TEST_ASSERT_EQUAL(0, r2fsMkfsCalculateLayout(64U * BLOCK_PER_SEGMENT, 1, &layout));

    TEST_ASSERT_EQUAL_UINT64(64U * BLOCK_PER_SEGMENT, layout.block_count);
    TEST_ASSERT_EQUAL_UINT32(64, layout.segment_count);
    TEST_ASSERT_EQUAL_UINT32(BLOCK_PER_SEGMENT, layout.meta_journal_start_lpa);
    TEST_ASSERT_EQUAL_UINT32(2U * BLOCK_PER_SEGMENT, layout.sit_start_lpa);
    TEST_ASSERT_EQUAL_UINT32(3U * BLOCK_PER_SEGMENT, layout.nat_start_lpa);
    TEST_ASSERT_EQUAL_UINT32(4U * BLOCK_PER_SEGMENT, layout.srmap_start_lpa);
    TEST_ASSERT_EQUAL_UINT32(5U * BLOCK_PER_SEGMENT, layout.main_start_lpa);
    TEST_ASSERT_EQUAL_UINT32(5, layout.main_start_segment);
    TEST_ASSERT_EQUAL_UINT32(59, layout.main_segment_count);
}

RTFS_TEST(R2fsMkfsCalculateLayout_WhenDiskTooSmall_ShouldReturnEnospc)
{
    R2fsMkfsLayout layout;

    TEST_ASSERT_EQUAL(ENOSPC, r2fsMkfsCalculateLayout(4U * BLOCK_PER_SEGMENT, 1, &layout));
}

RTFS_TEST(R2fsMkfsFormat_ShouldWriteInitialSuperAndRootMetadata)
{
    R2fsMkfsTestDisk disk;
    R2fsMkfsOptions options;
    R2fsMkfsLayout layout;
    struct RtfsSuperBlock *super_block;
    struct RtfsNode *root_node;
    struct RtfsNatBlock *root_nat_block;
    struct RtfsNatEntry *root_nat_entry;
    struct RtfsSitBlock *root_sit_block;
    struct RtfsSitEntry *root_sit_entry;
    struct RtfsSummaryBlock *root_srmap_block;

    r2fsMkfsTestDiskInit(&disk, 64U * BLOCK_PER_SEGMENT);

    memset(&options, 0, sizeof(options));
    options.lpa_count = disk.block_count;
    options.root_ino = 1;
    options.meta_journal_segment_count = 1;

    TEST_ASSERT_EQUAL(
        0,
        r2fsMkfsFormat(&options, r2fsMkfsTestWriteBlock, &disk, &layout)
    );
    TEST_ASSERT_GREATER_THAN_UINT32(0, disk.write_count);

    super_block = (struct RtfsSuperBlock *)r2fsMkfsTestBlockPtr(&disk, 0);
    TEST_ASSERT_EQUAL_UINT32(RTFS_MAGIC_NUMBER, super_block->magic);
    TEST_ASSERT_EQUAL_UINT64(layout.block_count, super_block->block_count);
    TEST_ASSERT_EQUAL_UINT32(layout.segment_count, super_block->segment_count);
    TEST_ASSERT_EQUAL_UINT32(layout.sit_start_lpa, super_block->sit_blkaddr);
    TEST_ASSERT_EQUAL_UINT32(layout.nat_start_lpa, super_block->nat_blkaddr);
    TEST_ASSERT_EQUAL_UINT32(layout.srmap_start_lpa, super_block->srmap_blkaddr);
    TEST_ASSERT_EQUAL_UINT32(layout.main_start_lpa, super_block->main_blkaddr);
    TEST_ASSERT_EQUAL_UINT32(1, super_block->root_ino);
    TEST_ASSERT_EQUAL_UINT32(layout.main_start_segment, super_block->current_node_segment_id);
    TEST_ASSERT_EQUAL_UINT32(1, super_block->current_node_segment_blkoff);
    TEST_ASSERT_EQUAL_UINT32(layout.main_start_segment + 1U, super_block->current_data_segment_id);
    TEST_ASSERT_EQUAL_UINT32(layout.main_start_segment + 2U, super_block->first_free_segment_id);
    TEST_ASSERT_EQUAL_UINT32(layout.main_segment_count - 2U, super_block->free_segment_count);
    TEST_ASSERT_EQUAL_UINT32(2, super_block->next_free_nid);

    root_node = (struct RtfsNode *)r2fsMkfsTestBlockPtr(&disk, layout.main_start_lpa);
    TEST_ASSERT_EQUAL_UINT32(1, root_node->footer.nid);
    TEST_ASSERT_EQUAL_UINT32(1, root_node->footer.ino);
    TEST_ASSERT_EQUAL_UINT32(RTFS_FT_DIR, root_node->i.i_type);
    TEST_ASSERT_EQUAL_UINT32(2, root_node->i.i_nlink);
    TEST_ASSERT_BITS_HIGH(RTFS_INLINE_DENTRY, root_node->i.i_inline);

    root_nat_block = (struct RtfsNatBlock *)r2fsMkfsTestBlockPtr(&disk, layout.nat_start_lpa);
    root_nat_entry = &root_nat_block->entries[1];
    TEST_ASSERT_EQUAL_UINT32(1, root_nat_entry->ino);
    TEST_ASSERT_EQUAL_UINT32(layout.main_start_lpa, root_nat_entry->block_addr);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, root_nat_block->entries[2].ino);
    TEST_ASSERT_EQUAL_UINT32(3, root_nat_block->entries[2].block_addr);

    root_sit_block = (struct RtfsSitBlock *)r2fsMkfsTestBlockPtr(&disk, layout.sit_start_lpa);
    root_sit_entry = &root_sit_block->entries[layout.main_start_segment % SIT_ENTRY_PER_BLOCK];
    TEST_ASSERT_TRUE(
        (root_sit_entry->valid_map[0] & 1U) != 0
    );
    TEST_ASSERT_EQUAL_UINT32(1, GET_SIT_VBLOCKS(root_sit_entry));

    root_srmap_block = (struct RtfsSummaryBlock *)r2fsMkfsTestBlockPtr(
        &disk,
        layout.srmap_start_lpa + (layout.main_start_lpa / ENTRIES_IN_SUM)
    );
    TEST_ASSERT_EQUAL_UINT32(
        1,
        root_srmap_block->entries[layout.main_start_lpa % ENTRIES_IN_SUM].nid
    );

    r2fsMkfsTestDiskDestroy(&disk);
}

RTFS_TEST(R2fsMkfsFormat_WhenWriteFails_ShouldReturnEio)
{
    R2fsMkfsTestDisk disk;
    R2fsMkfsOptions options;

    r2fsMkfsTestDiskInit(&disk, 64U * BLOCK_PER_SEGMENT);
    disk.fail_lpa = 0;

    memset(&options, 0, sizeof(options));
    options.lpa_count = disk.block_count;
    options.root_ino = 1;
    options.meta_journal_segment_count = 1;

    TEST_ASSERT_EQUAL(
        EIO,
        r2fsMkfsFormat(&options, r2fsMkfsTestWriteBlock, &disk, NULL)
    );

    r2fsMkfsTestDiskDestroy(&disk);
}
