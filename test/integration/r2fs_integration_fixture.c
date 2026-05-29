#include "integration/r2fs_integration_fixture.h"

#include "communication/comm_api.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "dir_inode/dir_inode_resolver.h"
#include "fs/fs_manager.h"
#include "fs/srmap_utils.h"
#include "inode/inode.h"
#include "rtfs_test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#define R2FS_ITEST_BLOCK_SIZE 4096U

typedef struct R2fsIntegrationFixtureState
{
    /* 模拟出来的盘面，以及块级故障注入的全部控制状态。 */
    R2fsIntegrationBlockStore store;
    /* 提供给 comm_dev 使用的最小 RTEMS 磁盘对象。 */
    rtems_disk_device disk;
    /* 文件系统运行时实际使用的 communication 层设备包装。 */
    comm_dev dev;
    /* mkfs 产出的布局边界，用来区分某个 LPA 属于 metadata 还是 data。 */
    R2fsMkfsLayout layout;
    /* 一次挂载实例对应的内存态挂载对象。 */
    rtems_filesystem_mount_table_entry_t mt_entry;
    rtems_filesystem_global_location_t root_gloc;
    /* 当前这组挂载对象是否处于有效 mounted 状态。 */
    bool mounted;
} R2fsIntegrationFixtureState;

static R2fsIntegrationFixtureState *g_r2fs_integration_fixture_state = NULL;

static R2fsIntegrationFixtureState *r2fsIntegrationFixtureState(
    const R2fsIntegrationFixture *fixture
)
{
    return fixture != NULL ? fixture->state : NULL;
}

static int r2fsIntegrationMkfsWriteBlock(
    void *ctx,
    uint32_t lpa,
    const void *block
)
{
    R2fsIntegrationBlockStore *store = (R2fsIntegrationBlockStore *)ctx;

    if (store == NULL || block == NULL || lpa >= store->lpa_count) {
        return EINVAL;
    }

    memcpy(store->bytes + (uint64_t)lpa * R2FS_ITEST_BLOCK_SIZE, block, R2FS_ITEST_BLOCK_SIZE);
    return 0;
}

