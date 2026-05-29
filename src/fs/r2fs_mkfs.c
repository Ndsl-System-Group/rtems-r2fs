#include "fs/r2fs_mkfs.h"

#include "communication/comm_api.h"
#include "communication/dev.h"
#include "utils/io_utils.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

static uint64_t r2fsMkfsDivRoundUp(uint64_t value, uint64_t divisor)
{
    return (value + divisor - 1U) / divisor;
}

static uint32_t r2fsMkfsSegToLpa(uint32_t seg_id)
{
    return seg_id * BLOCK_PER_SEGMENT;
}

static int r2fsMkfsWriteZeroBlock(
    R2fsMkfsWriteBlock write_block,
    void *write_ctx,
    uint32_t lpa
)
{
    unsigned char block[4096];

    memset(block, 0, sizeof(block));
    return write_block(write_ctx, lpa, block);
}

static int r2fsMkfsCommDevWriteBlock(
    void *ctx,
    uint32_t lpa,
    const void *block
)
{
    comm_dev *dev = (comm_dev *)ctx;

    if (dev == NULL || block == NULL) {
        return EINVAL;
    }

    return comm_submit_sync_rw_request(
        dev,
        (void *)block,
        LPA_TO_LBA(lpa),
        LBA_PER_LPA,
        COMM_IO_WRITE
    );
}

static void r2fsMkfsSitSetValid(
    struct RtfsSitBlock *sit_block,
    uint32_t seg_id,
    uint32_t seg_off
)
{
    struct RtfsSitEntry *entry;
    uint32_t idx;
    uint32_t byte_idx;
    uint32_t bit_idx;

    idx = seg_id % SIT_ENTRY_PER_BLOCK;
    entry = &sit_block->entries[idx];
    byte_idx = seg_off / 8U;
    bit_idx = seg_off % 8U;

    if ((entry->valid_map[byte_idx] & (uint8_t)(1U << bit_idx)) == 0) {
        entry->valid_map[byte_idx] |= (uint8_t)(1U << bit_idx);
        if (GET_SIT_VBLOCKS(entry) < 511U) {
            entry->vblocks += 1U;
        }
    }
}

static void r2fsMkfsSitSetNextSegment(
    struct RtfsSitBlock *sit_block,
    uint32_t seg_id,
    uint32_t next_seg_id
)
{
    struct RtfsSitEntry *entry;

    entry = &sit_block->entries[seg_id % SIT_ENTRY_PER_BLOCK];
    entry->vblocks &= SIT_VBLOCKS_MASK;
    SET_NEXT_SEG(entry, next_seg_id);
}

static int r2fsMkfsWriteInitialSuper(
    const R2fsMkfsOptions *options,
    const R2fsMkfsLayout *layout,
    R2fsMkfsWriteBlock write_block,
    void *write_ctx
)
{
    struct RtfsSuperBlock super_block;

    memset(&super_block, 0, sizeof(super_block));
    super_block.magic = RTFS_MAGIC_NUMBER;
    super_block.major_ver = 1;
    super_block.minor_ver = 0;
    super_block.log_sectorsize = 9;
    super_block.log_sectors_per_block = 3;
    super_block.log_blocksize = 12;
    super_block.log_blocks_per_seg = 9;
    super_block.block_count = layout->block_count;
    super_block.segment_count = layout->segment_count;
    super_block.segment_count_sit = layout->sit_segment_count;
    super_block.segment_count_nat = layout->nat_segment_count;
    super_block.segment_count_srmap = layout->srmap_segment_count;
    super_block.segment_count_meta_journal = layout->meta_journal_segment_count;
    super_block.segment_count_main = layout->main_segment_count;
    super_block.segment0_blkaddr = 0;
    super_block.sit_blkaddr = layout->sit_start_lpa;
    super_block.nat_blkaddr = layout->nat_start_lpa;
    super_block.srmap_blkaddr = layout->srmap_start_lpa;
    super_block.meta_journal_blkaddr = layout->meta_journal_start_lpa;
    super_block.main_blkaddr = layout->main_start_lpa;
    super_block.root_ino = options->root_ino;
    super_block.node_ino = options->root_ino;
    super_block.meta_ino = options->root_ino;
    super_block.first_node_segment_id = layout->main_start_segment;
    super_block.first_data_segment_id = layout->main_start_segment + 1U;
    super_block.current_data_segment_id = layout->main_start_segment + 1U;
    super_block.current_data_segment_blkoff = 0;
    super_block.current_node_segment_id = layout->main_start_segment;
    super_block.current_node_segment_blkoff = 1;
    super_block.meta_journal_start_blkoff = 0;
    super_block.meta_journal_end_blkoff = 0;
    super_block.next_free_nid = options->root_ino + 1U;

    if (layout->main_segment_count > 2U) {
        super_block.first_free_segment_id = layout->main_start_segment + 2U;
        super_block.free_segment_count = layout->main_segment_count - 2U;
    } else {
        super_block.first_free_segment_id = INVALID_SEGID;
        super_block.free_segment_count = 0;
    }

    return write_block(write_ctx, 0, &super_block);
}

