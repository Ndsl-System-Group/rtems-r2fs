#pragma once

#include "communication/dev.h"
#include "fs/rtfs_mkfs.h"
#include "utils/io_utils.h"

#include <dirent.h>
#include <rtems/ramdisk.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#define RTFS_RTEMS_ITEST_LPA_COUNT (8U * BLOCK_PER_SEGMENT)
#define RTFS_RTEMS_ITEST_DEVICE_PATH "/dev/rtfs-ram0"
#define RTFS_RTEMS_ITEST_MOUNT_PATH "/mnt/rtfs-itest"
#define RTFS_RTEMS_ITEST_PATH_MAX 512
#define RTFS_RTEMS_ITEST_MAX_DIRENTS 256
#define RTFS_RTEMS_MOUNT_FIXTURE_INITIALIZER {NULL}

typedef struct RtfsRtemsMountFixture
{
    struct RtfsRtemsMountFixtureState *state;
} RtfsRtemsMountFixture;

typedef struct RtfsRtemsMountDirEntries
{
    struct dirent entries[RTFS_RTEMS_ITEST_MAX_DIRENTS];
    size_t count;
} RtfsRtemsMountDirEntries;

int rtfsRtemsMountFixtureFormatAndMount(
    RtfsRtemsMountFixture *fixture,
    uint64_t lpa_count);

int rtfsRtemsMountFixtureRemount(
    RtfsRtemsMountFixture *fixture);

void rtfsRtemsMountFixtureUnmount(
    RtfsRtemsMountFixture *fixture);

void rtfsRtemsMountFixtureDestroy(
    RtfsRtemsMountFixture *fixture);

void rtfsRtemsMountFixtureCleanupActive(void);

const RtfsMkfsLayout *rtfsRtemsMountFixtureLayout(
    const RtfsRtemsMountFixture *fixture);

int rtfsRtemsMountFixtureMakePath(
    char *buffer,
    size_t buffer_size,
    const char *relative_path);

int rtfsRtemsMountFixtureStatRoot(
    struct stat *st);

int rtfsRtemsMountFixtureStatvfsRoot(
    struct statvfs *stvfs);

int rtfsRtemsMountFixtureStatPath(
    const char *relative_path,
    struct stat *st);

int rtfsRtemsMountFixtureStatvfsPath(
    const char *relative_path,
    struct statvfs *stvfs);

int rtfsRtemsMountFixtureMkdir(
    const char *relative_path,
    mode_t mode);

int rtfsRtemsMountFixtureCreateFile(
    const char *relative_path,
    mode_t mode);

int rtfsRtemsMountFixtureOpen(
    const char *relative_path,
    int flags,
    mode_t mode,
    int *out_fd);

int rtfsRtemsMountFixtureClose(
    int fd);

ssize_t rtfsRtemsMountFixtureWrite(
    int fd,
    const void *data,
    size_t size);

ssize_t rtfsRtemsMountFixtureRead(
    int fd,
    void *buffer,
    size_t size);

off_t rtfsRtemsMountFixtureLseek(
    int fd,
    off_t offset,
    int whence);

int rtfsRtemsMountFixtureFtruncate(
    int fd,
    off_t length);

int rtfsRtemsMountFixtureFdatasync(
    int fd);

int rtfsRtemsMountFixtureWriteFile(
    const char *relative_path,
    const void *data,
    size_t size);

ssize_t rtfsRtemsMountFixtureReadFile(
    const char *relative_path,
    void *buffer,
    size_t size);

int rtfsRtemsMountFixtureRename(
    const char *old_relative_path,
    const char *new_relative_path);

int rtfsRtemsMountFixtureUnlink(
    const char *relative_path);

int rtfsRtemsMountFixtureRemove(
    const char *relative_path);

int rtfsRtemsMountFixtureSymlink(
    const char *target,
    const char *link_relative_path);

int rtfsRtemsMountFixtureChown(
    const char *relative_path,
    uid_t owner,
    gid_t group);

int rtfsRtemsMountFixtureRmdir(
    const char *relative_path);

int rtfsRtemsMountFixtureReadDir(
    const char *relative_path,
    RtfsRtemsMountDirEntries *out_entries);
