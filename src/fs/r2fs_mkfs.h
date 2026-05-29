#pragma once

#include "fs/fs.h"
#include "utils/types.h"

#include <stddef.h>

struct comm_dev;

typedef int (*R2fsMkfsWriteBlock)(
    void *ctx,
    uint32_t lpa,
    const void *block
);

typedef struct R2fsMkfsOptions
{
    uint64_t lpa_count;
    uint32_t root_ino;
    uint32_t meta_journal_segment_count;
} R2fsMkfsOptions;

typedef struct R2fsMkfsLayout
{
    uint64_t block_count;
    uint32_t segment_count;
    uint32_t meta_journal_start_lpa;
    uint32_t meta_journal_segment_count;
    uint32_t sit_start_lpa;
    uint32_t sit_segment_count;
    uint32_t nat_start_lpa;
    uint32_t nat_segment_count;
    uint32_t srmap_start_lpa;
    uint32_t srmap_segment_count;
    uint32_t main_start_lpa;
    uint32_t main_start_segment;
    uint32_t main_segment_count;
} R2fsMkfsLayout;

int r2fsMkfsCalculateLayout(
    uint64_t lpa_count,
    uint32_t meta_journal_segment_count,
    R2fsMkfsLayout *out_layout
);

int r2fsMkfsFormat(
    const R2fsMkfsOptions *options,
    R2fsMkfsWriteBlock write_block,
    void *write_ctx,
    R2fsMkfsLayout *out_layout
);

int r2fsMkfsFormatCommDev(
    const R2fsMkfsOptions *options,
    struct comm_dev *dev,
    R2fsMkfsLayout *out_layout
);
