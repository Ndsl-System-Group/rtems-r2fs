#include "integration/rtfs_rtems_mount_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"
#include "fs/fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define RTFS_ITEST_PATH_A "/a"
#define RTFS_ITEST_PATH_B "/a/b"
#define RTFS_ITEST_PATH_FILE "/a/b/f.txt"
#define RTFS_ITEST_PATH_DIR "/dir"
#define RTFS_ITEST_PATH_OLD "/dir/old"
#define RTFS_ITEST_PATH_NEW "/dir/new"
#define RTFS_ITEST_PATH_REMOVE_DIR "/d"
#define RTFS_ITEST_PATH_REMOVE_FILE "/d/f"
#define RTFS_ITEST_PATH_MANY "/many"
#define RTFS_ITEST_PATH_SLOTS "/slots"
#define RTFS_ITEST_PATH_P1 "/p1"
#define RTFS_ITEST_PATH_P2 "/p2"
#define RTFS_ITEST_PATH_P1_FILE "/p1/file"
#define RTFS_ITEST_PATH_P2_FILE "/p2/file"
#define RTFS_ITEST_PATH_P1_DIR "/p1/dir"
#define RTFS_ITEST_PATH_P2_DIR "/p2/dir"
#define RTFS_ITEST_PATH_TYPE_FILE "/type-file"
#define RTFS_ITEST_PATH_DEEP_FILE "/deep/a/b/c/d/e/f.txt"

static void rtfsRtemsMountJoinPath(
    char *buffer,
    size_t buffer_size,
    const char *parent,
    const char *name)
{
    int written;

    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_NOT_NULL(parent);
    TEST_ASSERT_NOT_NULL(name);

    written = snprintf(buffer, buffer_size, "%s/%s", parent, name);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t)written < buffer_size);
}

static void rtfsRtemsMountBuildName(
    char *buffer,
    size_t name_len,
    char fill)
{
    size_t i;

    TEST_ASSERT_NOT_NULL(buffer);
    for (i = 0; i < name_len; ++i)
    {
        buffer[i] = fill;
    }
    buffer[name_len] = '\0';
}

static void rtfsRtemsMountAssertDirHasEntry(
    const RtfsRtemsMountDirEntries *dir_entries,
    const char *expected_name)
{
    bool found = false;
    size_t i;

    TEST_ASSERT_NOT_NULL(dir_entries);
    TEST_ASSERT_NOT_NULL(expected_name);

    for (i = 0; i < dir_entries->count; ++i)
    {
        if (strcmp(dir_entries->entries[i].d_name, expected_name) == 0)
        {
            found = true;
            break;
        }
    }

    TEST_ASSERT_TRUE(found);
}

static void rtfsRtemsMountAssertDirContains(
    const char *path,
    const char *expected_name,
    size_t expected_entries)
{
    RtfsRtemsMountDirEntries dir_entries;
    bool found = false;
    size_t i;

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureReadDir(path, &dir_entries));
    for (i = 0; i < dir_entries.count; ++i)
    {
        if (strcmp(dir_entries.entries[i].d_name, expected_name) == 0)
        {
            found = true;
        }
    }

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_size_t(expected_entries, dir_entries.count);
}

