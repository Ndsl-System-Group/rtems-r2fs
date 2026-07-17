#include "integration/rtfs_rtems_mount_fixture.h"

#include "rtfs_config.h"
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

#define RTFS_RTEMS_ITEST_BLOCK_SIZE 4096U
#define RTFS_RTEMS_ITEST_DEVICE_DIR "/dev"

typedef enum RtfsRtemsItestDeviceMode
{
    RTFS_RTEMS_ITEST_DEVICE_MODE_RAMDISK = 0,
    RTFS_RTEMS_ITEST_DEVICE_MODE_EXTERNAL = 1
} RtfsRtemsItestDeviceMode;

typedef struct RtfsRtemsMountFixtureState
{
    ramdisk *ramdisk;
    comm_dev dev;
    RtfsMkfsLayout layout;
    RtfsRtemsItestDeviceMode device_mode;
    char device_path[RTFS_RTEMS_ITEST_PATH_MAX];
    bool device_registered;
    bool mounted;
} RtfsRtemsMountFixtureState;

static bool g_rtfs_itest_fs_registered = false;
static RtfsRtemsMountFixtureState *g_rtfs_active_fixture_state = NULL;

static int rtfsRtemsMountFixturePathFromRelative(
    const char *relative_path,
    char *buffer,
    size_t buffer_size);

static int rtfsRtemsMountFixturePathStat(
    const char *path,
    struct stat *st);

static int rtfsRtemsMountFixturePathStatvfs(
    const char *path,
    struct statvfs *stvfs);

static const char *rtfsRtemsMountFixtureConfiguredDeviceMode(void);

static const char *rtfsRtemsMountFixtureConfiguredDevicePath(void);

static int rtfsRtemsMountFixtureResolveDeviceMode(
    RtfsRtemsItestDeviceMode *out_mode);

static const char *rtfsRtemsMountFixtureDevicePath(
    const RtfsRtemsMountFixtureState *state);

static bool rtfsRtemsMountFixtureProbeDiskPath(
    const char *path,
    uint32_t *out_media_block_size,
    rtems_blkdev_bnum *out_media_block_count);

static int rtfsRtemsMountFixtureResolveExternalDevicePath(
    char *buffer,
    size_t buffer_size,
    uint64_t required_lpa_count);

static const char *rtfsRtemsMountFixtureConfiguredDeviceMode(void)
{
    const char *mode = getenv(RTFS_RTEMS_ITEST_DEVICE_MODE_ENV);

    if (mode != NULL && mode[0] != '\0')
    {
        return mode;
    }

#ifdef RTFS_CONFIG_ITEST_DEVICE_MODE
    return RTFS_CONFIG_ITEST_DEVICE_MODE;
#else
    return NULL;
#endif
}

static const char *rtfsRtemsMountFixtureConfiguredDevicePath(void)
{
    const char *path = getenv(RTFS_RTEMS_ITEST_DEVICE_PATH_ENV);

    if (path != NULL && path[0] != '\0')
    {
        return path;
    }

#ifdef RTFS_CONFIG_ITEST_DEVICE_PATH
    return RTFS_CONFIG_ITEST_DEVICE_PATH;
#else
    return NULL;
#endif
}

static int rtfsRtemsMountFixtureResolveDeviceMode(
    RtfsRtemsItestDeviceMode *out_mode)
{
    const char *configured_mode;

    if (out_mode == NULL)
    {
        return EINVAL;
    }

    configured_mode = rtfsRtemsMountFixtureConfiguredDeviceMode();
    if (configured_mode == NULL || configured_mode[0] == '\0' ||
        strcmp(configured_mode, "ramdisk") == 0)
    {
        *out_mode = RTFS_RTEMS_ITEST_DEVICE_MODE_RAMDISK;
        return 0;
    }

    if (strcmp(configured_mode, "external") == 0)
    {
        *out_mode = RTFS_RTEMS_ITEST_DEVICE_MODE_EXTERNAL;
        return 0;
    }

    printf(
        "[ RTFS ] invalid itest device mode '%s', expected 'ramdisk' or 'external'\n",
        configured_mode);
    return EINVAL;
}

