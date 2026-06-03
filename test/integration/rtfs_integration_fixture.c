#include "integration/rtfs_integration_fixture.h"

#include "communication/comm_api.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "dir_inode/dir_inode_resolver.h"
#include "fs/fs_manager.h"
#include "fs/srmap_utils.h"
#include "inode/inode.h"
#include "journal/journal_type.h"
#include "rtfs_test.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#define RTFS_ITEST_BLOCK_SIZE 4096U

typedef struct RtfsIntegrationFixtureState
{
    /* 模拟出来的盘面，以及块级故障注入的全部控制状态。 */
    RtfsIntegrationBlockStore store;
    /* 提供给 comm_dev 使用的最小 RTEMS 磁盘对象。 */
    rtems_disk_device disk;
    /* 文件系统运行时实际使用的 communication 层设备包装。 */
    comm_dev dev;
    /* mkfs 产出的布局边界，用来区分某个 LPA 属于 metadata 还是 data。 */
    RtfsMkfsLayout layout;
    /* 一次挂载实例对应的内存态挂载对象。 */
    rtems_filesystem_mount_table_entry_t mt_entry;
    rtems_filesystem_global_location_t root_gloc;
    /* 当前这组挂载对象是否处于有效 mounted 状态。 */
    bool mounted;
} RtfsIntegrationFixtureState;

static RtfsIntegrationFixtureState *g_rtfs_integration_fixture_state = NULL;

static RtfsIntegrationFixtureState *rtfsIntegrationFixtureState(
    const RtfsIntegrationFixture *fixture)
{
    return fixture != NULL ? fixture->state : NULL;
}

static int rtfsIntegrationMkfsWriteBlock(
    void *ctx,
    uint32_t lpa,
    const void *block)
{
    RtfsIntegrationBlockStore *store = (RtfsIntegrationBlockStore *)ctx;

    if (store == NULL || block == NULL || lpa >= store->lpa_count)
    {
        return EINVAL;
    }

    memcpy(store->bytes + (uint64_t)lpa * RTFS_ITEST_BLOCK_SIZE, block, RTFS_ITEST_BLOCK_SIZE);
    return 0;
}