static void r2fsIntegrationBlockStoreInit(
    R2fsIntegrationBlockStore *store,
    uint64_t lpa_count
)
{
    memset(store, 0, sizeof(*store));
    store->lpa_count = lpa_count;
    store->fail_lpa = UINT32_MAX;
    store->fail_read_lpa = UINT32_MAX;
    store->fail_write_lpa = UINT32_MAX;
    store->bytes = (unsigned char *)calloc((size_t)lpa_count, R2FS_ITEST_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(store->bytes);
}

static void r2fsIntegrationBlockStoreDestroy(R2fsIntegrationBlockStore *store)
{
    free(store->bytes);
    memset(store, 0, sizeof(*store));
}

static void *r2fsIntegrationBlockPtr(
    R2fsIntegrationBlockStore *store,
    uint32_t lpa
)
{
    TEST_ASSERT_TRUE((uint64_t)lpa < store->lpa_count);
    return store->bytes + (uint64_t)lpa * R2FS_ITEST_BLOCK_SIZE;
}

static bool r2fsIntegrationIsMetaLpa(
    const R2fsIntegrationFixtureState *fixture,
    uint32_t lpa
)
{
    return fixture != NULL && lpa < fixture->layout.main_start_lpa;
}

static int r2fsIntegrationTestSyncRwHook(
    struct comm_dev *dev,
    void *buffer,
    uint64_t lba,
    uint32_t lbaCount,
    comm_io_direction dir
)
{
    R2fsIntegrationFixtureState *fixture = g_r2fs_integration_fixture_state;
    uint32_t lpa;

    (void)dev;

    if (fixture == NULL || buffer == NULL || lbaCount != LBA_PER_LPA || (lba % LBA_PER_LPA) != 0) {
        return EINVAL;
    }

    lpa = (uint32_t)(lba / LBA_PER_LPA);
    if ((uint64_t)lpa >= fixture->store.lpa_count) {
        return EINVAL;
    }

    if (lpa == fixture->store.fail_lpa) {
        return EIO;
    }

    if (dir == COMM_IO_READ) {
        if (lpa == fixture->store.fail_read_lpa) {
            return EIO;
        }
        fixture->store.sync_read_count++;
        memcpy(buffer, r2fsIntegrationBlockPtr(&fixture->store, lpa), R2FS_ITEST_BLOCK_SIZE);
        return 0;
    }

    if (dir != COMM_IO_WRITE) {
        return EINVAL;
    }

    if (lpa == fixture->store.fail_write_lpa) {
        return EIO;
    }

    if (fixture->store.fail_next_write_countdown == 1u) {
        fixture->store.fail_next_write_countdown = 0;
        return EIO;
    }
    if (fixture->store.fail_next_write_countdown > 1u) {
        fixture->store.fail_next_write_countdown--;
    }

    if (!r2fsIntegrationIsMetaLpa(fixture, lpa)) {
        if (fixture->store.fail_next_data_write_countdown == 1u) {
            fixture->store.fail_next_data_write_countdown = 0;
            return EIO;
        }
        if (fixture->store.fail_next_data_write_countdown > 1u) {
            fixture->store.fail_next_data_write_countdown--;
        }
    }

    if (fixture->store.stop_after_meta_writes != 0 &&
        r2fsIntegrationIsMetaLpa(fixture, lpa)) {
        fixture->store.meta_write_count++;
        if (fixture->store.meta_write_count > fixture->store.stop_after_meta_writes) {
            fixture->store.meta_write_limit_hit = true;
            return EIO;
        }
    }

    fixture->store.sync_write_count++;
    memcpy(r2fsIntegrationBlockPtr(&fixture->store, lpa), buffer, R2FS_ITEST_BLOCK_SIZE);
    return 0;
}

static int r2fsIntegrationTestAsyncRwHook(
    struct comm_dev *dev,
    void *buffer,
    uint64_t lba,
    uint32_t lbaCount,
    comm_async_cb_func cbFunc,
    void *cbArg,
    comm_io_direction dir
)
{
    R2fsIntegrationFixtureState *fixture = g_r2fs_integration_fixture_state;
    int ret;

    ret = r2fsIntegrationTestSyncRwHook(dev, buffer, lba, lbaCount, dir);
    if (ret == 0 && fixture != NULL && dir == COMM_IO_WRITE) {
        fixture->store.async_write_count++;
    }

    if (cbFunc != NULL) {
        cbFunc(ret == 0 ? COMM_CMD_SUCCESS : COMM_CMD_CQE_ERROR, cbArg);
    }

    return ret;
}

static int r2fsIntegrationTestGetMetaJournalHeadHook(
    struct comm_dev *dev,
    uint64_t *headLpa
)
{
    if (dev == NULL || headLpa == NULL) {
        return EINVAL;
    }

    *headLpa = dev->metaJournalHeadLpa;
    return 0;
}

static int r2fsIntegrationTestUpdateMetaJournalTailHook(
    struct comm_dev *dev,
    uint64_t originLpa,
    uint32_t writeBlockNum
)
{
    uint64_t journal_size;
    uint64_t offset;
    uint64_t new_offset;

    if (dev == NULL) {
        return EINVAL;
    }

    if (originLpa != dev->metaJournalTailLpa) {
        return EINVAL;
    }

    journal_size = dev->metaJournalEndLpa - dev->metaJournalStartLpa;
    if (journal_size == 0) {
        return EINVAL;
    }

    offset = originLpa - dev->metaJournalStartLpa;
    new_offset = (offset + writeBlockNum) % journal_size;
    dev->metaJournalTailLpa = dev->metaJournalStartLpa + new_offset;
    dev->metaJournalHeadLpa = dev->metaJournalTailLpa;
    return 0;
}

static void r2fsIntegrationInstallHooks(R2fsIntegrationFixtureState *fixture)
{
    g_r2fs_integration_fixture_state = fixture;
    commSetTestSyncRwHook(r2fsIntegrationTestSyncRwHook);
    commSetTestAsyncRwHook(r2fsIntegrationTestAsyncRwHook);
    commSetTestGetMetaJournalHeadHook(r2fsIntegrationTestGetMetaJournalHeadHook);
    commSetTestUpdateMetaJournalTailHook(r2fsIntegrationTestUpdateMetaJournalTailHook);
}

static void r2fsIntegrationResetHooks(void)
{
    commSetTestSyncRwHook(NULL);
    commSetTestAsyncRwHook(NULL);
    commSetTestGetMetaJournalHeadHook(NULL);
    commSetTestUpdateMetaJournalTailHook(NULL);
    g_r2fs_integration_fixture_state = NULL;
}

static void r2fsIntegrationFixtureSetActive(
    R2fsIntegrationFixtureState *fixture
)
{
    g_r2fs_integration_fixture_state = fixture;
}

static void r2fsIntegrationFixtureClearActiveIfMatches(
    R2fsIntegrationFixtureState *fixture
)
{
    if (fixture != NULL && g_r2fs_integration_fixture_state == fixture) {
        g_r2fs_integration_fixture_state = NULL;
    }
}

static void r2fsIntegrationFixtureDestroyState(
    R2fsIntegrationFixtureState *fixture
)
{
    if (fixture == NULL) {
        return;
    }

    if (fixture->mounted) {
        if (fixture->mt_entry.mt_fs_root != NULL) {
            if (fixture->mt_entry.mt_fs_root->location.node_access != NULL) {
                r2fsFsHandler.freenod_h(&fixture->mt_entry.mt_fs_root->location);
            }
            fixture->mt_entry.mt_fs_root->location.node_access = NULL;
            fixture->mt_entry.mt_fs_root->location.node_access_2 = NULL;
            fixture->mt_entry.mt_fs_root->location.handlers = NULL;
        }
        fixture->mt_entry.fs_info = NULL;
        fileSystemManagerFini();
        if (fixture->dev.diskDevice != NULL) {
            commDevDestroy(&fixture->dev);
        }
        fixture->mounted = false;
    }
    r2fsIntegrationResetHooks();
    r2fsIntegrationBlockStoreDestroy(&fixture->store);
    free(fixture);
}

static void r2fsIntegrationFixtureInitMount(
    R2fsIntegrationFixtureState *fixture
)
{
    memset(&fixture->mt_entry, 0, sizeof(fixture->mt_entry));
    memset(&fixture->root_gloc, 0, sizeof(fixture->root_gloc));
    memset(&fixture->mt_entry, 0, sizeof(fixture->mt_entry));
    memset(&fixture->root_gloc, 0, sizeof(fixture->root_gloc));
    fixture->mt_entry.mt_fs_root = &fixture->root_gloc;
    fixture->root_gloc.location.mt_entry = &fixture->mt_entry;
}

static int r2fsIntegrationFlushSitNatCacheToStore(
    SitNatCache *cache,
    R2fsIntegrationFixtureState *fixture
)
{
    khiter_t k;

    if (cache == NULL || fixture == NULL) {
        return EINVAL;
    }

    for (k = kh_begin(cache->cacheManager.index.index);
         k != kh_end(cache->cacheManager.index.index);
         ++k) {
        SitNatCacheEntry *entry;

        if (!kh_exist(cache->cacheManager.index.index, k)) {
            continue;
        }

        entry = (SitNatCacheEntry *)kh_val(cache->cacheManager.index.index, k);
        if (entry == NULL) {
            continue;
        }

        if ((uint64_t)entry->lpa >= fixture->store.lpa_count) {
            return EINVAL;
        }

        memcpy(
            r2fsIntegrationBlockPtr(&fixture->store, entry->lpa),
            blockBufferGetPtr(&entry->cache),
            R2FS_ITEST_BLOCK_SIZE
        );
    }

    return 0;
}

static int r2fsIntegrationFlushSrmapToStore(
    SrmapUtils *srmap_utils,
    R2fsIntegrationFixtureState *fixture
)
{
    khiter_t k;

    if (srmap_utils == NULL || fixture == NULL) {
        return EINVAL;
    }

    for (k = kh_begin(srmap_utils->srmapCache);
         k != kh_end(srmap_utils->srmapCache);
         ++k) {
        uint32_t lpa;
        BlockBuffer *blk;

        if (!kh_exist(srmap_utils->srmapCache, k)) {
            continue;
        }

        lpa = kh_key(srmap_utils->srmapCache, k);
        blk = &kh_value(srmap_utils->srmapCache, k);
        if ((uint64_t)lpa >= fixture->store.lpa_count) {
            return EINVAL;
        }

        memcpy(
            r2fsIntegrationBlockPtr(&fixture->store, lpa),
            blockBufferGetPtr(blk),
            R2FS_ITEST_BLOCK_SIZE
        );
    }

    return 0;
}

static int r2fsIntegrationFixtureMount(
    R2fsIntegrationFixtureState *fixture
)
{
    int ret;

    if (fixture == NULL) {
        return EINVAL;
    }

    r2fsIntegrationFixtureInitMount(fixture);
    ret = commDevInit(
        &fixture->dev,
        &fixture->disk,
        512,
        fixture->store.lpa_count * LBA_PER_LPA,
        fixture->layout.meta_journal_start_lpa,
        fixture->layout.meta_journal_start_lpa +
            (uint64_t)fixture->layout.meta_journal_segment_count * BLOCK_PER_SEGMENT
    );
    if (ret != 0) {
        return ret;
    }

    if (r2fsInitialize(&fixture->mt_entry, &fixture->dev) != 0) {
        ret = errno != 0 ? errno : EIO;
        commDevDestroy(&fixture->dev);
        return ret;
    }

    fixture->mounted = true;
    return 0;
}

static int r2fsIntegrationSplitParent(
    const char *path,
    char *parent_buf,
    size_t parent_buf_size,
    const char **out_leaf
)
{
    const char *last_slash;
    size_t parent_len;

    if (path == NULL || parent_buf == NULL || out_leaf == NULL || path[0] != '/') {
        return EINVAL;
    }

    last_slash = strrchr(path, '/');
    if (last_slash == NULL || last_slash[1] == '\0') {
        return EINVAL;
    }

    parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) {
        if (parent_buf_size < 2) {
            return ENAMETOOLONG;
        }
        parent_buf[0] = '/';
        parent_buf[1] = '\0';
    } else {
        if (parent_len + 1 > parent_buf_size) {
            return ENAMETOOLONG;
        }
        memcpy(parent_buf, path, parent_len);
        parent_buf[parent_len] = '\0';
    }

    *out_leaf = last_slash + 1;
    return 0;
}

