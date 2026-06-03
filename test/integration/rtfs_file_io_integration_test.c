#include "integration/rtfs_rtems_mount_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"
#include "fs/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RTFS_ITEST_FILE_IO_PATH "/small.bin"
#define RTFS_ITEST_FILE_IO_APPEND_PATH "/append.bin"
#define RTFS_ITEST_FILE_IO_SPARSE_PATH "/sparse.bin"
#define RTFS_ITEST_FILE_IO_OVERWRITE_PATH "/overwrite.bin"
#define RTFS_ITEST_FILE_IO_TRUNCATE_PATH "/truncate.bin"

static void rtfsFillPattern(
    char *buffer,
    size_t size,
    char seed)
{
    size_t i;

    TEST_ASSERT_NOT_NULL(buffer);
    for (i = 0; i < size; ++i)
    {
        buffer[i] = (char)(seed + (char)(i % 23u));
    }
}

RTFS_TEST(IntegrationFileIo_CreateWriteCloseReadCompare_ShouldSucceedViaVfs)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;
    int fd = -1;
    ssize_t io_size;
    char actual[32];
    const char expected[] = "rtfs-small-write-pattern";

    /*
     * 这条用例验证最小文件 I/O 闭环的对外语义：
     * 创建、写入、fsync、关闭、重新打开读取之后，读回内容和
     * 文件大小都应与写入结果一致。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_FILE_IO_PATH, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_ITEST_FILE_IO_PATH, O_WRONLY, 0, &fd));
    io_size = rtfsRtemsMountFixtureWrite(fd, expected, sizeof(expected));
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), (int)io_size);
    TEST_ASSERT_EQUAL(0, fsync(fd));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    memset(actual, 0, sizeof(actual));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_ITEST_FILE_IO_PATH, O_RDONLY, 0, &fd));
    io_size = rtfsRtemsMountFixtureRead(fd, actual, sizeof(expected));
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), (int)io_size);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(expected));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_FILE_IO_PATH, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(expected), (uint32_t)st.st_size);

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationFileIo_SparseWriteAndZeroRead_ShouldExposeHoleAsZeroes)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;
    int fd = -1;
    ssize_t io_size;
    char actual[3 * BLOCK_BUFFER_SIZE + 4];
    const char tail[] = "tail";
    const size_t tail_size = strlen(tail);
    size_t i;

    /*
     * 这条用例验证稀疏写入的对外语义：
     * 跳过中间偏移后写入尾部数据，读回时 hole 区域应全部读零，
     * 尾部数据和最终文件大小也应与写入结果一致。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_FILE_IO_SPARSE_PATH, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_ITEST_FILE_IO_SPARSE_PATH, O_RDWR, 0, &fd));
    TEST_ASSERT_EQUAL(
        (off_t)(3 * BLOCK_BUFFER_SIZE),
        rtfsRtemsMountFixtureLseek(fd, 3 * BLOCK_BUFFER_SIZE, SEEK_SET));
    io_size = rtfsRtemsMountFixtureWrite(fd, tail, tail_size);
    TEST_ASSERT_EQUAL_INT((int)tail_size, (int)io_size);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureLseek(fd, 0, SEEK_SET));

    memset(actual, 0x7f, sizeof(actual));
    io_size = rtfsRtemsMountFixtureRead(fd, actual, sizeof(actual));
    TEST_ASSERT_EQUAL_INT((int)sizeof(actual), (int)io_size);
    for (i = 0; i < 3u * BLOCK_BUFFER_SIZE; ++i)
    {
        TEST_ASSERT_EQUAL_HEX8(0, (unsigned char)actual[i]);
    }
    TEST_ASSERT_EQUAL_MEMORY(tail, actual + 3 * BLOCK_BUFFER_SIZE, tail_size);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_FILE_IO_SPARSE_PATH, &st));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(3 * BLOCK_BUFFER_SIZE + tail_size),
        (uint32_t)st.st_size);

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationFileIo_OverwritePartialBlock_ShouldPreserveUnwrittenBytes)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    int fd = -1;
    ssize_t io_size;
    char initial[BLOCK_BUFFER_SIZE];
    char patch[50];
    char actual[BLOCK_BUFFER_SIZE];

    /*
     * 这条用例验证块内局部覆盖的对外语义：
     * 只改写中间一段数据时，覆盖区前后的未改写字节必须保持原样。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    rtfsFillPattern(initial, sizeof(initial), 'A');
    rtfsFillPattern(patch, sizeof(patch), 'k');

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_FILE_IO_OVERWRITE_PATH, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_ITEST_FILE_IO_OVERWRITE_PATH, O_RDWR, 0, &fd));
    io_size = rtfsRtemsMountFixtureWrite(fd, initial, sizeof(initial));
    TEST_ASSERT_EQUAL_INT((int)sizeof(initial), (int)io_size);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_ITEST_FILE_IO_OVERWRITE_PATH, O_RDWR, 0, &fd));
    TEST_ASSERT_EQUAL(100, rtfsRtemsMountFixtureLseek(fd, 100, SEEK_SET));
    io_size = rtfsRtemsMountFixtureWrite(fd, patch, sizeof(patch));
    TEST_ASSERT_EQUAL_INT((int)sizeof(patch), (int)io_size);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureLseek(fd, 0, SEEK_SET));

    memset(actual, 0, sizeof(actual));
    io_size = rtfsRtemsMountFixtureRead(fd, actual, sizeof(actual));
    TEST_ASSERT_EQUAL_INT((int)sizeof(actual), (int)io_size);
    TEST_ASSERT_EQUAL_MEMORY(initial, actual, 100);
    TEST_ASSERT_EQUAL_MEMORY(patch, actual + 100, sizeof(patch));
    TEST_ASSERT_EQUAL_MEMORY(initial + 150, actual + 150, sizeof(actual) - 150u);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationFileIo_AppendSeekSemantics_ShouldHonorAppendAndRejectInvalidSeek)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    int fd = -1;
    ssize_t io_size;
    char actual[8];
    const char prefix[] = "abc";
    const char suffix[] = "def";
    const size_t prefix_size = strlen(prefix);
    const size_t suffix_size = strlen(suffix);

    /*
     * 这条用例验证追加写与偏移控制的对外语义：
     * O_APPEND 写入必须追加到文件尾，非法 lseek 参数则应返回
     * 失败并设置 EINVAL。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_FILE_IO_APPEND_PATH, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_ITEST_FILE_IO_APPEND_PATH, O_RDWR, 0, &fd));
    TEST_ASSERT_EQUAL_INT(
        (int)prefix_size,
        (int)rtfsRtemsMountFixtureWrite(fd, prefix, prefix_size));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(
            RTFS_ITEST_FILE_IO_APPEND_PATH,
            O_WRONLY | O_APPEND,
            0,
            &fd));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureLseek(fd, 0, SEEK_SET));
    TEST_ASSERT_EQUAL_INT(
        (int)suffix_size,
        (int)rtfsRtemsMountFixtureWrite(fd, suffix, suffix_size));
    TEST_ASSERT_EQUAL(
        (off_t)(prefix_size + suffix_size),
        rtfsRtemsMountFixtureLseek(fd, 0, SEEK_END));
    TEST_ASSERT_EQUAL((off_t)-1, rtfsRtemsMountFixtureLseek(fd, -1, SEEK_SET));
    TEST_ASSERT_EQUAL(EINVAL, errno);
    TEST_ASSERT_EQUAL((off_t)-1, rtfsRtemsMountFixtureLseek(fd, 0, 999));
    TEST_ASSERT_EQUAL(EINVAL, errno);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    memset(actual, 0, sizeof(actual));
    io_size = rtfsRtemsMountFixtureReadFile(
        RTFS_ITEST_FILE_IO_APPEND_PATH,
        actual,
        prefix_size + suffix_size);
    TEST_ASSERT_EQUAL_INT((int)(prefix_size + suffix_size), (int)io_size);
    TEST_ASSERT_EQUAL_MEMORY("abcdef", actual, 6u);
    TEST_ASSERT_EQUAL_HEX8(0, (unsigned char)actual[6]);

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationFileIo_TruncateShrinkAndExtend_ShouldAdjustSizeAndZeroFillGrowth)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;
    int fd = -1;
    char pattern[3 * BLOCK_BUFFER_SIZE + BLOCK_BUFFER_SIZE / 2];
    char actual_tail[256];
    char actual_extend[128];
    const off_t shrink_size = (off_t)(2 * BLOCK_BUFFER_SIZE + 100);
    const off_t extend_size = (off_t)(5 * BLOCK_BUFFER_SIZE);

    /*
     * 这条用例验证 truncate 的对外语义：
     * 缩小文件后尾部数据应保留到新边界；再次扩展文件后，
     * 新增长区域应读零，且文件大小应与目标长度一致。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    rtfsFillPattern(pattern, sizeof(pattern), 'Q');
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_FILE_IO_TRUNCATE_PATH, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_ITEST_FILE_IO_TRUNCATE_PATH, O_RDWR, 0, &fd));
    TEST_ASSERT_EQUAL_INT(
        (int)sizeof(pattern),
        (int)rtfsRtemsMountFixtureWrite(fd, pattern, sizeof(pattern)));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFtruncate(fd, shrink_size));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_FILE_IO_TRUNCATE_PATH, &st));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)shrink_size, (uint32_t)st.st_size);
    TEST_ASSERT_EQUAL(shrink_size, rtfsRtemsMountFixtureLseek(fd, 0, SEEK_END));
    TEST_ASSERT_EQUAL(shrink_size - 100, rtfsRtemsMountFixtureLseek(fd, -100, SEEK_END));
    memset(actual_tail, 0, sizeof(actual_tail));
    TEST_ASSERT_EQUAL_INT(
        100,
        (int)rtfsRtemsMountFixtureRead(fd, actual_tail, sizeof(actual_tail)));
    TEST_ASSERT_EQUAL_MEMORY(pattern + shrink_size - 100, actual_tail, 100u);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFtruncate(fd, extend_size));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_FILE_IO_TRUNCATE_PATH, &st));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)extend_size, (uint32_t)st.st_size);
    TEST_ASSERT_EQUAL(extend_size - (off_t)sizeof(actual_extend), rtfsRtemsMountFixtureLseek(
                                                                      fd,
                                                                      -(off_t)sizeof(actual_extend),
                                                                      SEEK_END));
    memset(actual_extend, 0x7f, sizeof(actual_extend));
    TEST_ASSERT_EQUAL_INT(
        (int)sizeof(actual_extend),
        (int)rtfsRtemsMountFixtureRead(fd, actual_extend, sizeof(actual_extend)));
    TEST_ASSERT_EACH_EQUAL_HEX8(0, actual_extend, sizeof(actual_extend));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationFileIo_WriteUnmountRemountRead_ShouldPersistContent)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    ssize_t io_size;
    char actual[32];
    const char expected[] = "rtfs-small-write-pattern";

    /*
     * 这条用例验证 clean unmount + remount 后的小文件持久化语义：
     * 写入成功的内容在重新挂载之后仍应按原样读回。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_FILE_IO_PATH, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureWriteFile(
            RTFS_ITEST_FILE_IO_PATH,
            expected,
            sizeof(expected)));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRemount(&fixture));

    memset(actual, 0, sizeof(actual));
    io_size = rtfsRtemsMountFixtureReadFile(
        RTFS_ITEST_FILE_IO_PATH,
        actual,
        sizeof(expected));
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), (int)io_size);
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(expected));

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationFileIo_FdatasyncRemount_ShouldKeepCommittedContentVisible)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;
    int fd = -1;
    ssize_t io_size;
    char actual[48];
    const char expected[] = "fdatasync-visible-after-remount";

    /*
     * 这条用例验证显式同步后的提交可见性：
     * fdatasync 成功返回后，重挂载之后文件内容和大小都应保持可见。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_FILE_IO_PATH, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_ITEST_FILE_IO_PATH, O_RDWR, 0, &fd));
    io_size = rtfsRtemsMountFixtureWrite(fd, expected, sizeof(expected));
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), (int)io_size);
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRemount(&fixture));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_FILE_IO_PATH, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(expected), (uint32_t)st.st_size);

    memset(actual, 0, sizeof(actual));
    io_size = rtfsRtemsMountFixtureReadFile(
        RTFS_ITEST_FILE_IO_PATH,
        actual,
        sizeof(expected));
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), (int)io_size);
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(expected));

    rtfsRtemsMountFixtureDestroy(&fixture);
}