static void rtfsIntegrationBlockStoreInit(
    RtfsIntegrationBlockStore *store,
    uint64_t lpa_count)
{
    memset(store, 0, sizeof(*store));
    store->lpa_count = lpa_count;
    store->fail_lpa = UINT32_MAX;
    store->fail_read_lpa = UINT32_MAX;
    store->fail_write_lpa = UINT32_MAX;
    store->bytes = (unsigned char *)calloc((size_t)lpa_count, RTFS_ITEST_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(store->bytes);
}

static void rtfsIntegrationBlockStoreDestroy(RtfsIntegrationBlockStore *store)
{
    free(store->bytes);
    memset(store, 0, sizeof(*store));
}

static void *rtfsIntegrationBlockPtr(
    RtfsIntegrationBlockStore *store,
    uint32_t lpa)
{
    TEST_ASSERT_TRUE((uint64_t)lpa < store->lpa_count);
    return store->bytes + (uint64_t)lpa * RTFS_ITEST_BLOCK_SIZE;
}

static bool rtfsIntegrationIsMetaLpa(
    const RtfsIntegrationFixtureState *fixture,
    uint32_t lpa)
{
    return fixture != NULL && lpa < fixture->layout.main_start_lpa;
}

static int rtfsIntegrationTestSyncRwHook(
    struct comm_dev *dev,
    void *buffer,
    uint64_t lba,
    uint32_t lbaCount,
    comm_io_direction dir)
{
    RtfsIntegrationFixtureState *fixture = g_rtfs_integration_fixture_state;
    uint32_t lpa;
    uint32_t block_count;
    uint32_t i;

    (void)dev;

    if (fixture == NULL || buffer == NULL ||
        lbaCount == 0 || (lba % LBA_PER_LPA) != 0 ||
        (lbaCount % LBA_PER_LPA) != 0)
    {
        return EINVAL;
    }

    lpa = (uint32_t)(lba / LBA_PER_LPA);
    block_count = lbaCount / LBA_PER_LPA;
    if ((uint64_t)lpa + block_count > fixture->store.lpa_count)
    {
        return EINVAL;
    }

    if (dir == COMM_IO_READ)
    {
        for (i = 0; i < block_count; ++i)
        {
            if (lpa + i == fixture->store.fail_lpa ||
                lpa + i == fixture->store.fail_read_lpa)
            {
                return EIO;
            }
        }

        fixture->store.sync_read_count += block_count;
        for (i = 0; i < block_count; ++i)
        {
            memcpy(
                (char *)buffer + (size_t)i * RTFS_ITEST_BLOCK_SIZE,
                rtfsIntegrationBlockPtr(&fixture->store, lpa + i),
                RTFS_ITEST_BLOCK_SIZE
            );
        }
        return 0;
    }

    if (dir != COMM_IO_WRITE)
    {
        return EINVAL;
    }

    for (i = 0; i < block_count; ++i)
    {
        uint32_t cur_lpa = lpa + i;

        if (cur_lpa == fixture->store.fail_lpa ||
            cur_lpa == fixture->store.fail_write_lpa)
        {
            return EIO;
        }

        if (fixture->store.fail_next_write_countdown == 1u)
        {
            fixture->store.fail_next_write_countdown = 0;
            return EIO;
        }
        if (fixture->store.fail_next_write_countdown > 1u)
        {
            fixture->store.fail_next_write_countdown--;
        }

        if (!rtfsIntegrationIsMetaLpa(fixture, cur_lpa))
        {
            if (fixture->store.fail_next_data_write_countdown == 1u)
            {
                fixture->store.fail_next_data_write_countdown = 0;
                return EIO;
            }
            if (fixture->store.fail_next_data_write_countdown > 1u)
            {
                fixture->store.fail_next_data_write_countdown--;
            }
        }

        if (fixture->store.stop_after_meta_writes != 0 &&
            rtfsIntegrationIsMetaLpa(fixture, cur_lpa))
        {
            if (fixture->store.meta_write_count + 1u >
                fixture->store.stop_after_meta_writes)
            {
                fixture->store.meta_write_limit_hit = true;
                return EIO;
            }
        }
    }

    fixture->store.sync_write_count += block_count;
    for (i = 0; i < block_count; ++i)
    {
        uint32_t cur_lpa = lpa + i;

        if (fixture->store.stop_after_meta_writes != 0 &&
            rtfsIntegrationIsMetaLpa(fixture, cur_lpa))
        {
            fixture->store.meta_write_count++;
        }

        memcpy(
            rtfsIntegrationBlockPtr(&fixture->store, cur_lpa),
            (const char *)buffer + (size_t)i * RTFS_ITEST_BLOCK_SIZE,
            RTFS_ITEST_BLOCK_SIZE
        );
    }
    return 0;
}

static int rtfsIntegrationTestAsyncRwHook(
    struct comm_dev *dev,
    void *buffer,
    uint64_t lba,
    uint32_t lbaCount,
    comm_async_cb_func cbFunc,
    void *cbArg,
    comm_io_direction dir)
{
    RtfsIntegrationFixtureState *fixture = g_rtfs_integration_fixture_state;
    int ret;

    ret = rtfsIntegrationTestSyncRwHook(dev, buffer, lba, lbaCount, dir);
    if (ret == 0 && fixture != NULL && dir == COMM_IO_WRITE)
    {
        fixture->store.async_write_count += lbaCount / LBA_PER_LPA;
    }

    if (cbFunc != NULL)
    {
        cbFunc(ret == 0 ? COMM_CMD_SUCCESS : COMM_CMD_CQE_ERROR, cbArg);
    }

    return ret;
}

static int rtfsIntegrationTestGetMetaJournalHeadHook(
    struct comm_dev *dev,
    uint64_t *headLpa)
{
    if (dev == NULL || headLpa == NULL)
    {
        return EINVAL;
    }

    *headLpa = dev->metaJournalHeadLpa;
    return 0;
}

static int rtfsIntegrationTestUpdateMetaJournalTailHook(
    struct comm_dev *dev,
    uint64_t originLpa,
    uint32_t writeBlockNum)
{
    uint64_t journal_size;
    uint64_t offset;
    uint64_t new_offset;

    if (dev == NULL)
    {
        return EINVAL;
    }

    if (originLpa != dev->metaJournalTailLpa)
    {
        return EINVAL;
    }

    journal_size = dev->metaJournalEndLpa - dev->metaJournalStartLpa;
    if (journal_size == 0)
    {
        return EINVAL;
    }

    offset = originLpa - dev->metaJournalStartLpa;
    new_offset = (offset + writeBlockNum) % journal_size;
    dev->metaJournalTailLpa = dev->metaJournalStartLpa + new_offset;
    dev->metaJournalHeadLpa = dev->metaJournalTailLpa;
    return 0;
}

static void rtfsIntegrationInstallHooks(RtfsIntegrationFixtureState *fixture)
{
    g_rtfs_integration_fixture_state = fixture;
    commSetTestSyncRwHook(rtfsIntegrationTestSyncRwHook);
    commSetTestAsyncRwHook(rtfsIntegrationTestAsyncRwHook);
    commSetTestGetMetaJournalHeadHook(rtfsIntegrationTestGetMetaJournalHeadHook);
    commSetTestUpdateMetaJournalTailHook(rtfsIntegrationTestUpdateMetaJournalTailHook);
}

static void rtfsIntegrationResetHooks(void)
{
    commSetTestSyncRwHook(NULL);
    commSetTestAsyncRwHook(NULL);
    commSetTestGetMetaJournalHeadHook(NULL);
    commSetTestUpdateMetaJournalTailHook(NULL);
    g_rtfs_integration_fixture_state = NULL;
}

static void rtfsIntegrationFixtureSetActive(
    RtfsIntegrationFixtureState *fixture)
{
    g_rtfs_integration_fixture_state = fixture;
}

static void rtfsIntegrationFixtureClearActiveIfMatches(
    RtfsIntegrationFixtureState *fixture)
{
    if (fixture != NULL && g_rtfs_integration_fixture_state == fixture)
    {
        g_rtfs_integration_fixture_state = NULL;
    }
}

static void rtfsIntegrationFixtureDestroyState(
    RtfsIntegrationFixtureState *fixture)
{
    if (fixture == NULL)
    {
        return;
    }

    if (fixture->mounted)
    {
        if (fixture->mt_entry.mt_fs_root != NULL)
        {
            if (fixture->mt_entry.mt_fs_root->location.node_access != NULL)
            {
                rtfsFsHandler.freenod_h(&fixture->mt_entry.mt_fs_root->location);
            }
            fixture->mt_entry.mt_fs_root->location.node_access = NULL;
            fixture->mt_entry.mt_fs_root->location.node_access_2 = NULL;
            fixture->mt_entry.mt_fs_root->location.handlers = NULL;
        }
        fixture->mt_entry.fs_info = NULL;
        fileSystemManagerFini();
        if (fixture->dev.diskDevice != NULL)
        {
            commDevDestroy(&fixture->dev);
        }
        fixture->mounted = false;
    }
    rtfsIntegrationResetHooks();
    rtfsIntegrationBlockStoreDestroy(&fixture->store);
    free(fixture);
}

static void rtfsIntegrationFixtureInitMount(
    RtfsIntegrationFixtureState *fixture)
{
    memset(&fixture->mt_entry, 0, sizeof(fixture->mt_entry));
    memset(&fixture->root_gloc, 0, sizeof(fixture->root_gloc));
    memset(&fixture->mt_entry, 0, sizeof(fixture->mt_entry));
    memset(&fixture->root_gloc, 0, sizeof(fixture->root_gloc));
    fixture->mt_entry.mt_fs_root = &fixture->root_gloc;
    fixture->root_gloc.location.mt_entry = &fixture->mt_entry;
}

static int rtfsIntegrationFlushSitNatCacheToStore(
    SitNatCache *cache,
    RtfsIntegrationFixtureState *fixture)
{
    khiter_t k;

    if (cache == NULL || fixture == NULL)
    {
        return EINVAL;
    }

    for (k = kh_begin(cache->cacheManager.index.index);
         k != kh_end(cache->cacheManager.index.index);
         ++k)
    {
        SitNatCacheEntry *entry;

        if (!kh_exist(cache->cacheManager.index.index, k))
        {
            continue;
        }

        entry = (SitNatCacheEntry *)kh_val(cache->cacheManager.index.index, k);
        if (entry == NULL)
        {
            continue;
        }

        if ((uint64_t)entry->lpa >= fixture->store.lpa_count)
        {
            return EINVAL;
        }

        memcpy(
            rtfsIntegrationBlockPtr(&fixture->store, entry->lpa),
            blockBufferGetPtr(&entry->cache),
            RTFS_ITEST_BLOCK_SIZE);
    }

    return 0;
}

static int rtfsIntegrationFlushSrmapToStore(
    SrmapUtils *srmap_utils,
    RtfsIntegrationFixtureState *fixture)
{
    khiter_t k;

    if (srmap_utils == NULL || fixture == NULL)
    {
        return EINVAL;
    }

    for (k = kh_begin(srmap_utils->srmapCache);
         k != kh_end(srmap_utils->srmapCache);
         ++k)
    {
        uint32_t lpa;
        BlockBuffer *blk;

        if (!kh_exist(srmap_utils->srmapCache, k))
        {
            continue;
        }

        lpa = kh_key(srmap_utils->srmapCache, k);
        blk = &kh_value(srmap_utils->srmapCache, k);
        if ((uint64_t)lpa >= fixture->store.lpa_count)
        {
            return EINVAL;
        }

        memcpy(
            rtfsIntegrationBlockPtr(&fixture->store, lpa),
            blockBufferGetPtr(blk),
            RTFS_ITEST_BLOCK_SIZE);
    }

    return 0;
}

static int rtfsIntegrationFixtureMount(
    RtfsIntegrationFixtureState *fixture)
{
    int ret;

    if (fixture == NULL)
    {
        return EINVAL;
    }

    rtfsIntegrationFixtureInitMount(fixture);
    ret = commDevInit(
        &fixture->dev,
        &fixture->disk,
        512,
        fixture->store.lpa_count * LBA_PER_LPA,
        fixture->layout.meta_journal_start_lpa,
        fixture->layout.meta_journal_start_lpa +
            (uint64_t)fixture->layout.meta_journal_segment_count * BLOCK_PER_SEGMENT);
    if (ret != 0)
    {
        return ret;
    }

    if (rtfsInitialize(&fixture->mt_entry, &fixture->dev) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        commDevDestroy(&fixture->dev);
        return ret;
    }

    fixture->mounted = true;
    return 0;
}

static int rtfsIntegrationSplitParent(
    const char *path,
    char *parent_buf,
    size_t parent_buf_size,
    const char **out_leaf)
{
    const char *last_slash;
    size_t parent_len;

    if (path == NULL || parent_buf == NULL || out_leaf == NULL || path[0] != '/')
    {
        return EINVAL;
    }

    last_slash = strrchr(path, '/');
    if (last_slash == NULL || last_slash[1] == '\0')
    {
        return EINVAL;
    }

    parent_len = (size_t)(last_slash - path);
    if (parent_len == 0)
    {
        if (parent_buf_size < 2)
        {
            return ENAMETOOLONG;
        }
        parent_buf[0] = '/';
        parent_buf[1] = '\0';
    }
    else
    {
        if (parent_len + 1 > parent_buf_size)
        {
            return ENAMETOOLONG;
        }
        memcpy(parent_buf, path, parent_len);
        parent_buf[parent_len] = '\0';
    }

    *out_leaf = last_slash + 1;
    return 0;
}

static int rtfsIntegrationLookup(
    RtfsIntegrationFixture *fixture,
    const char *path,
    rtems_filesystem_location_info_t *out_loc,
    RtfsRuntimeInodeView **out_view)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);
    char tmp[RTFS_ITEST_MAX_PATH_LEN];
    char *saveptr = NULL;
    char *token;
    rtems_filesystem_location_info_t current;
    RtfsRuntimeInodeView *current_view;

    if (state == NULL || path == NULL || out_loc == NULL || path[0] != '/')
    {
        return EINVAL;
    }

    memset(out_loc, 0, sizeof(*out_loc));

    if (strcmp(path, "/") == 0)
    {
        current = state->root_gloc.location;
        if (rtfsFsHandler.clonenod_h(&current) != 0)
        {
            return errno;
        }
        *out_loc = current;
        if (out_view != NULL)
        {
            *out_view = (RtfsRuntimeInodeView *)out_loc->node_access;
        }
        return 0;
    }

    if (strlen(path) >= sizeof(tmp))
    {
        return ENAMETOOLONG;
    }

    current = state->root_gloc.location;
    if (rtfsFsHandler.clonenod_h(&current) != 0)
    {
        return errno;
    }

    memcpy(tmp, path + 1, strlen(path));
    tmp[strlen(path) - 1] = '\0';

    token = strtok_r(tmp, "/", &saveptr);
    while (token != NULL)
    {
        RtfsDirLookupResult result;
        int ret;
        rtems_filesystem_location_info_t next = current;

        current_view = (RtfsRuntimeInodeView *)current.node_access;
        if (current_view == NULL || !rtfsInodeIsDirectoryType(current_view->file_type))
        {
            rtfsFsHandler.freenod_h(&current);
            return ENOTDIR;
        }

        ret = rtfsFsHandler.clonenod_h(&next);
        if (ret != 0)
        {
            rtfsFsHandler.freenod_h(&current);
            return errno;
        }

        {
            RtfsDirInode *dir_inode;
            RtfsDirInodeBuildRequest request = {
                .ino = current_view->ino,
                .mode = RTFS_DIR_BUILD_ON_DEMAND};
            file_system_manager *fs_manager = (file_system_manager *)current.mt_entry->fs_info;

            ret = rtfsDirInodeResolve(fs_manager, NULL, &request, &dir_inode);
            if (ret != 0)
            {
                rtfsFsHandler.freenod_h(&next);
                rtfsFsHandler.freenod_h(&current);
                return ret;
            }

            do
            {
                ret = rtfsDirInodeLookup(dir_inode, token, strlen(token), &result);
                if (ret != ENOENT || rtfsDirInodeIsFullyLoaded(dir_inode))
                {
                    break;
                }
                ret = rtfsDirInodeResolveNext(fs_manager, current_view->ino, dir_inode);
                if (ret != 0)
                {
                    rtfsDirInodePut(dir_inode);
                    rtfsFsHandler.freenod_h(&next);
                    rtfsFsHandler.freenod_h(&current);
                    return ret;
                }
            } while (true);

            rtfsDirInodePut(dir_inode);
            if (ret != 0)
            {
                rtfsFsHandler.freenod_h(&next);
                rtfsFsHandler.freenod_h(&current);
                return ret;
            }
        }

        next.node_access = NULL;
        next.node_access_2 = NULL;

        next.node_access = rtfsRuntimeInodeViewClone(&result.inode_view);
        if (next.node_access == NULL)
        {
            rtfsFsHandler.freenod_h(&current);
            return ENOMEM;
        }

        if (token[0] != '\0')
        {
            next.node_access_2 = strdup(token);
            if (next.node_access_2 == NULL)
            {
                rtfsFsHandler.freenod_h(&next);
                rtfsFsHandler.freenod_h(&current);
                return ENOMEM;
            }
        }

        if (rtfsInodeIsDirectoryType(result.inode_view.file_type))
        {
            next.handlers = &rtfsDirhandlers;
        }
        else
        {
            next.handlers = &rtfsFilehandlers;
        }

        rtfsFsHandler.freenod_h(&current);
        current = next;
        token = strtok_r(NULL, "/", &saveptr);
    }

    *out_loc = current;
    if (out_view != NULL)
    {
        *out_view = (RtfsRuntimeInodeView *)out_loc->node_access;
    }
    return 0;
}