static int r2fsIntegrationLookup(
    R2fsIntegrationFixture *fixture,
    const char *path,
    rtems_filesystem_location_info_t *out_loc,
    RtfsRuntimeInodeView **out_view
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);
    char tmp[R2FS_ITEST_MAX_PATH_LEN];
    char *saveptr = NULL;
    char *token;
    rtems_filesystem_location_info_t current;
    RtfsRuntimeInodeView *current_view;

    if (state == NULL || path == NULL || out_loc == NULL || path[0] != '/') {
        return EINVAL;
    }

    memset(out_loc, 0, sizeof(*out_loc));

    if (strcmp(path, "/") == 0) {
        current = state->root_gloc.location;
        if (r2fsFsHandler.clonenod_h(&current) != 0) {
            return errno;
        }
        *out_loc = current;
        if (out_view != NULL) {
            *out_view = (RtfsRuntimeInodeView *)out_loc->node_access;
        }
        return 0;
    }

    if (strlen(path) >= sizeof(tmp)) {
        return ENAMETOOLONG;
    }

    current = state->root_gloc.location;
    if (r2fsFsHandler.clonenod_h(&current) != 0) {
        return errno;
    }

    memcpy(tmp, path + 1, strlen(path));
    tmp[strlen(path) - 1] = '\0';

    token = strtok_r(tmp, "/", &saveptr);
    while (token != NULL) {
        RtfsDirLookupResult result;
        int ret;
        rtems_filesystem_location_info_t next = current;

        current_view = (RtfsRuntimeInodeView *)current.node_access;
        if (current_view == NULL || !rtfsInodeIsDirectoryType(current_view->file_type)) {
            r2fsFsHandler.freenod_h(&current);
            return ENOTDIR;
        }

        ret = r2fsFsHandler.clonenod_h(&next);
        if (ret != 0) {
            r2fsFsHandler.freenod_h(&current);
            return errno;
        }

        {
            RtfsDirInode *dir_inode;
            RtfsDirInodeBuildRequest request = {
                .ino = current_view->ino,
                .mode = RTFS_DIR_BUILD_ON_DEMAND
            };
            file_system_manager *fs_manager = (file_system_manager *)current.mt_entry->fs_info;

            ret = rtfsDirInodeResolve(fs_manager, NULL, &request, &dir_inode);
            if (ret != 0) {
                r2fsFsHandler.freenod_h(&next);
                r2fsFsHandler.freenod_h(&current);
                return ret;
            }

            do {
                ret = rtfsDirInodeLookup(dir_inode, token, strlen(token), &result);
                if (ret != ENOENT || rtfsDirInodeIsFullyLoaded(dir_inode)) {
                    break;
                }
                ret = rtfsDirInodeResolveNext(fs_manager, current_view->ino, dir_inode);
                if (ret != 0) {
                    rtfsDirInodePut(dir_inode);
                    r2fsFsHandler.freenod_h(&next);
                    r2fsFsHandler.freenod_h(&current);
                    return ret;
                }
            } while (true);

            rtfsDirInodePut(dir_inode);
            if (ret != 0) {
                r2fsFsHandler.freenod_h(&next);
                r2fsFsHandler.freenod_h(&current);
                return ret;
            }
        }

        next.node_access = NULL;
        next.node_access_2 = NULL;

        next.node_access = rtfsRuntimeInodeViewClone(&result.inode_view);
        if (next.node_access == NULL) {
            r2fsFsHandler.freenod_h(&current);
            return ENOMEM;
        }

        if (token[0] != '\0') {
            next.node_access_2 = strdup(token);
            if (next.node_access_2 == NULL) {
                r2fsFsHandler.freenod_h(&next);
                r2fsFsHandler.freenod_h(&current);
                return ENOMEM;
            }
        }

        if (rtfsInodeIsDirectoryType(result.inode_view.file_type)) {
            next.handlers = &rtfsDirhandlers;
        } else {
            next.handlers = &rtfsFilehandlers;
        }

        r2fsFsHandler.freenod_h(&current);
        current = next;
        token = strtok_r(NULL, "/", &saveptr);
    }

    *out_loc = current;
    if (out_view != NULL) {
        *out_view = (RtfsRuntimeInodeView *)out_loc->node_access;
    }
    return 0;
}

