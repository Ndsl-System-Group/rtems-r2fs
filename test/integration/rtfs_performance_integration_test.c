#include "integration/rtfs_rtems_mount_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"
#include "fs/fs.h"
#include "utils/rtfs_log.h"

#include <errno.h>
#include <fcntl.h>
#include <rtems/counter.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RTFS_PERF_LPA_COUNT (32U * BLOCK_PER_SEGMENT)
#define RTFS_PERF_STREAM_FILE "/perf-stream.bin"
#define RTFS_PERF_RANDOM_FILE "/perf-random.bin"
#define RTFS_PERF_MIXED_FILE "/perf-mixed.bin"
#define RTFS_PERF_META_DIR "/perf-meta"
#define RTFS_PERF_SMALL_DIR "/perf-small"

#define RTFS_PERF_STREAM_TOTAL_BYTES (8U * 1024U * 1024U)
#define RTFS_PERF_STREAM_CHUNK_BYTES (64U * 1024U)
#define RTFS_PERF_RANDOM_BLOCK_BYTES BLOCK_BUFFER_SIZE
#define RTFS_PERF_RANDOM_TOTAL_BYTES (8U * 1024U * 1024U)
#define RTFS_PERF_RANDOM_OPS 1024U
#define RTFS_PERF_MIXED_OPS 1024U
#define RTFS_PERF_META_STREAM_TOTAL_BYTES (1024U * 1024U)
#define RTFS_PERF_META_STREAM_CHUNK_BYTES BLOCK_BUFFER_SIZE
#define RTFS_PERF_META_RANDOM_TOTAL_BYTES (1024U * 1024U)
#define RTFS_PERF_META_RANDOM_OPS 256U
#define RTFS_PERF_META_MIXED_OPS 256U
#define RTFS_PERF_META_FILE_COUNT NR_INLINE_DENTRY
#define RTFS_PERF_SMALL_FILE_COUNT 128U
#define RTFS_PERF_SMALL_FILE_BYTES 1024U

typedef struct RtfsPerfLogGuard
{
    RtfsLogLevel previous_level;
} RtfsPerfLogGuard;

typedef enum RtfsPerfMetricId
{
    RTFS_PERF_METRIC_SEQUENTIAL_WRITE = 0,
    RTFS_PERF_METRIC_SEQUENTIAL_READ,
    RTFS_PERF_METRIC_METADATA_CREATE,
    RTFS_PERF_METRIC_METADATA_DELETE,
    RTFS_PERF_METRIC_RANDOM_WRITE_IOPS,
    RTFS_PERF_METRIC_SMALL_FILE_CREATION,
    RTFS_PERF_METRIC_MIXED_RW_IOPS,
    RTFS_PERF_METRIC_COUNT
} RtfsPerfMetricId;

typedef struct RtfsPerfSummary
{
    bool valid[RTFS_PERF_METRIC_COUNT];
    double value[RTFS_PERF_METRIC_COUNT];
    uint32_t ops[RTFS_PERF_METRIC_COUNT];
    uint64_t us[RTFS_PERF_METRIC_COUNT];
} RtfsPerfSummary;

static RtfsPerfSummary g_rtfs_perf_summary;

static void rtfsPerfSequentialWrite(
    int fd,
    const unsigned char *buffer,
    size_t chunk_size,
    size_t total_bytes);
static void rtfsPerfSequentialRead(
    int fd,
    unsigned char *buffer,
    size_t chunk_size,
    size_t total_bytes);
static void rtfsPerfPrepareSizedFile(
    const char *relative_path,
    size_t total_bytes,
    const unsigned char *buffer,
    size_t chunk_size);
static void rtfsPerfRandomWritePass(
    int fd,
    const unsigned char *buffer,
    size_t file_size,
    uint32_t op_count,
    uint32_t seed);
static void rtfsPerfRandomWriteIops(
    int fd,
    const unsigned char *buffer,
    size_t file_size,
    uint32_t op_count);
static void rtfsPerfMixedRwPass(
    int fd,
    unsigned char *buffer,
    size_t file_size,
    uint32_t op_count,
    uint32_t seed,
    uint32_t *out_read_ops,
    uint32_t *out_write_ops);
static void rtfsPerfMixedRwIops(
    int fd,
    unsigned char *buffer,
    size_t file_size,
    uint32_t op_count);