static int r2fsMkfsWriteInitialNat(
    const R2fsMkfsOptions *options,
    const R2fsMkfsLayout *layout,
    R2fsMkfsWriteBlock write_block,
    void *write_ctx
)
{
    struct RtfsNatBlock nat_block;
    uint64_t nat_block_count;
    uint64_t block_idx;
    uint32_t root_nat_block;
    uint32_t root_nat_idx;
    uint32_t next_free_nid;

    nat_block_count = (uint64_t)layout->nat_segment_count * BLOCK_PER_SEGMENT;
    root_nat_block = options->root_ino / NAT_ENTRY_PER_BLOCK;
    root_nat_idx = options->root_ino % NAT_ENTRY_PER_BLOCK;
    next_free_nid = options->root_ino + 1U;

    for (block_idx = 0; block_idx < nat_block_count; ++block_idx) {
        uint32_t i;

        memset(&nat_block, 0, sizeof(nat_block));
        for (i = 0; i < NAT_ENTRY_PER_BLOCK; ++i) {
            uint64_t nid = block_idx * NAT_ENTRY_PER_BLOCK + i;

            nat_block.entries[i].ino = INVALID_NID;
            nat_block.entries[i].block_addr =
                (nid + 1U < layout->block_count) ? (uint32_t)(nid + 1U) : INVALID_NID;
        }

        if (block_idx == root_nat_block) {
            nat_block.entries[root_nat_idx].ino = options->root_ino;
            nat_block.entries[root_nat_idx].block_addr = layout->main_start_lpa;
        }

        if (next_free_nid / NAT_ENTRY_PER_BLOCK == block_idx) {
            uint32_t next_idx = next_free_nid % NAT_ENTRY_PER_BLOCK;
            nat_block.entries[next_idx].ino = INVALID_NID;
            nat_block.entries[next_idx].block_addr =
                (next_free_nid + 1U < layout->block_count) ?
                next_free_nid + 1U :
                INVALID_NID;
        }

        if (write_block(write_ctx, layout->nat_start_lpa + (uint32_t)block_idx, &nat_block) != 0) {
            return EIO;
        }
    }

    return 0;
}

static int r2fsMkfsWriteInitialSit(
    const R2fsMkfsLayout *layout,
    R2fsMkfsWriteBlock write_block,
    void *write_ctx
)
{
    struct RtfsSitBlock sit_block;
    uint64_t sit_block_count;
    uint64_t block_idx;
    uint32_t seg_id;
    uint32_t metadata_end_segment;

    sit_block_count = (uint64_t)layout->sit_segment_count * BLOCK_PER_SEGMENT;
    metadata_end_segment = layout->main_start_segment;

    for (block_idx = 0; block_idx < sit_block_count; ++block_idx) {
        memset(&sit_block, 0, sizeof(sit_block));

        for (seg_id = (uint32_t)block_idx * SIT_ENTRY_PER_BLOCK;
             seg_id < (uint32_t)(block_idx + 1U) * SIT_ENTRY_PER_BLOCK &&
             seg_id < layout->segment_count;
             ++seg_id) {
            if (seg_id + 1U < layout->segment_count) {
                r2fsMkfsSitSetNextSegment(&sit_block, seg_id, seg_id + 1U);
            }
        }

        if (block_idx == 0) {
            r2fsMkfsSitSetValid(&sit_block, 0, 0);
        }

        for (seg_id = 1; seg_id < metadata_end_segment; ++seg_id) {
            if (seg_id / SIT_ENTRY_PER_BLOCK == block_idx) {
                uint32_t off;

                for (off = 0; off < BLOCK_PER_SEGMENT; ++off) {
                    r2fsMkfsSitSetValid(&sit_block, seg_id, off);
                }
            }
        }

        if (layout->main_start_segment / SIT_ENTRY_PER_BLOCK == block_idx) {
            r2fsMkfsSitSetValid(&sit_block, layout->main_start_segment, 0);
        }

        if (write_block(write_ctx, layout->sit_start_lpa + (uint32_t)block_idx, &sit_block) != 0) {
            return EIO;
        }
    }

    return 0;
}

