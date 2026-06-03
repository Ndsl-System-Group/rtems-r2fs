#include "integration/rtfs_rtems_mount_fixture.h"
#include "rtfs_test.h"

#include <string.h>
#include <sys/stat.h>

RTFS_TEST(IntegrationRtemsMount_FormatMountViaVfs_ShouldStatMountedRoot)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;
    struct statvfs stvfs;
    RtfsRtemsMountDirEntries dir_entries;
    bool saw_dot = false;
    bool saw_dotdot = false;
    size_t i;

    /*
     * 这条用例只验证最薄的外部 POSIX 语义：
     * format + mount 成功后，挂载点根目录必须能被 stat/statvfs/readdir
     * 看到，并表现为一个最小可访问目录。它不检查内部 manager、
     * layout 或任何盘面元数据。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatRoot(&st));
    TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, st.st_nlink);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatvfsRoot(&stvfs));
    TEST_ASSERT_EQUAL_UINT32(4096u, (uint32_t)stvfs.f_bsize);
    TEST_ASSERT_EQUAL_UINT32(RTFS_NAME_LEN, (uint32_t)stvfs.f_namemax);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureReadDir("/", &dir_entries));
    for (i = 0; i < dir_entries.count; ++i)
    {
        if (strcmp(dir_entries.entries[i].d_name, ".") == 0)
        {
            saw_dot = true;
        }
        else if (strcmp(dir_entries.entries[i].d_name, "..") == 0)
        {
            saw_dotdot = true;
        }
    }

    TEST_ASSERT_TRUE(saw_dot);
    TEST_ASSERT_TRUE(saw_dotdot);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(2u, dir_entries.count);

    rtfsRtemsMountFixtureDestroy(&fixture);
}