static int rtfsIntegrationLookupParent(
    RtfsIntegrationFixture *fixture,
    const char *path,
    rtems_filesystem_location_info_t *out_parent,
    const char **out_leaf)
{
    char parent_buf[RTFS_ITEST_MAX_PATH_LEN];
    int ret;

    ret = rtfsIntegrationSplitParent(path, parent_buf, sizeof(parent_buf), out_leaf);
    if (ret != 0)
    {
        return ret;
    }

    return rtfsIntegrationLookup(fixture, parent_buf, out_parent, NULL);
}

int rtfsIntegrationFixtureFormatAndMount(
    RtfsIntegrationFixture *fixture,
    uint64_t lpa_count)
{
    RtfsIntegrationFixtureState *state;
    RtfsMkfsOptions options;
    int ret;

    if (fixture == NULL)
    {
        return EINVAL;
    }

    fixture->state = NULL;
    state = (RtfsIntegrationFixtureState *)calloc(1, sizeof(*state));
    if (state == NULL)
    {
        return ENOMEM;
    }

    fixture->state = state;
    rtfsIntegrationBlockStoreInit(&state->store, lpa_count);
    rtfsIntegrationFixtureInitMount(state);
    rtfsIntegrationFixtureSetActive(state);
    rtfsIntegrationInstallHooks(state);

    memset(&options, 0, sizeof(options));
    options.lpa_count = lpa_count;
    options.root_ino = 1;
    options.meta_journal_segment_count = 1;

    ret = rtfsMkfsFormat(&options, rtfsIntegrationMkfsWriteBlock, &state->store, &state->layout);
    if (ret != 0)
    {
        rtfsIntegrationFixtureDestroy(fixture);
        return ret;
    }

    ret = rtfsIntegrationFixtureMount(state);
    if (ret != 0)
    {
        rtfsIntegrationFixtureDestroy(fixture);
        return ret;
    }

    return 0;
}