static int r2fsMkfsWriteInitialSrmap(
    const R2fsMkfsOptions *options,
    const R2fsMkfsLayout *layout,
    R2fsMkfsWriteBlock write_block,
    void *write_ctx
)
{
    struct RtfsSummaryBlock summary_block;
    uint64_t srmap_block_count;
    uint64_t block_idx;
    uint32_t root_node_lpa;
    uint32_t root_srmap_block;
    uint32_t root_srmap_idx;

    srmap_block_count = (uint64_t)layout->srmap_segment_count * BLOCK_PER_SEGMENT;
    root_node_lpa = layout->main_start_lpa;
    root_srmap_block = root_node_lpa / ENTRIES_IN_SUM;
    root_srmap_idx = root_node_lpa % ENTRIES_IN_SUM;

    for (block_idx = 0; block_idx < srmap_block_count; ++block_idx) {
        memset(&summary_block, 0, sizeof(summary_block));
        if (block_idx == root_srmap_block) {
            summary_block.entries[root_srmap_idx].nid = options->root_ino;
            summary_block.entries[root_srmap_idx].ofs_in_node = 0;
        }

        if (write_block(write_ctx, layout->srmap_start_lpa + (uint32_t)block_idx, &summary_block) != 0) {
            return EIO;
        }
    }

    return 0;
}

static int r2fsMkfsWriteRootInode(
    const R2fsMkfsOptions *options,
    const R2fsMkfsLayout *layout,
    R2fsMkfsWriteBlock write_block,
    void *write_ctx
)
{
    struct RtfsNode root_node;

    memset(&root_node, 0, sizeof(root_node));
    root_node.i.i_mode = 0755;
    root_node.i.i_type = RTFS_FT_DIR;
    root_node.i.i_nlink = 2;
    root_node.i.i_pino = options->root_ino;
    root_node.i.i_size = 0;
    root_node.i.i_blocks = 1;
    root_node.i.i_dentry_num = 0;
    root_node.i.i_current_depth = 0;
    root_node.i.i_inline = RTFS_INLINE_DENTRY;
    root_node.footer.nid = options->root_ino;
    root_node.footer.ino = options->root_ino;
    root_node.footer.offset = 0;
    root_node.footer.next_blkaddr = INVALID_LPA;

    return write_block(write_ctx, layout->main_start_lpa, &root_node);
}

