#include "integration/r2fs_rtems_mount_fixture.h"
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

#define R2FS_PERF_LPA_COUNT (32U * BLOCK_PER_SEGMENT)
#define R2FS_PERF_STREAM_FILE "/perf-stream.bin"
#define R2FS_PERF_RANDOM_FILE "/perf-random.bin"
#define R2FS_PERF_MIXED_FILE "/perf-mixed.bin"
#define R2FS_PERF_META_DIR "/perf-meta"
#define R2FS_PERF_SMALL_DIR "/perf-small"

#define R2FS_PERF_STREAM_TOTAL_BYTES (8U * 1024U * 1024U)
#define R2FS_PERF_STREAM_CHUNK_BYTES (64U * 1024U)
#define R2FS_PERF_RANDOM_BLOCK_BYTES BLOCK_BUFFER_SIZE
#define R2FS_PERF_RANDOM_TOTAL_BYTES (8U * 1024U * 1024U)
#define R2FS_PERF_RANDOM_OPS 1024U
#define R2FS_PERF_MIXED_OPS 1024U
#define R2FS_PERF_META_STREAM_TOTAL_BYTES (1024U * 1024U)
#define R2FS_PERF_META_STREAM_CHUNK_BYTES BLOCK_BUFFER_SIZE
#define R2FS_PERF_META_RANDOM_TOTAL_BYTES (1024U * 1024U)
#define R2FS_PERF_META_RANDOM_OPS 256U
#define R2FS_PERF_META_MIXED_OPS 256U
#define R2FS_PERF_META_FILE_COUNT NR_INLINE_DENTRY
#define R2FS_PERF_SMALL_FILE_COUNT 128U
#define R2FS_PERF_SMALL_FILE_BYTES 1024U

typedef struct R2fsPerfLogGuard
{
    RtfsLogLevel previous_level;
} R2fsPerfLogGuard;

typedef enum R2fsPerfMetricId
{
    R2FS_PERF_METRIC_SEQUENTIAL_WRITE = 0,
    R2FS_PERF_METRIC_SEQUENTIAL_READ,
    R2FS_PERF_METRIC_METADATA_CREATE,
    R2FS_PERF_METRIC_METADATA_DELETE,
    R2FS_PERF_METRIC_RANDOM_WRITE_IOPS,
    R2FS_PERF_METRIC_SMALL_FILE_CREATION,
    R2FS_PERF_METRIC_MIXED_RW_IOPS,
    R2FS_PERF_METRIC_COUNT
} R2fsPerfMetricId;

typedef struct R2fsPerfSummary
{
    bool valid[R2FS_PERF_METRIC_COUNT];
    double value[R2FS_PERF_METRIC_COUNT];
    uint32_t ops[R2FS_PERF_METRIC_COUNT];
    uint64_t us[R2FS_PERF_METRIC_COUNT];
} R2fsPerfSummary;

static R2fsPerfSummary g_r2fs_perf_summary;

static void r2fsPerfSequentialWrite(
    int fd,
    const unsigned char *buffer,
    size_t chunk_size,
    size_t total_bytes);
static void r2fsPerfSequentialRead(
    int fd,
    unsigned char *buffer,
    size_t chunk_size,
    size_t total_bytes);
static void r2fsPerfPrepareSizedFile(
    const char *relative_path,
    size_t total_bytes,
    const unsigned char *buffer,
    size_t chunk_size);
static void r2fsPerfRandomWriteIops(
    int fd,
    const unsigned char *buffer,
    size_t file_size,
    uint32_t op_count);
static void r2fsPerfMixedRwIops(
    int fd,
    unsigned char *buffer,
    size_t file_size,
    uint32_t op_count);

static uint64_t r2fsPerfCounterFreq(void)
{
    return rtems_counter_frequency();
}

static uint64_t r2fsPerfCounterNow(void)
{
    return rtems_counter_read();
}

static uint64_t r2fsPerfCounterToUs(uint64_t diff)
{
    return (diff * 1000000ULL) / r2fsPerfCounterFreq();
}

static R2fsPerfLogGuard r2fsPerfBeginQuietLogging(void)
{
    R2fsPerfLogGuard guard;

    guard.previous_level = rtfsLogGetMinLevel();
    rtfsLogSetMinLevel(RTFS_LOG_SILENT);
    return guard;
}