static uint64_t rtfsPerfCounterFreq(void)
{
    return rtems_counter_frequency();
}

static uint64_t rtfsPerfCounterNow(void)
{
    return rtems_counter_read();
}

static uint64_t rtfsPerfCounterToUs(uint64_t diff)
{
    return (diff * 1000000ULL) / rtfsPerfCounterFreq();
}

static RtfsPerfLogGuard rtfsPerfBeginQuietLogging(void)
{
    RtfsPerfLogGuard guard;

    guard.previous_level = rtfsLogGetMinLevel();
    rtfsLogSetMinLevel(RTFS_LOG_SILENT);
    return guard;
}

static void rtfsPerfEndQuietLogging(const RtfsPerfLogGuard *guard)
{
    TEST_ASSERT_NOT_NULL(guard);
    rtfsLogSetMinLevel(guard->previous_level);
}

static void rtfsPerfResetSummary(void)
{
    memset(&g_rtfs_perf_summary, 0, sizeof(g_rtfs_perf_summary));
}

static double rtfsPerfBytesToMbPerSec(uint64_t bytes, uint64_t us)
{
    TEST_ASSERT_TRUE(us > 0u);
    return ((double)bytes / 1000000.0) / ((double)us / 1000000.0);
}

static double rtfsPerfOpsToIops(uint32_t ops, uint64_t us)
{
    TEST_ASSERT_TRUE(us > 0u);
    return (double)ops / ((double)us / 1000000.0);
}

static double rtfsPerfUsPerOp(uint32_t ops, uint64_t us)
{
    TEST_ASSERT_TRUE(ops > 0u);
    TEST_ASSERT_TRUE(us > 0u);
    return (double)us / (double)ops;
}

static void rtfsPerfRecordValue(
    RtfsPerfMetricId metric_id,
    double value,
    uint32_t ops,
    uint64_t us)
{
    TEST_ASSERT_TRUE(metric_id < RTFS_PERF_METRIC_COUNT);
    g_rtfs_perf_summary.valid[metric_id] = true;
    g_rtfs_perf_summary.value[metric_id] = value;
    g_rtfs_perf_summary.ops[metric_id] = ops;
    g_rtfs_perf_summary.us[metric_id] = us;
}

static void rtfsPerfRecordThroughput(
    RtfsPerfMetricId metric_id,
    uint64_t bytes,
    uint64_t us)
{
    rtfsPerfRecordValue(
        metric_id,
        rtfsPerfBytesToMbPerSec(bytes, us),
        0u,
        us);
}

static void rtfsPerfRecordIops(
    RtfsPerfMetricId metric_id,
    uint32_t ops,
    uint64_t us)
{
    rtfsPerfRecordValue(
        metric_id,
        rtfsPerfOpsToIops(ops, us),
        ops,
        us);
}

static void rtfsPerfRecordUsPerOp(
    RtfsPerfMetricId metric_id,
    uint32_t ops,
    uint64_t us)
{
    rtfsPerfRecordValue(
        metric_id,
        rtfsPerfUsPerOp(ops, us),
        ops,
        us);
}

static void rtfsPerfPrintTableSeparator(void)
{
    printf("+---------------------------+---------------+-----------+\n");
}

static void rtfsPerfPrintTableHeader(const char *title)
{
    TEST_ASSERT_NOT_NULL(title);
    printf("\n--- %s ---\n", title);
    rtfsPerfPrintTableSeparator();
    printf("| %-25s | %-13s | %-9s |\n", "Test Name", "Result", "Unit");
    rtfsPerfPrintTableSeparator();
}

static void rtfsPerfPrintTableRow(
    const char *test_name,
    bool available,
    double value,
    const char *unit)
{
    TEST_ASSERT_NOT_NULL(test_name);
    TEST_ASSERT_NOT_NULL(unit);

    if (available)
    {
        printf(
            "| %-25s | %13.3f | %-9s |\n",
            test_name,
            value,
            unit);
    }
    else
    {
        printf(
            "| %-25s | %13s | %-9s |\n",
            test_name,
            "N/A",
            unit);
    }
}

