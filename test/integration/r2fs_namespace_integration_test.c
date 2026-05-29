#include "integration/r2fs_rtems_mount_fixture.h"
#include "rtfs_test.h"

#include "cache/block_buffer.h"
#include "fs/fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define R2FS_ITEST_PATH_A "/a"
#define R2FS_ITEST_PATH_B "/a/b"
#define R2FS_ITEST_PATH_FILE "/a/b/f.txt"
#define R2FS_ITEST_PATH_DIR "/dir"
#define R2FS_ITEST_PATH_OLD "/dir/old"
#define R2FS_ITEST_PATH_NEW "/dir/new"
#define R2FS_ITEST_PATH_REMOVE_DIR "/d"
#define R2FS_ITEST_PATH_REMOVE_FILE "/d/f"
#define R2FS_ITEST_PATH_MANY "/many"
#define R2FS_ITEST_PATH_SLOTS "/slots"
#define R2FS_ITEST_PATH_P1 "/p1"
#define R2FS_ITEST_PATH_P2 "/p2"
#define R2FS_ITEST_PATH_P1_FILE "/p1/file"
#define R2FS_ITEST_PATH_P2_FILE "/p2/file"
#define R2FS_ITEST_PATH_P1_DIR "/p1/dir"
#define R2FS_ITEST_PATH_P2_DIR "/p2/dir"
#define R2FS_ITEST_PATH_TYPE_FILE "/type-file"
#define R2FS_ITEST_PATH_DEEP_FILE "/deep/a/b/c/d/e/f.txt"

static void r2fsRtemsMountJoinPath(
    char *buffer,
    size_t buffer_size,
    const char *parent,
    const char *name
)
{
    int written;

    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_NOT_NULL(parent);
    TEST_ASSERT_NOT_NULL(name);

    written = snprintf(buffer, buffer_size, "%s/%s", parent, name);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t)written < buffer_size);
}

static void r2fsRtemsMountBuildName(
    char *buffer,
    size_t name_len,
    char fill
)
{
    size_t i;

    TEST_ASSERT_NOT_NULL(buffer);
    for (i = 0; i < name_len; ++i) {
        buffer[i] = fill;
    }
    buffer[name_len] = '\0';
}

static void r2fsRtemsMountAssertDirHasEntry(
    const R2fsRtemsMountDirEntries *dir_entries,
    const char *expected_name
)
{
    bool found = false;
    size_t i;

    TEST_ASSERT_NOT_NULL(dir_entries);
    TEST_ASSERT_NOT_NULL(expected_name);

    for (i = 0; i < dir_entries->count; ++i) {
        if (strcmp(dir_entries->entries[i].d_name, expected_name) == 0) {
            found = true;
            break;
        }
    }

    TEST_ASSERT_TRUE(found);
}

static void r2fsRtemsMountAssertDirContains(
    const char *path,
    const char *expected_name,
    size_t expected_entries
)
{
    R2fsRtemsMountDirEntries dir_entries;
    bool found = false;
    size_t i;

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureReadDir(path, &dir_entries));
    for (i = 0; i < dir_entries.count; ++i) {
        if (strcmp(dir_entries.entries[i].d_name, expected_name) == 0) {
            found = true;
        }
    }

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_size_t(expected_entries, dir_entries.count);
}