int rtfsIntegrationFixtureRemount(
    RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);
    int ret;

    if (state == NULL || state->store.bytes == NULL)
    {
        return EINVAL;
    }

    rtfsIntegrationFixtureUnmount(fixture);
    ret = rtfsIntegrationFixtureMount(state);
    if (ret != 0)
    {
        rtfsIntegrationFixtureDestroy(fixture);
        return ret;
    }

    return 0;
}

void rtfsIntegrationFixtureUnmount(
    RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);
    file_system_manager *fs_manager;

    if (state == NULL || !state->mounted)
    {
        return;
    }

    fs_manager = (file_system_manager *)state->mt_entry.fs_info;
    if (fs_manager != NULL)
    {
        TEST_ASSERT_EQUAL(0, fileSystemManagerFlushForUnmount(fs_manager));
    }

    if (state->mt_entry.mt_fs_root != NULL)
    {
        if (state->mt_entry.mt_fs_root->location.node_access != NULL)
        {
            rtfsFsHandler.freenod_h(&state->mt_entry.mt_fs_root->location);
        }
        state->mt_entry.mt_fs_root->location.node_access = NULL;
        state->mt_entry.mt_fs_root->location.node_access_2 = NULL;
        state->mt_entry.mt_fs_root->location.handlers = NULL;
    }
    state->mt_entry.fs_info = NULL;
    fileSystemManagerFini();
    if (state->dev.diskDevice != NULL)
    {
        commDevDestroy(&state->dev);
    }
    state->mounted = false;
}