static void r2fsPerfEndQuietLogging(const R2fsPerfLogGuard *guard)
{
    TEST_ASSERT_NOT_NULL(guard);
    rtfsLogSetMinLevel(guard->previous_level);
}

static void r2fsPerfResetSummary(void)
{
    memset(&g_r2fs_perf_summary, 0, sizeof(g_r2fs_perf_summary));
}

static double r2fsPerfBytesToMbPerSec(uint64_t bytes, uint64_t us)
{
    TEST_ASSERT_TRUE(us > 0u);
    return ((double)bytes / 1000000.0) / ((double)us / 1000000.0);
}

static double r2fsPerfOpsToIops(uint32_t ops, uint64_t us)
{
    TEST_ASSERT_TRUE(us > 0u);
    return (double)ops / ((double)us / 1000000.0);
}

static double r2fsPerfUsPerOp(uint32_t ops, uint64_t us)
{
    TEST_ASSERT_TRUE(ops > 0u);
    TEST_ASSERT_TRUE(us > 0u);
    return (double)us / (double)ops;
}

static void r2fsPerfRecordValue(
    R2fsPerfMetricId metric_id,
    double value,
    uint32_t ops,
    uint64_t us)
{
    TEST_ASSERT_TRUE(metric_id < R2FS_PERF_METRIC_COUNT);
    g_r2fs_perf_summary.valid[metric_id] = true;
    g_r2fs_perf_summary.value[metric_id] = value;
    g_r2fs_perf_summary.ops[metric_id] = ops;
    g_r2fs_perf_summary.us[metric_id] = us;
}

static void r2fsPerfRecordThroughput(
    R2fsPerfMetricId metric_id,
    uint64_t bytes,
    uint64_t us)
{
    r2fsPerfRecordValue(
        metric_id,
        r2fsPerfBytesToMbPerSec(bytes, us),
        0u,
        us);
}

static void r2fsPerfRecordIops(
    R2fsPerfMetricId metric_id,
    uint32_t ops,
    uint64_t us)
{
    r2fsPerfRecordValue(
        metric_id,
        r2fsPerfOpsToIops(ops, us),
        ops,
        us);
}

static void r2fsPerfRecordUsPerOp(
    R2fsPerfMetricId metric_id,
    uint32_t ops,
    uint64_t us)
{
    r2fsPerfRecordValue(
        metric_id,
        r2fsPerfUsPerOp(ops, us),
        ops,
        us);
}

static void r2fsPerfPrintTableSeparator(void)
{
    printf("+---------------------------+---------------+-----------+\n");
}

static void r2fsPerfPrintTableHeader(const char *title)
{
    TEST_ASSERT_NOT_NULL(title);
    printf("\n--- %s ---\n", title);
    r2fsPerfPrintTableSeparator();
    printf("| %-25s | %-13s | %-9s |\n", "Test Name", "Result", "Unit");
    r2fsPerfPrintTableSeparator();
}

