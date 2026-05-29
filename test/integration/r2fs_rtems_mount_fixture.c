#include "integration/r2fs_rtems_mount_fixture.h"

#include "rtfs_test.h"

#include "fs/fs.h"
#include "fs/fs_handler.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <rtems/blkdev.h>
#include <rtems/libio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define R2FS_RTEMS_ITEST_BLOCK_SIZE 4096U

typedef struct R2fsRtemsMountFixtureState
{
    ramdisk *ramdisk;
    comm_dev dev;
    R2fsMkfsLayout layout;
    bool device_registered;
    bool mounted;
} R2fsRtemsMountFixtureState;

static bool g_r2fs_itest_fs_registered = false;
static R2fsRtemsMountFixtureState *g_r2fs_active_fixture_state = NULL;

static int r2fsRtemsMountFixturePathFromRelative(
    const char *relative_path,
    char *buffer,
    size_t buffer_size
);

static int r2fsRtemsMountFixturePathStat(
    const char *path,
    struct stat *st
);

static int r2fsRtemsMountFixturePathStatvfs(
    const char *path,
    struct statvfs *stvfs
);

static int r2fsRtemsMountFixtureMkdirParents(void)
{
    struct stat st;

    if (stat("/mnt", &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
    }

    if (errno != ENOENT) {
        return errno != 0 ? errno : EIO;
    }

    if (mkdir("/mnt", 0777) != 0 && errno != EEXIST) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

static void r2fsRtemsMountFixtureSetActive(
    R2fsRtemsMountFixture *fixture
)
{
    if (fixture == NULL) {
        g_r2fs_active_fixture_state = NULL;
        return;
    }

    g_r2fs_active_fixture_state = fixture->state;
}

static void r2fsRtemsMountFixtureClearActiveIfMatches(
    R2fsRtemsMountFixture *fixture
)
{
    if (fixture != NULL && g_r2fs_active_fixture_state == fixture->state) {
        g_r2fs_active_fixture_state = NULL;
    }
}

static int r2fsRtemsMountFixtureMount(
    R2fsRtemsMountFixture *fixture
)
{
    int ret;

    if (fixture == NULL || fixture->state == NULL ||
        fixture->state->dev.diskDevice == NULL) {
        return EINVAL;
    }

    ret = r2fsRtemsMountFixtureMkdirParents();
    if (ret != 0) {
        return ret;
    }

    if (!g_r2fs_itest_fs_registered) {
        ret = rtems_filesystem_register("r2fs-itest", r2fsInitialize);
        if (ret != 0) {
            return errno != 0 ? errno : EIO;
        }
        g_r2fs_itest_fs_registered = true;
    }

    ret = mount_and_make_target_path(
        R2FS_RTEMS_ITEST_DEVICE_PATH,
        R2FS_RTEMS_ITEST_MOUNT_PATH,
        "r2fs-itest",
        RTEMS_FILESYSTEM_READ_WRITE,
        &fixture->state->dev
    );
    if (ret != 0) {
        return errno != 0 ? errno : EIO;
    }

    fixture->state->mounted = true;
    r2fsRtemsMountFixtureSetActive(fixture);
    return 0;
}

static int r2fsRtemsMountFixtureWriteBlock(
    void *ctx,
    uint32_t lpa,
    const void *block
)
{
    ramdisk *rd = (ramdisk *)ctx;

    if (rd == NULL || block == NULL || lpa >= rd->block_num) {
        return EINVAL;
    }

    memcpy(
        (char *)rd->area + (uint64_t)lpa * R2FS_RTEMS_ITEST_BLOCK_SIZE,
        block,
        R2FS_RTEMS_ITEST_BLOCK_SIZE
    );
    return 0;
}

static int r2fsRtemsMountFixtureBuildCommDev(
    R2fsRtemsMountFixture *fixture,
    uint64_t lpa_count
)
{
    int fd;
    rtems_disk_device *disk_device = NULL;
    uint32_t media_block_size = 0;
    rtems_blkdev_bnum media_block_count = 0;
    int ret = 0;

    if (fixture == NULL || fixture->state == NULL) {
        return EINVAL;
    }

    fd = open(R2FS_RTEMS_ITEST_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        return errno != 0 ? errno : EIO;
    }

    if (rtems_disk_fd_get_disk_device(fd, &disk_device) != 0) {
        ret = errno != 0 ? errno : EIO;
        goto out;
    }

    if (rtems_disk_fd_get_media_block_size(fd, &media_block_size) != 0) {
        ret = errno != 0 ? errno : EIO;
        goto out;
    }

    if (rtems_disk_fd_get_block_count(fd, &media_block_count) != 0) {
        ret = errno != 0 ? errno : EIO;
        goto out;
    }

    if (media_block_size != 512U ||
        media_block_count != lpa_count * LBA_PER_LPA) {
        ret = EINVAL;
        goto out;
    }

    ret = commDevInit(
        &fixture->state->dev,
        disk_device,
        512U,
        media_block_count,
        fixture->state->layout.meta_journal_start_lpa,
        fixture->state->layout.meta_journal_start_lpa +
            (uint64_t)fixture->state->layout.meta_journal_segment_count *
                BLOCK_PER_SEGMENT
    );

out:
    close(fd);
    return ret;
}

static void r2fsRtemsMountFixtureDestroyState(
    R2fsRtemsMountFixtureState *state
)
{
    if (state == NULL) {
        return;
    }

    if (state->mounted) {
        if (unmount(R2FS_RTEMS_ITEST_MOUNT_PATH) != 0) {
            TEST_FAIL_MESSAGE("unmount failed");
        }
        state->mounted = false;
    }

    if (state->dev.diskDevice != NULL) {
        commDevDestroy(&state->dev);
    }

    if (state->device_registered) {
        (void)unlink(R2FS_RTEMS_ITEST_DEVICE_PATH);
        state->device_registered = false;
    }

    if (state->ramdisk != NULL) {
        ramdisk_free(state->ramdisk);
        state->ramdisk = NULL;
    }

    if (g_r2fs_active_fixture_state == state) {
        g_r2fs_active_fixture_state = NULL;
    }

    free(state);
}

static int r2fsRtemsMountFixturePathFromRelative(
    const char *relative_path,
    char *buffer,
    size_t buffer_size
)
{
    int written;

    if (buffer == NULL || buffer_size == 0) {
        return EINVAL;
    }

    if (relative_path == NULL || relative_path[0] == '\0') {
        relative_path = "/";
    }

    if (strcmp(relative_path, "/") == 0) {
        if (buffer_size <= strlen(R2FS_RTEMS_ITEST_MOUNT_PATH)) {
            return ENAMETOOLONG;
        }
        strcpy(buffer, R2FS_RTEMS_ITEST_MOUNT_PATH);
        return 0;
    }

    if (relative_path[0] != '/') {
        return EINVAL;
    }

    written = snprintf(
        buffer,
        buffer_size,
        "%s%s",
        R2FS_RTEMS_ITEST_MOUNT_PATH,
        relative_path
    );
    if (written < 0 || (size_t)written >= buffer_size) {
        return ENAMETOOLONG;
    }

    return 0;
}

static int r2fsRtemsMountFixturePathStat(
    const char *path,
    struct stat *st
)
{
    if (path == NULL || st == NULL) {
        return EINVAL;
    }

    if (stat(path, st) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

static int r2fsRtemsMountFixturePathStatvfs(
    const char *path,
    struct statvfs *stvfs
)
{
    if (path == NULL || stvfs == NULL) {
        return EINVAL;
    }

    if (statvfs(path, stvfs) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureFormatAndMount(
    R2fsRtemsMountFixture *fixture,
    uint64_t lpa_count
)
{
    R2fsMkfsOptions options;
    R2fsRtemsMountFixtureState *state;
    rtems_status_code sc;
    int ret;

    if (fixture == NULL) {
        return EINVAL;
    }

    fixture->state = NULL;

    state = (R2fsRtemsMountFixtureState *)calloc(1, sizeof(*state));
    if (state == NULL) {
        fixture->state = NULL;
        return ENOMEM;
    }

    fixture->state = state;
    r2fsRtemsMountFixtureSetActive(fixture);

    state->ramdisk = ramdisk_allocate(NULL, 512U, lpa_count * LBA_PER_LPA, false);
    if (state->ramdisk == NULL) {
        r2fsRtemsMountFixtureDestroy(fixture);
        return ENOMEM;
    }

    sc = rtems_blkdev_create(
        R2FS_RTEMS_ITEST_DEVICE_PATH,
        512U,
        lpa_count * LBA_PER_LPA,
        ramdisk_ioctl,
        state->ramdisk
    );
    if (sc != RTEMS_SUCCESSFUL) {
        r2fsRtemsMountFixtureDestroy(fixture);
        return EIO;
    }
    state->device_registered = true;

    memset(&options, 0, sizeof(options));
    options.lpa_count = lpa_count;
    options.root_ino = 1;
    options.meta_journal_segment_count = 1;

    ret = r2fsMkfsFormat(
        &options,
        r2fsRtemsMountFixtureWriteBlock,
        state->ramdisk,
        &state->layout
    );
    if (ret != 0) {
        r2fsRtemsMountFixtureDestroy(fixture);
        return ret;
    }

    ret = r2fsRtemsMountFixtureBuildCommDev(fixture, lpa_count);
    if (ret != 0) {
        r2fsRtemsMountFixtureDestroy(fixture);
        return ret;
    }

    ret = r2fsRtemsMountFixtureMount(fixture);
    if (ret != 0) {
        r2fsRtemsMountFixtureDestroy(fixture);
        return ret;
    }

    return 0;
}

int r2fsRtemsMountFixtureRemount(
    R2fsRtemsMountFixture *fixture
)
{
    if (fixture == NULL || fixture->state == NULL ||
        !fixture->state->device_registered ||
        fixture->state->dev.diskDevice == NULL) {
        return EINVAL;
    }

    r2fsRtemsMountFixtureUnmount(fixture);
    return r2fsRtemsMountFixtureMount(fixture);
}

void r2fsRtemsMountFixtureUnmount(
    R2fsRtemsMountFixture *fixture
)
{
    if (fixture == NULL || fixture->state == NULL || !fixture->state->mounted) {
        return;
    }

    if (unmount(R2FS_RTEMS_ITEST_MOUNT_PATH) != 0) {
        TEST_FAIL_MESSAGE("unmount failed");
    }

    fixture->state->mounted = false;
}

void r2fsRtemsMountFixtureDestroy(
    R2fsRtemsMountFixture *fixture
)
{
    R2fsRtemsMountFixtureState *state;

    if (fixture == NULL) {
        return;
    }

    state = fixture->state;
    fixture->state = NULL;
    r2fsRtemsMountFixtureClearActiveIfMatches(fixture);
    r2fsRtemsMountFixtureDestroyState(state);
}

void r2fsRtemsMountFixtureCleanupActive(void)
{
    R2fsRtemsMountFixtureState *state = g_r2fs_active_fixture_state;

    g_r2fs_active_fixture_state = NULL;
    r2fsRtemsMountFixtureDestroyState(state);
}

const R2fsMkfsLayout *r2fsRtemsMountFixtureLayout(
    const R2fsRtemsMountFixture *fixture
)
{
    if (fixture == NULL || fixture->state == NULL) {
        return NULL;
    }

    return &fixture->state->layout;
}

int r2fsRtemsMountFixtureMakePath(
    char *buffer,
    size_t buffer_size,
    const char *relative_path
)
{
    return r2fsRtemsMountFixturePathFromRelative(
        relative_path,
        buffer,
        buffer_size
    );
}

int r2fsRtemsMountFixtureStatRoot(
    struct stat *st
)
{
    return r2fsRtemsMountFixturePathStat(R2FS_RTEMS_ITEST_MOUNT_PATH, st);
}

int r2fsRtemsMountFixtureStatvfsRoot(
    struct statvfs *stvfs
)
{
    return r2fsRtemsMountFixturePathStatvfs(R2FS_RTEMS_ITEST_MOUNT_PATH, stvfs);
}

int r2fsRtemsMountFixtureStatPath(
    const char *relative_path,
    struct stat *st
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    return r2fsRtemsMountFixturePathStat(path, st);
}

int r2fsRtemsMountFixtureStatvfsPath(
    const char *relative_path,
    struct statvfs *stvfs
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    return r2fsRtemsMountFixturePathStatvfs(path, stvfs);
}

int r2fsRtemsMountFixtureMkdir(
    const char *relative_path,
    mode_t mode
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    if (mkdir(path, mode) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureCreateFile(
    const char *relative_path,
    mode_t mode
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int fd;
    int ret;

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    fd = open(path, O_CREAT | O_EXCL | O_RDWR, mode);
    if (fd < 0) {
        return errno != 0 ? errno : EIO;
    }

    if (close(fd) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureOpen(
    const char *relative_path,
    int flags,
    mode_t mode,
    int *out_fd
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int fd;
    int ret;

    if (out_fd == NULL) {
        return EINVAL;
    }

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    fd = open(path, flags, mode);
    if (fd < 0) {
        return errno != 0 ? errno : EIO;
    }

    *out_fd = fd;
    return 0;
}

int r2fsRtemsMountFixtureClose(
    int fd
)
{
    if (fd < 0) {
        return EINVAL;
    }

    if (close(fd) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

ssize_t r2fsRtemsMountFixtureWrite(
    int fd,
    const void *data,
    size_t size
)
{
    ssize_t written;

    if (fd < 0 || (data == NULL && size != 0)) {
        errno = EINVAL;
        return -1;
    }

    written = write(fd, data, size);
    if (written < 0) {
        errno = errno != 0 ? errno : EIO;
    }

    return written;
}

ssize_t r2fsRtemsMountFixtureRead(
    int fd,
    void *buffer,
    size_t size
)
{
    ssize_t bytes_read;

    if (fd < 0 || (buffer == NULL && size != 0)) {
        errno = EINVAL;
        return -1;
    }

    bytes_read = read(fd, buffer, size);
    if (bytes_read < 0) {
        errno = errno != 0 ? errno : EIO;
    }

    return bytes_read;
}

off_t r2fsRtemsMountFixtureLseek(
    int fd,
    off_t offset,
    int whence
)
{
    off_t new_offset;

    if (fd < 0) {
        errno = EINVAL;
        return (off_t)-1;
    }

    new_offset = lseek(fd, offset, whence);
    if (new_offset < 0) {
        errno = errno != 0 ? errno : EIO;
    }

    return new_offset;
}

int r2fsRtemsMountFixtureFtruncate(
    int fd,
    off_t length
)
{
    if (fd < 0) {
        return EINVAL;
    }

    if (ftruncate(fd, length) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureFdatasync(
    int fd
)
{
    if (fd < 0) {
        return EINVAL;
    }

    if (fdatasync(fd) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureWriteFile(
    const char *relative_path,
    const void *data,
    size_t size
)
{
    int fd;
    int ret;
    ssize_t written;

    if (data == NULL && size != 0) {
        return EINVAL;
    }

    ret = r2fsRtemsMountFixtureOpen(relative_path, O_WRONLY | O_TRUNC, 0, &fd);
    if (ret != 0) {
        return ret;
    }

    written = r2fsRtemsMountFixtureWrite(fd, data, size);
    if (written < 0 || (size_t)written != size) {
        ret = errno != 0 ? errno : EIO;
        (void)r2fsRtemsMountFixtureClose(fd);
        return ret;
    }

    if (fsync(fd) != 0) {
        ret = errno != 0 ? errno : EIO;
        (void)r2fsRtemsMountFixtureClose(fd);
        return ret;
    }

    return r2fsRtemsMountFixtureClose(fd);
}

ssize_t r2fsRtemsMountFixtureReadFile(
    const char *relative_path,
    void *buffer,
    size_t size
)
{
    int fd;
    int ret;
    ssize_t bytes_read;

    if (buffer == NULL && size != 0) {
        errno = EINVAL;
        return -1;
    }

    ret = r2fsRtemsMountFixtureOpen(relative_path, O_RDONLY, 0, &fd);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    bytes_read = r2fsRtemsMountFixtureRead(fd, buffer, size);
    ret = errno;
    if (r2fsRtemsMountFixtureClose(fd) != 0 && bytes_read >= 0) {
        errno = errno != 0 ? errno : EIO;
        return -1;
    }

    if (bytes_read < 0) {
        errno = ret != 0 ? ret : EIO;
    }

    return bytes_read;
}

int r2fsRtemsMountFixtureRename(
    const char *old_relative_path,
    const char *new_relative_path
)
{
    char old_path[R2FS_RTEMS_ITEST_PATH_MAX];
    char new_path[R2FS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = r2fsRtemsMountFixturePathFromRelative(
        old_relative_path,
        old_path,
        sizeof(old_path)
    );
    if (ret != 0) {
        return ret;
    }

    ret = r2fsRtemsMountFixturePathFromRelative(
        new_relative_path,
        new_path,
        sizeof(new_path)
    );
    if (ret != 0) {
        return ret;
    }

    if (rename(old_path, new_path) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureUnlink(
    const char *relative_path
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    if (unlink(path) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureRemove(
    const char *relative_path
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    if (remove(path) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureSymlink(
    const char *target,
    const char *link_relative_path
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int ret;

    if (target == NULL) {
        return EINVAL;
    }

    ret = r2fsRtemsMountFixturePathFromRelative(
        link_relative_path,
        path,
        sizeof(path)
    );
    if (ret != 0) {
        return ret;
    }

    if (symlink(target, path) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureChown(
    const char *relative_path,
    uid_t owner,
    gid_t group
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    if (chown(path, owner, group) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureRmdir(
    const char *relative_path
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    if (rmdir(path) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsRtemsMountFixtureReadDir(
    const char *relative_path,
    R2fsRtemsMountDirEntries *out_entries
)
{
    char path[R2FS_RTEMS_ITEST_PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    int ret;

    if (out_entries == NULL) {
        return EINVAL;
    }

    ret = r2fsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }

    memset(out_entries, 0, sizeof(*out_entries));

    dir = opendir(path);
    if (dir == NULL) {
        return errno != 0 ? errno : EIO;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (out_entries->count >=
            sizeof(out_entries->entries) / sizeof(out_entries->entries[0])) {
            closedir(dir);
            return ENOSPC;
        }

        memcpy(
            &out_entries->entries[out_entries->count],
            entry,
            sizeof(out_entries->entries[0])
        );
        out_entries->count++;
    }

    if (closedir(dir) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}