void rtfsIntegrationFixtureCrash(
    RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL)
    {
        return;
    }

    if (state->mt_entry.mt_fs_root != NULL &&
        state->mt_entry.mt_fs_root->location.node_access != NULL)
    {
        rtfsFsHandler.freenod_h(&state->mt_entry.mt_fs_root->location);
    }

    state->mt_entry.mt_fs_root = NULL;
    state->mt_entry.fs_info = NULL;
    state->mounted = false;
    fileSystemManagerFini();
    commDevDestroy(&state->dev);
}

void rtfsIntegrationFixtureDestroy(
    RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state;

    if (fixture == NULL)
    {
        return;
    }

    state = fixture->state;
    fixture->state = NULL;
    rtfsIntegrationFixtureDestroyState(state);
}

void rtfsIntegrationFixtureCleanupActive(void)
{
    RtfsIntegrationFixtureState *fixture = g_rtfs_integration_fixture_state;

    rtfsIntegrationFixtureDestroyState(fixture);
}

int rtfsIntegrationStatPath(
    RtfsIntegrationFixture *fixture,
    const char *path,
    struct stat *st)
{
    rtems_filesystem_location_info_t loc;
    int ret;

    ret = rtfsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0)
    {
        return ret;
    }

    if (loc.handlers == NULL || loc.handlers->fstat_h == NULL)
    {
        rtfsFsHandler.freenod_h(&loc);
        return EINVAL;
    }

    if (loc.handlers->fstat_h(&loc, st) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&loc);
        return ret;
    }

    rtfsFsHandler.freenod_h(&loc);
    return 0;
}

int rtfsIntegrationStatvfsRoot(
    RtfsIntegrationFixture *fixture,
    struct statvfs *stvfs)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    if (state == NULL || stvfs == NULL)
    {
        return EINVAL;
    }

    if (rtfsFsHandler.statvfs_h(&state->root_gloc.location, stvfs) != 0)
    {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int rtfsIntegrationReadDir(
    RtfsIntegrationFixture *fixture,
    const char *path,
    struct dirent *entries,
    size_t capacity,
    size_t *out_count)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t bytes_read;
    size_t entry_count;
    int ret;

    if (entries == NULL || out_count == NULL || capacity == 0)
    {
        return EINVAL;
    }

    ret = rtfsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0)
    {
        return ret;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsDirhandlers.open_h(&iop, path, O_RDONLY, 0) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    memset(entries, 0, capacity * sizeof(*entries));
    bytes_read = rtfsDirhandlers.read_h(&iop, entries, capacity * sizeof(*entries));
    if (bytes_read < 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    entry_count = (size_t)bytes_read / sizeof(*entries);
    *out_count = entry_count;
    rtfsFsHandler.freenod_h(&iop.pathinfo);
    return 0;
}

int rtfsIntegrationMkdir(
    RtfsIntegrationFixture *fixture,
    const char *path,
    mode_t mode)
{
    rtems_filesystem_location_info_t parentloc;
    const char *leaf;
    int ret;

    ret = rtfsIntegrationLookupParent(fixture, path, &parentloc, &leaf);
    if (ret != 0)
    {
        return ret;
    }

    if (rtfsFsHandler.mknod_h(&parentloc, leaf, strlen(leaf), S_IFDIR | mode, 0) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&parentloc);
        return ret;
    }

    rtfsFsHandler.freenod_h(&parentloc);
    return 0;
}