static void r2fsPerfPrintTableRow(
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

static double r2fsPerfMetadataCreateDeleteUsPerOp(void)
{
    uint32_t total_ops =
        g_r2fs_perf_summary.ops[R2FS_PERF_METRIC_METADATA_CREATE] +
        g_r2fs_perf_summary.ops[R2FS_PERF_METRIC_METADATA_DELETE];
    uint64_t total_us =
        g_r2fs_perf_summary.us[R2FS_PERF_METRIC_METADATA_CREATE] +
        g_r2fs_perf_summary.us[R2FS_PERF_METRIC_METADATA_DELETE];

    return r2fsPerfUsPerOp(total_ops, total_us);
}

static void r2fsPerfPrintBenchmarkTable(const char *title)
{
    bool metadata_available =
        g_r2fs_perf_summary.valid[R2FS_PERF_METRIC_METADATA_CREATE] &&
        g_r2fs_perf_summary.valid[R2FS_PERF_METRIC_METADATA_DELETE];

    r2fsPerfPrintTableHeader(title);
    r2fsPerfPrintTableRow(
        "Sequential Write",
        g_r2fs_perf_summary.valid[R2FS_PERF_METRIC_SEQUENTIAL_WRITE],
        g_r2fs_perf_summary.value[R2FS_PERF_METRIC_SEQUENTIAL_WRITE],
        "MB/s");
    r2fsPerfPrintTableRow(
        "Sequential Read",
        g_r2fs_perf_summary.valid[R2FS_PERF_METRIC_SEQUENTIAL_READ],
        g_r2fs_perf_summary.value[R2FS_PERF_METRIC_SEQUENTIAL_READ],
        "MB/s");
    r2fsPerfPrintTableRow(
        "Metadata Create/Delete",
        metadata_available,
        metadata_available ? r2fsPerfMetadataCreateDeleteUsPerOp() : 0.0,
        "us/op");
    r2fsPerfPrintTableRow(
        "Random Write IOPS",
        g_r2fs_perf_summary.valid[R2FS_PERF_METRIC_RANDOM_WRITE_IOPS],
        g_r2fs_perf_summary.value[R2FS_PERF_METRIC_RANDOM_WRITE_IOPS],
        "IOPS");
    r2fsPerfPrintTableRow(
        "Small File Creation",
        g_r2fs_perf_summary.valid[R2FS_PERF_METRIC_SMALL_FILE_CREATION],
        g_r2fs_perf_summary.value[R2FS_PERF_METRIC_SMALL_FILE_CREATION],
        "Files/sec");
    r2fsPerfPrintTableRow(
        "Mixed R/W IOPS",
        g_r2fs_perf_summary.valid[R2FS_PERF_METRIC_MIXED_RW_IOPS],
        g_r2fs_perf_summary.value[R2FS_PERF_METRIC_MIXED_RW_IOPS],
        "IOPS");
    r2fsPerfPrintTableSeparator();
}

static void r2fsPerfFillPattern(
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

static uint32_t r2fsPerfNextRand(uint32_t *state)
{
    TEST_ASSERT_NOT_NULL(state);
    *state = (*state * 1103515245u) + 12345u;
    return *state;
}

static void r2fsPerfEnsureFullWrite(
    int fd,
    const unsigned char *buffer,
    size_t size)
{
    size_t total = 0;

    while (total < size)
    {
        ssize_t written = r2fsRtemsMountFixtureWrite(
            fd,
            buffer + total,
            size - total);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, (int)written);
        total += (size_t)written;
    }
}

static void r2fsPerfEnsureFullRead(
    int fd,
    unsigned char *buffer,
    size_t size)
{
    size_t total = 0;

    while (total < size)
    {
        ssize_t bytes_read = r2fsRtemsMountFixtureRead(
            fd,
            buffer + total,
            size - total);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, (int)bytes_read);
        TEST_ASSERT_NOT_EQUAL_size_t(0u, (size_t)bytes_read);
        total += (size_t)bytes_read;
    }
}

static void r2fsPerfRunIoProfile(
    size_t stream_total_bytes,
    size_t stream_chunk_bytes,
    size_t random_total_bytes,
    uint32_t random_ops,
    uint32_t mixed_ops,
    uint32_t stream_seed,
    uint32_t io_seed)
{
    int fd = -1;
    unsigned char stream_buffer[R2FS_PERF_STREAM_CHUNK_BYTES];
    unsigned char io_buffer[R2FS_PERF_RANDOM_BLOCK_BYTES];

    TEST_ASSERT_TRUE(stream_chunk_bytes <= sizeof(stream_buffer));

    r2fsPerfFillPattern(stream_buffer, stream_chunk_bytes, stream_seed);
    r2fsPerfFillPattern(io_buffer, sizeof(io_buffer), io_seed);

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(R2FS_PERF_STREAM_FILE, 0644));
    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureOpen(R2FS_PERF_STREAM_FILE, O_RDWR, 0, &fd));
    r2fsPerfSequentialWrite(
        fd,
        stream_buffer,
        stream_chunk_bytes,
        stream_total_bytes);
    r2fsPerfSequentialRead(
        fd,
        stream_buffer,
        stream_chunk_bytes,
        stream_total_bytes);
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
    fd = -1;

    r2fsPerfPrepareSizedFile(
        R2FS_PERF_RANDOM_FILE,
        random_total_bytes,
        io_buffer,
        sizeof(io_buffer));
    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureOpen(R2FS_PERF_RANDOM_FILE, O_RDWR, 0, &fd));
    r2fsPerfRandomWriteIops(
        fd,
        io_buffer,
        random_total_bytes,
        random_ops);
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
    fd = -1;

    r2fsPerfPrepareSizedFile(
        R2FS_PERF_MIXED_FILE,
        random_total_bytes,
        io_buffer,
        sizeof(io_buffer));
    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureOpen(R2FS_PERF_MIXED_FILE, O_RDWR, 0, &fd));
    r2fsPerfMixedRwIops(
        fd,
        io_buffer,
        random_total_bytes,
        mixed_ops);
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
}