static int r2fsIntegrationLookupParent(
    R2fsIntegrationFixture *fixture,
    const char *path,
    rtems_filesystem_location_info_t *out_parent,
    const char **out_leaf
)
{
    char parent_buf[R2FS_ITEST_MAX_PATH_LEN];
    int ret;

    ret = r2fsIntegrationSplitParent(path, parent_buf, sizeof(parent_buf), out_leaf);
    if (ret != 0) {
        return ret;
    }

    return r2fsIntegrationLookup(fixture, parent_buf, out_parent, NULL);
}

int r2fsIntegrationFixtureFormatAndMount(
    R2fsIntegrationFixture *fixture,
    uint64_t lpa_count
)
{
    R2fsIntegrationFixtureState *state;
    R2fsMkfsOptions options;
    int ret;

    if (fixture == NULL) {
        return EINVAL;
    }

    fixture->state = NULL;
    state = (R2fsIntegrationFixtureState *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return ENOMEM;
    }

    fixture->state = state;
    r2fsIntegrationBlockStoreInit(&state->store, lpa_count);
    r2fsIntegrationFixtureInitMount(state);
    r2fsIntegrationFixtureSetActive(state);
    r2fsIntegrationInstallHooks(state);

    memset(&options, 0, sizeof(options));
    options.lpa_count = lpa_count;
    options.root_ino = 1;
    options.meta_journal_segment_count = 1;

    ret = r2fsMkfsFormat(&options, r2fsIntegrationMkfsWriteBlock, &state->store, &state->layout);
    if (ret != 0) {
        r2fsIntegrationFixtureDestroy(fixture);
        return ret;
    }

    ret = r2fsIntegrationFixtureMount(state);
    if (ret != 0) {
        r2fsIntegrationFixtureDestroy(fixture);
        return ret;
    }

    return 0;
}

