#pragma once

#include "communication/dev.h"
#include "dir_inode/dir_inode.h"
#include "dir_inode/dir_handler.h"
#include "file_inode/file_handler.h"
#include "fs/fs.h"
#include "fs/fs_handler.h"
#include "fs/rtfs_mkfs.h"
#include "inode/inode.h"
#include "utils/io_utils.h"

#include <dirent.h>
#include <rtems/fs.h>
#include <rtems/libio_.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define RTFS_ITEST_DISK_LPA_COUNT (64U * BLOCK_PER_SEGMENT)
#define RTFS_ITEST_MAX_PATH_COMPONENTS 16
#define RTFS_ITEST_MAX_PATH_LEN 256
#define RTFS_ITEST_MAX_DIRENTS 64

typedef struct RtfsIntegrationBlockStore
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
} RtfsIntegrationBlockStore;

/* 对测试代码公开的句柄类型。真正的运行态细节保持不透明，避免用例耦合内部实现。 */
struct RtfsIntegrationFixtureState;

typedef struct RtfsIntegrationFixture
{
    /* 由 FormatAndMount 创建、由 Destroy 释放的私有运行态。 */
    struct RtfsIntegrationFixtureState *state;
} RtfsIntegrationFixture;

int rtfsIntegrationFixtureFormatAndMount(
    RtfsIntegrationFixture *fixture,
    uint64_t lpa_count);

int rtfsIntegrationFixtureRemount(
    RtfsIntegrationFixture *fixture);

void rtfsIntegrationFixtureUnmount(
    RtfsIntegrationFixture *fixture);

void rtfsIntegrationFixtureCrash(
    RtfsIntegrationFixture *fixture);

void rtfsIntegrationFixtureDestroy(
    RtfsIntegrationFixture *fixture);

void rtfsIntegrationFixtureCleanupActive(void);

const RtfsIntegrationBlockStore *rtfsIntegrationFixtureBlockStore(
    const RtfsIntegrationFixture *fixture);

int rtfsIntegrationFixtureSetFailLpa(
    RtfsIntegrationFixture *fixture,
    uint32_t lpa);

int rtfsIntegrationFixtureSetFailReadLpa(
    RtfsIntegrationFixture *fixture,
    uint32_t lpa);

int rtfsIntegrationFixtureSetFailWriteLpa(
    RtfsIntegrationFixture *fixture,
    uint32_t lpa);

int rtfsIntegrationFixtureFailNextWrite(
    RtfsIntegrationFixture *fixture);

int rtfsIntegrationFixtureFailNextDataWrite(
    RtfsIntegrationFixture *fixture);

int rtfsIntegrationFixtureSetStopAfterMetaWrites(
    RtfsIntegrationFixture *fixture,
    uint32_t limit);

int rtfsIntegrationFixtureCorruptLatestJournalEndEntry(
    RtfsIntegrationFixture *fixture);

int rtfsIntegrationFixtureClearFaults(
    RtfsIntegrationFixture *fixture);

bool rtfsIntegrationFixtureMetaWriteLimitHit(
    const RtfsIntegrationFixture *fixture);

int rtfsIntegrationFlushMetadataToStore(
    RtfsIntegrationFixture *fixture);

int rtfsIntegrationStatPath(
    RtfsIntegrationFixture *fixture,
    const char *path,
    struct stat *st);

int rtfsIntegrationStatvfsRoot(
    RtfsIntegrationFixture *fixture,
    struct statvfs *stvfs);

int rtfsIntegrationReadDir(
    RtfsIntegrationFixture *fixture,
    const char *path,
    struct dirent *entries,
    size_t capacity,
    size_t *out_count);

int rtfsIntegrationMkdir(
    RtfsIntegrationFixture *fixture,
    const char *path,
    mode_t mode);

int rtfsIntegrationCreateFile(
    RtfsIntegrationFixture *fixture,
    const char *path,
    mode_t mode);

int rtfsIntegrationRename(
    RtfsIntegrationFixture *fixture,
    const char *old_path,
    const char *new_path);

int rtfsIntegrationRemove(
    RtfsIntegrationFixture *fixture,
    const char *path);

int rtfsIntegrationWriteFile(
    RtfsIntegrationFixture *fixture,
    const char *path,
    const void *data,
    size_t size);

ssize_t rtfsIntegrationReadFile(
    RtfsIntegrationFixture *fixture,
    const char *path,
    void *buffer,
    size_t size);

int rtfsIntegrationWriteAt(
    RtfsIntegrationFixture *fixture,
    const char *path,
    off_t offset,
    const void *data,
    size_t size);

ssize_t rtfsIntegrationReadAt(
    RtfsIntegrationFixture *fixture,
    const char *path,
    off_t offset,
    void *buffer,
    size_t size);

int rtfsIntegrationReadCurrentFileMapping(
    RtfsIntegrationFixture *fixture,
    const char *path,
    uint32_t *out_ino,
    uint32_t *out_inode_lpa,
    uint32_t *out_first_data_lpa);

bool rtfsIntegrationBlockStoreIsZeroed(
    const RtfsIntegrationBlockStore *store,
    uint32_t lpa);