int rtfsIntegrationCreateFile(
    RtfsIntegrationFixture *fixture,
    const char *path,
    mode_t mode)
{
    rtems_filesystem_location_info_t parentloc;
    const char *leaf;
    int ret;

    ret = rtfsIntegrationLookupParent(fixture, path, &parentloc, &leaf);
    if (ret != 0)
    {
        return ret;
    }

    if (rtfsFsHandler.mknod_h(&parentloc, leaf, strlen(leaf), S_IFREG | mode, 0) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&parentloc);
        return ret;
    }

    rtfsFsHandler.freenod_h(&parentloc);
    return 0;
}

int rtfsIntegrationRename(
    RtfsIntegrationFixture *fixture,
    const char *old_path,
    const char *new_path)
{
    rtems_filesystem_location_info_t old_parentloc;
    rtems_filesystem_location_info_t old_loc;
    rtems_filesystem_location_info_t new_parentloc;
    const char *old_leaf;
    const char *new_leaf;
    int ret;

    ret = rtfsIntegrationLookupParent(fixture, old_path, &old_parentloc, &old_leaf);
    if (ret != 0)
    {
        return ret;
    }

    ret = rtfsIntegrationLookup(fixture, old_path, &old_loc, NULL);
    if (ret != 0)
    {
        rtfsFsHandler.freenod_h(&old_parentloc);
        return ret;
    }

    ret = rtfsIntegrationLookupParent(fixture, new_path, &new_parentloc, &new_leaf);
    if (ret != 0)
    {
        rtfsFsHandler.freenod_h(&old_loc);
        rtfsFsHandler.freenod_h(&old_parentloc);
        return ret;
    }

    if (rtfsFsHandler.rename_h(
            &old_parentloc,
            &old_loc,
            &new_parentloc,
            new_leaf,
            strlen(new_leaf)) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&new_parentloc);
        rtfsFsHandler.freenod_h(&old_loc);
        rtfsFsHandler.freenod_h(&old_parentloc);
        return ret;
    }

    rtfsFsHandler.freenod_h(&new_parentloc);
    rtfsFsHandler.freenod_h(&old_loc);
    rtfsFsHandler.freenod_h(&old_parentloc);
    return 0;
}

int rtfsIntegrationRemove(
    RtfsIntegrationFixture *fixture,
    const char *path)
{
    rtems_filesystem_location_info_t parentloc;
    rtems_filesystem_location_info_t loc;
    const char *leaf;
    int ret;

    ret = rtfsIntegrationLookupParent(fixture, path, &parentloc, &leaf);
    if (ret != 0)
    {
        return ret;
    }

    ret = rtfsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0)
    {
        rtfsFsHandler.freenod_h(&parentloc);
        return ret;
    }

    if (rtfsFsHandler.rmnod_h(&parentloc, &loc) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&loc);
        rtfsFsHandler.freenod_h(&parentloc);
        return ret;
    }

    rtfsFsHandler.freenod_h(&loc);
    rtfsFsHandler.freenod_h(&parentloc);
    return 0;
}

int rtfsIntegrationWriteFile(
    RtfsIntegrationFixture *fixture,
    const char *path,
    const void *data,
    size_t size)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t written;
    int ret;

    ret = rtfsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0)
    {
        return ret;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsFilehandlers.open_h(&iop, path, O_RDWR, 0) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    written = rtfsFilehandlers.write_h(&iop, data, size);
    if (written < 0 || (size_t)written != size)
    {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.fdatasync_h(&iop) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.close_h(&iop) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    rtfsFsHandler.freenod_h(&iop.pathinfo);
    return 0;
}

ssize_t rtfsIntegrationReadFile(
    RtfsIntegrationFixture *fixture,
    const char *path,
    void *buffer,
    size_t size)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t bytes_read;
    int ret;

    ret = rtfsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0)
    {
        errno = ret;
        return -1;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsFilehandlers.open_h(&iop, path, O_RDONLY, 0) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        errno = ret;
        return -1;
    }

    bytes_read = rtfsFilehandlers.read_h(&iop, buffer, size);
    ret = errno;
    (void)rtfsFilehandlers.close_h(&iop);
    rtfsFsHandler.freenod_h(&iop.pathinfo);
    if (bytes_read < 0)
    {
        errno = ret != 0 ? ret : EIO;
    }
    return bytes_read;
}