int r2fsIntegrationFixtureRemount(
    R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);
    int ret;

    if (state == NULL || state->store.bytes == NULL) {
        return EINVAL;
    }

    r2fsIntegrationFixtureUnmount(fixture);
    ret = r2fsIntegrationFixtureMount(state);
    if (ret != 0) {
        r2fsIntegrationFixtureDestroy(fixture);
        return ret;
    }

    return 0;
}

void r2fsIntegrationFixtureUnmount(
    R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);
    file_system_manager *fs_manager;

    if (state == NULL || !state->mounted) {
        return;
    }

    fs_manager = (file_system_manager *)state->mt_entry.fs_info;
    if (fs_manager != NULL) {
        TEST_ASSERT_EQUAL(0, fileSystemManagerFlushForUnmount(fs_manager));
    }

    if (state->mt_entry.mt_fs_root != NULL) {
        if (state->mt_entry.mt_fs_root->location.node_access != NULL) {
            r2fsFsHandler.freenod_h(&state->mt_entry.mt_fs_root->location);
        }
        state->mt_entry.mt_fs_root->location.node_access = NULL;
        state->mt_entry.mt_fs_root->location.node_access_2 = NULL;
        state->mt_entry.mt_fs_root->location.handlers = NULL;
    }
    state->mt_entry.fs_info = NULL;
    fileSystemManagerFini();
    if (state->dev.diskDevice != NULL) {
        commDevDestroy(&state->dev);
    }
    state->mounted = false;
}

void r2fsIntegrationFixtureCrash(
    R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL) {
        return;
    }

    if (state->mt_entry.mt_fs_root != NULL &&
        state->mt_entry.mt_fs_root->location.node_access != NULL) {
        r2fsFsHandler.freenod_h(&state->mt_entry.mt_fs_root->location);
    }

    state->mt_entry.mt_fs_root = NULL;
    state->mt_entry.fs_info = NULL;
    state->mounted = false;
    fileSystemManagerFini();
    commDevDestroy(&state->dev);
}

void r2fsIntegrationFixtureDestroy(
    R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state;

    if (fixture == NULL) {
        return;
    }

    state = fixture->state;
    fixture->state = NULL;
    r2fsIntegrationFixtureDestroyState(state);
}

void r2fsIntegrationFixtureCleanupActive(void)
{
    R2fsIntegrationFixtureState *fixture = g_r2fs_integration_fixture_state;

    r2fsIntegrationFixtureDestroyState(fixture);
}

int r2fsIntegrationStatPath(
    R2fsIntegrationFixture *fixture,
    const char *path,
    struct stat *st
)
{
    rtems_filesystem_location_info_t loc;
    int ret;

    ret = r2fsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0) {
        return ret;
    }

    if (loc.handlers == NULL || loc.handlers->fstat_h == NULL) {
        r2fsFsHandler.freenod_h(&loc);
        return EINVAL;
    }

    if (loc.handlers->fstat_h(&loc, st) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&loc);
        return ret;
    }

    r2fsFsHandler.freenod_h(&loc);
    return 0;
}