RTFS_TEST(IntegrationNamespace_MkdirReadDirRemount_ShouldExposeSmallTree)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
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
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatvfsRoot(&before_stvfs));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_A, 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_B, 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(R2FS_ITEST_PATH_FILE, 0644));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_A, &st_a));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_B, &st_b));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_FILE, &st_file));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatvfsRoot(&after_stvfs));

    TEST_ASSERT_TRUE(S_ISDIR(st_a.st_mode));
    TEST_ASSERT_TRUE(S_ISDIR(st_b.st_mode));
    TEST_ASSERT_TRUE(S_ISREG(st_file.st_mode));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(3, st_a.st_nlink);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, st_b.st_nlink);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(before_stvfs.f_ffree - 3u),
        (uint32_t)after_stvfs.f_ffree
    );
    r2fsRtemsMountAssertDirContains(R2FS_ITEST_PATH_B, "f.txt", 3u);

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRemount(&fixture));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_FILE, &st_file));
    TEST_ASSERT_TRUE(S_ISREG(st_file.st_mode));
    r2fsRtemsMountAssertDirContains(R2FS_ITEST_PATH_B, "f.txt", 3u);

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_ReaddirManyEntries_ShouldEnumerateCompleteDirectoryAndGrowSize)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    R2fsRtemsMountDirEntries dir_entries;
    struct stat st_many;
    size_t i;
    char entry_name[16];
    char entry_path[R2FS_RTEMS_ITEST_PATH_MAX];
    const size_t entry_count = NR_INLINE_DENTRY + 1u;

    /*
     * 这条用例验证目录项数量越过 inline dentry 上限后的对外语义：
     * 目录枚举必须保持完整，且目录尺寸应体现出已经扩展到
     * 常规目录数据块的状态。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_MANY, 0755));
    for (i = 0; i < entry_count; ++i) {
        snprintf(entry_name, sizeof(entry_name), "f%03u", (unsigned)i);
        r2fsRtemsMountJoinPath(
            entry_path,
            sizeof(entry_path),
            R2FS_ITEST_PATH_MANY,
            entry_name
        );
        TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(entry_path, 0644));
    }

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureReadDir(R2FS_ITEST_PATH_MANY, &dir_entries));
    TEST_ASSERT_EQUAL_size_t(entry_count + 2u, dir_entries.count);
    for (i = 0; i < entry_count; ++i) {
        snprintf(entry_name, sizeof(entry_name), "f%03u", (unsigned)i);
        r2fsRtemsMountAssertDirHasEntry(&dir_entries, entry_name);
    }

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_MANY, &st_many));
    TEST_ASSERT_TRUE(S_ISDIR(st_many.st_mode));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(BLOCK_BUFFER_SIZE, (uint32_t)st_many.st_size);

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_NameLengthBoundaries_ShouldAcceptValidNamesAndRejectTooLong)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    R2fsRtemsMountDirEntries dir_entries;
    struct stat st;
    char name_255[RTFS_NAME_LEN + 1];
    char too_long_name[RTFS_NAME_LEN + 2];
    char path_8[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_9[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_255[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_too_long[R2FS_RTEMS_ITEST_PATH_MAX];
    const char *name_8 = "12345678";
    const char *name_9 = "123456789";

    /*
     * 这条用例验证目录项名字长度边界的对外语义：
     * 8/9/255 字节名字应可创建并可枚举，超过 RTFS_NAME_LEN 的
     * 名字应返回 ENAMETOOLONG。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    r2fsRtemsMountBuildName(name_255, RTFS_NAME_LEN, 'x');
    r2fsRtemsMountBuildName(too_long_name, RTFS_NAME_LEN + 1u, 'z');

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_SLOTS, 0755));

    r2fsRtemsMountJoinPath(path_8, sizeof(path_8), R2FS_ITEST_PATH_SLOTS, name_8);
    r2fsRtemsMountJoinPath(path_9, sizeof(path_9), R2FS_ITEST_PATH_SLOTS, name_9);
    r2fsRtemsMountJoinPath(path_255, sizeof(path_255), R2FS_ITEST_PATH_SLOTS, name_255);
    r2fsRtemsMountJoinPath(
        path_too_long,
        sizeof(path_too_long),
        R2FS_ITEST_PATH_SLOTS,
        too_long_name
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path_8, 0644));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path_9, 0644));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path_255, 0644));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, r2fsRtemsMountFixtureCreateFile(path_too_long, 0644));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(path_8, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(path_9, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(path_255, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureReadDir(R2FS_ITEST_PATH_SLOTS, &dir_entries));
    TEST_ASSERT_EQUAL_size_t(5u, dir_entries.count);
    r2fsRtemsMountAssertDirHasEntry(&dir_entries, name_8);
    r2fsRtemsMountAssertDirHasEntry(&dir_entries, name_9);
    r2fsRtemsMountAssertDirHasEntry(&dir_entries, name_255);

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RenameLongNames_ShouldReplaceOldPathWithNewPath)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;
    char name_255[RTFS_NAME_LEN + 1];
    char rename_255[RTFS_NAME_LEN + 1];
    char path_9[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_255[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_rename_9[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_rename_255[R2FS_RTEMS_ITEST_PATH_MAX];
    const char *name_9 = "123456789";
    const char *rename_9 = "abcdefghi";

    /*
     * 这条用例验证长名字目录项的 rename 语义：
     * rename 之后旧路径必须消失，新路径必须可见。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    r2fsRtemsMountBuildName(name_255, RTFS_NAME_LEN, 'x');
    r2fsRtemsMountBuildName(rename_255, RTFS_NAME_LEN, 'y');

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_SLOTS, 0755));

    r2fsRtemsMountJoinPath(path_9, sizeof(path_9), R2FS_ITEST_PATH_SLOTS, name_9);
    r2fsRtemsMountJoinPath(path_255, sizeof(path_255), R2FS_ITEST_PATH_SLOTS, name_255);
    r2fsRtemsMountJoinPath(path_rename_9, sizeof(path_rename_9), R2FS_ITEST_PATH_SLOTS, rename_9);
    r2fsRtemsMountJoinPath(
        path_rename_255,
        sizeof(path_rename_255),
        R2FS_ITEST_PATH_SLOTS,
        rename_255
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path_9, 0644));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path_255, 0644));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRename(path_9, path_rename_9));
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(path_9, &st));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(path_rename_9, &st));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRename(path_255, path_rename_255));
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(path_255, &st));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(path_rename_255, &st));

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_UnlinkAfterRename_ShouldRemoveEntriesFromDirectory)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    R2fsRtemsMountDirEntries dir_entries;
    struct stat st;
    char name_255[RTFS_NAME_LEN + 1];
    char rename_255[RTFS_NAME_LEN + 1];
    char path_8[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_9[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_255[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_rename_9[R2FS_RTEMS_ITEST_PATH_MAX];
    char path_rename_255[R2FS_RTEMS_ITEST_PATH_MAX];
    const char *name_8 = "12345678";
    const char *name_9 = "123456789";
    const char *rename_9 = "abcdefghi";

    /*
     * 这条用例验证 rename 后目录项的 unlink 语义：
     * 删除后路径必须不可见，父目录枚举结果也应回收至空目录状态。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    r2fsRtemsMountBuildName(name_255, RTFS_NAME_LEN, 'x');
    r2fsRtemsMountBuildName(rename_255, RTFS_NAME_LEN, 'y');

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_SLOTS, 0755));

    r2fsRtemsMountJoinPath(path_8, sizeof(path_8), R2FS_ITEST_PATH_SLOTS, name_8);
    r2fsRtemsMountJoinPath(path_9, sizeof(path_9), R2FS_ITEST_PATH_SLOTS, name_9);
    r2fsRtemsMountJoinPath(path_255, sizeof(path_255), R2FS_ITEST_PATH_SLOTS, name_255);
    r2fsRtemsMountJoinPath(path_rename_9, sizeof(path_rename_9), R2FS_ITEST_PATH_SLOTS, rename_9);
    r2fsRtemsMountJoinPath(
        path_rename_255,
        sizeof(path_rename_255),
        R2FS_ITEST_PATH_SLOTS,
        rename_255
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path_8, 0644));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path_9, 0644));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(path_255, 0644));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRename(path_9, path_rename_9));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRename(path_255, path_rename_255));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureUnlink(path_8));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureUnlink(path_rename_9));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureUnlink(path_rename_255));
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(path_8, &st));
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(path_rename_9, &st));
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(path_rename_255, &st));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureReadDir(R2FS_ITEST_PATH_SLOTS, &dir_entries));
    TEST_ASSERT_EQUAL_size_t(2u, dir_entries.count);

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RenameSameParentRemount_ShouldPreserveInode)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat before_st;
    struct stat after_st;

    /*
     * 这条用例验证同目录 rename 的对象身份语义：
     * rename 只应改变目录项绑定关系，不应更换文件 inode，
     * 且 clean remount 后这一结果仍应保持成立。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_DIR, 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(R2FS_ITEST_PATH_OLD, 0644));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_OLD, &before_st));

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureRename(R2FS_ITEST_PATH_OLD, R2FS_ITEST_PATH_NEW)
    );
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_OLD, &after_st));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_NEW, &after_st));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_st.st_ino, (uint32_t)after_st.st_ino);

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRemount(&fixture));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_NEW, &after_st));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before_st.st_ino, (uint32_t)after_st.st_ino);

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RenameFileAcrossParents_ShouldMovePathAndPreserveFile)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st_moved;

    /*
     * 这条用例验证普通文件跨父目录 rename 的对外语义：
     * 旧路径必须消失，新路径必须可见，且对象类型仍应保持为普通文件。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_P1, 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_P2, 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(R2FS_ITEST_PATH_P1_FILE, 0644));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRename(R2FS_ITEST_PATH_P1_FILE, R2FS_ITEST_PATH_P2_FILE));
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_P1_FILE, &st_moved));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_P2_FILE, &st_moved));
    TEST_ASSERT_TRUE(S_ISREG(st_moved.st_mode));

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RenameDirectoryAcrossParents_ShouldMovePathAndUpdateParentNlink)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
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
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_P1, 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_P2, 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_P1_DIR, 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_P1, &st_p1_before));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_P2, &st_p2_before));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRename(R2FS_ITEST_PATH_P1_DIR, R2FS_ITEST_PATH_P2_DIR));
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_P1_DIR, &st_moved));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_P2_DIR, &st_moved));
    TEST_ASSERT_TRUE(S_ISDIR(st_moved.st_mode));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_P1, &st_p1_after));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_P2, &st_p2_after));
    TEST_ASSERT_EQUAL_INT((int)st_p1_before.st_nlink - 1, (int)st_p1_after.st_nlink);
    TEST_ASSERT_EQUAL_INT((int)st_p2_before.st_nlink + 1, (int)st_p2_after.st_nlink);

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_RemoveNonEmptyDirThenCleanup_ShouldHonorVfsRules)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;

    /*
     * 这条用例验证目录删除规则的对外语义：
     * 非空目录必须返回 ENOTEMPTY；清空后目录应可删除，且 remount
     * 之后该目录仍应保持不可见。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir(R2FS_ITEST_PATH_REMOVE_DIR, 0755));
    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureCreateFile(R2FS_ITEST_PATH_REMOVE_FILE, 0644)
    );

    TEST_ASSERT_EQUAL(ENOTEMPTY, r2fsRtemsMountFixtureRmdir(R2FS_ITEST_PATH_REMOVE_DIR));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureUnlink(R2FS_ITEST_PATH_REMOVE_FILE));
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_REMOVE_FILE, &st));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRmdir(R2FS_ITEST_PATH_REMOVE_DIR));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureRemount(&fixture));
    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath(R2FS_ITEST_PATH_REMOVE_DIR, &st));

    r2fsRtemsMountFixtureDestroy(&fixture);
}

RTFS_TEST(IntegrationNamespace_PathResolutionEdges_ShouldValidateMissingAndIntermediateTypes)
{
    R2fsRtemsMountFixture fixture = R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER;
    struct stat st;

    /*
     * 这条用例验证路径解析边界的对外语义：
     * 缺失路径应返回 ENOENT，中间分量若不是目录应返回 ENOTDIR，
     * 含 '.' 的深层路径也应被正确解析。
     */

    TEST_ASSERT_EQUAL(
        0,
        r2fsRtemsMountFixtureFormatAndMount(&fixture, R2FS_RTEMS_ITEST_LPA_COUNT)
    );

    TEST_ASSERT_EQUAL(ENOENT, r2fsRtemsMountFixtureStatPath("/no/such/path", &st));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(R2FS_ITEST_PATH_TYPE_FILE, 0644));
    TEST_ASSERT_EQUAL(ENOTDIR, r2fsRtemsMountFixtureStatPath("/type-file/child", &st));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir("/deep", 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir("/deep/a", 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir("/deep/a/b", 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir("/deep/a/b/c", 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir("/deep/a/b/c/d", 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureMkdir("/deep/a/b/c/d/e", 0755));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureCreateFile(R2FS_ITEST_PATH_DEEP_FILE, 0644));

    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath("/deep/a/./b/./c/d/./e", &st));
    TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
    TEST_ASSERT_EQUAL(0, r2fsRtemsMountFixtureStatPath("/deep/a/./b/c/./d/e/f.txt", &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));

    r2fsRtemsMountFixtureDestroy(&fixture);
}
