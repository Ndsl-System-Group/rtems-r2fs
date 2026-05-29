#pragma once

#include "communication/dev.h"
#include "fs/r2fs_mkfs.h"
#include "utils/io_utils.h"

#include <dirent.h>
#include <rtems/ramdisk.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#define R2FS_RTEMS_ITEST_LPA_COUNT (8U * BLOCK_PER_SEGMENT)
#define R2FS_RTEMS_ITEST_DEVICE_PATH "/dev/r2fs-ram0"
#define R2FS_RTEMS_ITEST_MOUNT_PATH "/mnt/r2fs-itest"
#define R2FS_RTEMS_ITEST_PATH_MAX 512
#define R2FS_RTEMS_ITEST_MAX_DIRENTS 256
#define R2FS_RTEMS_MOUNT_FIXTURE_INITIALIZER { NULL }

typedef struct R2fsRtemsMountFixture
{
    struct R2fsRtemsMountFixtureState *state;
} R2fsRtemsMountFixture;

typedef struct R2fsRtemsMountDirEntries
{
    struct dirent entries[R2FS_RTEMS_ITEST_MAX_DIRENTS];
    size_t count;
} R2fsRtemsMountDirEntries;

int r2fsRtemsMountFixtureFormatAndMount(
    R2fsRtemsMountFixture *fixture,
    uint64_t lpa_count
);

int r2fsRtemsMountFixtureRemount(
    R2fsRtemsMountFixture *fixture
);

void r2fsRtemsMountFixtureUnmount(
    R2fsRtemsMountFixture *fixture
);

void r2fsRtemsMountFixtureDestroy(
    R2fsRtemsMountFixture *fixture
);

void r2fsRtemsMountFixtureCleanupActive(void);

const R2fsMkfsLayout *r2fsRtemsMountFixtureLayout(
    const R2fsRtemsMountFixture *fixture
);

int r2fsRtemsMountFixtureMakePath(
    char *buffer,
    size_t buffer_size,
    const char *relative_path
);

int r2fsRtemsMountFixtureStatRoot(
    struct stat *st
);

int r2fsRtemsMountFixtureStatvfsRoot(
    struct statvfs *stvfs
);

int r2fsRtemsMountFixtureStatPath(
    const char *relative_path,
    struct stat *st
);

int r2fsRtemsMountFixtureStatvfsPath(
    const char *relative_path,
    struct statvfs *stvfs
);

int r2fsRtemsMountFixtureMkdir(
    const char *relative_path,
    mode_t mode
);

int r2fsRtemsMountFixtureCreateFile(
    const char *relative_path,
    mode_t mode
);

int r2fsRtemsMountFixtureOpen(
    const char *relative_path,
    int flags,
    mode_t mode,
    int *out_fd
);

int r2fsRtemsMountFixtureClose(
    int fd
);

ssize_t r2fsRtemsMountFixtureWrite(
    int fd,
    const void *data,
    size_t size
);

ssize_t r2fsRtemsMountFixtureRead(
    int fd,
    void *buffer,
    size_t size
);

off_t r2fsRtemsMountFixtureLseek(
    int fd,
    off_t offset,
    int whence
);

int r2fsRtemsMountFixtureFtruncate(
    int fd,
    off_t length
);

int r2fsRtemsMountFixtureFdatasync(
    int fd
);

int r2fsRtemsMountFixtureWriteFile(
    const char *relative_path,
    const void *data,
    size_t size
);

ssize_t r2fsRtemsMountFixtureReadFile(
    const char *relative_path,
    void *buffer,
    size_t size
);

int r2fsRtemsMountFixtureRename(
    const char *old_relative_path,
    const char *new_relative_path
);

int r2fsRtemsMountFixtureUnlink(
    const char *relative_path
);

int r2fsRtemsMountFixtureRemove(
    const char *relative_path
);

int r2fsRtemsMountFixtureSymlink(
    const char *target,
    const char *link_relative_path
);

int r2fsRtemsMountFixtureChown(
    const char *relative_path,
    uid_t owner,
    gid_t group
);

int r2fsRtemsMountFixtureRmdir(
    const char *relative_path
);

int r2fsRtemsMountFixtureReadDir(
    const char *relative_path,
    R2fsRtemsMountDirEntries *out_entries
);