int r2fsIntegrationStatvfsRoot(
    R2fsIntegrationFixture *fixture,
    struct statvfs *stvfs
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    if (state == NULL || stvfs == NULL) {
        return EINVAL;
    }

    if (r2fsFsHandler.statvfs_h(&state->root_gloc.location, stvfs) != 0) {
        return errno != 0 ? errno : EIO;
    }

    return 0;
}

int r2fsIntegrationReadDir(
    R2fsIntegrationFixture *fixture,
    const char *path,
    struct dirent *entries,
    size_t capacity,
    size_t *out_count
)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t bytes_read;
    size_t entry_count;
    int ret;

    if (entries == NULL || out_count == NULL || capacity == 0) {
        return EINVAL;
    }

    ret = r2fsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0) {
        return ret;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsDirhandlers.open_h(&iop, path, O_RDONLY, 0) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    memset(entries, 0, capacity * sizeof(*entries));
    bytes_read = rtfsDirhandlers.read_h(&iop, entries, capacity * sizeof(*entries));
    if (bytes_read < 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    entry_count = (size_t)bytes_read / sizeof(*entries);
    *out_count = entry_count;
    r2fsFsHandler.freenod_h(&iop.pathinfo);
    return 0;
}

int r2fsIntegrationMkdir(
    R2fsIntegrationFixture *fixture,
    const char *path,
    mode_t mode
)
{
    rtems_filesystem_location_info_t parentloc;
    const char *leaf;
    int ret;

    ret = r2fsIntegrationLookupParent(fixture, path, &parentloc, &leaf);
    if (ret != 0) {
        return ret;
    }

    if (r2fsFsHandler.mknod_h(&parentloc, leaf, strlen(leaf), S_IFDIR | mode, 0) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&parentloc);
        return ret;
    }

    r2fsFsHandler.freenod_h(&parentloc);
    return 0;
}

int r2fsIntegrationCreateFile(
    R2fsIntegrationFixture *fixture,
    const char *path,
    mode_t mode
)
{
    rtems_filesystem_location_info_t parentloc;
    const char *leaf;
    int ret;

    ret = r2fsIntegrationLookupParent(fixture, path, &parentloc, &leaf);
    if (ret != 0) {
        return ret;
    }

    if (r2fsFsHandler.mknod_h(&parentloc, leaf, strlen(leaf), S_IFREG | mode, 0) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&parentloc);
        return ret;
    }

    r2fsFsHandler.freenod_h(&parentloc);
    return 0;
}

int r2fsIntegrationRename(
    R2fsIntegrationFixture *fixture,
    const char *old_path,
    const char *new_path
)
{
    rtems_filesystem_location_info_t old_parentloc;
    rtems_filesystem_location_info_t old_loc;
    rtems_filesystem_location_info_t new_parentloc;
    const char *old_leaf;
    const char *new_leaf;
    int ret;

    ret = r2fsIntegrationLookupParent(fixture, old_path, &old_parentloc, &old_leaf);
    if (ret != 0) {
        return ret;
    }

    ret = r2fsIntegrationLookup(fixture, old_path, &old_loc, NULL);
    if (ret != 0) {
        r2fsFsHandler.freenod_h(&old_parentloc);
        return ret;
    }

    ret = r2fsIntegrationLookupParent(fixture, new_path, &new_parentloc, &new_leaf);
    if (ret != 0) {
        r2fsFsHandler.freenod_h(&old_loc);
        r2fsFsHandler.freenod_h(&old_parentloc);
        return ret;
    }

    if (r2fsFsHandler.rename_h(
            &old_parentloc,
            &old_loc,
            &new_parentloc,
            new_leaf,
            strlen(new_leaf)
        ) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&new_parentloc);
        r2fsFsHandler.freenod_h(&old_loc);
        r2fsFsHandler.freenod_h(&old_parentloc);
        return ret;
    }

    r2fsFsHandler.freenod_h(&new_parentloc);
    r2fsFsHandler.freenod_h(&old_loc);
    r2fsFsHandler.freenod_h(&old_parentloc);
    return 0;
}

int r2fsIntegrationRemove(
    R2fsIntegrationFixture *fixture,
    const char *path
)
{
    rtems_filesystem_location_info_t parentloc;
    rtems_filesystem_location_info_t loc;
    const char *leaf;
    int ret;

    ret = r2fsIntegrationLookupParent(fixture, path, &parentloc, &leaf);
    if (ret != 0) {
        return ret;
    }

    ret = r2fsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0) {
        r2fsFsHandler.freenod_h(&parentloc);
        return ret;
    }

    if (r2fsFsHandler.rmnod_h(&parentloc, &loc) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&loc);
        r2fsFsHandler.freenod_h(&parentloc);
        return ret;
    }

    r2fsFsHandler.freenod_h(&loc);
    r2fsFsHandler.freenod_h(&parentloc);
    return 0;
}