static void r2fsPerfSequentialWrite(
    int fd,
    const unsigned char *buffer,
    size_t chunk_size,
    size_t total_bytes)
{
    uint64_t begin;
    uint64_t end;
    size_t remaining = total_bytes;

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureFtruncate(fd, 0));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureLseek(fd, 0, SEEK_SET));

    begin = r2fsPerfCounterNow();
    while (remaining > 0u)
    {
        size_t to_write = remaining > chunk_size ? chunk_size : remaining;
        r2fsPerfEnsureFullWrite(fd, buffer, to_write);
        remaining -= to_write;
    }
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureFdatasync(fd));
    end = r2fsPerfCounterNow();

    r2fsPerfRecordThroughput(
        R2FS_PERF_METRIC_SEQUENTIAL_WRITE,
        total_bytes,
        r2fsPerfCounterToUs(end - begin));
}

static void r2fsPerfSequentialRead(
    int fd,
    unsigned char *buffer,
    size_t chunk_size,
    size_t total_bytes)
{
    uint64_t begin;
    uint64_t end;
    size_t remaining = total_bytes;

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureLseek(fd, 0, SEEK_SET));

    begin = r2fsPerfCounterNow();
    while (remaining > 0u)
    {
        size_t to_read = remaining > chunk_size ? chunk_size : remaining;
        r2fsPerfEnsureFullRead(fd, buffer, to_read);
        remaining -= to_read;
    }
    end = r2fsPerfCounterNow();

    r2fsPerfRecordThroughput(
        R2FS_PERF_METRIC_SEQUENTIAL_READ,
        total_bytes,
        r2fsPerfCounterToUs(end - begin));
}

static void r2fsPerfPrepareSizedFile(
    const char *relative_path,
    size_t total_bytes,
    const unsigned char *buffer,
    size_t chunk_size)
{
    int fd = -1;
    size_t remaining = total_bytes;

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(relative_path, 0644));
    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureOpen(relative_path, O_RDWR, 0, &fd));

    while (remaining > 0u)
    {
        size_t to_write = remaining > chunk_size ? chunk_size : remaining;
        r2fsPerfEnsureFullWrite(fd, buffer, to_write);
        remaining -= to_write;
    }
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureFdatasync(fd));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
}

static void r2fsPerfRandomWriteIops(
    int fd,
    const unsigned char *buffer,
    size_t file_size,
    uint32_t op_count)
{
    uint32_t rand_state = 0x13572468u;
    uint32_t block_count = (uint32_t)(file_size / R2FS_PERF_RANDOM_BLOCK_BYTES);
    uint64_t begin;
    uint64_t end;
    uint32_t i;

    TEST_ASSERT_TRUE(block_count > 0u);

    begin = r2fsPerfCounterNow();
    for (i = 0; i < op_count; ++i)
    {
        uint32_t block_index = r2fsPerfNextRand(&rand_state) % block_count;
        off_t offset = (off_t)((uint64_t)block_index * R2FS_PERF_RANDOM_BLOCK_BYTES);

        TEST_ASSERT_EQUAL(offset, r2fsRtemsMountFixtureLseek(fd, offset, SEEK_SET));
        r2fsPerfEnsureFullWrite(fd, buffer, R2FS_PERF_RANDOM_BLOCK_BYTES);
    }
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureFdatasync(fd));
    end = r2fsPerfCounterNow();

    r2fsPerfRecordIops(
        R2FS_PERF_METRIC_RANDOM_WRITE_IOPS,
        op_count,
        r2fsPerfCounterToUs(end - begin));
}

