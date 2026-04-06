#include "rtfs_test.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>

#include "fs/dir_inode.h"


static RtfsRuntimeInodeView makeChildView(rtfs_ino ino, rtfs_ino parent_ino, uint8_t file_type)
{
    RtfsRuntimeInodeView view;

    rtfsRuntimeInodeViewInit(&view, ino, parent_ino, file_type);
    return view;
}

static void assertLookupResult(
    const RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino expected_ino,
    rtfs_ino expected_parent_ino,
    uint8_t expected_file_type
)
{
    RtfsDirLookupResult result;

    TEST_ASSERT_EQUAL(
        0,
        rtfsDirInodeLookup(dir_inode, name, strlen(name), &result)
    );
    TEST_ASSERT_EQUAL(expected_ino, result.inode_view.ino);
    TEST_ASSERT_EQUAL(expected_parent_ino, result.inode_view.parent_ino);
    TEST_ASSERT_EQUAL(expected_file_type, result.inode_view.file_type);
}

static struct dirent *direntAt(void *buffer, size_t index)
{
    return (struct dirent *)((char *)buffer + index * sizeof(struct dirent));
}


RTFS_TEST(DirInodeGetAndPut_ShouldCreateEmptyDirectoryObject)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 100);

    TEST_ASSERT_NOT_NULL(dir_inode);
    assertLookupResult(dir_inode, ".", 100, 100, RTFS_FT_DIR);
    assertLookupResult(dir_inode, "..", 100, 100, RTFS_FT_DIR);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodePut_WhenNull_ShouldBeSafe)
{
    rtfsDirInodePut(NULL);
    TEST_PASS();
}