RTFS_TEST(IntegrationNamespace_MkdirReadDirRemount_ShouldExposeSmallTree)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct statvfs before_stvfs;
    struct statvfs after_stvfs;
    struct stat st_a;
    struct stat st_b;
    struct stat st_file;

    /*
     * 这条用例验证一棵小型命名空间树的基础对外语义：
     * 创建两级目录和一个普通文件后，路径查找、目录枚举和
     * clean remount 后的可见性都应保持正确。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatvfsRoot(&before_stvfs));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_A, 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_B, 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_PATH_FILE, 0644));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_A, &st_a));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_B, &st_b));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_FILE, &st_file));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatvfsRoot(&after_stvfs));

    TEST_ASSERT_TRUE(S_ISDIR(st_a.st_mode));
    TEST_ASSERT_TRUE(S_ISDIR(st_b.st_mode));
    TEST_ASSERT_TRUE(S_ISREG(st_file.st_mode));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(3, st_a.st_nlink);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, st_b.st_nlink);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(before_stvfs.f_ffree - 3u),
        (uint32_t)after_stvfs.f_ffree);
    rtfsRtemsMountAssertDirContains(RTFS_ITEST_PATH_B, "f.txt", 3u);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRemount(&fixture));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_FILE, &st_file));
    TEST_ASSERT_TRUE(S_ISREG(st_file.st_mode));
    rtfsRtemsMountAssertDirContains(RTFS_ITEST_PATH_B, "f.txt", 3u);

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_ReaddirManyEntries_ShouldEnumerateCompleteDirectoryAndGrowSize)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    RtfsRtemsMountDirEntries dir_entries;
    struct stat st_many;
    size_t i;
    char entry_name[16];
    char entry_path[RTFS_RTEMS_ITEST_PATH_MAX];
    const size_t entry_count = NR_INLINE_DENTRY + 1u;

    /*
     * 这条用例验证目录项数量越过 inline dentry 上限后的对外语义：
     * 目录枚举必须保持完整，且目录尺寸应体现出已经扩展到
     * 常规目录数据块的状态。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_MANY, 0755));
    for (i = 0; i < entry_count; ++i)
    {
        snprintf(entry_name, sizeof(entry_name), "f%03u", (unsigned)i);
        rtfsRtemsMountJoinPath(
            entry_path,
            sizeof(entry_path),
            RTFS_ITEST_PATH_MANY,
            entry_name);
        TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(entry_path, 0644));
    }

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureReadDir(RTFS_ITEST_PATH_MANY, &dir_entries));
    TEST_ASSERT_EQUAL_size_t(entry_count + 2u, dir_entries.count);
    for (i = 0; i < entry_count; ++i)
    {
        snprintf(entry_name, sizeof(entry_name), "f%03u", (unsigned)i);
        rtfsRtemsMountAssertDirHasEntry(&dir_entries, entry_name);
    }

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_MANY, &st_many));
    TEST_ASSERT_TRUE(S_ISDIR(st_many.st_mode));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(BLOCK_BUFFER_SIZE, (uint32_t)st_many.st_size);

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_NameLengthBoundaries_ShouldAcceptValidNamesAndRejectTooLong)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    RtfsRtemsMountDirEntries dir_entries;
    struct stat st;
    char name_255[RTFS_NAME_LEN + 1];
    char too_long_name[RTFS_NAME_LEN + 2];
    char path_8[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_9[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_255[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_too_long[RTFS_RTEMS_ITEST_PATH_MAX];
    const char *name_8 = "12345678";
    const char *name_9 = "123456789";

    /*
     * 这条用例验证目录项名字长度边界的对外语义：
     * 8/9/255 字节名字应可创建并可枚举，超过 RTFS_NAME_LEN 的
     * 名字应返回 ENAMETOOLONG。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    rtfsRtemsMountBuildName(name_255, RTFS_NAME_LEN, 'x');
    rtfsRtemsMountBuildName(too_long_name, RTFS_NAME_LEN + 1u, 'z');

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_SLOTS, 0755));

    rtfsRtemsMountJoinPath(path_8, sizeof(path_8), RTFS_ITEST_PATH_SLOTS, name_8);
    rtfsRtemsMountJoinPath(path_9, sizeof(path_9), RTFS_ITEST_PATH_SLOTS, name_9);
    rtfsRtemsMountJoinPath(path_255, sizeof(path_255), RTFS_ITEST_PATH_SLOTS, name_255);
    rtfsRtemsMountJoinPath(
        path_too_long,
        sizeof(path_too_long),
        RTFS_ITEST_PATH_SLOTS,
        too_long_name);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path_8, 0644));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path_9, 0644));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path_255, 0644));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, rtfsRtemsMountFixtureCreateFile(path_too_long, 0644));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(path_8, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(path_9, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(path_255, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureReadDir(RTFS_ITEST_PATH_SLOTS, &dir_entries));
    TEST_ASSERT_EQUAL_size_t(5u, dir_entries.count);
    rtfsRtemsMountAssertDirHasEntry(&dir_entries, name_8);
    rtfsRtemsMountAssertDirHasEntry(&dir_entries, name_9);
    rtfsRtemsMountAssertDirHasEntry(&dir_entries, name_255);

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RenameLongNames_ShouldReplaceOldPathWithNewPath)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;
    char name_255[RTFS_NAME_LEN + 1];
    char rename_255[RTFS_NAME_LEN + 1];
    char path_9[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_255[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_rename_9[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_rename_255[RTFS_RTEMS_ITEST_PATH_MAX];
    const char *name_9 = "123456789";
    const char *rename_9 = "abcdefghi";

    /*
     * 这条用例验证长名字目录项的 rename 语义：
     * rename 之后旧路径必须消失，新路径必须可见。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    rtfsRtemsMountBuildName(name_255, RTFS_NAME_LEN, 'x');
    rtfsRtemsMountBuildName(rename_255, RTFS_NAME_LEN, 'y');

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_SLOTS, 0755));

    rtfsRtemsMountJoinPath(path_9, sizeof(path_9), RTFS_ITEST_PATH_SLOTS, name_9);
    rtfsRtemsMountJoinPath(path_255, sizeof(path_255), RTFS_ITEST_PATH_SLOTS, name_255);
    rtfsRtemsMountJoinPath(path_rename_9, sizeof(path_rename_9), RTFS_ITEST_PATH_SLOTS, rename_9);
    rtfsRtemsMountJoinPath(
        path_rename_255,
        sizeof(path_rename_255),
        RTFS_ITEST_PATH_SLOTS,
        rename_255);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path_9, 0644));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path_255, 0644));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRename(path_9, path_rename_9));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(path_9, &st));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(path_rename_9, &st));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRename(path_255, path_rename_255));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(path_255, &st));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(path_rename_255, &st));

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_UnlinkAfterRename_ShouldRemoveEntriesFromDirectory)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    RtfsRtemsMountDirEntries dir_entries;
    struct stat st;
    char name_255[RTFS_NAME_LEN + 1];
    char rename_255[RTFS_NAME_LEN + 1];
    char path_8[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_9[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_255[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_rename_9[RTFS_RTEMS_ITEST_PATH_MAX];
    char path_rename_255[RTFS_RTEMS_ITEST_PATH_MAX];
    const char *name_8 = "12345678";
    const char *name_9 = "123456789";
    const char *rename_9 = "abcdefghi";

    /*
     * 这条用例验证 rename 后目录项的 unlink 语义：
     * 删除后路径必须不可见，父目录枚举结果也应回收至空目录状态。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    rtfsRtemsMountBuildName(name_255, RTFS_NAME_LEN, 'x');
    rtfsRtemsMountBuildName(rename_255, RTFS_NAME_LEN, 'y');

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_SLOTS, 0755));

    rtfsRtemsMountJoinPath(path_8, sizeof(path_8), RTFS_ITEST_PATH_SLOTS, name_8);
    rtfsRtemsMountJoinPath(path_9, sizeof(path_9), RTFS_ITEST_PATH_SLOTS, name_9);
    rtfsRtemsMountJoinPath(path_255, sizeof(path_255), RTFS_ITEST_PATH_SLOTS, name_255);
    rtfsRtemsMountJoinPath(path_rename_9, sizeof(path_rename_9), RTFS_ITEST_PATH_SLOTS, rename_9);
    rtfsRtemsMountJoinPath(
        path_rename_255,
        sizeof(path_rename_255),
        RTFS_ITEST_PATH_SLOTS,
        rename_255);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path_8, 0644));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path_9, 0644));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(path_255, 0644));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRename(path_9, path_rename_9));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRename(path_255, path_rename_255));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureUnlink(path_8));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureUnlink(path_rename_9));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureUnlink(path_rename_255));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(path_8, &st));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(path_rename_9, &st));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(path_rename_255, &st));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureReadDir(RTFS_ITEST_PATH_SLOTS, &dir_entries));
    TEST_ASSERT_EQUAL_size_t(2u, dir_entries.count);

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RenameSameParentRemount_ShouldPreserveInode)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat before_st;
    struct stat after_st;

    /*
     * 这条用例验证同目录 rename 的对象身份语义：
     * rename 只应改变目录项绑定关系，不应更换文件 inode，
     * 且 clean remount 后这一结果仍应保持成立。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_DIR, 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_PATH_OLD, 0644));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_OLD, &before_st));

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureRename(RTFS_ITEST_PATH_OLD, RTFS_ITEST_PATH_NEW));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_OLD, &after_st));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_NEW, &after_st));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_st.st_ino, (uint32_t)after_st.st_ino);

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRemount(&fixture));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_NEW, &after_st));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_st.st_ino, (uint32_t)after_st.st_ino);

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RenameFileAcrossParents_ShouldMovePathAndPreserveFile)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st_moved;

    /*
     * 这条用例验证普通文件跨父目录 rename 的对外语义：
     * 旧路径必须消失，新路径必须可见，且对象类型仍应保持为普通文件。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_P1, 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_P2, 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_PATH_P1_FILE, 0644));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRename(RTFS_ITEST_PATH_P1_FILE, RTFS_ITEST_PATH_P2_FILE));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_P1_FILE, &st_moved));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_P2_FILE, &st_moved));
    TEST_ASSERT_TRUE(S_ISREG(st_moved.st_mode));

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RenameDirectoryAcrossParents_ShouldMovePathAndUpdateParentNlink)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st_p1_before;
    struct stat st_p2_before;
    struct stat st_p1_after;
    struct stat st_p2_after;
    struct stat st_moved;

    /*
     * 这条用例验证目录跨父目录 rename 的对外语义：
     * 旧路径必须消失，新路径必须可见，且父目录的 nlink 计数
     * 应反映子目录被移出和移入后的变化。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_P1, 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_P2, 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_P1_DIR, 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_P1, &st_p1_before));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_P2, &st_p2_before));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRename(RTFS_ITEST_PATH_P1_DIR, RTFS_ITEST_PATH_P2_DIR));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_P1_DIR, &st_moved));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_P2_DIR, &st_moved));
    TEST_ASSERT_TRUE(S_ISDIR(st_moved.st_mode));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_P1, &st_p1_after));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_P2, &st_p2_after));
    TEST_ASSERT_EQUAL_INT((int)st_p1_before.st_nlink - 1, (int)st_p1_after.st_nlink);
    TEST_ASSERT_EQUAL_INT((int)st_p2_before.st_nlink + 1, (int)st_p2_after.st_nlink);

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RemoveNonEmptyDirThenCleanup_ShouldHonorVfsRules)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;

    /*
     * 这条用例验证目录删除规则的对外语义：
     * 非空目录必须返回 ENOTEMPTY；清空后目录应可删除，且 remount
     * 之后该目录仍应保持不可见。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir(RTFS_ITEST_PATH_REMOVE_DIR, 0755));
    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_PATH_REMOVE_FILE, 0644));

    TEST_ASSERT_EQUAL(ENOTEMPTY, rtfsRtemsMountFixtureRmdir(RTFS_ITEST_PATH_REMOVE_DIR));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureUnlink(RTFS_ITEST_PATH_REMOVE_FILE));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_REMOVE_FILE, &st));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRmdir(RTFS_ITEST_PATH_REMOVE_DIR));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureRemount(&fixture));
    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath(RTFS_ITEST_PATH_REMOVE_DIR, &st));

    rtfsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_PathResolutionEdges_ShouldValidateMissingAndIntermediateTypes)
{
    RtfsRtemsMountFixture fixture = RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;

    /*
     * 这条用例验证路径解析边界的对外语义：
     * 缺失路径应返回 ENOENT，中间分量若不是目录应返回 ENOTDIR，
     * 含 '.' 的深层路径也应被正确解析。
     */

    TEST_ASSERT_EQUAL(
        0,
        rtfsRtemsMountFixtureFormatAndMount(&fixture, RTFS_RTEMS_ITEST_LPA_COUNT));

    TEST_ASSERT_EQUAL(ENOENT, rtfsRtemsMountFixtureStatPath("/no/such/path", &st));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_PATH_TYPE_FILE, 0644));
    TEST_ASSERT_EQUAL(ENOTDIR, rtfsRtemsMountFixtureStatPath("/type-file/child", &st));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir("/deep", 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir("/deep/a", 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir("/deep/a/b", 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir("/deep/a/b/c", 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir("/deep/a/b/c/d", 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureMkdir("/deep/a/b/c/d/e", 0755));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureCreateFile(RTFS_ITEST_PATH_DEEP_FILE, 0644));

    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath("/deep/a/./b/./c/d/./e", &st));
    TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
    TEST_ASSERT_EQUAL(0, rtfsRtemsMountFixtureStatPath("/deep/a/./b/c/./d/e/f.txt", &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));

    rtfsRtemsMountFixtureDestroy(&fixture);
}