static double rtfsPerfMetadataCreateDeleteUsPerOp(void)
{
    uint32_t total_ops =
        g_rtfs_perf_summary.ops[RTFS_PERF_METRIC_METADATA_CREATE] +
        g_rtfs_perf_summary.ops[RTFS_PERF_METRIC_METADATA_DELETE];
    uint64_t total_us =
        g_rtfs_perf_summary.us[RTFS_PERF_METRIC_METADATA_CREATE] +
        g_rtfs_perf_summary.us[RTFS_PERF_METRIC_METADATA_DELETE];

    return rtfsPerfUsPerOp(total_ops, total_us);
}

static void rtfsPerfPrintBenchmarkTable(const char *title)
{
    bool metadata_available =
        g_rtfs_perf_summary.valid[RTFS_PERF_METRIC_METADATA_CREATE] &&
        g_rtfs_perf_summary.valid[RTFS_PERF_METRIC_METADATA_DELETE];

    rtfsPerfPrintTableHeader(title);
    rtfsPerfPrintTableRow(
        "Sequential Write",
        g_rtfs_perf_summary.valid[RTFS_PERF_METRIC_SEQUENTIAL_WRITE],
        g_rtfs_perf_summary.value[RTFS_PERF_METRIC_SEQUENTIAL_WRITE],
        "MB/s");
    rtfsPerfPrintTableRow(
        "Sequential Read",
        g_rtfs_perf_summary.valid[RTFS_PERF_METRIC_SEQUENTIAL_READ],
        g_rtfs_perf_summary.value[RTFS_PERF_METRIC_SEQUENTIAL_READ],
        "MB/s");
    rtfsPerfPrintTableRow(
        "Metadata Create/Delete",
        metadata_available,
        metadata_available ? rtfsPerfMetadataCreateDeleteUsPerOp() : 0.0,
        "us/op");
    rtfsPerfPrintTableRow(
        "Random Write IOPS",
        g_rtfs_perf_summary.valid[RTFS_PERF_METRIC_RANDOM_WRITE_IOPS],
        g_rtfs_perf_summary.value[RTFS_PERF_METRIC_RANDOM_WRITE_IOPS],
        "IOPS");
    rtfsPerfPrintTableRow(
        "Small File Creation",
        g_rtfs_perf_summary.valid[RTFS_PERF_METRIC_SMALL_FILE_CREATION],
        g_rtfs_perf_summary.value[RTFS_PERF_METRIC_SMALL_FILE_CREATION],
        "Files/sec");
    rtfsPerfPrintTableRow(
        "Mixed R/W IOPS",
        g_rtfs_perf_summary.valid[RTFS_PERF_METRIC_MIXED_RW_IOPS],
        g_rtfs_perf_summary.value[RTFS_PERF_METRIC_MIXED_RW_IOPS],
        "IOPS");
    rtfsPerfPrintTableSeparator();
}

static void rtfsPerfFillPattern(
    unsigned char *buffer,
    size_t size,
    uint32_t seed)
{
    size_t i;

    TEST_ASSERT_NOT_NULL(buffer);
    for (i = 0; i < size; ++i)
    {
        buffer[i] = (unsigned char)((seed + (uint32_t)i * 17u) & 0xffu);
    }
}

static uint32_t rtfsPerfNextRand(uint32_t *state)
{
    TEST_ASSERT_NOT_NULL(state);
    *state = (*state * 1103515245u) + 12345u;
    return *state;
}

static void rtfsPerfEnsureFullWrite(
    int fd,
    const unsigned char *buffer,
    size_t size)
{
    ssize_t written = rtfsRtemsMountFixtureWrite(fd, buffer, size);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, (int)written);
    TEST_ASSERT_EQUAL_size_t(size, (size_t)written);
}

static void rtfsPerfEnsureFullRead(
    int fd,
    unsigned char *buffer,
    size_t size)
{
    ssize_t bytes_read = rtfsRtemsMountFixtureRead(fd, buffer, size);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, (int)bytes_read);
    TEST_ASSERT_EQUAL_size_t(size, (size_t)bytes_read);
}

