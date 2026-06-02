#include "integration/r2fs_rtems_mount_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"
#include "fs/fs.h"

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
#define R2FS_PERF_META_FILE_COUNT NR_INLINE_DENTRY
#define R2FS_PERF_SMALL_FILE_COUNT 128U
#define R2FS_PERF_SMALL_FILE_BYTES 1024U

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

static void r2fsPerfPrintThroughput(
    const char *workload_class,
    const char *metric,
    uint64_t bytes,
    uint64_t us)
{
    double mib_per_sec;

    TEST_ASSERT_NOT_NULL(workload_class);
    TEST_ASSERT_NOT_NULL(metric);
    TEST_ASSERT_TRUE(us > 0u);

    mib_per_sec =
        ((double)bytes / (1024.0 * 1024.0)) /
        ((double)us / 1000000.0);
    printf(
        "[ PERF ] class=%s metric=%s bytes=%llu time_us=%llu throughput_mib_s=%.3f\n",
        workload_class,
        metric,
        (unsigned long long)bytes,
        (unsigned long long)us,
        mib_per_sec);
}

static void r2fsPerfPrintIops(
    const char *workload_class,
    const char *metric,
    uint32_t ops,
    uint64_t bytes,
    uint64_t us)
{
    double iops;
    double mib_per_sec;

    TEST_ASSERT_NOT_NULL(workload_class);
    TEST_ASSERT_NOT_NULL(metric);
    TEST_ASSERT_TRUE(us > 0u);

    iops = (double)ops / ((double)us / 1000000.0);
    mib_per_sec =
        ((double)bytes / (1024.0 * 1024.0)) /
        ((double)us / 1000000.0);
    printf(
        "[ PERF ] class=%s metric=%s ops=%u bytes=%llu time_us=%llu iops=%.3f throughput_mib_s=%.3f\n",
        workload_class,
        metric,
        ops,
        (unsigned long long)bytes,
        (unsigned long long)us,
        iops,
        mib_per_sec);
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

    r2fsPerfPrintThroughput(
        "streaming",
        "sequential_write",
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

    r2fsPerfPrintThroughput(
        "streaming",
        "sequential_read",
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

    r2fsPerfPrintIops(
        "streaming",
        "random_write_iops",
        op_count,
        (uint64_t)op_count * R2FS_PERF_RANDOM_BLOCK_BYTES,
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

    printf(
        "[ PERF ] class=streaming metric=mixed_rw_profile read_ops=%u write_ops=%u ratio=50_50\n",
        read_ops,
        write_ops);
    r2fsPerfPrintIops(
        "streaming",
        "mixed_rw_iops",
        op_count,
        (uint64_t)op_count * R2FS_PERF_RANDOM_BLOCK_BYTES,
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
    r2fsPerfPrintIops(
        "metadata",
        "metadata_create",
        R2FS_PERF_META_FILE_COUNT,
        0u,
        r2fsPerfCounterToUs(create_end - create_begin));

    delete_begin = r2fsPerfCounterNow();
    for (i = 0; i < R2FS_PERF_META_FILE_COUNT; ++i)
    {
        char path[64];

        snprintf(path, sizeof(path), "%s/f%03u", R2FS_PERF_META_DIR, i);
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureUnlink(path));
    }
    delete_end = r2fsPerfCounterNow();
    r2fsPerfPrintIops(
        "metadata",
        "metadata_delete",
        R2FS_PERF_META_FILE_COUNT,
        0u,
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

    r2fsPerfPrintIops(
        "metadata",
        "small_file_creation",
        R2FS_PERF_SMALL_FILE_COUNT,
        (uint64_t)R2FS_PERF_SMALL_FILE_COUNT * buffer_size,
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
    int fd = -1;
    unsigned char stream_buffer[R2FS_PERF_STREAM_CHUNK_BYTES];
    unsigned char io_buffer[R2FS_PERF_RANDOM_BLOCK_BYTES];

    /*
     * 大文件流式场景：
     * 1. Sequential Write: 8 MiB, 64 KiB chunk, 单次 fdatasync 收尾
     * 2. Sequential Read:   8 MiB, 64 KiB chunk
     * 3. Random Write IOPS: 4 KiB random overwrite, 1024 ops
     * 4. Mixed R/W IOPS:    4 KiB random 50/50 read/write, 1024 ops
     */

    r2fsPerfFillPattern(stream_buffer, sizeof(stream_buffer), 0x31u);
    r2fsPerfFillPattern(io_buffer, sizeof(io_buffer), 0x57u);

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_PERF_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(R2FS_PERF_STREAM_FILE, 0644));
    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureOpen(R2FS_PERF_STREAM_FILE, O_RDWR, 0, &fd));
    r2fsPerfSequentialWrite(
        fd,
        stream_buffer,
        sizeof(stream_buffer),
        R2FS_PERF_STREAM_TOTAL_BYTES);
    r2fsPerfSequentialRead(
        fd,
        stream_buffer,
        sizeof(stream_buffer),
        R2FS_PERF_STREAM_TOTAL_BYTES);
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
    fd = -1;

    r2fsPerfPrepareSizedFile(
        R2FS_PERF_RANDOM_FILE,
        R2FS_PERF_RANDOM_TOTAL_BYTES,
        io_buffer,
        sizeof(io_buffer));
    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureOpen(R2FS_PERF_RANDOM_FILE, O_RDWR, 0, &fd));
    r2fsPerfRandomWriteIops(
        fd,
        io_buffer,
        R2FS_PERF_RANDOM_TOTAL_BYTES,
        R2FS_PERF_RANDOM_OPS);
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
    fd = -1;

    r2fsPerfPrepareSizedFile(
        R2FS_PERF_MIXED_FILE,
        R2FS_PERF_RANDOM_TOTAL_BYTES,
        io_buffer,
        sizeof(io_buffer));
    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureOpen(R2FS_PERF_MIXED_FILE, O_RDWR, 0, &fd));
    r2fsPerfMixedRwIops(
        fd,
        io_buffer,
        R2FS_PERF_RANDOM_TOTAL_BYTES,
        R2FS_PERF_MIXED_OPS);
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST_GROUP(
    "performance",
    PerformanceMetadata_CreateDeleteAndSmallFiles_ShouldReportMetrics)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    unsigned char small_file_buffer[R2FS_PERF_SMALL_FILE_BYTES];

    /*
     * 小文件 / 高元数据场景：
     * 1. Metadata Create/Delete: NR_INLINE_DENTRY 个空文件
     * 2. Small File Creation:    128 个 1 KiB 文件，逐文件 fdatasync
     */

    r2fsPerfFillPattern(
        small_file_buffer,
        sizeof(small_file_buffer),
        0x73u);

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_PERF_LPA_COUNT));

    r2fsPerfMetadataCreateDelete();
    r2fsPerfSmallFileCreation(
        small_file_buffer,
        sizeof(small_file_buffer));

    r2fsRtemsMountFixtureDestroy(&fixture);
}