int rtfsIntegrationWriteAt(
    RtfsIntegrationFixture *fixture,
    const char *path,
    off_t offset,
    const void *data,
    size_t size)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t written;
    int ret;

    if (data == NULL && size != 0)
    {
        return EINVAL;
    }

    ret = rtfsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0)
    {
        return ret;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsFilehandlers.open_h(&iop, path, O_RDWR, 0) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.lseek_h(&iop, offset, SEEK_SET) < 0)
    {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    written = rtfsFilehandlers.write_h(&iop, data, size);
    if (written < 0 || (size_t)written != size)
    {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.fdatasync_h(&iop) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.close_h(&iop) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    rtfsFsHandler.freenod_h(&iop.pathinfo);
    return 0;
}

ssize_t rtfsIntegrationReadAt(
    RtfsIntegrationFixture *fixture,
    const char *path,
    off_t offset,
    void *buffer,
    size_t size)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t bytes_read;
    int ret;

    if (buffer == NULL && size != 0)
    {
        errno = EINVAL;
        return -1;
    }

    ret = rtfsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0)
    {
        errno = ret;
        return -1;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsFilehandlers.open_h(&iop, path, O_RDONLY, 0) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        errno = ret;
        return -1;
    }

    if (rtfsFilehandlers.lseek_h(&iop, offset, SEEK_SET) < 0)
    {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        rtfsFsHandler.freenod_h(&iop.pathinfo);
        errno = ret;
        return -1;
    }

    bytes_read = rtfsFilehandlers.read_h(&iop, buffer, size);
    ret = errno;
    (void)rtfsFilehandlers.close_h(&iop);
    rtfsFsHandler.freenod_h(&iop.pathinfo);
    if (bytes_read < 0)
    {
        errno = ret != 0 ? ret : EIO;
    }
    return bytes_read;
}

int rtfsIntegrationReadCurrentFileMapping(
    RtfsIntegrationFixture *fixture,
    const char *path,
    uint32_t *out_ino,
    uint32_t *out_inode_lpa,
    uint32_t *out_first_data_lpa)
{
    rtems_filesystem_location_info_t loc;
    RtfsRuntimeInodeView *view = NULL;
    file_system_manager *fs_manager;
    NodeBlockCacheHelper helper;
    NodeBlockCacheEntryHandle inode_handle;
    const struct RtfsNode *inode_node;
    int ret;

    if (fixture == NULL || path == NULL || out_first_data_lpa == NULL)
    {
        return EINVAL;
    }

    ret = rtfsIntegrationLookup(fixture, path, &loc, &view);
    if (ret != 0)
    {
        return ret;
    }

    if (view == NULL || rtfsInodeIsDirectoryType(view->file_type))
    {
        rtfsFsHandler.freenod_h(&loc);
        return EINVAL;
    }

    fs_manager = (file_system_manager *)loc.mt_entry->fs_info;
    if (fs_manager == NULL)
    {
        rtfsFsHandler.freenod_h(&loc);
        return EINVAL;
    }

    nodeBlockCacheHelperInit(&helper, fs_manager);
    ret = nodeBlockCacheHelperGetNodeEntry(
        &helper,
        view->ino,
        view->ino,
        &inode_handle);
    if (ret != 0 || nodeBlockCacheEntryHandleIsEmpty(&inode_handle))
    {
        rtfsFsHandler.freenod_h(&loc);
        return ret != 0 ? ret : ENOENT;
    }

    inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    if (out_ino != NULL)
    {
        *out_ino = view->ino;
    }
    if (out_inode_lpa != NULL)
    {
        *out_inode_lpa = nodeBlockCacheEntryGetLpa(inode_handle.entry);
    }
    *out_first_data_lpa = inode_node->i.i_addr[0];

    nodeBlockCacheEntryHandleDestroy(&inode_handle);
    rtfsFsHandler.freenod_h(&loc);
    return 0;
}

bool rtfsIntegrationBlockStoreIsZeroed(
    const RtfsIntegrationBlockStore *store,
    uint32_t lpa)
{
    const unsigned char *ptr;
    size_t i;

    if (store == NULL || store->bytes == NULL || (uint64_t)lpa >= store->lpa_count)
    {
        return false;
    }

    ptr = store->bytes + (uint64_t)lpa * RTFS_ITEST_BLOCK_SIZE;
    for (i = 0; i < RTFS_ITEST_BLOCK_SIZE; ++i)
    {
        if (ptr[i] != 0)
        {
            return false;
        }
    }

    return true;
}

const RtfsIntegrationBlockStore *rtfsIntegrationFixtureBlockStore(
    const RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    return state != NULL ? &state->store : NULL;
}

int rtfsIntegrationFixtureSetFailLpa(
    RtfsIntegrationFixture *fixture,
    uint32_t lpa)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL)
    {
        return EINVAL;
    }

    if (lpa != UINT32_MAX && (uint64_t)lpa >= state->store.lpa_count)
    {
        return EINVAL;
    }

    state->store.fail_lpa = lpa;
    return 0;
}

int rtfsIntegrationFixtureSetFailReadLpa(
    RtfsIntegrationFixture *fixture,
    uint32_t lpa)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL)
    {
        return EINVAL;
    }

    if (lpa != UINT32_MAX && (uint64_t)lpa >= state->store.lpa_count)
    {
        return EINVAL;
    }

    state->store.fail_read_lpa = lpa;
    return 0;
}

int rtfsIntegrationFixtureSetFailWriteLpa(
    RtfsIntegrationFixture *fixture,
    uint32_t lpa)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL)
    {
        return EINVAL;
    }

    if (lpa != UINT32_MAX && (uint64_t)lpa >= state->store.lpa_count)
    {
        return EINVAL;
    }

    state->store.fail_write_lpa = lpa;
    return 0;
}

