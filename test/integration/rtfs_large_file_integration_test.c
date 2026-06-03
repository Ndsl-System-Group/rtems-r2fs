#include "integration/rtfs_rtems_mount_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"
#include "fs/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#define RTFS_ITEST_LARGE_FILE_PATH "/large-boundary.bin"

typedef struct RtfsLargeFileBoundaryCase
{
    uint32_t block_index;
    char fill;
} RtfsLargeFileBoundaryCase;

static void rtfsLargeFileFill(
    char *buffer,
    size_t size,
    char fill)
{
    size_t i;

    TEST_ASSERT_NOT_NULL(buffer);
    for (i = 0; i < size; ++i)
    {
        buffer[i] = (char)(fill + (char)(i % 19u));
    }
}

RTFS_TEST(IntegrationFileIo_DirectIndirectDoubleIndirectBoundaries_ShouldRoundTripSparseWrites)
{
    static const RtfsLargeFileBoundaryCase cases[] = {
        {DEF_ADDRS_PER_INODE - 1u, 'D'},
        {DEF_ADDRS_PER_INODE, 'E'},
        {DEF_ADDRS_PER_INODE + 2u * DEF_ADDRS_PER_BLOCK - 1u, 'F'},
        {DEF_ADDRS_PER_INODE + 2u * DEF_ADDRS_PER_BLOCK, 'G'},
        {DEF_ADDRS_PER_INODE +
             2u * DEF_ADDRS_PER_BLOCK +
             2u * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK - 1u,
         'H'},
        {DEF_ADDRS_PER_INODE +
             2u * DEF_ADDRS_PER_BLOCK +
             2u * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK,
         'I'}};
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;
    int fd = -1;
    char expected[64];
    char actual[64];
    size_t i;
    off_t expected_size;

    /*
     * 这条用例验证大文件块映射边界上的对外语义：
     * 在 direct / indirect / double indirect 的关键分界块上执行
     * 稀疏写入后，数据应能立即读回，并在重新打开文件后继续保持一致。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(
            &fixture,
            RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_LARGE_FILE_PATH, 0644));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(
            RTFS_ITEST_LARGE_FILE_PATH,
            O_RDWR,
            0,
            &fd));

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        off_t offset;
        ssize_t io_size;

        rtfsLargeFileFill(expected, sizeof(expected), cases[i].fill);
        offset = (off_t)((uint64_t)cases[i].block_index * BLOCK_BUFFER_SIZE);

        TEST_ASSERT_EQUAL(
            offset,
            rtfsRtemsMountFixtureLseek(fd, offset, SEEK_SET));
        io_size = rtfsRtemsMountFixtureWrite(fd, expected, sizeof(expected));
        TEST_ASSERT_EQUAL_INT((int)sizeof(expected), (int)io_size);
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));

        TEST_ASSERT_EQUAL(
            offset,
            rtfsRtemsMountFixtureLseek(fd, offset, SEEK_SET));
        memset(actual, 0, sizeof(actual));
        io_size = rtfsRtemsMountFixtureRead(fd, actual, sizeof(actual));
        TEST_ASSERT_EQUAL_INT((int)sizeof(actual), (int)io_size);
        TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(expected));
    }

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureStatPath(RTFS_ITEST_LARGE_FILE_PATH, &st));
    expected_size =
        (off_t)((uint64_t)cases[(sizeof(cases) / sizeof(cases[0])) - 1u].block_index * BLOCK_BUFFER_SIZE) + (off_t)sizeof(expected);
    TEST_ASSERT_EQUAL(expected_size, st.st_size);

    memset(actual, 0, sizeof(actual));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(
            RTFS_ITEST_LARGE_FILE_PATH,
            O_RDONLY,
            0,
            &fd));

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        off_t offset;
        ssize_t io_size;

        rtfsLargeFileFill(expected, sizeof(expected), cases[i].fill);
        offset = (off_t)((uint64_t)cases[i].block_index * BLOCK_BUFFER_SIZE);

        TEST_ASSERT_EQUAL(
            offset,
            rtfsRtemsMountFixtureLseek(fd, offset, SEEK_SET));
        memset(actual, 0, sizeof(actual));
        io_size = rtfsRtemsMountFixtureRead(fd, actual, sizeof(actual));
        TEST_ASSERT_EQUAL_INT((int)sizeof(actual), (int)io_size);
        TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(expected));
    }

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    rtfsRtemsMountFixtureDestroy(&fixture);
}
