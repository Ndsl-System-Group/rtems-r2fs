#include "integration/rtfs_rtems_mount_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define RTFS_SEQ_WRITE_PROBE_PATH "/seq-write-probe.bin"
#define RTFS_SEQ_WRITE_PROBE_CHUNK_BYTES BLOCK_BUFFER_SIZE
#define RTFS_SEQ_WRITE_PROBE_CHUNK_COUNT 2U
#define RTFS_SEQ_WRITE_PROBE_TOTAL_BYTES \
    (RTFS_SEQ_WRITE_PROBE_CHUNK_BYTES * RTFS_SEQ_WRITE_PROBE_CHUNK_COUNT)

static void rtfsSequentialWriteProbeFill(
    unsigned char *buffer,
    size_t size,
    unsigned char seed)
{
    size_t i;

    TEST_ASSERT_NOT_NULL(buffer);
    for (i = 0; i < size; ++i)
    {
        buffer[i] = (unsigned char)(seed + (unsigned char)(i * 13u));
    }
}

RTFS_TEST_GROUP(
    "write-probe",
    SequentialWriteProbe_WriteTwoChunksAndFdatasync_ShouldSucceed)
{
    RTFS_TEST_ANNOUNCE("IT-WRITE-PROBE-01");
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    unsigned char buffer[RTFS_SEQ_WRITE_PROBE_CHUNK_BYTES];
    struct stat st;
    int fd = -1;
    uint32_t i;

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(
            &fixture,
            RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureCreateFile(RTFS_SEQ_WRITE_PROBE_PATH, 0644));

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureOpen(RTFS_SEQ_WRITE_PROBE_PATH, O_RDWR, 0, &fd));

    for (i = 0; i < RTFS_SEQ_WRITE_PROBE_CHUNK_COUNT; ++i)
    {
        ssize_t written;

        rtfsSequentialWriteProbeFill(buffer, sizeof(buffer), (unsigned char)i);
        written = rtfsRtemsMountFixtureWrite(fd, buffer, sizeof(buffer));
        TEST_ASSERT_EQUAL_INT((int)sizeof(buffer), (int)written);
    }

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureFdatasync(fd));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureClose(fd));
    fd = -1;

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureStatPath(RTFS_SEQ_WRITE_PROBE_PATH, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)RTFS_SEQ_WRITE_PROBE_TOTAL_BYTES,
        (uint32_t)st.st_size);

    rtfsRtemsMountFixtureDestroy(&fixture);
}
