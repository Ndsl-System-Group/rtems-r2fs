#pragma once

#include "fs/fs.h"
#include "utils/types.h"

#include <stddef.h>

struct comm_dev;

typedef int (*RtfsMkfsWriteBlock)(
    void *ctx,
    uint32_t lpa,
    const void *block);

typedef struct RtfsMkfsOptions
{
    uint64_t lpa_count;
    uint32_t root_ino;
    uint32_t meta_journal_segment_count;
} RtfsMkfsOptions;

typedef struct RtfsMkfsLayout
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
} RtfsMkfsLayout;

int rtfsMkfsCalculateLayout(
    uint64_t lpa_count,
    uint32_t meta_journal_segment_count,
    RtfsMkfsLayout *out_layout);

int rtfsMkfsFormat(
    const RtfsMkfsOptions *options,
    RtfsMkfsWriteBlock write_block,
    void *write_ctx,
    RtfsMkfsLayout *out_layout);

int rtfsMkfsFormatCommDev(
    const RtfsMkfsOptions *options,
    struct comm_dev *dev,
    RtfsMkfsLayout *out_layout);