int r2fsIntegrationWriteFile(
    R2fsIntegrationFixture *fixture,
    const char *path,
    const void *data,
    size_t size
)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t written;
    int ret;

    ret = r2fsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0) {
        return ret;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsFilehandlers.open_h(&iop, path, O_RDWR, 0) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    written = rtfsFilehandlers.write_h(&iop, data, size);
    if (written < 0 || (size_t)written != size) {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.fdatasync_h(&iop) != 0) {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.close_h(&iop) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    r2fsFsHandler.freenod_h(&iop.pathinfo);
    return 0;
}

ssize_t r2fsIntegrationReadFile(
    R2fsIntegrationFixture *fixture,
    const char *path,
    void *buffer,
    size_t size
)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t bytes_read;
    int ret;

    ret = r2fsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsFilehandlers.open_h(&iop, path, O_RDONLY, 0) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        errno = ret;
        return -1;
    }

    bytes_read = rtfsFilehandlers.read_h(&iop, buffer, size);
    ret = errno;
    (void)rtfsFilehandlers.close_h(&iop);
    r2fsFsHandler.freenod_h(&iop.pathinfo);
    if (bytes_read < 0) {
        errno = ret != 0 ? ret : EIO;
    }
    return bytes_read;
}

int r2fsIntegrationWriteAt(
    R2fsIntegrationFixture *fixture,
    const char *path,
    off_t offset,
    const void *data,
    size_t size
)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t written;
    int ret;

    if (data == NULL && size != 0) {
        return EINVAL;
    }

    ret = r2fsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0) {
        return ret;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsFilehandlers.open_h(&iop, path, O_RDWR, 0) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.lseek_h(&iop, offset, SEEK_SET) < 0) {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    written = rtfsFilehandlers.write_h(&iop, data, size);
    if (written < 0 || (size_t)written != size) {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.fdatasync_h(&iop) != 0) {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    if (rtfsFilehandlers.close_h(&iop) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        return ret;
    }

    r2fsFsHandler.freenod_h(&iop.pathinfo);
    return 0;
}

ssize_t r2fsIntegrationReadAt(
    R2fsIntegrationFixture *fixture,
    const char *path,
    off_t offset,
    void *buffer,
    size_t size
)
{
    rtems_filesystem_location_info_t loc;
    rtems_libio_t iop;
    ssize_t bytes_read;
    int ret;

    if (buffer == NULL && size != 0) {
        errno = EINVAL;
        return -1;
    }

    ret = r2fsIntegrationLookup(fixture, path, &loc, NULL);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    memset(&iop, 0, sizeof(iop));
    iop.pathinfo = loc;

    if (rtfsFilehandlers.open_h(&iop, path, O_RDONLY, 0) != 0) {
        ret = errno != 0 ? errno : EIO;
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        errno = ret;
        return -1;
    }

    if (rtfsFilehandlers.lseek_h(&iop, offset, SEEK_SET) < 0) {
        ret = errno != 0 ? errno : EIO;
        (void)rtfsFilehandlers.close_h(&iop);
        r2fsFsHandler.freenod_h(&iop.pathinfo);
        errno = ret;
        return -1;
    }

    bytes_read = rtfsFilehandlers.read_h(&iop, buffer, size);
    ret = errno;
    (void)rtfsFilehandlers.close_h(&iop);
    r2fsFsHandler.freenod_h(&iop.pathinfo);
    if (bytes_read < 0) {
        errno = ret != 0 ? ret : EIO;
    }
    return bytes_read;
}

int r2fsIntegrationReadCurrentFileMapping(
    R2fsIntegrationFixture *fixture,
    const char *path,
    uint32_t *out_ino,
    uint32_t *out_inode_lpa,
    uint32_t *out_first_data_lpa
)
{
    rtems_filesystem_location_info_t loc;
    RtfsRuntimeInodeView *view = NULL;
    file_system_manager *fs_manager;
    NodeBlockCacheHelper helper;
    NodeBlockCacheEntryHandle inode_handle;
    const struct RtfsNode *inode_node;
    int ret;

    if (fixture == NULL || path == NULL || out_first_data_lpa == NULL) {
        return EINVAL;
    }

    ret = r2fsIntegrationLookup(fixture, path, &loc, &view);
    if (ret != 0) {
        return ret;
    }

    if (view == NULL || rtfsInodeIsDirectoryType(view->file_type)) {
        r2fsFsHandler.freenod_h(&loc);
        return EINVAL;
    }

    fs_manager = (file_system_manager *)loc.mt_entry->fs_info;
    if (fs_manager == NULL) {
        r2fsFsHandler.freenod_h(&loc);
        return EINVAL;
    }

    nodeBlockCacheHelperInit(&helper, fs_manager);
    ret = nodeBlockCacheHelperGetNodeEntry(
        &helper,
        view->ino,
        view->ino,
        &inode_handle
    );
    if (ret != 0 || nodeBlockCacheEntryHandleIsEmpty(&inode_handle)) {
        r2fsFsHandler.freenod_h(&loc);
        return ret != 0 ? ret : ENOENT;
    }

    inode_node = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    if (out_ino != NULL) {
        *out_ino = view->ino;
    }
    if (out_inode_lpa != NULL) {
        *out_inode_lpa = nodeBlockCacheEntryGetLpa(inode_handle.entry);
    }
    *out_first_data_lpa = inode_node->i.i_addr[0];

    nodeBlockCacheEntryHandleDestroy(&inode_handle);
    r2fsFsHandler.freenod_h(&loc);
    return 0;
}

bool r2fsIntegrationBlockStoreIsZeroed(
    const R2fsIntegrationBlockStore *store,
    uint32_t lpa
)
{
    const unsigned char *ptr;
    size_t i;

    if (store == NULL || store->bytes == NULL || (uint64_t)lpa >= store->lpa_count) {
        return false;
    }

    ptr = store->bytes + (uint64_t)lpa * R2FS_ITEST_BLOCK_SIZE;
    for (i = 0; i < R2FS_ITEST_BLOCK_SIZE; ++i) {
        if (ptr[i] != 0) {
            return false;
        }
    }

    return true;
}

const R2fsIntegrationBlockStore *r2fsIntegrationFixtureBlockStore(
    const R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    return state != NULL ? &state->store : NULL;
}

int r2fsIntegrationFixtureSetFailLpa(
    R2fsIntegrationFixture *fixture,
    uint32_t lpa
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL) {
        return EINVAL;
    }

    if (lpa != UINT32_MAX && (uint64_t)lpa >= state->store.lpa_count) {
        return EINVAL;
    }

    state->store.fail_lpa = lpa;
    return 0;
}