static void r2fsPerfMixedRwIops(
    int fd,
    unsigned char *buffer,
    size_t file_size,
    uint32_t op_count)
{
    uint32_t rand_state = 0x24681357u;
    uint32_t block_count = (uint32_t)(file_size / R2FS_PERF_RANDOM_BLOCK_BYTES);
    uint32_t read_ops = 0;
    uint32_t write_ops = 0;
    uint64_t begin;
    uint64_t end;
    uint32_t i;

    TEST_ASSERT_TRUE(block_count > 0u);

    begin = r2fsPerfCounterNow();
    for (i = 0; i < op_count; ++i)
    {
        uint32_t block_index = r2fsPerfNextRand(&rand_state) % block_count;
        off_t offset = (off_t)((uint64_t)block_index * R2FS_PERF_RANDOM_BLOCK_BYTES);

        TEST_ASSERT_EQUAL(offset, r2fsRtemsMountFixtureLseek(fd, offset, SEEK_SET));
        if ((i & 1u) == 0u)
        {
            r2fsPerfEnsureFullWrite(fd, buffer, R2FS_PERF_RANDOM_BLOCK_BYTES);
            ++write_ops;
        }
        else
        {
            r2fsPerfEnsureFullRead(fd, buffer, R2FS_PERF_RANDOM_BLOCK_BYTES);
            ++read_ops;
        }
    }
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureFdatasync(fd));
    end = r2fsPerfCounterNow();

    TEST_ASSERT_EQUAL_UINT32(op_count / 2u, read_ops);
    TEST_ASSERT_EQUAL_UINT32(op_count / 2u, write_ops);
    r2fsPerfRecordIops(
        R2FS_PERF_METRIC_MIXED_RW_IOPS,
        op_count,
        r2fsPerfCounterToUs(end - begin));
}

static void r2fsPerfMetadataCreateDelete(void)
{
    uint64_t create_begin;
    uint64_t create_end;
    uint64_t delete_begin;
    uint64_t delete_end;
    uint32_t i;

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_PERF_META_DIR, 0755));

    create_begin = r2fsPerfCounterNow();
    for (i = 0; i < R2FS_PERF_META_FILE_COUNT; ++i)
    {
        char path[64];
        int fd = -1;

        snprintf(path, sizeof(path), "%s/f%03u", R2FS_PERF_META_DIR, i);
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path, 0644));
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureOpen(path, O_RDWR, 0, &fd));
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
    }
    create_end = r2fsPerfCounterNow();
    r2fsPerfRecordUsPerOp(
        R2FS_PERF_METRIC_METADATA_CREATE,
        R2FS_PERF_META_FILE_COUNT,
        r2fsPerfCounterToUs(create_end - create_begin));

    delete_begin = r2fsPerfCounterNow();
    for (i = 0; i < R2FS_PERF_META_FILE_COUNT; ++i)
    {
        char path[64];

        snprintf(path, sizeof(path), "%s/f%03u", R2FS_PERF_META_DIR, i);
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureUnlink(path));
    }
    delete_end = r2fsPerfCounterNow();
    r2fsPerfRecordUsPerOp(
        R2FS_PERF_METRIC_METADATA_DELETE,
        R2FS_PERF_META_FILE_COUNT,
        r2fsPerfCounterToUs(delete_end - delete_begin));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRmdir(R2FS_PERF_META_DIR));
}