static const char *rtfsRtemsMountFixtureDevicePath(
    const RtfsRtemsMountFixtureState *state)
{
    if (state == NULL || state->device_path[0] == '\0')
    {
        return RTFS_RTEMS_ITEST_RAMDISK_DEVICE_PATH;
    }

    return state->device_path;
}

static bool rtfsRtemsMountFixtureProbeDiskPath(
    const char *path,
    uint32_t *out_media_block_size,
    rtems_blkdev_bnum *out_media_block_count)
{
    int fd;
    rtems_disk_device *disk_device = NULL;
    uint32_t media_block_size = 0;
    rtems_blkdev_bnum media_block_count = 0;
    bool ok = false;

    if (path == NULL || path[0] == '\0')
    {
        return false;
    }

    fd = open(path, O_RDWR);
    if (fd < 0)
    {
        return false;
    }

    if (rtems_disk_fd_get_disk_device(fd, &disk_device) != 0)
    {
        goto out;
    }

    if (rtems_disk_fd_get_media_block_size(fd, &media_block_size) != 0)
    {
        goto out;
    }

    if (rtems_disk_fd_get_block_count(fd, &media_block_count) != 0)
    {
        goto out;
    }

    if (disk_device == NULL)
    {
        goto out;
    }

    if (out_media_block_size != NULL)
    {
        *out_media_block_size = media_block_size;
    }

    if (out_media_block_count != NULL)
    {
        *out_media_block_count = media_block_count;
    }

    ok = true;

out:
    close(fd);
    return ok;
}