int r2fsIntegrationFixtureSetFailReadLpa(
    R2fsIntegrationFixture *fixture,
    uint32_t lpa
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL) {
        return EINVAL;
    }

    if (lpa != UINT32_MAX && (uint64_t)lpa >= state->store.lpa_count) {
        return EINVAL;
    }

    state->store.fail_read_lpa = lpa;
    return 0;
}

int r2fsIntegrationFixtureSetFailWriteLpa(
    R2fsIntegrationFixture *fixture,
    uint32_t lpa
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL) {
        return EINVAL;
    }

    if (lpa != UINT32_MAX && (uint64_t)lpa >= state->store.lpa_count) {
        return EINVAL;
    }

    state->store.fail_write_lpa = lpa;
    return 0;
}

int r2fsIntegrationFixtureFailNextWrite(
    R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL) {
        return EINVAL;
    }

    state->store.fail_next_write_countdown = 1u;
    return 0;
}

int r2fsIntegrationFixtureFailNextDataWrite(
    R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL) {
        return EINVAL;
    }

    state->store.fail_next_data_write_countdown = 1u;
    return 0;
}

int r2fsIntegrationFixtureSetStopAfterMetaWrites(
    R2fsIntegrationFixture *fixture,
    uint32_t limit
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL) {
        return EINVAL;
    }

    state->store.stop_after_meta_writes = limit;
    state->store.meta_write_count = 0;
    state->store.meta_write_limit_hit = false;
    return 0;
}

int r2fsIntegrationFixtureClearFaults(
    R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    if (state == NULL || state->store.bytes == NULL) {
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

bool r2fsIntegrationFixtureMetaWriteLimitHit(
    const R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);

    return state != NULL && state->store.meta_write_limit_hit;
}

int r2fsIntegrationFlushMetadataToStore(
    R2fsIntegrationFixture *fixture
)
{
    R2fsIntegrationFixtureState *state = r2fsIntegrationFixtureState(fixture);
    file_system_manager *fs_manager;
    int ret;

    if (state == NULL || !state->mounted) {
        return EINVAL;
    }

    fs_manager = (file_system_manager *)state->mt_entry.fs_info;
    if (fs_manager == NULL) {
        return EINVAL;
    }

    ret = r2fsIntegrationFlushSitNatCacheToStore(
        fileSystemManagerGetNatCache(fs_manager),
        state
    );
    if (ret != 0) {
        return ret;
    }

    ret = r2fsIntegrationFlushSitNatCacheToStore(
        fileSystemManagerGetSitCache(fs_manager),
        state
    );
    if (ret != 0) {
        return ret;
    }

    return r2fsIntegrationFlushSrmapToStore(
        fileSystemManagerGetSrmapUtils(fs_manager),
        state
    );
}