static void r2fsPerfSmallFileCreation(
    const unsigned char *buffer,
    size_t buffer_size)
{
    uint64_t begin;
    uint64_t end;
    uint32_t i;

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_PERF_SMALL_DIR, 0755));

    begin = r2fsPerfCounterNow();
    for (i = 0; i < R2FS_PERF_SMALL_FILE_COUNT; ++i)
    {
        char path[64];
        int fd = -1;

        snprintf(path, sizeof(path), "%s/s%03u.bin", R2FS_PERF_SMALL_DIR, i);
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path, 0644));
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureOpen(path, O_RDWR, 0, &fd));
        r2fsPerfEnsureFullWrite(fd, buffer, buffer_size);
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureFdatasync(fd));
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
    }
    end = r2fsPerfCounterNow();

    r2fsPerfRecordIops(
        R2FS_PERF_METRIC_SMALL_FILE_CREATION,
        R2FS_PERF_SMALL_FILE_COUNT,
        r2fsPerfCounterToUs(end - begin));

    for (i = 0; i < R2FS_PERF_SMALL_FILE_COUNT; ++i)
    {
        char path[64];

        snprintf(path, sizeof(path), "%s/s%03u.bin", R2FS_PERF_SMALL_DIR, i);
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureUnlink(path));
    }

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRmdir(R2FS_PERF_SMALL_DIR));
}

RTFS_TEST_GROUP(
    "performance",
    PerformanceStreaming_SequentialReadWriteRandomAndMixed_ShouldReportMetrics)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    R2fsPerfLogGuard log_guard = r2fsPerfBeginQuietLogging();
    unsigned char small_file_buffer[R2FS_PERF_SMALL_FILE_BYTES];

    r2fsPerfResetSummary();

    /*
     * 大文件流式场景：
     * 1. Sequential Write/Read: 8 MiB, 64 KiB chunk
     * 2. Random/Mixed R/W:      8 MiB 工作集, 4 KiB 访问粒度
     * 3. Metadata Create/Delete 与 Small File Creation 也补测，
     *    但使用同一大文件导向设备规模
     */

    r2fsPerfFillPattern(
        small_file_buffer,
        sizeof(small_file_buffer),
        0x73u);

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_PERF_LPA_COUNT));

    r2fsPerfRunIoProfile(
        R2FS_PERF_STREAM_TOTAL_BYTES,
        R2FS_PERF_STREAM_CHUNK_BYTES,
        R2FS_PERF_RANDOM_TOTAL_BYTES,
        R2FS_PERF_RANDOM_OPS,
        R2FS_PERF_MIXED_OPS,
        0x31u,
        0x57u);
    r2fsPerfMetadataCreateDelete();
    r2fsPerfSmallFileCreation(
        small_file_buffer,
        sizeof(small_file_buffer));

    r2fsRtemsMountFixtureDestroy(&fixture);
    r2fsPerfEndQuietLogging(&log_guard);
    r2fsPerfPrintBenchmarkTable("Large File Streaming Benchmark Results");
}

RTFS_TEST_GROUP(
    "performance",
    PerformanceMetadata_CreateDeleteAndSmallFiles_ShouldReportMetrics)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    R2fsPerfLogGuard log_guard = r2fsPerfBeginQuietLogging();
    unsigned char small_file_buffer[R2FS_PERF_SMALL_FILE_BYTES];

    r2fsPerfResetSummary();

    /*
     * 小文件 / 高元数据场景：
     * 1. Sequential Write/Read: 1 MiB, 4 KiB chunk
     * 2. Random/Mixed R/W:      1 MiB 工作集, 4 KiB 访问粒度
     * 3. Metadata Create/Delete: NR_INLINE_DENTRY 个空文件
     * 4. Small File Creation:    128 个 1 KiB 文件，逐文件 fdatasync
     */

    r2fsPerfFillPattern(
        small_file_buffer,
        sizeof(small_file_buffer),
        0x73u);

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_PERF_LPA_COUNT));

    r2fsPerfRunIoProfile(
        R2FS_PERF_META_STREAM_TOTAL_BYTES,
        R2FS_PERF_META_STREAM_CHUNK_BYTES,
        R2FS_PERF_META_RANDOM_TOTAL_BYTES,
        R2FS_PERF_META_RANDOM_OPS,
        R2FS_PERF_META_MIXED_OPS,
        0x21u,
        0x43u);
    r2fsPerfMetadataCreateDelete();
    r2fsPerfSmallFileCreation(
        small_file_buffer,
        sizeof(small_file_buffer));

    r2fsRtemsMountFixtureDestroy(&fixture);
    r2fsPerfEndQuietLogging(&log_guard);
    r2fsPerfPrintBenchmarkTable("Small File / Metadata Benchmark Results");
}