static void rtfsPerfRunIoProfile(
    size_t stream_total_bytes,
    size_t stream_chunk_bytes,
    size_t random_total_bytes,
    uint32_t random_ops,
    uint32_t mixed_ops,
    uint32_t stream_seed,
    uint32_t io_seed)
{
    int fd = -1;
    unsigned char stream_buffer[RTFS_PERF_STREAM_CHUNK_BYTES];
    unsigned char io_buffer[RTFS_PERF_RANDOM_BLOCK_BYTES];

    TEST_ASSERT_TRUE(stream_chunk_bytes <= sizeof(stream_buffer));

    rtfsPerfFillPattern(stream_buffer, stream_chunk_bytes, stream_seed);
    rtfsPerfFillPattern(io_buffer, sizeof(io_buffer), io_seed);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_PERF_STREAM_FILE, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_PERF_STREAM_FILE, O_RDWR, 0, &fd));
    rtfsPerfSequentialWrite(
        fd,
        stream_buffer,
        stream_chunk_bytes,
        stream_total_bytes);
    rtfsPerfSequentialRead(
        fd,
        stream_buffer,
        stream_chunk_bytes,
        stream_total_bytes);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    rtfsPerfPrepareSizedFile(
        RTFS_PERF_RANDOM_FILE,
        random_total_bytes,
        io_buffer,
        sizeof(io_buffer));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_PERF_RANDOM_FILE, O_RDWR, 0, &fd));
    rtfsPerfRandomWriteIops(
        fd,
        io_buffer,
        random_total_bytes,
        random_ops);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    rtfsPerfPrepareSizedFile(
        RTFS_PERF_MIXED_FILE,
        random_total_bytes,
        io_buffer,
        sizeof(io_buffer));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_PERF_MIXED_FILE, O_RDWR, 0, &fd));
    rtfsPerfMixedRwIops(
        fd,
        io_buffer,
        random_total_bytes,
        mixed_ops);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
}

static void rtfsPerfSequentialWrite(
    int fd,
    const unsigned char *buffer,
    size_t chunk_size,
    size_t total_bytes)
{
    size_t remaining;
    uint64_t cold_begin;
    uint64_t cold_end;
    uint64_t hot_begin;
    uint64_t hot_end;
    uint64_t cold_us;
    uint64_t hot_us;

    /*
     * 顺序写采用冷热各半的混合口径：
     * 1. 冷写：空文件首次顺序写；
     * 2. 热写：fdatasync 后保留同一打开句柄与 page cache，再整文件覆盖写。
     *
     * 最终吞吐按等字节权重合成，避免结果过度贴近热缓存顺序读。
     */
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFtruncate(fd, 0));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureLseek(fd, 0, SEEK_SET));
    remaining = total_bytes;
    cold_begin = rtfsPerfCounterNow();
    while (remaining > 0u)
    {
        size_t to_write = remaining > chunk_size ? chunk_size : remaining;
        rtfsPerfEnsureFullWrite(fd, buffer, to_write);
        remaining -= to_write;
    }
    cold_end = rtfsPerfCounterNow();
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureLseek(fd, 0, SEEK_SET));
    remaining = total_bytes;
    hot_begin = rtfsPerfCounterNow();
    while (remaining > 0u)
    {
        size_t to_write = remaining > chunk_size ? chunk_size : remaining;
        rtfsPerfEnsureFullWrite(fd, buffer, to_write);
        remaining -= to_write;
    }
    hot_end = rtfsPerfCounterNow();

    cold_us = rtfsPerfCounterToUs(cold_end - cold_begin);
    hot_us = rtfsPerfCounterToUs(hot_end - hot_begin);

    rtfsPerfRecordThroughput(
        RTFS_PERF_METRIC_SEQUENTIAL_WRITE,
        total_bytes * 2u,
        cold_us + hot_us);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
}

static void rtfsPerfSequentialRead(
    int fd,
    unsigned char *buffer,
    size_t chunk_size,
    size_t total_bytes)
{
    uint64_t begin;
    uint64_t end;
    size_t remaining = total_bytes;

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureLseek(fd, 0, SEEK_SET));

    begin = rtfsPerfCounterNow();
    while (remaining > 0u)
    {
        size_t to_read = remaining > chunk_size ? chunk_size : remaining;
        rtfsPerfEnsureFullRead(fd, buffer, to_read);
        remaining -= to_read;
    }
    end = rtfsPerfCounterNow();

    rtfsPerfRecordThroughput(
        RTFS_PERF_METRIC_SEQUENTIAL_READ,
        total_bytes,
        rtfsPerfCounterToUs(end - begin));
}

