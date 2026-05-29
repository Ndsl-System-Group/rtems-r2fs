#pragma once

#include "communication/dev.h"
#include "dir_inode/dir_inode.h"
#include "dir_inode/dir_handler.h"
#include "file_inode/file_handler.h"
#include "fs/fs.h"
#include "fs/fs_handler.h"
#include "fs/r2fs_mkfs.h"
#include "inode/inode.h"
#include "utils/io_utils.h"

#include <dirent.h>
#include <rtems/fs.h>
#include <rtems/libio_.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define R2FS_ITEST_DISK_LPA_COUNT (64U * BLOCK_PER_SEGMENT)
#define R2FS_ITEST_MAX_PATH_COMPONENTS 16
#define R2FS_ITEST_MAX_PATH_LEN 256
#define R2FS_ITEST_MAX_DIRENTS 64

typedef struct R2fsIntegrationBlockStore
{
    /* 用连续的 4KB block 内存模拟测试磁盘镜像。 */
    unsigned char *bytes;
    /* 当前这块测试磁盘一共提供多少个逻辑页。 */
    uint64_t lpa_count;
    /* 通过 comm hook 发生的块级 I/O 计数，便于测试观察实际读写次数。 */
    uint32_t sync_read_count;
    uint32_t sync_write_count;
    uint32_t async_write_count;
    /* 按 LPA 精确注入故障，用来指定某个块读写失败。 */
    uint32_t fail_lpa;
    uint32_t fail_read_lpa;
    uint32_t fail_write_lpa;
    /* 倒计时式故障注入，用来表达“下一次写失败”这类场景。 */
    uint32_t fail_next_write_countdown;
    uint32_t fail_next_data_write_countdown;
    /* 恢复测试需要制造“部分元数据已落盘”的中间态，因此这里记录元数据写预算。 */
    uint32_t stop_after_meta_writes;
    uint32_t meta_write_count;
    bool meta_write_limit_hit;
} R2fsIntegrationBlockStore;

/* 对测试代码公开的句柄类型。真正的运行态细节保持不透明，避免用例耦合内部实现。 */
struct R2fsIntegrationFixtureState;

typedef struct R2fsIntegrationFixture
{
    /* 由 FormatAndMount 创建、由 Destroy 释放的私有运行态。 */
    struct R2fsIntegrationFixtureState *state;
} R2fsIntegrationFixture;

int r2fsIntegrationFixtureFormatAndMount(
    R2fsIntegrationFixture *fixture,
    uint64_t lpa_count
);

int r2fsIntegrationFixtureRemount(
    R2fsIntegrationFixture *fixture
);

void r2fsIntegrationFixtureUnmount(
    R2fsIntegrationFixture *fixture
);

void r2fsIntegrationFixtureCrash(
    R2fsIntegrationFixture *fixture
);

void r2fsIntegrationFixtureDestroy(
    R2fsIntegrationFixture *fixture
);

void r2fsIntegrationFixtureCleanupActive(void);

const R2fsIntegrationBlockStore *r2fsIntegrationFixtureBlockStore(
    const R2fsIntegrationFixture *fixture
);

int r2fsIntegrationFixtureSetFailLpa(
    R2fsIntegrationFixture *fixture,
    uint32_t lpa
);

int r2fsIntegrationFixtureSetFailReadLpa(
    R2fsIntegrationFixture *fixture,
    uint32_t lpa
);

int r2fsIntegrationFixtureSetFailWriteLpa(
    R2fsIntegrationFixture *fixture,
    uint32_t lpa
);

int r2fsIntegrationFixtureFailNextWrite(
    R2fsIntegrationFixture *fixture
);

int r2fsIntegrationFixtureFailNextDataWrite(
    R2fsIntegrationFixture *fixture
);

int r2fsIntegrationFixtureSetStopAfterMetaWrites(
    R2fsIntegrationFixture *fixture,
    uint32_t limit
);

int r2fsIntegrationFixtureClearFaults(
    R2fsIntegrationFixture *fixture
);

bool r2fsIntegrationFixtureMetaWriteLimitHit(
    const R2fsIntegrationFixture *fixture
);

int r2fsIntegrationFlushMetadataToStore(
    R2fsIntegrationFixture *fixture
);

int r2fsIntegrationStatPath(
    R2fsIntegrationFixture *fixture,
    const char *path,
    struct stat *st
);

int r2fsIntegrationStatvfsRoot(
    R2fsIntegrationFixture *fixture,
    struct statvfs *stvfs
);

int r2fsIntegrationReadDir(
    R2fsIntegrationFixture *fixture,
    const char *path,
    struct dirent *entries,
    size_t capacity,
    size_t *out_count
);

int r2fsIntegrationMkdir(
    R2fsIntegrationFixture *fixture,
    const char *path,
    mode_t mode
);

int r2fsIntegrationCreateFile(
    R2fsIntegrationFixture *fixture,
    const char *path,
    mode_t mode
);

int r2fsIntegrationRename(
    R2fsIntegrationFixture *fixture,
    const char *old_path,
    const char *new_path
);

int r2fsIntegrationRemove(
    R2fsIntegrationFixture *fixture,
    const char *path
);

int r2fsIntegrationWriteFile(
    R2fsIntegrationFixture *fixture,
    const char *path,
    const void *data,
    size_t size
);

ssize_t r2fsIntegrationReadFile(
    R2fsIntegrationFixture *fixture,
    const char *path,
    void *buffer,
    size_t size
);

int r2fsIntegrationWriteAt(
    R2fsIntegrationFixture *fixture,
    const char *path,
    off_t offset,
    const void *data,
    size_t size
);

ssize_t r2fsIntegrationReadAt(
    R2fsIntegrationFixture *fixture,
    const char *path,
    off_t offset,
    void *buffer,
    size_t size
);

int r2fsIntegrationReadCurrentFileMapping(
    R2fsIntegrationFixture *fixture,
    const char *path,
    uint32_t *out_ino,
    uint32_t *out_inode_lpa,
    uint32_t *out_first_data_lpa
);

bool r2fsIntegrationBlockStoreIsZeroed(
    const R2fsIntegrationBlockStore *store,
    uint32_t lpa
);