RTFS_TEST(DirInodeLookup_WhenEntryExists_ShouldReturnChildRuntimeView)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 200);
    RtfsRuntimeInodeView child_view = makeChildView(300, 999, RTFS_FT_REG_FILE);

    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "file.txt", &child_view));

    assertLookupResult(dir_inode, "file.txt", 300, 200, RTFS_FT_REG_FILE);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeLookup_WhenNameMissing_ShouldReturnENOENT)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 88);
    RtfsDirLookupResult result;

    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(
        ENOENT,
        rtfsDirInodeLookup(dir_inode, "missing", strlen("missing"), &result)
    );

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeLookup_WhenNameLengthDoesNotMatchStoredName_ShouldReturnENOENT)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 201);
    RtfsRuntimeInodeView child_view = makeChildView(301, 0, RTFS_FT_REG_FILE);
    RtfsDirLookupResult result;

    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "hello", &child_view));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeLookup(dir_inode, "hello", 4, &result));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeLookup_WhenArgumentsAreInvalid_ShouldReject)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 55);
    RtfsDirLookupResult result;

    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(ENOTDIR, rtfsDirInodeLookup(NULL, "a", 1, &result));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeLookup(dir_inode, NULL, 1, &result));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeLookup(dir_inode, "a", 1, NULL));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenDirectoryIsEmpty_ShouldReturnDotAndDotDot)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 42);
    struct dirent entries[4];
    off_t offset = 0;
    ssize_t bytes_read;

    TEST_ASSERT_NOT_NULL(dir_inode);
    memset(entries, 0, sizeof(entries));

    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));

    TEST_ASSERT_EQUAL(2 * (ssize_t)sizeof(struct dirent), bytes_read);
    TEST_ASSERT_EQUAL(2 * (off_t)sizeof(struct dirent), offset);
    TEST_ASSERT_EQUAL_STRING(".", direntAt(entries, 0)->d_name);
    TEST_ASSERT_EQUAL(42, direntAt(entries, 0)->d_ino);
    TEST_ASSERT_EQUAL_STRING("..", direntAt(entries, 1)->d_name);
    TEST_ASSERT_EQUAL(42, direntAt(entries, 1)->d_ino);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenDirectoryHasChildren_ShouldReturnAllEntriesInSequence)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 500);
    RtfsRuntimeInodeView file_view = makeChildView(501, 0, RTFS_FT_REG_FILE);
    RtfsRuntimeInodeView subdir_view = makeChildView(502, 0, RTFS_FT_DIR);
    struct dirent entries[6];
    off_t offset = 0;
    ssize_t bytes_read;

    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "alpha", &file_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "beta", &subdir_view));
    memset(entries, 0, sizeof(entries));

    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));

    TEST_ASSERT_EQUAL(4 * (ssize_t)sizeof(struct dirent), bytes_read);
    TEST_ASSERT_EQUAL(4 * (off_t)sizeof(struct dirent), offset);
    TEST_ASSERT_EQUAL_STRING(".", direntAt(entries, 0)->d_name);
    TEST_ASSERT_EQUAL_STRING("..", direntAt(entries, 1)->d_name);
    TEST_ASSERT_EQUAL_STRING("alpha", direntAt(entries, 2)->d_name);
    TEST_ASSERT_EQUAL(501, direntAt(entries, 2)->d_ino);
    TEST_ASSERT_EQUAL_STRING("beta", direntAt(entries, 3)->d_name);
    TEST_ASSERT_EQUAL(502, direntAt(entries, 3)->d_ino);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenReadingWithSmallBuffer_ShouldAdvanceOffsetIncrementally)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 700);
    RtfsRuntimeInodeView child_view = makeChildView(701, 0, RTFS_FT_REG_FILE);
    struct dirent entry;
    off_t offset = 0;

    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "gamma", &child_view));

    memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_EQUAL(
        (ssize_t)sizeof(struct dirent),
        rtfsDirInodeReadEntries(dir_inode, &offset, &entry, sizeof(entry))
    );
    TEST_ASSERT_EQUAL_STRING(".", entry.d_name);
    TEST_ASSERT_EQUAL((off_t)sizeof(struct dirent), offset);

    memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_EQUAL(
        (ssize_t)sizeof(struct dirent),
        rtfsDirInodeReadEntries(dir_inode, &offset, &entry, sizeof(entry))
    );
    TEST_ASSERT_EQUAL_STRING("..", entry.d_name);
    TEST_ASSERT_EQUAL(2 * (off_t)sizeof(struct dirent), offset);

    memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_EQUAL(
        (ssize_t)sizeof(struct dirent),
        rtfsDirInodeReadEntries(dir_inode, &offset, &entry, sizeof(entry))
    );
    TEST_ASSERT_EQUAL_STRING("gamma", entry.d_name);
    TEST_ASSERT_EQUAL(701, entry.d_ino);
    TEST_ASSERT_EQUAL(3 * (off_t)sizeof(struct dirent), offset);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenBufferIsSmallerThanOneDirent_ShouldReturnZeroAndKeepOffset)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 710);
    char buffer[sizeof(struct dirent) - 1];
    off_t offset = 0;

    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeReadEntries(dir_inode, &offset, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(0, offset);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenOffsetIsAtEnd_ShouldReturnZeroAndKeepOffset)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 720);
    struct dirent entries[4];
    off_t offset = 0;
    ssize_t bytes_read;
    off_t end_offset;

    TEST_ASSERT_NOT_NULL(dir_inode);

    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));
    TEST_ASSERT_EQUAL(2 * (ssize_t)sizeof(struct dirent), bytes_read);
    end_offset = offset;

    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries));
    TEST_ASSERT_EQUAL(0, bytes_read);
    TEST_ASSERT_EQUAL(end_offset, offset);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeAddEntry_WhenDuplicateOrInvalid_ShouldReject)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 900);
    RtfsRuntimeInodeView child_view = makeChildView(901, 0, RTFS_FT_REG_FILE);
    char long_name[RTFS_NAME_LEN + 2];

    TEST_ASSERT_NOT_NULL(dir_inode);
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "dup", &child_view));
    TEST_ASSERT_EQUAL(EEXIST, rtfsDirInodeAddEntry(dir_inode, "dup", &child_view));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, rtfsDirInodeAddEntry(dir_inode, long_name, &child_view));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, rtfsDirInodeAddEntry(dir_inode, "", &child_view));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeAddEntry(NULL, "x", &child_view));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeAddEntry(dir_inode, NULL, &child_view));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeAddEntry(dir_inode, "x", NULL));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeRemoveEntry_WhenEntryExists_ShouldRemoveItFromLookupAndReadDir)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 1000);
    RtfsRuntimeInodeView first_view = makeChildView(1001, 0, RTFS_FT_REG_FILE);
    RtfsRuntimeInodeView second_view = makeChildView(1002, 0, RTFS_FT_DIR);
    struct dirent entries[5];
    off_t offset = 0;
    RtfsDirLookupResult result;

    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "first", &first_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "second", &second_view));

    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, "first"));
    TEST_ASSERT_EQUAL(
        ENOENT,
        rtfsDirInodeLookup(dir_inode, "first", strlen("first"), &result)
    );
    assertLookupResult(dir_inode, "second", 1002, 1000, RTFS_FT_DIR);

    memset(entries, 0, sizeof(entries));
    TEST_ASSERT_EQUAL(
        3 * (ssize_t)sizeof(struct dirent),
        rtfsDirInodeReadEntries(dir_inode, &offset, entries, sizeof(entries))
    );
    TEST_ASSERT_EQUAL_STRING(".", direntAt(entries, 0)->d_name);
    TEST_ASSERT_EQUAL_STRING("..", direntAt(entries, 1)->d_name);
    TEST_ASSERT_EQUAL_STRING("second", direntAt(entries, 2)->d_name);

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeRemoveEntry_WhenEntryDoesNotExistOrArgumentsInvalid_ShouldReject)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 1100);
    char long_name[RTFS_NAME_LEN + 2];

    TEST_ASSERT_NOT_NULL(dir_inode);
    memset(long_name, 'b', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeRemoveEntry(dir_inode, "missing"));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, rtfsDirInodeRemoveEntry(dir_inode, ""));
    TEST_ASSERT_EQUAL(ENAMETOOLONG, rtfsDirInodeRemoveEntry(dir_inode, long_name));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeRemoveEntry(NULL, "x"));
    TEST_ASSERT_EQUAL(EINVAL, rtfsDirInodeRemoveEntry(dir_inode, NULL));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeRemoveEntry_WhenRemovingSameEntryTwice_ShouldReturnENOENTSecondTime)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 1110);
    RtfsRuntimeInodeView child_view = makeChildView(1111, 0, RTFS_FT_REG_FILE);

    TEST_ASSERT_NOT_NULL(dir_inode);
    TEST_ASSERT_EQUAL(0, rtfsDirInodeAddEntry(dir_inode, "once", &child_view));
    TEST_ASSERT_EQUAL(0, rtfsDirInodeRemoveEntry(dir_inode, "once"));
    TEST_ASSERT_EQUAL(ENOENT, rtfsDirInodeRemoveEntry(dir_inode, "once"));

    rtfsDirInodePut(dir_inode);
}

RTFS_TEST(DirInodeReadEntries_WhenArgumentsInvalid_ShouldReturnMinusOneAndSetErrno)
{
    RtfsDirInode *dir_inode = rtfsDirInodeGet(NULL, 1200);
    struct dirent entry;
    off_t offset = 0;

    TEST_ASSERT_NOT_NULL(dir_inode);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsDirInodeReadEntries(NULL, &offset, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(ENOTDIR, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsDirInodeReadEntries(dir_inode, NULL, &entry, sizeof(entry)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    errno = 0;
    TEST_ASSERT_EQUAL(-1, rtfsDirInodeReadEntries(dir_inode, &offset, NULL, sizeof(entry)));
    TEST_ASSERT_EQUAL(EINVAL, errno);

    rtfsDirInodePut(dir_inode);
}