static void rtfsPerfPrepareSizedFile(
    const char *relative_path,
    size_t total_bytes,
    const unsigned char *buffer,
    size_t chunk_size)
{
    int fd = -1;
    size_t remaining = total_bytes;

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(relative_path, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(relative_path, O_RDWR, 0, &fd));

    while (remaining > 0u)
    {
        size_t to_write = remaining > chunk_size ? chunk_size : remaining;
        rtfsPerfEnsureFullWrite(fd, buffer, to_write);
        remaining -= to_write;
    }
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
}

static void rtfsPerfRandomWritePass(
    int fd,
    const unsigned char *buffer,
    size_t file_size,
    uint32_t op_count,
    uint32_t seed)
{
    uint32_t rand_state = seed;
    uint32_t block_count = (uint32_t)(file_size / RTFS_PERF_RANDOM_BLOCK_BYTES);
    uint32_t i;

    TEST_ASSERT_TRUE(block_count > 0u);

    for (i = 0; i < op_count; ++i)
    {
        uint32_t block_index = rtfsPerfNextRand(&rand_state) % block_count;
        off_t offset = (off_t)((uint64_t)block_index * RTFS_PERF_RANDOM_BLOCK_BYTES);

        TEST_ASSERT_EQUAL(offset, rtfsRtemsMountFixtureLseek(fd, offset, SEEK_SET));
        rtfsPerfEnsureFullWrite(fd, buffer, RTFS_PERF_RANDOM_BLOCK_BYTES);
    }
}

static void rtfsPerfRandomWriteIops(
    int fd,
    const unsigned char *buffer,
    size_t file_size,
    uint32_t op_count)
{
    uint64_t cold_begin;
    uint64_t cold_end;
    uint64_t hot_begin;
    uint64_t hot_end;
    uint64_t cold_us;
    uint64_t hot_us;

    /*
     * 随机写采用冷热各半的混合口径：
     * 1. 冷写：在当前文件页缓存状态下执行一轮随机写；
     * 2. 热写：保留同一打开句柄与 page cache，再执行一轮相同随机写。
     */
    cold_begin = rtfsPerfCounterNow();
    rtfsPerfRandomWritePass(fd, buffer, file_size, op_count / 2u, 0x13572468u);
    cold_end = rtfsPerfCounterNow();

    hot_begin = rtfsPerfCounterNow();
    rtfsPerfRandomWritePass(fd, buffer, file_size, op_count / 2u, 0x13572468u);
    hot_end = rtfsPerfCounterNow();

    cold_us = rtfsPerfCounterToUs(cold_end - cold_begin);
    hot_us = rtfsPerfCounterToUs(hot_end - hot_begin);

    rtfsPerfRecordIops(
        RTFS_PERF_METRIC_RANDOM_WRITE_IOPS,
        op_count,
        cold_us + hot_us);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
}

static void rtfsPerfMixedRwPass(
    int fd,
    unsigned char *buffer,
    size_t file_size,
    uint32_t op_count,
    uint32_t seed,
    uint32_t *out_read_ops,
    uint32_t *out_write_ops)
{
    uint32_t rand_state = seed;
    uint32_t block_count = (uint32_t)(file_size / RTFS_PERF_RANDOM_BLOCK_BYTES);
    uint32_t read_ops = 0;
    uint32_t write_ops = 0;
    uint32_t i;

    TEST_ASSERT_TRUE(block_count > 0u);

    for (i = 0; i < op_count; ++i)
    {
        uint32_t block_index = rtfsPerfNextRand(&rand_state) % block_count;
        off_t offset = (off_t)((uint64_t)block_index * RTFS_PERF_RANDOM_BLOCK_BYTES);

        TEST_ASSERT_EQUAL(offset, rtfsRtemsMountFixtureLseek(fd, offset, SEEK_SET));
        if ((i & 1u) == 0u)
        {
            rtfsPerfEnsureFullWrite(fd, buffer, RTFS_PERF_RANDOM_BLOCK_BYTES);
            ++write_ops;
        }
        else
        {
            rtfsPerfEnsureFullRead(fd, buffer, RTFS_PERF_RANDOM_BLOCK_BYTES);
            ++read_ops;
        }
    }

    if (out_read_ops != NULL)
    {
        *out_read_ops = read_ops;
    }
    if (out_write_ops != NULL)
    {
        *out_write_ops = write_ops;
    }
}