int rtfsIntegrationFixtureFailNextWrite(
    RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL)
    {
        return EINVAL;
    }

    state->store.fail_next_write_countdown = 1u;
    return 0;
}

int rtfsIntegrationFixtureFailNextDataWrite(
    RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL)
    {
        return EINVAL;
    }

    state->store.fail_next_data_write_countdown = 1u;
    return 0;
}

int rtfsIntegrationFixtureSetStopAfterMetaWrites(
    RtfsIntegrationFixture *fixture,
    uint32_t limit)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL)
    {
        return EINVAL;
    }

    state->store.stop_after_meta_writes = limit;
    state->store.meta_write_count = 0;
    state->store.meta_write_limit_hit = false;
    return 0;
}

int rtfsIntegrationFixtureCorruptLatestJournalEndEntry(
    RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);
    struct RtfsSuperBlock *super_block;
    uint64_t journal_write_tail_lpa;
    uint64_t journal_start_lpa;
    uint64_t journal_end_lpa;
    uint64_t journal_size;
    uint64_t cur_lpa;
    uint64_t scanned_blocks;
    uint32_t attempt;

    if (state == NULL || state->store.bytes == NULL)
    {
        return EINVAL;
    }

    super_block = (struct RtfsSuperBlock *)rtfsIntegrationBlockPtr(
        &state->store,
        0u);
    if (super_block->magic != RTFS_MAGIC_NUMBER)
    {
        return EIO;
    }

    journal_start_lpa = super_block->meta_journal_blkaddr;
    journal_end_lpa = journal_start_lpa +
                      (uint64_t)super_block->segment_count_meta_journal * BLOCK_PER_SEGMENT;
    journal_size = journal_end_lpa - journal_start_lpa;
    if (journal_size == 0)
    {
        return EIO;
    }

    cur_lpa = journal_start_lpa +
              ((uint64_t)super_block->meta_journal_end_blkoff % journal_size);
    journal_write_tail_lpa = cur_lpa;

    for (attempt = 0; attempt < 10000u; ++attempt)
    {
        if (state->dev.metaJournalTailLpa != journal_write_tail_lpa)
        {
            break;
        }
        sched_yield();
    }
    if (state->dev.metaJournalTailLpa == journal_write_tail_lpa)
    {
        return ETIMEDOUT;
    }

    scanned_blocks = 0;

    while (scanned_blocks < journal_size)
    {
        unsigned char *block = rtfsIntegrationBlockPtr(
            &state->store,
            (uint32_t)cur_lpa);
        size_t off = 0;

        while (off + sizeof(MetaJournalEntry) <= BLOCK_BUFFER_SIZE)
        {
            MetaJournalEntry *entry = (MetaJournalEntry *)(block + off);

            if (entry->len == 0)
            {
                break;
            }
            if (entry->len < sizeof(MetaJournalEntry) ||
                off + entry->len > BLOCK_BUFFER_SIZE)
            {
                return EIO;
            }

            if (entry->type == JOURNAL_TYPE_END &&
                entry->len == sizeof(MetaJournalEntry))
            {
                entry->len = 0;
                entry->type = 0;
                entry->rsv = 0;
                return 0;
            }

            if (entry->type == JOURNAL_TYPE_NOP)
            {
                break;
            }

            off += entry->len;
        }

        cur_lpa++;
        if (cur_lpa >= journal_end_lpa)
        {
            cur_lpa = journal_start_lpa;
        }
        ++scanned_blocks;
    }

    return ENOENT;
}

int rtfsIntegrationFixtureClearFaults(
    RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL)
    {
        return EINVAL;
    }

    state->store.fail_lpa = UINT32_MAX;
    state->store.fail_read_lpa = UINT32_MAX;
    state->store.fail_write_lpa = UINT32_MAX;
    state->store.fail_next_write_countdown = 0;
    state->store.fail_next_data_write_countdown = 0;
    state->store.stop_after_meta_writes = 0;
    state->store.meta_write_count = 0;
    state->store.meta_write_limit_hit = false;
    return 0;
}

bool rtfsIntegrationFixtureMetaWriteLimitHit(
    const RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);

    return state != NULL && state->store.meta_write_limit_hit;
}

int rtfsIntegrationFlushMetadataToStore(
    RtfsIntegrationFixture *fixture)
{
    RtfsIntegrationFixtureState *state = rtfsIntegrationFixtureState(fixture);
    file_system_manager *fs_manager;
    int ret;

    if (state == NULL || !state->mounted)
    {
        return EINVAL;
    }

    fs_manager = (file_system_manager *)state->mt_entry.fs_info;
    if (fs_manager == NULL)
    {
        return EINVAL;
    }

    ret = rtfsIntegrationFlushSitNatCacheToStore(
        fileSystemManagerGetNatCache(fs_manager),
        state);
    if (ret != 0)
    {
        return ret;
    }

    ret = rtfsIntegrationFlushSitNatCacheToStore(
        fileSystemManagerGetSitCache(fs_manager),
        state);
    if (ret != 0)
    {
        return ret;
    }

    return rtfsIntegrationFlushSrmapToStore(
        fileSystemManagerGetSrmapUtils(fs_manager),
        state);
}
