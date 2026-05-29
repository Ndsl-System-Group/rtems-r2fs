#include "integration/r2fs_rtems_mount_fixture.h"
#include "integration/r2fs_integration_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"
#include "fs/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define R2FS_ITEST_ERR_FILE "/err-file"
#define R2FS_ITEST_ERR_DIR "/err-dir"
#define R2FS_ITEST_ERR_SYMLINK "/err-link"
#define R2FS_ITEST_ERR_IO_DIR "/err-io"
#define R2FS_ITEST_ERR_IO_FILE "/err-io/data.bin"
RTFS_TEST(IntegrationError_InvalidOperations_ShouldSurfaceVfsSemantics)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    int fd = -1;
    char buffer[8];

    /*
     * 这条用例验证非法 VFS/POSIX 操作的对外错误语义：
     * 读写方向不匹配、把目录当文件打开，以及未支持能力调用时，
     * 都应返回与当前入口层一致的错误码。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(
            &fixture,
            R2FS_RTEMS_ITEST_LPA_COUNT
        )
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(R2FS_ITEST_ERR_FILE, 0644));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_ERR_DIR, 0755));

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureOpen(R2FS_ITEST_ERR_FILE, O_RDONLY, 0, &fd)
    );
    TEST_ASSERT_EQUAL_INT(-1, (int)r2fsRtemsMountFixtureWrite(fd, "x", 1u));
    TEST_ASSERT_EQUAL(EBADF, errno);
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
    fd = -1;

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureOpen(R2FS_ITEST_ERR_FILE, O_WRONLY, 0, &fd)
    );
    TEST_ASSERT_EQUAL_INT(-1, (int)r2fsRtemsMountFixtureRead(fd, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(EBADF, errno);
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureClose(fd));
    fd = -1;

    TEST_ASSERT_EQUAL(EISDIR, r2fsRtemsMountFixtureOpen(R2FS_ITEST_ERR_DIR, O_WRONLY, 0, &fd));
    TEST_ASSERT_EQUAL(EINVAL, r2fsRtemsMountFixtureOpen(R2FS_ITEST_ERR_DIR, O_RDONLY | O_TRUNC, 0, &fd));

    TEST_ASSERT_EQUAL(
        ENOTSUP,
        r2fsRtemsMountFixtureSymlink("/target", R2FS_ITEST_ERR_SYMLINK)
    );
    TEST_ASSERT_EQUAL(
        ENOTSUP,
        r2fsRtemsMountFixtureChown(R2FS_ITEST_ERR_FILE, 1u, 1u)
    );

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationError_DeviceIoFailure_ShouldReturnEioAndKeepCommittedStateStable)
{
    R2fsIntegrationFixture fixture;
    uint32_t ino = 0;
    uint32_t inode_lpa = INVALID_LPA;
    uint32_t data_lpa = INVALID_LPA;
    uint32_t after_inode_lpa = INVALID_LPA;
    uint32_t after_data_lpa = INVALID_LPA;
    char initial[BLOCK_BUFFER_SIZE];
    char overwrite[BLOCK_BUFFER_SIZE];
    char actual[BLOCK_BUFFER_SIZE];

    /*
     * 这条用例验证设备 I/O 故障下的错误传播与状态稳定性：
     * 读失败和写失败都应返回 EIO；故障清除后，已提交映射和数据
     * 不应被破坏或悄悄替换。
     */

    memset(initial, 'A', sizeof(initial));
    memset(overwrite, 'B', sizeof(overwrite));
    memset(actual, 0, sizeof(actual));

    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationFixtureFormatAndMount(&fixture, R2FS_ITEST_DISK_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsIntegrationMkdir(&fixture, R2FS_ITEST_ERR_IO_DIR, 0755));
    TEST_ASSERT_EQUAL(0, r2fsIntegrationCreateFile(&fixture, R2FS_ITEST_ERR_IO_FILE, 0644));
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationWriteFile(
            &fixture,
            R2FS_ITEST_ERR_IO_FILE,
            initial,
            sizeof(initial)
        )
    );
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationReadCurrentFileMapping(
            &fixture,
            R2FS_ITEST_ERR_IO_FILE,
            &ino,
            &inode_lpa,
            &data_lpa
        )
    );
    TEST_ASSERT_NOT_EQUAL_UINT32(INVALID_LPA, inode_lpa);
    TEST_ASSERT_NOT_EQUAL_UINT32(INVALID_LPA, data_lpa);

    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureSetFailReadLpa(&fixture, data_lpa));
    errno = 0;
    TEST_ASSERT_EQUAL_INT(
        -1,
        (int)r2fsIntegrationReadAt(
            &fixture,
            R2FS_ITEST_ERR_IO_FILE,
            0,
            actual,
            sizeof(actual)
        )
    );
    TEST_ASSERT_EQUAL(EIO, errno);
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureClearFaults(&fixture));

    memset(actual, 0, sizeof(actual));
    TEST_ASSERT_EQUAL_INT(
        (int)sizeof(actual),
        (int)r2fsIntegrationReadAt(
            &fixture,
            R2FS_ITEST_ERR_IO_FILE,
            0,
            actual,
            sizeof(actual)
        )
    );
    TEST_ASSERT_EQUAL_MEMORY(initial, actual, sizeof(initial));
    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationReadCurrentFileMapping(
            &fixture,
            R2FS_ITEST_ERR_IO_FILE,
            NULL,
            &inode_lpa,
            &data_lpa
        )
    );

    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureFailNextDataWrite(&fixture));
    errno = 0;
    TEST_ASSERT_EQUAL(
        EIO,
        r2fsIntegrationWriteAt(
            &fixture,
            R2FS_ITEST_ERR_IO_FILE,
            0,
            overwrite,
            sizeof(overwrite)
        )
    );
    TEST_ASSERT_EQUAL(0, r2fsIntegrationFixtureClearFaults(&fixture));

    TEST_ASSERT_EQUAL(
        0,
        r2fsIntegrationReadCurrentFileMapping(
            &fixture,
            R2FS_ITEST_ERR_IO_FILE,
            NULL,
            &after_inode_lpa,
            &after_data_lpa
        )
    );
    TEST_ASSERT_EQUAL_UINT32(inode_lpa, after_inode_lpa);
    TEST_ASSERT_EQUAL_UINT32(data_lpa, after_data_lpa);

    memset(actual, 0, sizeof(actual));
    TEST_ASSERT_EQUAL_INT(
        (int)sizeof(actual),
        (int)r2fsIntegrationReadAt(
            &fixture,
            R2FS_ITEST_ERR_IO_FILE,
            0,
            actual,
            sizeof(actual)
        )
    );
    TEST_ASSERT_EQUAL_MEMORY(initial, actual, sizeof(initial));

    r2fsIntegrationFixtureDestroy(&fixture);
}