static void rtfsPerfMixedRwIops(
    int fd,
    unsigned char *buffer,
    size_t file_size,
    uint32_t op_count)
{
    uint32_t cold_read_ops = 0;
    uint32_t cold_write_ops = 0;
    uint32_t hot_read_ops = 0;
    uint32_t hot_write_ops = 0;
    uint64_t cold_begin;
    uint64_t cold_end;
    uint64_t hot_begin;
    uint64_t hot_end;
    uint64_t cold_us;
    uint64_t hot_us;

    /*
     * 混合读写采用冷热各半的混合口径：
     * 1. 冷阶段：执行一轮 50/50 随机读写；
     * 2. 热阶段：保留同一打开句柄与 page cache，再执行一轮相同随机读写。
     */
    cold_begin = rtfsPerfCounterNow();
    rtfsPerfMixedRwPass(
        fd,
        buffer,
        file_size,
        op_count / 2u,
        0x24681357u,
        &cold_read_ops,
        &cold_write_ops);
    cold_end = rtfsPerfCounterNow();

    hot_begin = rtfsPerfCounterNow();
    rtfsPerfMixedRwPass(
        fd,
        buffer,
        file_size,
        op_count / 2u,
        0x24681357u,
        &hot_read_ops,
        &hot_write_ops);
    hot_end = rtfsPerfCounterNow();

    TEST_ASSERT_EQUAL_UINT32(op_count / 4u, cold_read_ops);
    TEST_ASSERT_EQUAL_UINT32(op_count / 4u, cold_write_ops);
    TEST_ASSERT_EQUAL_UINT32(op_count / 4u, hot_read_ops);
    TEST_ASSERT_EQUAL_UINT32(op_count / 4u, hot_write_ops);

    cold_us = rtfsPerfCounterToUs(cold_end - cold_begin);
    hot_us = rtfsPerfCounterToUs(hot_end - hot_begin);

    rtfsPerfRecordIops(
        RTFS_PERF_METRIC_MIXED_RW_IOPS,
        op_count,
        cold_us + hot_us);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
}

static void rtfsPerfMetadataCreateDelete(void)
{
    uint64_t create_begin;
    uint64_t create_end;
    uint64_t delete_begin;
    uint64_t delete_end;
    uint32_t i;

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_PERF_META_DIR, 0755));

    create_begin = rtfsPerfCounterNow();
    for (i = 0; i < RTFS_PERF_META_FILE_COUNT; ++i)
    {
        char path[64];
        int fd = -1;

        snprintf(path, sizeof(path), "%s/f%03u", RTFS_PERF_META_DIR, i);
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path, 0644));
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureOpen(path, O_RDWR, 0, &fd));
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    }
    create_end = rtfsPerfCounterNow();
    rtfsPerfRecordUsPerOp(
        RTFS_PERF_METRIC_METADATA_CREATE,
        RTFS_PERF_META_FILE_COUNT,
        rtfsPerfCounterToUs(create_end - create_begin));

    delete_begin = rtfsPerfCounterNow();
    for (i = 0; i < RTFS_PERF_META_FILE_COUNT; ++i)
    {
        char path[64];

        snprintf(path, sizeof(path), "%s/f%03u", RTFS_PERF_META_DIR, i);
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureUnlink(path));
    }
    delete_end = rtfsPerfCounterNow();
    rtfsPerfRecordUsPerOp(
        RTFS_PERF_METRIC_METADATA_DELETE,
        RTFS_PERF_META_FILE_COUNT,
        rtfsPerfCounterToUs(delete_end - delete_begin));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRmdir(RTFS_PERF_META_DIR));
}

static void rtfsPerfSmallFileCreation(
    const unsigned char *buffer,
    size_t buffer_size)
{
    uint64_t begin;
    uint64_t end;
    uint32_t i;

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_PERF_SMALL_DIR, 0755));

    begin = rtfsPerfCounterNow();
    for (i = 0; i < RTFS_PERF_SMALL_FILE_COUNT; ++i)
    {
        char path[64];
        int fd = -1;

        snprintf(path, sizeof(path), "%s/s%03u.bin", RTFS_PERF_SMALL_DIR, i);
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path, 0644));
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureOpen(path, O_RDWR, 0, &fd));
        rtfsPerfEnsureFullWrite(fd, buffer, buffer_size);
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    }
    end = rtfsPerfCounterNow();

    rtfsPerfRecordIops(
        RTFS_PERF_METRIC_SMALL_FILE_CREATION,
        RTFS_PERF_SMALL_FILE_COUNT,
        rtfsPerfCounterToUs(end - begin));

    for (i = 0; i < RTFS_PERF_SMALL_FILE_COUNT; ++i)
    {
        char path[64];

        snprintf(path, sizeof(path), "%s/s%03u.bin", RTFS_PERF_SMALL_DIR, i);
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureUnlink(path));
    }

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRmdir(RTFS_PERF_SMALL_DIR));
}