int r2fsMkfsCalculateLayout(
    uint64_t lpa_count,
    uint32_t meta_journal_segment_count,
    R2fsMkfsLayout *out_layout
)
{
    uint64_t segment_count;
    uint64_t block_count;
    uint64_t sit_block_count;
    uint64_t nat_block_count;
    uint64_t srmap_block_count;
    uint64_t sit_segment_count;
    uint64_t nat_segment_count;
    uint64_t srmap_segment_count;
    uint64_t main_start_segment;

    if (out_layout == NULL || meta_journal_segment_count == 0) {
        return EINVAL;
    }

    segment_count = lpa_count / BLOCK_PER_SEGMENT;
    block_count = segment_count * BLOCK_PER_SEGMENT;
    if (segment_count > UINT32_MAX || block_count == 0) {
        return EINVAL;
    }

    sit_block_count = r2fsMkfsDivRoundUp(segment_count, SIT_ENTRY_PER_BLOCK);
    sit_segment_count = r2fsMkfsDivRoundUp(sit_block_count, BLOCK_PER_SEGMENT);
    nat_block_count = r2fsMkfsDivRoundUp(block_count, NAT_ENTRY_PER_BLOCK);
    nat_segment_count = r2fsMkfsDivRoundUp(nat_block_count, BLOCK_PER_SEGMENT);
    srmap_block_count = r2fsMkfsDivRoundUp(block_count, ENTRIES_IN_SUM);
    srmap_segment_count = r2fsMkfsDivRoundUp(srmap_block_count, BLOCK_PER_SEGMENT);

    main_start_segment =
        1U +
        meta_journal_segment_count +
        sit_segment_count +
        nat_segment_count +
        srmap_segment_count;

    if (main_start_segment + 2U > segment_count ||
        sit_segment_count > UINT32_MAX ||
        nat_segment_count > UINT32_MAX ||
        srmap_segment_count > UINT32_MAX) {
        return ENOSPC;
    }

    memset(out_layout, 0, sizeof(*out_layout));
    out_layout->block_count = block_count;
    out_layout->segment_count = (uint32_t)segment_count;
    out_layout->meta_journal_start_lpa = BLOCK_PER_SEGMENT;
    out_layout->meta_journal_segment_count = meta_journal_segment_count;
    out_layout->sit_segment_count = (uint32_t)sit_segment_count;
    out_layout->nat_segment_count = (uint32_t)nat_segment_count;
    out_layout->srmap_segment_count = (uint32_t)srmap_segment_count;
    out_layout->sit_start_lpa = r2fsMkfsSegToLpa(1U + meta_journal_segment_count);
    out_layout->nat_start_lpa = r2fsMkfsSegToLpa(
        1U + meta_journal_segment_count + out_layout->sit_segment_count
    );
    out_layout->srmap_start_lpa = r2fsMkfsSegToLpa(
        1U +
        meta_journal_segment_count +
        out_layout->sit_segment_count +
        out_layout->nat_segment_count
    );
    out_layout->main_start_segment = (uint32_t)main_start_segment;
    out_layout->main_start_lpa = r2fsMkfsSegToLpa(out_layout->main_start_segment);
    out_layout->main_segment_count =
        out_layout->segment_count - out_layout->main_start_segment;

    return 0;
}

int r2fsMkfsFormat(
    const R2fsMkfsOptions *options,
    R2fsMkfsWriteBlock write_block,
    void *write_ctx,
    R2fsMkfsLayout *out_layout
)
{
    R2fsMkfsOptions effective_options;
    R2fsMkfsLayout layout;
    uint32_t seg_id;
    int ret;

    if (options == NULL || write_block == NULL) {
        return EINVAL;
    }

    effective_options = *options;
    if (effective_options.root_ino == INVALID_NID) {
        effective_options.root_ino = 1;
    }
    if (effective_options.meta_journal_segment_count == 0) {
        effective_options.meta_journal_segment_count = 1;
    }

    ret = r2fsMkfsCalculateLayout(
        effective_options.lpa_count,
        effective_options.meta_journal_segment_count,
        &layout
    );
    if (ret != 0) {
        return ret;
    }

    if (r2fsMkfsWriteInitialSuper(&effective_options, &layout, write_block, write_ctx) != 0) {
        return EIO;
    }

    for (seg_id = 1; seg_id < layout.main_start_segment; ++seg_id) {
        uint32_t off;

        for (off = 0; off < BLOCK_PER_SEGMENT; ++off) {
            ret = r2fsMkfsWriteZeroBlock(
                write_block,
                write_ctx,
                r2fsMkfsSegToLpa(seg_id) + off
            );
            if (ret != 0) {
                return EIO;
            }
        }
    }

    ret = r2fsMkfsWriteInitialNat(&effective_options, &layout, write_block, write_ctx);
    if (ret != 0) {
        return ret;
    }

    ret = r2fsMkfsWriteInitialSit(&layout, write_block, write_ctx);
    if (ret != 0) {
        return ret;
    }

    ret = r2fsMkfsWriteInitialSrmap(&effective_options, &layout, write_block, write_ctx);
    if (ret != 0) {
        return ret;
    }

    if (r2fsMkfsWriteRootInode(&effective_options, &layout, write_block, write_ctx) != 0) {
        return EIO;
    }

    if (out_layout != NULL) {
        *out_layout = layout;
    }

    return 0;
}

int r2fsMkfsFormatCommDev(
    const R2fsMkfsOptions *options,
    comm_dev *dev,
    R2fsMkfsLayout *out_layout
)
{
    if (dev == NULL) {
        return EINVAL;
    }

    return r2fsMkfsFormat(
        options,
        r2fsMkfsCommDevWriteBlock,
        dev,
        out_layout
    );
}