static int rtfsRtemsMountFixtureResolveExternalDevicePath(
    char *buffer,
    size_t buffer_size,
    uint64_t required_lpa_count)
{
    const char *configured_path;
    DIR *dir;
    struct dirent *entry;
    uint64_t required_block_count;
    char first_match[RTFS_RTEMS_ITEST_PATH_MAX];
    size_t match_count = 0;

    if (buffer == NULL || buffer_size == 0)
    {
        return EINVAL;
    }

    required_block_count = required_lpa_count * LBA_PER_LPA;
    configured_path = rtfsRtemsMountFixtureConfiguredDevicePath();
    if (configured_path != NULL && configured_path[0] != '\0')
    {
        uint32_t media_block_size = 0;
        rtems_blkdev_bnum media_block_count = 0;

        if (!rtfsRtemsMountFixtureProbeDiskPath(
                configured_path,
                &media_block_size,
                &media_block_count))
        {
            printf(
                "[ RTFS ] configured external device '%s' is not an RTEMS block device\n",
                configured_path);
            return ENODEV;
        }

        if (media_block_size != 512U ||
            media_block_count < required_block_count)
        {
            printf(
                "[ RTFS ] configured external device '%s' is not usable: block_size=%lu block_count=%llu required_blocks=%llu\n",
                configured_path,
                (unsigned long)media_block_size,
                (unsigned long long)media_block_count,
                (unsigned long long)required_block_count);
            return EINVAL;
        }

        if (strlen(configured_path) >= buffer_size)
        {
            return ENAMETOOLONG;
        }

        strcpy(buffer, configured_path);
        printf(
            "[ RTFS ] selected external device %s block_size=%lu block_count=%llu\n",
            buffer,
            (unsigned long)media_block_size,
            (unsigned long long)media_block_count);
        return 0;
    }

    dir = opendir(RTFS_RTEMS_ITEST_DEVICE_DIR);
    if (dir == NULL)
    {
        return errno != 0 ? errno : EIO;
    }

    first_match[0] = '\0';
    printf("[ RTFS ] scanning %s for RTEMS block devices\n", RTFS_RTEMS_ITEST_DEVICE_DIR);

    while ((entry = readdir(dir)) != NULL)
    {
        char candidate_path[RTFS_RTEMS_ITEST_PATH_MAX];
        uint32_t media_block_size = 0;
        rtems_blkdev_bnum media_block_count = 0;
        bool usable;
        int written;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        written = snprintf(
            candidate_path,
            sizeof(candidate_path),
            "%s/%s",
            RTFS_RTEMS_ITEST_DEVICE_DIR,
            entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(candidate_path))
        {
            continue;
        }

        if (!rtfsRtemsMountFixtureProbeDiskPath(
                candidate_path,
                &media_block_size,
                &media_block_count))
        {
            continue;
        }

        usable = media_block_size == 512U &&
                 media_block_count >= required_block_count;

        printf(
            "[ RTFS ] block device candidate %s block_size=%lu block_count=%llu usable=%s\n",
            candidate_path,
            (unsigned long)media_block_size,
            (unsigned long long)media_block_count,
            usable ? "yes" : "no");

        if (!usable)
        {
            continue;
        }

        if (match_count == 0)
        {
            strcpy(first_match, candidate_path);
        }
        ++match_count;
    }

    if (closedir(dir) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    if (match_count == 0)
    {
        printf(
            "[ RTFS ] no usable external block device found under %s, required_blocks=%llu\n",
            RTFS_RTEMS_ITEST_DEVICE_DIR,
            (unsigned long long)required_block_count);
        return ENODEV;
    }

    if (match_count > 1)
    {
        printf(
            "[ RTFS ] multiple usable external block devices found (%lu); set %s to choose one explicitly\n",
            (unsigned long)match_count,
            RTFS_RTEMS_ITEST_DEVICE_PATH_ENV);
        return EEXIST;
    }

    if (strlen(first_match) >= buffer_size)
    {
        return ENAMETOOLONG;
    }

    strcpy(buffer, first_match);
    printf("[ RTFS ] selected external device %s\n", buffer);
    return 0;
}

static int rtfsRtemsMountFixtureMkdirParents(void)
{
    struct stat st;

    if (stat("/mnt", &st) == 0)
    {
        return S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
    }

    if (errno != ENOENT)
    {
        return errno != 0 ? errno : EIO;
    }

    if (mkdir("/mnt", 0777) != 0 && errno != EEXIST)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

static void rtfsRtemsMountFixtureSetActive(
    RtfsRtemsMountFixture *fixture)
{
    if (fixture == NULL)
    {
        g_rtfs_active_fixture_state = NULL;
        return;
    }

    g_rtfs_active_fixture_state = fixture->state;
}

static void rtfsRtemsMountFixtureClearActiveIfMatches(
    RtfsRtemsMountFixture *fixture)
{
    if (fixture != NULL && g_rtfs_active_fixture_state == fixture->state)
    {
        g_rtfs_active_fixture_state = NULL;
    }
}

static int rtfsRtemsMountFixtureMount(
    RtfsRtemsMountFixture *fixture)
{
    int ret;

    if (fixture == NULL || fixture->state == NULL ||
        fixture->state->dev.diskDevice == NULL)
    {
        return EINVAL;
    }

    ret = rtfsRtemsMountFixtureMkdirParents();
    if (ret != 0)
    {
        return ret;
    }

    if (!g_rtfs_itest_fs_registered)
    {
        ret = rtems_filesystem_register("rtfs-itest", rtfsInitialize);
        if (ret != 0)
        {
            return errno != 0 ? errno : EIO;
        }
        g_rtfs_itest_fs_registered = true;
    }

    ret = mount_and_make_target_path(
        rtfsRtemsMountFixtureDevicePath(fixture->state),
        RTFS_RTEMS_ITEST_MOUNT_PATH,
        "rtfs-itest",
        RTEMS_FILESYSTEM_READ_WRITE,
        &fixture->state->dev);
    if (ret != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    fixture->state->mounted = true;
    rtfsRtemsMountFixtureSetActive(fixture);
    return 0;
}

static int rtfsRtemsMountFixtureWriteBlock(
    void *ctx,
    uint32_t lpa,
    const void *block)
{
    ramdisk *rd = (ramdisk *)ctx;

    if (rd == NULL || block == NULL || lpa >= rd->block_num)
    {
        return EINVAL;
    }

    memcpy(
        (char *)rd->area + (uint64_t)lpa * RTFS_RTEMS_ITEST_BLOCK_SIZE,
        block,
        RTFS_RTEMS_ITEST_BLOCK_SIZE);
    return 0;
}

static int rtfsRtemsMountFixtureBuildCommDev(
    RtfsRtemsMountFixture *fixture,
    uint64_t lpa_count)
{
    int fd;
    rtems_disk_device *disk_device = NULL;
    uint32_t media_block_size = 0;
    rtems_blkdev_bnum media_block_count = 0;
    int ret = 0;

    if (fixture == NULL || fixture->state == NULL)
    {
        return EINVAL;
    }

    fd = open(rtfsRtemsMountFixtureDevicePath(fixture->state), O_RDWR);
    if (fd < 0)
    {
        return errno != 0 ? errno : EIO;
    }

    if (rtems_disk_fd_get_disk_device(fd, &disk_device) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        goto out;
    }

    if (rtems_disk_fd_get_media_block_size(fd, &media_block_size) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        goto out;
    }

    if (rtems_disk_fd_get_block_count(fd, &media_block_count) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        goto out;
    }

    if (media_block_size != 512U ||
        media_block_count < lpa_count * LBA_PER_LPA)
    {
        ret = EINVAL;
        goto out;
    }

    ret = commDevInit(
        &fixture->state->dev,
        disk_device,
        512U,
        lpa_count * LBA_PER_LPA,
        fixture->state->layout.meta_journal_start_lpa,
        fixture->state->layout.meta_journal_start_lpa +
            (uint64_t)fixture->state->layout.meta_journal_segment_count *
                BLOCK_PER_SEGMENT);

out:
    close(fd);
    return ret;
}

static void rtfsRtemsMountFixtureDestroyState(
    RtfsRtemsMountFixtureState *state)
{
    if (state == NULL)
    {
        return;
    }

    if (state->mounted)
    {
        if (unmount(RTFS_RTEMS_ITEST_MOUNT_PATH) != 0)
        {
            TEST_FAIL_MESSAGE("unmount failed");
        }
        state->mounted = false;
    }

    if (state->dev.diskDevice != NULL)
    {
        commDevDestroy(&state->dev);
    }

    if (state->device_registered)
    {
        (void)unlink(rtfsRtemsMountFixtureDevicePath(state));
        state->device_registered = false;
    }

    if (state->ramdisk != NULL)
    {
        ramdisk_free(state->ramdisk);
        state->ramdisk = NULL;
    }

    if (g_rtfs_active_fixture_state == state)
    {
        g_rtfs_active_fixture_state = NULL;
    }

    free(state);
}

static int rtfsRtemsMountFixturePathFromRelative(
    const char *relative_path,
    char *buffer,
    size_t buffer_size)
{
    int written;

    if (buffer == NULL || buffer_size == 0)
    {
        return EINVAL;
    }

    if (relative_path == NULL || relative_path[0] == '\0')
    {
        relative_path = "/";
    }

    if (strcmp(relative_path, "/") == 0)
    {
        if (buffer_size <= strlen(RTFS_RTEMS_ITEST_MOUNT_PATH))
        {
            return ENAMETOOLONG;
        }
        strcpy(buffer, RTFS_RTEMS_ITEST_MOUNT_PATH);
        return 0;
    }

    if (relative_path[0] != '/')
    {
        return EINVAL;
    }

    written = snprintf(
        buffer,
        buffer_size,
        "%s%s",
        RTFS_RTEMS_ITEST_MOUNT_PATH,
        relative_path);
    if (written < 0 || (size_t)written >= buffer_size)
    {
        return ENAMETOOLONG;
    }

    return 0;
}

static int rtfsRtemsMountFixturePathStat(
    const char *path,
    struct stat *st)
{
    if (path == NULL || st == NULL)
    {
        return EINVAL;
    }

    if (stat(path, st) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

static int rtfsRtemsMountFixturePathStatvfs(
    const char *path,
    struct statvfs *stvfs)
{
    if (path == NULL || stvfs == NULL)
    {
        return EINVAL;
    }

    if (statvfs(path, stvfs) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureFormatAndMount(
    RtfsRtemsMountFixture *fixture,
    uint64_t lpa_count)
{
    RtfsMkfsOptions options;
    RtfsRtemsMountFixtureState *state;
    int ret;

    if (fixture == NULL)
    {
        return EINVAL;
    }

    fixture->state = NULL;

    state = (RtfsRtemsMountFixtureState *)calloc(1, sizeof(*state));
    if (state == NULL)
    {
        fixture->state = NULL;
        return ENOMEM;
    }

    fixture->state = state;
    rtfsRtemsMountFixtureSetActive(fixture);

    ret = rtfsRtemsMountFixtureResolveDeviceMode(&state->device_mode);
    if (ret != 0)
    {
        rtfsRtemsMountFixtureDestroy(fixture);
        return ret;
    }

    memset(&options, 0, sizeof(options));
    options.lpa_count = lpa_count;
    options.root_ino = 1;
    options.meta_journal_segment_count = 1;

    if (state->device_mode == RTFS_RTEMS_ITEST_DEVICE_MODE_RAMDISK)
    {
        rtems_status_code sc;

        strcpy(state->device_path, RTFS_RTEMS_ITEST_RAMDISK_DEVICE_PATH);
        state->ramdisk = ramdisk_allocate(NULL, 512U, lpa_count * LBA_PER_LPA, false);
        if (state->ramdisk == NULL)
        {
            rtfsRtemsMountFixtureDestroy(fixture);
            return ENOMEM;
        }

        sc = rtems_blkdev_create(
            state->device_path,
            512U,
            lpa_count * LBA_PER_LPA,
            ramdisk_ioctl,
            state->ramdisk);
        if (sc != RTEMS_SUCCESSFUL)
        {
            rtfsRtemsMountFixtureDestroy(fixture);
            return EIO;
        }
        state->device_registered = true;

        ret = rtfsMkfsFormat(
            &options,
            rtfsRtemsMountFixtureWriteBlock,
            state->ramdisk,
            &state->layout);
        if (ret != 0)
        {
            rtfsRtemsMountFixtureDestroy(fixture);
            return ret;
        }
    }
    else
    {
        ret = rtfsRtemsMountFixtureResolveExternalDevicePath(
            state->device_path,
            sizeof(state->device_path),
            lpa_count);
        if (ret != 0)
        {
            rtfsRtemsMountFixtureDestroy(fixture);
            return ret;
        }

        ret = rtfsMkfsCalculateLayout(
            lpa_count,
            options.meta_journal_segment_count,
            &state->layout);
        if (ret != 0)
        {
            rtfsRtemsMountFixtureDestroy(fixture);
            return ret;
        }
    }

    ret = rtfsRtemsMountFixtureBuildCommDev(fixture, lpa_count);
    if (ret != 0)
    {
        rtfsRtemsMountFixtureDestroy(fixture);
        return ret;
    }

    if (state->device_mode == RTFS_RTEMS_ITEST_DEVICE_MODE_EXTERNAL)
    {
        ret = rtfsMkfsFormatCommDev(&options, &state->dev, &state->layout);
        if (ret != 0)
        {
            rtfsRtemsMountFixtureDestroy(fixture);
            return ret;
        }
    }

    ret = rtfsRtemsMountFixtureMount(fixture);
    if (ret != 0)
    {
        rtfsRtemsMountFixtureDestroy(fixture);
        return ret;
    }

    return 0;
}

int rtfsRtemsMountFixtureRemount(
    RtfsRtemsMountFixture *fixture)
{
    if (fixture == NULL || fixture->state == NULL ||
        fixture->state->dev.diskDevice == NULL)
    {
        return EINVAL;
    }

    rtfsRtemsMountFixtureUnmount(fixture);
    return rtfsRtemsMountFixtureMount(fixture);
}

void rtfsRtemsMountFixtureUnmount(
    RtfsRtemsMountFixture *fixture)
{
    if (fixture == NULL || fixture->state == NULL || !fixture->state->mounted)
    {
        return;
    }

    if (unmount(RTFS_RTEMS_ITEST_MOUNT_PATH) != 0)
    {
        TEST_FAIL_MESSAGE("unmount failed");
    }

    fixture->state->mounted = false;
}

void rtfsRtemsMountFixtureDestroy(
    RtfsRtemsMountFixture *fixture)
{
    RtfsRtemsMountFixtureState *state;

    if (fixture == NULL)
    {
        return;
    }

    state = fixture->state;
    fixture->state = NULL;
    rtfsRtemsMountFixtureClearActiveIfMatches(fixture);
    rtfsRtemsMountFixtureDestroyState(state);
}

void rtfsRtemsMountFixtureCleanupActive(void)
{
    RtfsRtemsMountFixtureState *state = g_rtfs_active_fixture_state;

    g_rtfs_active_fixture_state = NULL;
    rtfsRtemsMountFixtureDestroyState(state);
}

const RtfsMkfsLayout *rtfsRtemsMountFixtureLayout(
    const RtfsRtemsMountFixture *fixture)
{
    if (fixture == NULL || fixture->state == NULL)
    {
        return NULL;
    }

    return &fixture->state->layout;
}

int rtfsRtemsMountFixtureMakePath(
    char *buffer,
    size_t buffer_size,
    const char *relative_path)
{
    return rtfsRtemsMountFixturePathFromRelative(
        relative_path,
        buffer,
        buffer_size);
}

int rtfsRtemsMountFixtureStatRoot(
    struct stat *st)
{
    return rtfsRtemsMountFixturePathStat(RTFS_RTEMS_ITEST_MOUNT_PATH, st);
}

int rtfsRtemsMountFixtureStatvfsRoot(
    struct statvfs *stvfs)
{
    return rtfsRtemsMountFixturePathStatvfs(RTFS_RTEMS_ITEST_MOUNT_PATH, stvfs);
}

int rtfsRtemsMountFixtureStatPath(
    const char *relative_path,
    struct stat *st)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    return rtfsRtemsMountFixturePathStat(path, st);
}

int rtfsRtemsMountFixtureStatvfsPath(
    const char *relative_path,
    struct statvfs *stvfs)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    return rtfsRtemsMountFixturePathStatvfs(path, stvfs);
}

int rtfsRtemsMountFixtureMkdir(
    const char *relative_path,
    mode_t mode)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    if (mkdir(path, mode) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureCreateFile(
    const char *relative_path,
    mode_t mode)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int fd;
    int ret;

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    fd = open(path, O_CREAT | O_EXCL | O_RDWR, mode);
    if (fd < 0)
    {
        return errno != 0 ? errno : EIO;
    }

    if (close(fd) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureOpen(
    const char *relative_path,
    int flags,
    mode_t mode,
    int *out_fd)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int fd;
    int ret;

    if (out_fd == NULL)
    {
        return EINVAL;
    }

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    fd = open(path, flags, mode);
    if (fd < 0)
    {
        return errno != 0 ? errno : EIO;
    }

    *out_fd = fd;
    return 0;
}

int rtfsRtemsMountFixtureClose(
    int fd)
{
    if (fd < 0)
    {
        return EINVAL;
    }

    if (close(fd) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

ssize_t rtfsRtemsMountFixtureWrite(
    int fd,
    const void *data,
    size_t size)
{
    ssize_t written;

    if (fd < 0 || (data == NULL && size != 0))
    {
        errno = EINVAL;
        return -1;
    }

    written = write(fd, data, size);
    if (written < 0)
    {
        errno = errno != 0 ? errno : EIO;
    }

    return written;
}

ssize_t rtfsRtemsMountFixtureRead(
    int fd,
    void *buffer,
    size_t size)
{
    ssize_t bytes_read;

    if (fd < 0 || (buffer == NULL && size != 0))
    {
        errno = EINVAL;
        return -1;
    }

    bytes_read = read(fd, buffer, size);
    if (bytes_read < 0)
    {
        errno = errno != 0 ? errno : EIO;
    }

    return bytes_read;
}

off_t rtfsRtemsMountFixtureLseek(
    int fd,
    off_t offset,
    int whence)
{
    off_t new_offset;

    if (fd < 0)
    {
        errno = EINVAL;
        return (off_t)-1;
    }

    new_offset = lseek(fd, offset, whence);
    if (new_offset < 0)
    {
        errno = errno != 0 ? errno : EIO;
    }

    return new_offset;
}

int rtfsRtemsMountFixtureFtruncate(
    int fd,
    off_t length)
{
    if (fd < 0)
    {
        return EINVAL;
    }

    if (ftruncate(fd, length) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureFdatasync(
    int fd)
{
    if (fd < 0)
    {
        return EINVAL;
    }

    if (fdatasync(fd) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureWriteFile(
    const char *relative_path,
    const void *data,
    size_t size)
{
    int fd;
    int ret;
    ssize_t written;

    if (data == NULL && size != 0)
    {
        return EINVAL;
    }

    ret = rtfsRtemsMountFixtureOpen(relative_path, O_WRONLY | O_TRUNC, 0, &fd);
    if (ret != 0)
    {
        return ret;
    }

    written = rtfsRtemsMountFixtureWrite(fd, data, size);
    if (written < 0 || (size_t)written != size)
    {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsRtemsMountFixtureClose(fd);
        return ret;
    }

    if (fsync(fd) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsRtemsMountFixtureClose(fd);
        return ret;
    }

    return rtfsRtemsMountFixtureClose(fd);
}

ssize_t rtfsRtemsMountFixtureReadFile(
    const char *relative_path,
    void *buffer,
    size_t size)
{
    int fd;
    int ret;
    ssize_t bytes_read;

    if (buffer == NULL && size != 0)
    {
        errno = EINVAL;
        return -1;
    }

    ret = rtfsRtemsMountFixtureOpen(relative_path, O_RDONLY, 0, &fd);
    if (ret != 0)
    {
        errno = ret;
        return -1;
    }

    bytes_read = rtfsRtemsMountFixtureRead(fd, buffer, size);
    ret = errno;
    if (rtfsRtemsMountFixtureClose(fd) != 0 && bytes_read >= 0)
    {
        errno = errno != 0 ? errno : EIO;
        return -1;
    }

    if (bytes_read < 0)
    {
        errno = ret != 0 ? ret : EIO;
    }

    return bytes_read;
}

int rtfsRtemsMountFixtureRename(
    const char *old_relative_path,
    const char *new_relative_path)
{
    char old_path[RTFS_RTEMS_ITEST_PATH_MAX];
    char new_path[RTFS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = rtfsRtemsMountFixturePathFromRelative(
        old_relative_path,
        old_path,
        sizeof(old_path));
    if (ret != 0)
    {
        return ret;
    }

    ret = rtfsRtemsMountFixturePathFromRelative(
        new_relative_path,
        new_path,
        sizeof(new_path));
    if (ret != 0)
    {
        return ret;
    }

    if (rename(old_path, new_path) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureUnlink(
    const char *relative_path)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    if (unlink(path) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureRemove(
    const char *relative_path)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    if (remove(path) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureSymlink(
    const char *target,
    const char *link_relative_path)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int ret;

    if (target == NULL)
    {
        return EINVAL;
    }

    ret = rtfsRtemsMountFixturePathFromRelative(
        link_relative_path,
        path,
        sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    if (symlink(target, path) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureChown(
    const char *relative_path,
    uid_t owner,
    gid_t group)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    if (chown(path, owner, group) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureRmdir(
    const char *relative_path)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    int ret;

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    if (rmdir(path) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsRtemsMountFixtureReadDir(
    const char *relative_path,
    RtfsRtemsMountDirEntries *out_entries)
{
    char path[RTFS_RTEMS_ITEST_PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    int ret;

    if (out_entries == NULL)
    {
        return EINVAL;
    }

    ret = rtfsRtemsMountFixturePathFromRelative(relative_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    memset(out_entries, 0, sizeof(*out_entries));

    dir = opendir(path);
    if (dir == NULL)
    {
        return errno != 0 ? errno : EIO;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (out_entries->count >=
            sizeof(out_entries->entries) / sizeof(out_entries->entries[0]))
        {
            closedir(dir);
            return ENOSPC;
        }

        memcpy(
            &out_entries->entries[out_entries->count],
            entry,
            sizeof(out_entries->entries[0]));
        out_entries->count++;
    }

    if (closedir(dir) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}