RTFS_TEST_GROUP(
    "performance",
    PerformanceStreaming_SequentialReadWriteRandomAndMixed_ShouldReportMetrics)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    RtfsPerfLogGuard log_guard = rtfsPerfBeginQuietLogging();
    unsigned char small_file_buffer[RTFS_PERF_SMALL_FILE_BYTES];

    rtfsPerfResetSummary();

    /*
     * 大文件流式场景：
     * 1. Sequential Write:      8 MiB 文件预热后整文件覆盖写, 64 KiB chunk
     * 2. Sequential Read:       热缓存顺序读 8 MiB, 64 KiB chunk
     * 3. Random/Mixed R/W:      8 MiB 工作集冷热各半, 4 KiB 访问粒度
     * 4. Metadata Create/Delete 与 Small File Creation 也补测，
     *    但使用同一大文件导向设备规模
     */

    rtfsPerfFillPattern(
        small_file_buffer,
        sizeof(small_file_buffer),
        0x73u);

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_PERF_LPA_COUNT));

    rtfsPerfRunIoProfile(
        RTFS_PERF_STREAM_TOTAL_BYTES,
        RTFS_PERF_STREAM_CHUNK_BYTES,
        RTFS_PERF_RANDOM_TOTAL_BYTES,
        RTFS_PERF_RANDOM_OPS,
        RTFS_PERF_MIXED_OPS,
        0x31u,
        0x57u);
    rtfsPerfMetadataCreateDelete();
    rtfsPerfSmallFileCreation(
        small_file_buffer,
        sizeof(small_file_buffer));

    rtfsRtemsMountFixtureDestroy(&fixture);
    rtfsPerfEndQuietLogging(&log_guard);
    rtfsPerfPrintBenchmarkTable("Large File Streaming Benchmark Results");
}

RTFS_TEST_GROUP(
    "performance",
    PerformanceMetadata_CreateDeleteAndSmallFiles_ShouldReportMetrics)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    RtfsPerfLogGuard log_guard = rtfsPerfBeginQuietLogging();
    unsigned char small_file_buffer[RTFS_PERF_SMALL_FILE_BYTES];

    rtfsPerfResetSummary();

    /*
     * 小文件 / 高元数据场景：
     * 1. Sequential Write:      1 MiB 文件预热后整文件覆盖写, 4 KiB chunk
     * 2. Sequential Read:       热缓存顺序读 1 MiB, 4 KiB chunk
     * 3. Random/Mixed R/W:      1 MiB 工作集冷热各半, 4 KiB 访问粒度
     * 4. Metadata Create/Delete: NR_INLINE_DENTRY 个空文件
     * 5. Small File Creation:    128 个 1 KiB 文件，逐文件 fdatasync
     */

    rtfsPerfFillPattern(
        small_file_buffer,
        sizeof(small_file_buffer),
        0x73u);

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_PERF_LPA_COUNT));

    rtfsPerfRunIoProfile(
        RTFS_PERF_META_STREAM_TOTAL_BYTES,
        RTFS_PERF_META_STREAM_CHUNK_BYTES,
        RTFS_PERF_META_RANDOM_TOTAL_BYTES,
        RTFS_PERF_META_RANDOM_OPS,
        RTFS_PERF_META_MIXED_OPS,
        0x21u,
        0x43u);
    rtfsPerfMetadataCreateDelete();
    rtfsPerfSmallFileCreation(
        small_file_buffer,
        sizeof(small_file_buffer));

    rtfsRtemsMountFixtureDestroy(&fixture);
    rtfsPerfEndQuietLogging(&log_guard);
    rtfsPerfPrintBenchmarkTable("Small File / Metadata Benchmark Results");
}
