#include "dir_inode.h"

#include "cache/block_buffer.h"
#include "cache/node_block_cache.h"
#include "fs/cow_reclaim_registry.h"
#include "fs/fs_manager.h"
#include "fs/sit_utils.h"
#include "fs/super_manager.h"
#include "journal/journal_container.h"
#include "journal/journal_process_env.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define RTFS_DIR_STATIC_ENTRY_COUNT 2

static rtfs_dir_inode_write_block_hook g_rtfs_dir_inode_write_block_hook = NULL;
static rtfs_dir_inode_journal_commit_hook g_rtfs_dir_inode_journal_commit_hook = NULL;

/* ===== Internal Types ===== */

typedef struct RtfsMemDirEntry
{
    rtfs_ino ino;
    uint8_t file_type;
    char name[RTFS_NAME_LEN + 1];
    bool source_is_inline;
    uint32_t source_block_index;
    uint16_t source_slot_index;
} RtfsMemDirEntry;

typedef enum RtfsDirBackingKind
{
    RTFS_DIR_BACKING_EMPTY = 0,
    RTFS_DIR_BACKING_INLINE,
    RTFS_DIR_BACKING_BLOCKS
} RtfsDirBackingKind;

typedef struct RtfsLoadedDirBlock
{
    uint32_t block_index;
    uint32_t lpa;
    uint32_t cow_new_lpa;
    bool has_pending_cow_relocation;
    struct RtfsDentryBlock *block;
    bool is_dirty;
} RtfsLoadedDirBlock;

typedef struct RtfsDirInode
{
    /* Minimal identity. */
    rtfs_ino ino;
    rtfs_ino parent_ino;
    uint8_t file_type;

    /* Directory metadata mirrored from on-disk inode when available. */
    uint64_t i_size;
    uint32_t i_dentry_num;
    uint64_t i_mtime;
    uint32_t i_current_depth;

    /* Sequential load progress for non-inline directory blocks. */
    size_t loaded_block_count;
    size_t total_block_count;
    bool is_fully_loaded;

    /* Backing view of the loaded directory content. */
    RtfsDirBackingKind backing_kind;
    struct RtfsInode *disk_inode;
    struct RtfsInlineDentry *inline_dentry;
    RtfsLoadedDirBlock *loaded_blocks;
    size_t loaded_block_slots;
    size_t loaded_block_count_actual;

    /* Runtime directory entry set. */
    size_t entry_count;
    size_t capacity;
    RtfsMemDirEntry *entries;

    /* Optional underlying node-cache reference. */
    void *cache_handle;

    /* Runtime dirty marker for directory-level mutations. */
    bool is_dirty;
} RtfsDirInode;

struct RtfsDirInodeCache
{
    NodeBlockCache *node_cache;
};

typedef struct RtfsDirInodeNodeHandle
{
    NodeBlockCacheEntryHandle node_handle;
    NodeBlockCache *node_cache;
} RtfsDirInodeNodeHandle;

/* ===== Internal Helpers ===== */

/**
 * @brief 创建一个空的目录运行时对象。
 */
static RtfsDirInode *rtfsCreateDirInode(rtfs_ino ino, rtfs_ino parent_ino);

/**
 * @brief 销毁目录运行时对象，并释放其持有的底层资源。
 */
static void rtfsDestroyDirInode(RtfsDirInode *dir_inode);

/**
 * @brief 从可选的 cache 中转换目录元数据和目录项。
 */
static int rtfsDirInodeBuildFromCache(
    RtfsDirInode *dir_inode,
    RtfsDirInodeCache *cache,
    rtfs_ino ino
);

/**
 * @brief 从磁盘 inode 中提取目录元数据。
 */
static int rtfsDirInodeBuildMetadata(
    RtfsDirInode *dir_inode,
    const struct RtfsInode *disk_inode
);

/**
 * @brief 解析 inline dentry 并填充运行时目录项数组。
 */
static int rtfsDirInodeBuildInlineEntries(
    RtfsDirInode *dir_inode,
    const struct RtfsInode *disk_inode
);

/**
 * @brief 解析普通目录块并填充运行时目录项数组。
 */
static int rtfsDirInodeAppendRegularEntries(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block,
    uint32_t block_index,
    uint32_t lpa
);

/**
 * @brief 解析一个 inline dentry 槽位对应的目录项。
 */
static int rtfsDirInodeBuildInlineEntryAt(
    RtfsDirInode *dir_inode,
    const struct RtfsInlineDentry *inline_dentry,
    size_t index
);

/**
 * @brief 解析一个普通 dentry 槽位对应的目录项。
 */
static int rtfsDirInodeBuildRegularEntryAt(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block,
    uint32_t block_index,
    size_t index
);

static int rtfsDirInodeRememberLoadedBlock(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block,
    uint32_t block_index,
    uint32_t lpa
);

static RtfsLoadedDirBlock *rtfsDirInodeFindLoadedBlock(
    RtfsDirInode *dir_inode,
    uint32_t block_index
);

static void rtfsDirInodeTouchMetadata(RtfsDirInode *dir_inode);

static void rtfsDirInodeClearDentryBit(
    uint8_t *bitmap,
    size_t bit_index
);

static void rtfsDirInodeClearRegularEntryBacking(
    RtfsDirInode *dir_inode,
    const RtfsMemDirEntry *entry
);

static void rtfsDirInodeClearInlineEntryBacking(
    RtfsDirInode *dir_inode,
    const RtfsMemDirEntry *entry
);

static int rtfsDirInodeAddInlineBacking(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
);
static int rtfsDirInodeConvertInlineToRegular(
    RtfsDirInode *dir_inode
);

static void rtfsDirInodeSetDentryBit(
    uint8_t *bitmap,
    size_t bit_index
);

static void rtfsDirInodeWriteInlineName(
    struct RtfsInlineDentry *inline_dentry,
    size_t index,
    const char *name
);

static void rtfsDirInodeWriteRegularName(
    struct RtfsDentryBlock *dentry_block,
    size_t index,
    const char *name
);

static int rtfsDirInodeAddRegularBacking(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
);

static int rtfsDirInodeGrowRegularBlock(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
);
static void rtfsDirInodeInitCreatedDirectNode(struct RtfsNode *node);
static void rtfsDirInodeInitCreatedIndirectNode(struct RtfsNode *node);
static int rtfsDirInodeWriteDirtyRegularBlocksCow(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
);
static int rtfsDirInodeApplyPendingRegularBlockRelocations(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
);
static int rtfsDirInodeCanRelocateOneRegularBlock(
    RtfsDirInode *dir_inode,
    uint32_t block_index
);
static int rtfsDirInodeRelocateOneRegularBlock(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode,
    uint32_t block_index,
    uint32_t new_lpa
);
static int rtfsDirInodeCollectPendingDataCowOldLpas(
    RtfsDirInode *dir_inode,
    uint32_t *out_array,
    size_t max_count,
    size_t *out_count
);
static JournalContainer *rtfsDirInodeCloneJournalContainer(const JournalContainer *src);
static int rtfsDirInodeSubmitJournal(
    file_system_manager *fs_manager,
    JournalContainer *journal,
    uint64_t *out_tx_id
);

/**
 * @brief 从目录对象提取最小运行时 inode 视图。
 */
static void rtfsDirBuildViewFromDirInode(
    const RtfsDirInode *dir_inode,
    RtfsRuntimeInodeView *view
);

/**
 * @brief 用给定 inode 视图和名字填充一个 dirent。
 */
static int rtfsDirFillDirent(
    struct dirent *entry,
    const RtfsRuntimeInodeView *inode_view,
    const char *name
);

/**
 * @brief 按顺序索引获取目录项，包括动态生成的 . 和 ..
 */
static int rtfsDirGetEntryByIndex(
    const RtfsDirInode *dir_inode,
    size_t index,
    struct dirent *entry
);

/**
 * @brief 在运行时目录项数组中查找指定名字。
 */
static const RtfsMemDirEntry *rtfsDirInodeFindEntry(
    const RtfsDirInode *dir_inode,
    const char *name,
    size_t namelen
);

/**
 * @brief 执行目录项插入的内部实现。
 */
static int rtfsDirInodeAddEntryInternal(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
);

/**
 * @brief 执行目录项删除的内部实现。
 */
static int rtfsDirInodeRemoveEntryInternal(
    RtfsDirInode *dir_inode,
    const char *name
);

/**
 * @brief 判断位图中的某一位是否被置位。
 */
static bool rtfsDirIsBitmapBitSet(const uint8_t *bitmap, size_t bit_index);

/* ===== Public API ===== */

/**
 * @brief 创建目录 inode 层使用的轻量 cache 包装器。
 */
RtfsDirInodeCache *rtfsDirInodeCacheCreate(NodeBlockCache *node_cache)
{
    RtfsDirInodeCache *cache = malloc(sizeof(*cache));

    if (cache == NULL) {
        return NULL;
    }

    cache->node_cache = node_cache;
    return cache;
}

/**
 * @brief 销毁目录 inode cache 包装器本身。
 */
void rtfsDirInodeCacheDestroy(RtfsDirInodeCache *cache)
{
    free(cache);
}

/**
 * @brief 根据给定 ino 构造目录运行时对象，并返回明确错误码。
 */
int rtfsDirInodeBuild(
    RtfsDirInodeCache *cache,
    rtfs_ino ino,
    RtfsDirInode **out_dir_inode
)
{
    RtfsDirInode *dir_inode = rtfsCreateDirInode(ino, ino);

    if (out_dir_inode == NULL) {
        return EINVAL;
    }

    *out_dir_inode = NULL;

    if (dir_inode == NULL) {
        return ENOMEM;
    }

    {
        int ret = rtfsDirInodeBuildFromCache(dir_inode, cache, ino);
        if (ret != 0) {
            rtfsDestroyDirInode(dir_inode);
            return ret;
        }
    }

    *out_dir_inode = dir_inode;
    return 0;
}

/**
 * @brief 释放一个目录运行时对象。
 */
void rtfsDirInodePut(RtfsDirInode *dir_inode)
{
    rtfsDestroyDirInode(dir_inode);
}

/**
 * @brief 将一个普通目录块中的目录项导入到目录运行时对象中。
 */
int rtfsDirInodeAppendDentryBlock(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block
)
{
    return rtfsDirInodeAppendRegularEntries(dir_inode, dentry_block, 0, INVALID_LPA);
}

int rtfsDirInodeAppendDentryBlockAt(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block,
    uint32_t block_index,
    uint32_t lpa
)
{
    return rtfsDirInodeAppendRegularEntries(dir_inode, dentry_block, block_index, lpa);
}

/**
 * @brief 返回目录对象当前是否已经完成全部目录块装载。
 */
bool rtfsDirInodeIsFullyLoaded(const RtfsDirInode *dir_inode)
{
    return dir_inode != NULL && dir_inode->is_fully_loaded;
}

/**
 * @brief 返回目录对象当前已经装载的目录逻辑块数量。
 */
size_t rtfsDirInodeGetLoadedBlockCount(const RtfsDirInode *dir_inode)
{
    return dir_inode != NULL ? dir_inode->loaded_block_count : 0;
}

/**
 * @brief 返回目录对象理论上的目录逻辑块总数。
 */
size_t rtfsDirInodeGetTotalBlockCount(const RtfsDirInode *dir_inode)
{
    return dir_inode != NULL ? dir_inode->total_block_count : 0;
}

bool rtfsDirInodeLoadedBlockHasPendingCowRelocation(
    const RtfsDirInode *dir_inode,
    uint32_t block_index
)
{
    RtfsLoadedDirBlock *loaded_block;

    if (dir_inode == NULL) {
        return false;
    }

    loaded_block = rtfsDirInodeFindLoadedBlock((RtfsDirInode *)dir_inode, block_index);
    return loaded_block != NULL && loaded_block->has_pending_cow_relocation;
}

uint32_t rtfsDirInodeGetLoadedBlockLpa(
    const RtfsDirInode *dir_inode,
    uint32_t block_index
)
{
    RtfsLoadedDirBlock *loaded_block;

    if (dir_inode == NULL) {
        return INVALID_LPA;
    }

    loaded_block = rtfsDirInodeFindLoadedBlock((RtfsDirInode *)dir_inode, block_index);
    return loaded_block != NULL ? loaded_block->lpa : INVALID_LPA;
}

uint32_t rtfsDirInodeGetLoadedBlockCowNewLpa(
    const RtfsDirInode *dir_inode,
    uint32_t block_index
)
{
    RtfsLoadedDirBlock *loaded_block;

    if (dir_inode == NULL) {
        return INVALID_LPA;
    }

    loaded_block = rtfsDirInodeFindLoadedBlock((RtfsDirInode *)dir_inode, block_index);
    return loaded_block != NULL ? loaded_block->cow_new_lpa : INVALID_LPA;
}

/**
 * @brief 更新目录对象的已加载块进度，并自动刷新 fully-loaded 状态。
 */
void rtfsDirInodeSetLoadedBlockCount(
    RtfsDirInode *dir_inode,
    size_t loaded_block_count
)
{
    if (dir_inode == NULL) {
        return;
    }

    dir_inode->loaded_block_count = loaded_block_count;
    if (dir_inode->loaded_block_count > dir_inode->total_block_count) {
        dir_inode->loaded_block_count = dir_inode->total_block_count;
    }

    dir_inode->is_fully_loaded =
        dir_inode->loaded_block_count >= dir_inode->total_block_count;
}

/**
 * @brief 在目录对象中查找指定名字的子项。
 */
int rtfsDirInodeLookup(
    const RtfsDirInode *dir_inode,
    const char *name,
    size_t namelen,
    RtfsDirLookupResult *result
)
{
    size_t i;

    if (dir_inode == NULL) {
        return ENOTDIR;
    }

    if (name == NULL || result == NULL) {
        return EINVAL;
    }

    if (namelen == 1 && name[0] == '.') {
        rtfsDirBuildViewFromDirInode(dir_inode, &result->inode_view);
        return 0;
    }

    if (namelen == 2 && name[0] == '.' && name[1] == '.') {
        rtfsRuntimeInodeViewInit(
            &result->inode_view,
            dir_inode->parent_ino,
            dir_inode->parent_ino,
            RTFS_FT_DIR
        );
        return 0;
    }

    for (i = 0; i < dir_inode->entry_count; ++i) {
        if (strlen(dir_inode->entries[i].name) == namelen &&
            memcmp(dir_inode->entries[i].name, name, namelen) == 0) {
            rtfsRuntimeInodeViewInit(
                &result->inode_view,
                dir_inode->entries[i].ino,
                dir_inode->ino,
                dir_inode->entries[i].file_type
            );
            return 0;
        }
    }

    return ENOENT;
}

/**
 * @brief 以 struct dirent 序列的形式顺序读出目录项。
 */
ssize_t rtfsDirInodeReadEntries(
    const RtfsDirInode *dir_inode,
    off_t *offset,
    void *buffer,
    size_t count
)
{
    size_t entry_index;
    size_t bytes_read;
    struct dirent current;

    if (dir_inode == NULL) {
        errno = ENOTDIR;
        return -1;
    }

    if (offset == NULL || buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    entry_index = (size_t)(*offset / (off_t)sizeof(struct dirent));
    count = (count / sizeof(struct dirent)) * sizeof(struct dirent);
    bytes_read = 0;

    while (count >= sizeof(struct dirent)) {
        int ret = rtfsDirGetEntryByIndex(dir_inode, entry_index, &current);
        if (ret == ENOENT) {
            break;
        }
        if (ret != 0) {
            errno = ret;
            return -1;
        }

        current.d_off = (off_t)((entry_index + 1) * sizeof(struct dirent));
        memcpy((char *)buffer + bytes_read, &current, sizeof(current));

        ++entry_index;
        bytes_read += sizeof(struct dirent);
        count -= sizeof(struct dirent);
    }

    *offset = (off_t)(entry_index * sizeof(struct dirent));
    return (ssize_t)bytes_read;
}

/**
 * @brief 向目录对象添加一个目录项。
 */
int rtfsDirInodeAddEntry(
    RtfsDirInode *dir_inode,
    const char *name,
    const RtfsRuntimeInodeView *child_view
)
{
    int ret;

    if (dir_inode == NULL || name == NULL || child_view == NULL) {
        return EINVAL;
    }

    if (dir_inode->backing_kind == RTFS_DIR_BACKING_INLINE) {
        return rtfsDirInodeAddInlineBacking(
            dir_inode,
            name,
            child_view->ino,
            child_view->file_type
        );
    }

    if (dir_inode->backing_kind == RTFS_DIR_BACKING_BLOCKS) {
        return rtfsDirInodeAddRegularBacking(
            dir_inode,
            name,
            child_view->ino,
            child_view->file_type
        );
    }

    if (dir_inode->disk_inode != NULL &&
        (dir_inode->disk_inode->i_inline & RTFS_INLINE_DENTRY) == 0) {
        return rtfsDirInodeAddRegularBacking(
            dir_inode,
            name,
            child_view->ino,
            child_view->file_type
        );
    }

    ret = rtfsDirInodeAddEntryInternal(
        dir_inode,
        name,
        child_view->ino,
        child_view->file_type
    );
    if (ret == 0) {
        rtfsDirInodeTouchMetadata(dir_inode);
    }
    return ret;
}

/**
 * @brief 从目录对象中删除一个目录项。
 */
int rtfsDirInodeRemoveEntry(
    RtfsDirInode *dir_inode,
    const char *name
)
{
    if (dir_inode == NULL || name == NULL) {
        return EINVAL;
    }

    return rtfsDirInodeRemoveEntryInternal(dir_inode, name);
}

int rtfsDirInodeWritebackContentCow(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
)
{
    if (fs_manager == NULL || dir_inode == NULL) {
        return EINVAL;
    }

    return rtfsDirInodeWriteDirtyRegularBlocksCow(fs_manager, dir_inode);
}

int rtfsDirInodeApplyPendingCowRelocations(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
)
{
    if (fs_manager == NULL || dir_inode == NULL) {
        return EINVAL;
    }

    return rtfsDirInodeApplyPendingRegularBlockRelocations(fs_manager, dir_inode);
}

void rtfsDirInodeSetWriteBlockHook(rtfs_dir_inode_write_block_hook hook)
{
    g_rtfs_dir_inode_write_block_hook = hook;
}

void rtfsDirInodeSetJournalCommitHook(rtfs_dir_inode_journal_commit_hook hook)
{
    g_rtfs_dir_inode_journal_commit_hook = hook;
}

int rtfsDirInodeCommitCowWriteback(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
)
{
    NodeBlockCache *node_cache;
    JournalContainer *cur_journal;
    JournalContainer *to_commit = NULL;
    NodeBlockCacheCowRelocation *node_relocations = NULL;
    uint32_t *old_data_lpas = NULL;
    uint32_t *old_node_lpas = NULL;
    size_t old_data_count = 0;
    size_t old_node_count = 0;
    size_t i;
    uint64_t tx_id = 0;
    bool journal_submitted = false;
    int ret;

    if (fs_manager == NULL || dir_inode == NULL) {
        return EINVAL;
    }

    node_cache = fileSystemManagerGetNodeCache(fs_manager);
    cur_journal = fileSystemManagerGetCurJournal(fs_manager);
    if (node_cache == NULL || cur_journal == NULL) {
        return EINVAL;
    }

    ret = cowReclaimRegistryDrainCompleted();
    if (ret != 0) {
        return ret;
    }

    ret = rtfsDirInodeWritebackContentCow(fs_manager, dir_inode);
    if (ret != 0) {
        return ret;
    }

    if (dir_inode->loaded_block_count_actual > 0) {
        old_data_lpas = (uint32_t *)malloc(
            dir_inode->loaded_block_count_actual * sizeof(*old_data_lpas)
        );
        if (old_data_lpas == NULL) {
            return ENOMEM;
        }

        ret = rtfsDirInodeCollectPendingDataCowOldLpas(
            dir_inode,
            old_data_lpas,
            dir_inode->loaded_block_count_actual,
            &old_data_count
        );
        if (ret != 0) {
            free(old_data_lpas);
            return ret;
        }
    }

    ret = rtfsDirInodeApplyPendingCowRelocations(fs_manager, dir_inode);
    if (ret != 0) {
        free(old_data_lpas);
        return ret;
    }

    ret = nodeBlockCacheWritebackDirtyContentCow(node_cache);
    if (ret != 0) {
        free(old_data_lpas);
        return ret;
    }

    if (node_cache->curSize > 0) {
        node_relocations = (NodeBlockCacheCowRelocation *)malloc(
            node_cache->curSize * sizeof(*node_relocations)
        );
        if (node_relocations == NULL) {
            free(old_data_lpas);
            return ENOMEM;
        }

        ret = nodeBlockCacheCollectPendingCowRelocations(
            node_cache,
            node_relocations,
            node_cache->curSize,
            &old_node_count
        );
        if (ret != 0) {
            free(node_relocations);
            free(old_data_lpas);
            return ret;
        }

        if (old_node_count > 0) {
            old_node_lpas = (uint32_t *)malloc(old_node_count * sizeof(*old_node_lpas));
            if (old_node_lpas == NULL) {
                free(node_relocations);
                free(old_data_lpas);
                return ENOMEM;
            }

            for (i = 0; i < old_node_count; ++i) {
                old_node_lpas[i] = node_relocations[i].oldLpa;
            }
        }
    }

    ret = nodeBlockCacheApplyPendingCowRelocations(node_cache);
    if (ret != 0) {
        free(old_node_lpas);
        free(node_relocations);
        free(old_data_lpas);
        return ret;
    }

    if (!journalContainerIsEmpty(cur_journal)) {
        to_commit = rtfsDirInodeCloneJournalContainer(cur_journal);
        if (to_commit == NULL) {
            free(old_node_lpas);
            free(node_relocations);
            free(old_data_lpas);
            return ENOMEM;
        }

        ret = rtfsDirInodeSubmitJournal(fs_manager, to_commit, &tx_id);
        if (ret != 0) {
            journalContainerDestroy(to_commit);
            free(to_commit);
            free(old_node_lpas);
            free(node_relocations);
            free(old_data_lpas);
            return ret;
        }
        journal_submitted = true;

        journalContainerDestroy(cur_journal);
        journalContainerInit(cur_journal);
    }

    if (journal_submitted) {
        (void)cowReclaimRegistryRegister(
            tx_id,
            old_data_lpas,
            old_data_count,
            old_node_lpas,
            old_node_count
        );
    }

    free(old_node_lpas);
    free(node_relocations);
    free(old_data_lpas);
    dir_inode->is_dirty = false;
    return 0;
}

/* ===== Internal Helper Implementations ===== */

/**
 * @brief 创建一个空的目录运行时对象。
 */
static RtfsDirInode *rtfsCreateDirInode(rtfs_ino ino, rtfs_ino parent_ino)
{
    RtfsDirInode *dir_inode = malloc(sizeof(*dir_inode));
    if (dir_inode == NULL) {
        return NULL;
    }

    memset(dir_inode, 0, sizeof(*dir_inode));
    dir_inode->ino = ino;
    dir_inode->parent_ino = parent_ino;
    dir_inode->file_type = RTFS_FT_DIR;

    return dir_inode;
}

/**
 * @brief 销毁目录运行时对象，并释放其持有的底层资源。
 */
static void rtfsDestroyDirInode(RtfsDirInode *dir_inode)
{
    if (dir_inode == NULL) {
        return;
    }

    free(dir_inode->entries);
    if (dir_inode->loaded_blocks != NULL) {
        size_t i;
        for (i = 0; i < dir_inode->loaded_block_count_actual; ++i) {
            free(dir_inode->loaded_blocks[i].block);
        }
    }
    free(dir_inode->loaded_blocks);
    dir_inode->entries = NULL;
    dir_inode->loaded_blocks = NULL;
    if (dir_inode->cache_handle != NULL) {
        RtfsDirInodeNodeHandle *handle = (RtfsDirInodeNodeHandle *)dir_inode->cache_handle;
        nodeBlockCacheEntryHandleDestroy(&handle->node_handle);
        free(handle);
        dir_inode->cache_handle = NULL;
    }

    free(dir_inode);
}

/**
 * @brief 从可选的 cache 中转换目录元数据和目录项。
 */
static int rtfsDirInodeBuildFromCache(
    RtfsDirInode *dir_inode,
    RtfsDirInodeCache *cache,
    rtfs_ino ino
)
{
    NodeBlockCacheEntryHandle node_handle;
    RtfsDirInodeNodeHandle *owned_handle;
    struct RtfsNode *node;
    int ret;

    if (dir_inode == NULL) {
        return EINVAL;
    }

    if (cache == NULL || cache->node_cache == NULL) {
        dir_inode->cache_handle = NULL;
        dir_inode->is_dirty = false;
        return 0;
    }

    /* TODO:
     * NodeBlockCache miss is intentionally not handled here.
     * The caller / upper-level coordinator must ensure the inode block
     * is already cached before dir_inode conversion begins.
     */
    node_handle = nodeBlockCacheGet(cache->node_cache, ino);
    if (nodeBlockCacheEntryHandleIsEmpty(&node_handle)) {
        return ENOENT;
    }

    node = nodeBlockCacheEntryGetNodeBlockPtr(node_handle.entry);
    dir_inode->disk_inode = &node->i;
    ret = rtfsDirInodeBuildMetadata(dir_inode, &node->i);
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&node_handle);
        return ret;
    }

    if ((node->i.i_inline & RTFS_INLINE_DENTRY) != 0) {
        dir_inode->backing_kind = RTFS_DIR_BACKING_INLINE;
        dir_inode->inline_dentry = (struct RtfsInlineDentry *)node->i.i_addr;
        ret = rtfsDirInodeBuildInlineEntries(dir_inode, &node->i);
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&node_handle);
            return ret;
        }
    } else {
        dir_inode->backing_kind = RTFS_DIR_BACKING_BLOCKS;
        dir_inode->inline_dentry = NULL;
    }

    owned_handle = malloc(sizeof(*owned_handle));
    if (owned_handle == NULL) {
        nodeBlockCacheEntryHandleDestroy(&node_handle);
        return ENOMEM;
    }

    owned_handle->node_handle = node_handle;
    owned_handle->node_cache = cache->node_cache;
    dir_inode->cache_handle = owned_handle;
    dir_inode->is_dirty = false;
    return 0;
}

/**
 * @brief 从磁盘 inode 中提取目录元数据。
 */
static int rtfsDirInodeBuildMetadata(
    RtfsDirInode *dir_inode,
    const struct RtfsInode *disk_inode
)
{
    if (dir_inode == NULL || disk_inode == NULL) {
        return EINVAL;
    }

    dir_inode->parent_ino = disk_inode->i_pino;
    dir_inode->file_type = (uint8_t)disk_inode->i_type;
    dir_inode->i_size = disk_inode->i_size;
    dir_inode->i_dentry_num = (uint32_t)disk_inode->i_dentry_num;
    dir_inode->i_mtime = disk_inode->i_mtime;
    dir_inode->i_current_depth = disk_inode->i_current_depth;
    dir_inode->total_block_count = (size_t)SIZE_TO_BLOCK(disk_inode->i_size);
    dir_inode->loaded_block_count = 0;
    dir_inode->is_fully_loaded = (dir_inode->total_block_count == 0);
    dir_inode->backing_kind = RTFS_DIR_BACKING_EMPTY;
    return 0;
}

/**
 * @brief 解析 inline dentry 并填充运行时目录项数组。
 */
static int rtfsDirInodeBuildInlineEntries(
    RtfsDirInode *dir_inode,
    const struct RtfsInode *disk_inode
)
{
    const struct RtfsInlineDentry *inline_dentry;
    uint32_t disk_dentry_num;
    size_t i;

    if (dir_inode == NULL || disk_inode == NULL) {
        return EINVAL;
    }

    inline_dentry = (const struct RtfsInlineDentry *)disk_inode->i_addr;
    disk_dentry_num = dir_inode->i_dentry_num;
    dir_inode->i_dentry_num = 0;
    for (i = 0; i < NR_INLINE_DENTRY; ++i) {
        int ret;
        size_t slot_count;

        if (!rtfsDirIsBitmapBitSet(inline_dentry->dentry_bitmap, i)) {
            continue;
        }

        ret = rtfsDirInodeBuildInlineEntryAt(dir_inode, inline_dentry, i);
        if (ret != 0) {
            return ret;
        }

        slot_count = GET_DENTRY_SLOTS(inline_dentry->dentry[i].name_len);
        if (slot_count > 0) {
            i += slot_count - 1;
        }
    }

    dir_inode->i_dentry_num = disk_dentry_num;
    dir_inode->loaded_block_count = dir_inode->total_block_count;
    dir_inode->is_fully_loaded = true;
    return 0;
}

/**
 * @brief 解析普通目录块并填充运行时目录项数组。
 */
static int rtfsDirInodeAppendRegularEntries(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block,
    uint32_t block_index,
    uint32_t lpa
)
{
    uint32_t disk_dentry_num;
    bool was_dirty;
    size_t i;

    if (dir_inode == NULL || dentry_block == NULL) {
        return EINVAL;
    }

    if (dir_inode->backing_kind == RTFS_DIR_BACKING_EMPTY) {
        dir_inode->backing_kind = RTFS_DIR_BACKING_BLOCKS;
    }

    {
        int remember_ret = rtfsDirInodeRememberLoadedBlock(dir_inode, dentry_block, block_index, lpa);
        if (remember_ret != 0) {
            return remember_ret;
        }
    }

    disk_dentry_num = dir_inode->i_dentry_num;
    was_dirty = dir_inode->is_dirty;
    dir_inode->i_dentry_num = 0;

    for (i = 0; i < NR_DENTRY_IN_BLOCK; ++i) {
        int ret;
        size_t slot_count;

        if (!rtfsDirIsBitmapBitSet(dentry_block->dentry_bitmap, i)) {
            continue;
        }

        ret = rtfsDirInodeBuildRegularEntryAt(dir_inode, dentry_block, block_index, i);
        if (ret != 0) {
            dir_inode->i_dentry_num = disk_dentry_num;
            dir_inode->is_dirty = was_dirty;
            return ret;
        }

        slot_count = GET_DENTRY_SLOTS(dentry_block->dentry[i].name_len);
        if (slot_count > 0) {
            i += slot_count - 1;
        }
    }

    dir_inode->i_dentry_num = disk_dentry_num;
    dir_inode->is_dirty = was_dirty;
    return 0;
}

/**
 * @brief 解析一个 inline dentry 槽位对应的目录项。
 */
static int rtfsDirInodeBuildInlineEntryAt(
    RtfsDirInode *dir_inode,
    const struct RtfsInlineDentry *inline_dentry,
    size_t index
)
{
    const struct RtfsDirEntry *disk_entry;
    char name[RTFS_NAME_LEN + 1];
    size_t name_len;
    size_t slot_count;
    size_t slot;
    size_t offset;

    if (dir_inode == NULL || inline_dentry == NULL || index >= NR_INLINE_DENTRY) {
        return EINVAL;
    }

    disk_entry = &inline_dentry->dentry[index];
    name_len = disk_entry->name_len;
    if (name_len == 0 || name_len > RTFS_NAME_LEN) {
        return EINVAL;
    }

    slot_count = GET_DENTRY_SLOTS(name_len);
    if (index + slot_count > NR_INLINE_DENTRY) {
        return EINVAL;
    }

    memset(name, 0, sizeof(name));
    offset = 0;
    for (slot = 0; slot < slot_count; ++slot) {
        size_t copy_len = RTFS_SLOT_LEN;

        if (offset + copy_len > name_len) {
            copy_len = name_len - offset;
        }

        memcpy(name + offset, inline_dentry->filename[index + slot], copy_len);
        offset += copy_len;
    }
    name[name_len] = '\0';

    {
        int ret = rtfsDirInodeAddEntryInternal(
            dir_inode,
            name,
            disk_entry->ino,
            disk_entry->file_type
        );
        if (ret == 0) {
            RtfsMemDirEntry *entry = &dir_inode->entries[dir_inode->entry_count - 1];
            entry->source_is_inline = true;
            entry->source_block_index = 0;
            entry->source_slot_index = (uint16_t)index;
        }
        return ret;
    }
}

/**
 * @brief 解析一个普通 dentry 槽位对应的目录项。
 */
static int rtfsDirInodeBuildRegularEntryAt(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block,
    uint32_t block_index,
    size_t index
)
{
    const struct RtfsDirEntry *disk_entry;
    char name[RTFS_NAME_LEN + 1];
    size_t name_len;
    size_t slot_count;
    size_t slot;
    size_t offset;

    if (dir_inode == NULL || dentry_block == NULL || index >= NR_DENTRY_IN_BLOCK) {
        return EINVAL;
    }

    disk_entry = &dentry_block->dentry[index];
    name_len = disk_entry->name_len;
    if (name_len == 0 || name_len > RTFS_NAME_LEN) {
        return EINVAL;
    }

    slot_count = GET_DENTRY_SLOTS(name_len);
    if (index + slot_count > NR_DENTRY_IN_BLOCK) {
        return EINVAL;
    }

    memset(name, 0, sizeof(name));
    offset = 0;
    for (slot = 0; slot < slot_count; ++slot) {
        size_t copy_len = RTFS_SLOT_LEN;

        if (offset + copy_len > name_len) {
            copy_len = name_len - offset;
        }

        memcpy(name + offset, dentry_block->filename[index + slot], copy_len);
        offset += copy_len;
    }
    name[name_len] = '\0';

    if ((name_len == 1 && name[0] == '.') ||
        (name_len == 2 && name[0] == '.' && name[1] == '.')) {
        return 0;
    }

    {
        int ret = rtfsDirInodeAddEntryInternal(
            dir_inode,
            name,
            disk_entry->ino,
            disk_entry->file_type
        );
        if (ret == 0) {
            RtfsMemDirEntry *entry = &dir_inode->entries[dir_inode->entry_count - 1];
            entry->source_is_inline = false;
            entry->source_block_index = block_index;
            entry->source_slot_index = (uint16_t)index;
        }
        return ret;
    }
}

/**
 * @brief 从目录对象提取最小运行时 inode 视图。
 */
static void rtfsDirBuildViewFromDirInode(
    const RtfsDirInode *dir_inode,
    RtfsRuntimeInodeView *view
)
{
    if (dir_inode == NULL || view == NULL) {
        return;
    }

    rtfsRuntimeInodeViewInit(
        view,
        dir_inode->ino,
        dir_inode->parent_ino,
        dir_inode->file_type
    );
}

/**
 * @brief 用给定 inode 视图和名字填充一个 dirent。
 */
static int rtfsDirFillDirent(
    struct dirent *entry,
    const RtfsRuntimeInodeView *inode_view,
    const char *name
)
{
    size_t namelen;

    if (entry == NULL || inode_view == NULL || name == NULL) {
        return EINVAL;
    }

    namelen = strnlen(name, RTFS_NAME_LEN + 1);
    if (namelen == 0 || namelen > RTFS_NAME_LEN) {
        return EINVAL;
    }

    memset(entry, 0, sizeof(*entry));
    entry->d_ino = (ino_t)inode_view->ino;
    entry->d_reclen = sizeof(*entry);
    entry->d_namlen = namelen;
#ifdef DT_DIR
    entry->d_type = rtfsInodeIsDirectoryType(inode_view->file_type) ? DT_DIR : DT_REG;
#else
    (void)inode_view;
#endif
    memcpy(entry->d_name, name, namelen);
    entry->d_name[namelen] = '\0';

    return 0;
}

/**
 * @brief 按顺序索引获取目录项，包括动态生成的 . 和 ..
 */
static int rtfsDirGetEntryByIndex(
    const RtfsDirInode *dir_inode,
    size_t index,
    struct dirent *entry
)
{
    RtfsRuntimeInodeView inode_view;

    if (dir_inode == NULL || entry == NULL) {
        return EINVAL;
    }

    rtfsDirBuildViewFromDirInode(dir_inode, &inode_view);

    if (index == 0) {
        return rtfsDirFillDirent(entry, &inode_view, ".");
    }

    if (index == 1) {
        RtfsRuntimeInodeView parent_view;

        rtfsRuntimeInodeViewInit(
            &parent_view,
            dir_inode->parent_ino,
            dir_inode->parent_ino,
            RTFS_FT_DIR
        );
        return rtfsDirFillDirent(entry, &parent_view, "..");
    }

    index -= RTFS_DIR_STATIC_ENTRY_COUNT;
    if (index >= dir_inode->entry_count) {
        return ENOENT;
    }

    return rtfsDirFillDirent(
        entry,
        &(RtfsRuntimeInodeView){
            .ino = dir_inode->entries[index].ino,
            .parent_ino = dir_inode->ino,
            .file_type = dir_inode->entries[index].file_type,
        },
        dir_inode->entries[index].name
    );
}

/**
 * @brief 在运行时目录项数组中查找指定名字。
 */
static const RtfsMemDirEntry *rtfsDirInodeFindEntry(
    const RtfsDirInode *dir_inode,
    const char *name,
    size_t namelen
)
{
    size_t i;

    if (dir_inode == NULL || name == NULL) {
        return NULL;
    }

    for (i = 0; i < dir_inode->entry_count; ++i) {
        if (strlen(dir_inode->entries[i].name) == namelen &&
            memcmp(dir_inode->entries[i].name, name, namelen) == 0) {
            return &dir_inode->entries[i];
        }
    }

    return NULL;
}

/**
 * @brief 执行目录项插入的内部实现。
 */
static int rtfsDirInodeAddEntryInternal(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
)
{
    RtfsMemDirEntry *new_entries;
    size_t namelen;

    if (dir_inode == NULL || name == NULL) {
        return EINVAL;
    }

    namelen = strnlen(name, RTFS_NAME_LEN + 1);
    if (namelen == 0 || namelen > RTFS_NAME_LEN) {
        return ENAMETOOLONG;
    }

    if (rtfsDirInodeFindEntry(dir_inode, name, namelen) != NULL) {
        return EEXIST;
    }

    if (dir_inode->entry_count == dir_inode->capacity) {
        size_t new_capacity = dir_inode->capacity == 0 ? 4 : dir_inode->capacity * 2;
        new_entries = realloc(dir_inode->entries, new_capacity * sizeof(*new_entries));
        if (new_entries == NULL) {
            return ENOMEM;
        }
        dir_inode->entries = new_entries;
        dir_inode->capacity = new_capacity;
    }

    memset(&dir_inode->entries[dir_inode->entry_count], 0,
           sizeof(dir_inode->entries[dir_inode->entry_count]));
    dir_inode->entries[dir_inode->entry_count].ino = child_ino;
    dir_inode->entries[dir_inode->entry_count].file_type = child_type;
    memcpy(dir_inode->entries[dir_inode->entry_count].name, name, namelen);
    dir_inode->entries[dir_inode->entry_count].name[namelen] = '\0';

    dir_inode->entry_count++;
    dir_inode->i_dentry_num++;
    dir_inode->is_dirty = true;

    return 0;
}

/**
 * @brief 执行目录项删除的内部实现。
 */
static int rtfsDirInodeRemoveEntryInternal(
    RtfsDirInode *dir_inode,
    const char *name
)
{
    size_t i;
    size_t namelen;

    if (dir_inode == NULL || name == NULL) {
        return EINVAL;
    }

    namelen = strnlen(name, RTFS_NAME_LEN + 1);
    if (namelen == 0 || namelen > RTFS_NAME_LEN) {
        return ENAMETOOLONG;
    }

    for (i = 0; i < dir_inode->entry_count; ++i) {
        if (strlen(dir_inode->entries[i].name) == namelen &&
            memcmp(dir_inode->entries[i].name, name, namelen) == 0) {
            RtfsMemDirEntry removed_entry = dir_inode->entries[i];

            if (removed_entry.source_is_inline) {
                rtfsDirInodeClearInlineEntryBacking(dir_inode, &removed_entry);
            } else {
                rtfsDirInodeClearRegularEntryBacking(dir_inode, &removed_entry);
            }

            if (i + 1 < dir_inode->entry_count) {
                dir_inode->entries[i] = dir_inode->entries[dir_inode->entry_count - 1];
            }

            dir_inode->entry_count--;
            if (dir_inode->i_dentry_num > 0) {
                dir_inode->i_dentry_num--;
            }
            rtfsDirInodeTouchMetadata(dir_inode);
            return 0;
        }
    }

    return ENOENT;
}

/**
 * @brief 判断位图中的某一位是否被置位。
 */
static bool rtfsDirIsBitmapBitSet(const uint8_t *bitmap, size_t bit_index)
{
    if (bitmap == NULL) {
        return false;
    }

    return (bitmap[bit_index / 8] & (uint8_t)(1u << (bit_index % 8))) != 0;
}

static int rtfsDirInodeRememberLoadedBlock(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block,
    uint32_t block_index,
    uint32_t lpa
)
{
    RtfsLoadedDirBlock *new_blocks;
    struct RtfsDentryBlock *block_copy;

    if (dir_inode == NULL || dentry_block == NULL) {
        return EINVAL;
    }

    if (rtfsDirInodeFindLoadedBlock(dir_inode, block_index) != NULL) {
        return 0;
    }

    if (dir_inode->loaded_block_count_actual == dir_inode->loaded_block_slots) {
        size_t new_slots = dir_inode->loaded_block_slots == 0 ? 4 : dir_inode->loaded_block_slots * 2;
        new_blocks = realloc(dir_inode->loaded_blocks, new_slots * sizeof(*new_blocks));
        if (new_blocks == NULL) {
            return ENOMEM;
        }
        dir_inode->loaded_blocks = new_blocks;
        dir_inode->loaded_block_slots = new_slots;
    }

    block_copy = malloc(sizeof(*block_copy));
    if (block_copy == NULL) {
        return ENOMEM;
    }

    memcpy(block_copy, dentry_block, sizeof(*block_copy));
    dir_inode->loaded_blocks[dir_inode->loaded_block_count_actual].block_index = block_index;
    dir_inode->loaded_blocks[dir_inode->loaded_block_count_actual].lpa = lpa;
    dir_inode->loaded_blocks[dir_inode->loaded_block_count_actual].cow_new_lpa = INVALID_LPA;
    dir_inode->loaded_blocks[dir_inode->loaded_block_count_actual].has_pending_cow_relocation = false;
    dir_inode->loaded_blocks[dir_inode->loaded_block_count_actual].block = block_copy;
    dir_inode->loaded_blocks[dir_inode->loaded_block_count_actual].is_dirty = false;
    dir_inode->loaded_block_count_actual++;
    return 0;
}

static RtfsLoadedDirBlock *rtfsDirInodeFindLoadedBlock(
    RtfsDirInode *dir_inode,
    uint32_t block_index
)
{
    size_t i;

    if (dir_inode == NULL) {
        return NULL;
    }

    for (i = 0; i < dir_inode->loaded_block_count_actual; ++i) {
        if (dir_inode->loaded_blocks[i].block_index == block_index) {
            return &dir_inode->loaded_blocks[i];
        }
    }

    return NULL;
}

static void rtfsDirInodeTouchMetadata(RtfsDirInode *dir_inode)
{
    if (dir_inode == NULL) {
        return;
    }

    dir_inode->is_dirty = true;
    dir_inode->i_mtime++;

    if (dir_inode->disk_inode != NULL) {
        dir_inode->disk_inode->i_dentry_num = dir_inode->i_dentry_num;
        dir_inode->disk_inode->i_mtime = dir_inode->i_mtime;
    }
}

static void rtfsDirInodeClearDentryBit(
    uint8_t *bitmap,
    size_t bit_index
)
{
    if (bitmap == NULL) {
        return;
    }

    bitmap[bit_index / 8] &= (uint8_t)~(1u << (bit_index % 8));
}

static void rtfsDirInodeSetDentryBit(
    uint8_t *bitmap,
    size_t bit_index
)
{
    if (bitmap == NULL) {
        return;
    }

    bitmap[bit_index / 8] |= (uint8_t)(1u << (bit_index % 8));
}

static void rtfsDirInodeClearRegularEntryBacking(
    RtfsDirInode *dir_inode,
    const RtfsMemDirEntry *entry
)
{
    RtfsLoadedDirBlock *loaded_block;
    size_t slot_count;
    size_t slot;

    if (dir_inode == NULL || entry == NULL) {
        return;
    }

    loaded_block = rtfsDirInodeFindLoadedBlock(dir_inode, entry->source_block_index);
    if (loaded_block == NULL || loaded_block->block == NULL) {
        return;
    }

    slot_count = GET_DENTRY_SLOTS(strlen(entry->name));
    for (slot = 0; slot < slot_count; ++slot) {
        rtfsDirInodeClearDentryBit(
            loaded_block->block->dentry_bitmap,
            entry->source_slot_index + slot
        );
        memset(
            loaded_block->block->filename[entry->source_slot_index + slot],
            0,
            RTFS_SLOT_LEN
        );
    }
    memset(&loaded_block->block->dentry[entry->source_slot_index], 0,
           sizeof(loaded_block->block->dentry[entry->source_slot_index]));
    loaded_block->is_dirty = true;
}

static void rtfsDirInodeClearInlineEntryBacking(
    RtfsDirInode *dir_inode,
    const RtfsMemDirEntry *entry
)
{
    size_t slot_count;
    size_t slot;

    if (dir_inode == NULL || entry == NULL || dir_inode->inline_dentry == NULL) {
        return;
    }

    slot_count = GET_DENTRY_SLOTS(strlen(entry->name));
    for (slot = 0; slot < slot_count; ++slot) {
        rtfsDirInodeClearDentryBit(
            dir_inode->inline_dentry->dentry_bitmap,
            entry->source_slot_index + slot
        );
        memset(
            dir_inode->inline_dentry->filename[entry->source_slot_index + slot],
            0,
            RTFS_SLOT_LEN
        );
    }
    memset(&dir_inode->inline_dentry->dentry[entry->source_slot_index], 0,
           sizeof(dir_inode->inline_dentry->dentry[entry->source_slot_index]));
}

static void rtfsDirInodeWriteInlineName(
    struct RtfsInlineDentry *inline_dentry,
    size_t index,
    const char *name
)
{
    size_t name_len;
    size_t slot_count;
    size_t slot;
    size_t offset;

    if (inline_dentry == NULL || name == NULL) {
        return;
    }

    name_len = strlen(name);
    slot_count = GET_DENTRY_SLOTS(name_len);
    offset = 0;

    for (slot = 0; slot < slot_count; ++slot) {
        size_t copy_len = RTFS_SLOT_LEN;

        if (offset + copy_len > name_len) {
            copy_len = name_len - offset;
        }

        memset(inline_dentry->filename[index + slot], 0, RTFS_SLOT_LEN);
        memcpy(inline_dentry->filename[index + slot], name + offset, copy_len);
        offset += copy_len;
    }
}

static void rtfsDirInodeWriteRegularName(
    struct RtfsDentryBlock *dentry_block,
    size_t index,
    const char *name
)
{
    size_t name_len;
    size_t slot_count;
    size_t slot;
    size_t offset;

    if (dentry_block == NULL || name == NULL) {
        return;
    }

    name_len = strlen(name);
    slot_count = GET_DENTRY_SLOTS(name_len);
    offset = 0;

    for (slot = 0; slot < slot_count; ++slot) {
        size_t copy_len = RTFS_SLOT_LEN;

        if (offset + copy_len > name_len) {
            copy_len = name_len - offset;
        }

        memset(dentry_block->filename[index + slot], 0, RTFS_SLOT_LEN);
        memcpy(dentry_block->filename[index + slot], name + offset, copy_len);
        offset += copy_len;
    }
}

static int rtfsDirInodeAddInlineBacking(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
)
{
    size_t namelen;
    size_t slot_count;
    size_t start_slot;
    size_t i;
    int ret;

    if (dir_inode == NULL || dir_inode->inline_dentry == NULL || dir_inode->disk_inode == NULL || name == NULL) {
        return EINVAL;
    }

    namelen = strnlen(name, RTFS_NAME_LEN + 1);
    if (namelen == 0 || namelen > RTFS_NAME_LEN) {
        return ENAMETOOLONG;
    }

    if (rtfsDirInodeFindEntry(dir_inode, name, namelen) != NULL) {
        return EEXIST;
    }

    slot_count = GET_DENTRY_SLOTS(namelen);
    start_slot = NR_INLINE_DENTRY;

    for (i = 0; i + slot_count <= NR_INLINE_DENTRY; ++i) {
        size_t slot;
        bool available = true;

        for (slot = 0; slot < slot_count; ++slot) {
            if (rtfsDirIsBitmapBitSet(dir_inode->inline_dentry->dentry_bitmap, i + slot)) {
                available = false;
                i += slot;
                break;
            }
        }

        if (available) {
            start_slot = i;
            break;
        }
    }

    if (start_slot == NR_INLINE_DENTRY) {
        int convert_ret = rtfsDirInodeConvertInlineToRegular(dir_inode);

        if (convert_ret != 0) {
            return convert_ret;
        }

        return rtfsDirInodeAddRegularBacking(dir_inode, name, child_ino, child_type);
    }

    dir_inode->inline_dentry->dentry[start_slot].ino = child_ino;
    dir_inode->inline_dentry->dentry[start_slot].name_len = namelen;
    dir_inode->inline_dentry->dentry[start_slot].file_type = child_type;
    dir_inode->inline_dentry->dentry[start_slot].hash_code = 0;

    for (i = 0; i < slot_count; ++i) {
        rtfsDirInodeSetDentryBit(dir_inode->inline_dentry->dentry_bitmap, start_slot + i);
    }
    rtfsDirInodeWriteInlineName(dir_inode->inline_dentry, start_slot, name);

    ret = rtfsDirInodeAddEntryInternal(dir_inode, name, child_ino, child_type);
    if (ret != 0) {
        for (i = 0; i < slot_count; ++i) {
            rtfsDirInodeClearDentryBit(dir_inode->inline_dentry->dentry_bitmap, start_slot + i);
            memset(dir_inode->inline_dentry->filename[start_slot + i], 0, RTFS_SLOT_LEN);
        }
        memset(&dir_inode->inline_dentry->dentry[start_slot], 0,
               sizeof(dir_inode->inline_dentry->dentry[start_slot]));
        return ret;
    }

    dir_inode->entries[dir_inode->entry_count - 1].source_is_inline = true;
    dir_inode->entries[dir_inode->entry_count - 1].source_block_index = 0;
    dir_inode->entries[dir_inode->entry_count - 1].source_slot_index = (uint16_t)start_slot;
    rtfsDirInodeTouchMetadata(dir_inode);
    return 0;
}

static int rtfsDirInodeConvertInlineToRegular(
    RtfsDirInode *dir_inode
)
{
    RtfsDirInodeNodeHandle *owned_handle;
    struct RtfsDentryBlock regular_block;
    RtfsLoadedDirBlock *loaded_block;
    uint8_t old_i_inline;
    uint64_t old_i_size;
    uint32_t old_i_addr[DEF_ADDRS_PER_INODE];
    uint32_t old_i_nid[DEF_NIDS_PER_INODE];
    uint16_t slot_map[NR_INLINE_DENTRY];
    size_t old_total_block_count;
    size_t old_loaded_block_count;
    bool old_is_fully_loaded;
    size_t regular_slot = 0;
    size_t i;
    int ret;

    if (dir_inode == NULL || dir_inode->disk_inode == NULL || dir_inode->inline_dentry == NULL) {
        return EINVAL;
    }

    if (dir_inode->backing_kind != RTFS_DIR_BACKING_INLINE || dir_inode->cache_handle == NULL) {
        return EINVAL;
    }

    memset(&regular_block, 0, sizeof(regular_block));
    memset(slot_map, 0xff, sizeof(slot_map));
    old_i_inline = dir_inode->disk_inode->i_inline;
    old_i_size = dir_inode->disk_inode->i_size;
    memcpy(old_i_addr, dir_inode->disk_inode->i_addr, sizeof(old_i_addr));
    memcpy(old_i_nid, dir_inode->disk_inode->i_nid, sizeof(old_i_nid));
    old_total_block_count = dir_inode->total_block_count;
    old_loaded_block_count = dir_inode->loaded_block_count;
    old_is_fully_loaded = dir_inode->is_fully_loaded;

    for (i = 0; i < NR_INLINE_DENTRY; ++i) {
        const struct RtfsDirEntry *src_entry;
        size_t slot_count;
        size_t slot;

        if (!rtfsDirIsBitmapBitSet(dir_inode->inline_dentry->dentry_bitmap, i)) {
            continue;
        }

        src_entry = &dir_inode->inline_dentry->dentry[i];
        if (src_entry->name_len == 0 || src_entry->name_len > RTFS_NAME_LEN) {
            return EINVAL;
        }

        slot_count = GET_DENTRY_SLOTS(src_entry->name_len);
        if (i + slot_count > NR_INLINE_DENTRY ||
            regular_slot + slot_count > NR_DENTRY_IN_BLOCK) {
            return ENOSPC;
        }

        slot_map[i] = (uint16_t)regular_slot;
        regular_block.dentry[regular_slot] = *src_entry;
        for (slot = 0; slot < slot_count; ++slot) {
            rtfsDirInodeSetDentryBit(regular_block.dentry_bitmap, regular_slot + slot);
            memcpy(
                regular_block.filename[regular_slot + slot],
                dir_inode->inline_dentry->filename[i + slot],
                RTFS_SLOT_LEN
            );
        }

        regular_slot += slot_count;
        i += slot_count - 1;
    }

    dir_inode->disk_inode->i_inline &= (uint8_t)~RTFS_INLINE_DENTRY;
    memset(dir_inode->disk_inode->i_addr, 0, sizeof(dir_inode->disk_inode->i_addr));
    memset(dir_inode->disk_inode->i_nid, 0, sizeof(dir_inode->disk_inode->i_nid));
    dir_inode->disk_inode->i_size = 0;
    dir_inode->i_size = 0;
    dir_inode->total_block_count = 0;
    dir_inode->loaded_block_count = 0;
    dir_inode->is_fully_loaded = true;

    ret = rtfsDirInodeGrowRegularBlock(dir_inode, NULL, INVALID_NID, RTFS_FT_UNKNOWN);
    if (ret != 0) {
        dir_inode->disk_inode->i_inline = old_i_inline;
        dir_inode->disk_inode->i_size = old_i_size;
        memcpy(dir_inode->disk_inode->i_addr, old_i_addr, sizeof(old_i_addr));
        memcpy(dir_inode->disk_inode->i_nid, old_i_nid, sizeof(old_i_nid));
        dir_inode->i_size = old_i_size;
        dir_inode->total_block_count = old_total_block_count;
        dir_inode->loaded_block_count = old_loaded_block_count;
        dir_inode->is_fully_loaded = old_is_fully_loaded;
        return ret;
    }

    loaded_block = &dir_inode->loaded_blocks[dir_inode->loaded_block_count_actual - 1];
    memcpy(loaded_block->block, &regular_block, sizeof(regular_block));
    loaded_block->is_dirty = true;
    dir_inode->backing_kind = RTFS_DIR_BACKING_BLOCKS;
    dir_inode->inline_dentry = NULL;
    dir_inode->loaded_block_count = dir_inode->total_block_count;
    dir_inode->is_fully_loaded = true;

    for (i = 0; i < dir_inode->entry_count; ++i) {
        RtfsMemDirEntry *entry = &dir_inode->entries[i];

        if (!entry->source_is_inline ||
            entry->source_slot_index >= NR_INLINE_DENTRY ||
            slot_map[entry->source_slot_index] == UINT16_MAX) {
            continue;
        }

        entry->source_is_inline = false;
        entry->source_block_index = loaded_block->block_index;
        entry->source_slot_index = slot_map[entry->source_slot_index];
    }

    owned_handle = (RtfsDirInodeNodeHandle *)dir_inode->cache_handle;
    nodeBlockCacheEntryHandleMarkDirty(&owned_handle->node_handle);
    return 0;
}

static int rtfsDirInodeAddRegularBacking(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
)
{
    size_t namelen;
    size_t slot_count;
    size_t i;

    if (dir_inode == NULL || name == NULL) {
        return EINVAL;
    }

    namelen = strnlen(name, RTFS_NAME_LEN + 1);
    if (namelen == 0 || namelen > RTFS_NAME_LEN) {
        return ENAMETOOLONG;
    }

    if (rtfsDirInodeFindEntry(dir_inode, name, namelen) != NULL) {
        return EEXIST;
    }

    slot_count = GET_DENTRY_SLOTS(namelen);

    for (i = 0; i < dir_inode->loaded_block_count_actual; ++i) {
        RtfsLoadedDirBlock *loaded_block = &dir_inode->loaded_blocks[i];
        size_t start_slot;

        if (loaded_block->block == NULL) {
            continue;
        }

        for (start_slot = 0; start_slot + slot_count <= NR_DENTRY_IN_BLOCK; ++start_slot) {
            size_t slot;
            bool available = true;
            int ret;

            for (slot = 0; slot < slot_count; ++slot) {
                if (rtfsDirIsBitmapBitSet(loaded_block->block->dentry_bitmap, start_slot + slot)) {
                    available = false;
                    start_slot += slot;
                    break;
                }
            }

            if (!available) {
                continue;
            }

            loaded_block->block->dentry[start_slot].ino = child_ino;
            loaded_block->block->dentry[start_slot].name_len = namelen;
            loaded_block->block->dentry[start_slot].file_type = child_type;
            loaded_block->block->dentry[start_slot].hash_code = 0;

            for (slot = 0; slot < slot_count; ++slot) {
                rtfsDirInodeSetDentryBit(loaded_block->block->dentry_bitmap, start_slot + slot);
            }
            rtfsDirInodeWriteRegularName(loaded_block->block, start_slot, name);

            ret = rtfsDirInodeAddEntryInternal(dir_inode, name, child_ino, child_type);
            if (ret != 0) {
                for (slot = 0; slot < slot_count; ++slot) {
                    rtfsDirInodeClearDentryBit(loaded_block->block->dentry_bitmap, start_slot + slot);
                    memset(loaded_block->block->filename[start_slot + slot], 0, RTFS_SLOT_LEN);
                }
                memset(&loaded_block->block->dentry[start_slot], 0,
                       sizeof(loaded_block->block->dentry[start_slot]));
                return ret;
            }

            dir_inode->entries[dir_inode->entry_count - 1].source_is_inline = false;
            dir_inode->entries[dir_inode->entry_count - 1].source_block_index = loaded_block->block_index;
            dir_inode->entries[dir_inode->entry_count - 1].source_slot_index = (uint16_t)start_slot;
            loaded_block->is_dirty = true;
            rtfsDirInodeTouchMetadata(dir_inode);
            return 0;
        }
    }

    return rtfsDirInodeGrowRegularBlock(dir_inode, name, child_ino, child_type);
}

static int rtfsDirInodeGrowRegularBlock(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
)
{
    RtfsDirInodeNodeHandle *owned_handle;
    file_system_manager *fs_manager;
    super_manager *sp_manager;
    NodeBlockCacheHelper helper;
    NodeBlockCacheEntryHandle created_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *created_node;
    struct RtfsDentryBlock new_block;
    uint32_t new_block_index;
    uint32_t new_lpa;
    uint32_t local_index;
    int ret;
    bool mapping_attached = false;
    SitOperator sit_op;

    if (dir_inode == NULL || dir_inode->disk_inode == NULL || dir_inode->cache_handle == NULL) {
        return ENOSPC;
    }

    owned_handle = (RtfsDirInodeNodeHandle *)dir_inode->cache_handle;
    if (owned_handle->node_cache == NULL || owned_handle->node_handle.entry == NULL) {
        return ENOSPC;
    }

    fs_manager = owned_handle->node_cache->fsManager;
    if (fs_manager == NULL) {
        return ENOSPC;
    }

    sp_manager = fileSystemManagerGetSuperManager(fs_manager);
    if (sp_manager == NULL) {
        return ENOSPC;
    }

    new_block_index = (uint32_t)dir_inode->total_block_count;
    new_lpa = superManagerAllocDataLpa(sp_manager);
    if (new_lpa == INVALID_LPA) {
        return ENOSPC;
    }
    sitOperatorInit(&sit_op, fs_manager);

    memset(&new_block, 0, sizeof(new_block));
    if (new_block_index < DEF_ADDRS_PER_INODE) {
        dir_inode->disk_inode->i_addr[new_block_index] = new_lpa;
        mapping_attached = true;
    } else if (new_block_index < DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK) {
        uint32_t direct_node_slot = (new_block_index - DEF_ADDRS_PER_INODE) / DEF_ADDRS_PER_BLOCK;

        local_index = (new_block_index - DEF_ADDRS_PER_INODE) % DEF_ADDRS_PER_BLOCK;
        if (dir_inode->disk_inode->i_nid[direct_node_slot] == INVALID_NID) {
            nodeBlockCacheHelperInit(&helper, fs_manager);
            created_handle = nodeBlockCacheHelperCreateNodeEntry(
                &helper,
                dir_inode->ino,
                DEF_ADDRS_PER_INODE + direct_node_slot + 1,
                (uint32_t)dir_inode->ino
            );
            nodeBlockCacheHelperDestroy(&helper);
            if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry);
            rtfsDirInodeInitCreatedDirectNode(created_node);
            dir_inode->disk_inode->i_nid[direct_node_slot] = created_node->footer.nid;
        }

        {
            NodeBlockCacheEntryHandle direct_handle =
                nodeBlockCacheGet(owned_handle->node_cache, dir_inode->disk_inode->i_nid[direct_node_slot]);
            if (nodeBlockCacheEntryHandleIsEmpty(&direct_handle)) {
                nodeBlockCacheEntryHandleDestroy(&created_handle);
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
            created_node->dn.addr[local_index] = new_lpa;
            nodeBlockCacheEntryHandleMarkDirty(&direct_handle);
            nodeBlockCacheEntryHandleDestroy(&direct_handle);
        }
        nodeBlockCacheEntryHandleDestroy(&created_handle);
        mapping_attached = true;
    } else if (new_block_index <
               DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK +
                   2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK) {
        uint32_t indirect_block_index =
            new_block_index - DEF_ADDRS_PER_INODE - 2U * DEF_ADDRS_PER_BLOCK;
        uint32_t indirect_node_slot = indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        uint32_t indirect_node_offset = indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        uint32_t direct_node_slot = indirect_node_offset / DEF_ADDRS_PER_BLOCK;

        local_index = indirect_node_offset % DEF_ADDRS_PER_BLOCK;
        if (dir_inode->disk_inode->i_nid[2 + indirect_node_slot] == INVALID_NID) {
            nodeBlockCacheHelperInit(&helper, fs_manager);
            created_handle = nodeBlockCacheHelperCreateNodeEntry(
                &helper,
                dir_inode->ino,
                NODE_IND1_BLOCK + indirect_node_slot,
                (uint32_t)dir_inode->ino
            );
            nodeBlockCacheHelperDestroy(&helper);
            if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry);
            rtfsDirInodeInitCreatedIndirectNode(created_node);
            dir_inode->disk_inode->i_nid[2 + indirect_node_slot] = created_node->footer.nid;
            nodeBlockCacheEntryHandleDestroy(&created_handle);
            created_handle.entry = NULL;
            created_handle.cache = NULL;
        }

        {
            NodeBlockCacheEntryHandle indirect_handle =
                nodeBlockCacheGet(owned_handle->node_cache, dir_inode->disk_inode->i_nid[2 + indirect_node_slot]);
            if (nodeBlockCacheEntryHandleIsEmpty(&indirect_handle)) {
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
            if (created_node->in.nid[direct_node_slot] == INVALID_NID) {
                nodeBlockCacheHelperInit(&helper, fs_manager);
                created_handle = nodeBlockCacheHelperCreateNodeEntry(
                    &helper,
                    dir_inode->ino,
                    0,
                    created_node->footer.nid
                );
                nodeBlockCacheHelperDestroy(&helper);
                if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                    nodeBlockCacheEntryHandleDestroy(&indirect_handle);
                    goto rollback_new_lpa;
                }

                rtfsDirInodeInitCreatedDirectNode(
                    nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry)
                );
                created_node->in.nid[direct_node_slot] =
                    nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry)->footer.nid;
                nodeBlockCacheEntryHandleMarkDirty(&indirect_handle);
            }
            nodeBlockCacheEntryHandleDestroy(&indirect_handle);
        }

        {
            NodeBlockCacheEntryHandle indirect_handle =
                nodeBlockCacheGet(owned_handle->node_cache, dir_inode->disk_inode->i_nid[2 + indirect_node_slot]);
            NodeBlockCacheEntryHandle direct_handle;

            if (nodeBlockCacheEntryHandleIsEmpty(&indirect_handle)) {
                nodeBlockCacheEntryHandleDestroy(&created_handle);
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
            direct_handle =
                nodeBlockCacheGet(owned_handle->node_cache, created_node->in.nid[direct_node_slot]);
            nodeBlockCacheEntryHandleDestroy(&indirect_handle);
            if (nodeBlockCacheEntryHandleIsEmpty(&direct_handle)) {
                nodeBlockCacheEntryHandleDestroy(&created_handle);
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
            created_node->dn.addr[local_index] = new_lpa;
            nodeBlockCacheEntryHandleMarkDirty(&direct_handle);
            nodeBlockCacheEntryHandleDestroy(&direct_handle);
        }
        nodeBlockCacheEntryHandleDestroy(&created_handle);
        mapping_attached = true;
    } else if (new_block_index <
               DEF_ADDRS_PER_INODE + 2U * DEF_ADDRS_PER_BLOCK +
                   2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK +
                   NIDS_PER_BLOCK * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK) {
        uint32_t double_indirect_block_index =
            new_block_index - DEF_ADDRS_PER_INODE -
            2U * DEF_ADDRS_PER_BLOCK -
            2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
        uint32_t first_level_slot = double_indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        uint32_t double_indirect_offset = double_indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        uint32_t second_level_slot = double_indirect_offset / DEF_ADDRS_PER_BLOCK;
        uint32_t dind_local_index = double_indirect_offset % DEF_ADDRS_PER_BLOCK;

        if (dir_inode->disk_inode->i_nid[4] == INVALID_NID) {
            nodeBlockCacheHelperInit(&helper, fs_manager);
            created_handle = nodeBlockCacheHelperCreateNodeEntry(
                &helper,
                dir_inode->ino,
                NODE_DIND_BLOCK,
                (uint32_t)dir_inode->ino
            );
            nodeBlockCacheHelperDestroy(&helper);
            if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry);
            rtfsDirInodeInitCreatedIndirectNode(created_node);
            dir_inode->disk_inode->i_nid[4] = created_node->footer.nid;
            nodeBlockCacheEntryHandleDestroy(&created_handle);
            created_handle.entry = NULL;
            created_handle.cache = NULL;
        }

        {
            NodeBlockCacheEntryHandle dind_handle =
                nodeBlockCacheGet(owned_handle->node_cache, dir_inode->disk_inode->i_nid[4]);
            if (nodeBlockCacheEntryHandleIsEmpty(&dind_handle)) {
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(dind_handle.entry);
            if (created_node->in.nid[first_level_slot] == INVALID_NID) {
                nodeBlockCacheHelperInit(&helper, fs_manager);
                created_handle = nodeBlockCacheHelperCreateNodeEntry(
                    &helper,
                    dir_inode->ino,
                    0,
                    created_node->footer.nid
                );
                nodeBlockCacheHelperDestroy(&helper);
                if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                    nodeBlockCacheEntryHandleDestroy(&dind_handle);
                    goto rollback_new_lpa;
                }

                rtfsDirInodeInitCreatedIndirectNode(
                    nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry)
                );
                created_node->in.nid[first_level_slot] =
                    nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry)->footer.nid;
                nodeBlockCacheEntryHandleMarkDirty(&dind_handle);
                nodeBlockCacheEntryHandleDestroy(&created_handle);
                created_handle.entry = NULL;
                created_handle.cache = NULL;
            }
            nodeBlockCacheEntryHandleDestroy(&dind_handle);
        }

        {
            NodeBlockCacheEntryHandle ind_handle =
                nodeBlockCacheGet(owned_handle->node_cache, dir_inode->disk_inode->i_nid[4]);
            NodeBlockCacheEntryHandle level1_handle;

            if (nodeBlockCacheEntryHandleIsEmpty(&ind_handle)) {
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(ind_handle.entry);
            level1_handle =
                nodeBlockCacheGet(owned_handle->node_cache, created_node->in.nid[first_level_slot]);
            nodeBlockCacheEntryHandleDestroy(&ind_handle);
            if (nodeBlockCacheEntryHandleIsEmpty(&level1_handle)) {
                return ENOSPC;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(level1_handle.entry);
            if (created_node->in.nid[second_level_slot] == INVALID_NID) {
                nodeBlockCacheHelperInit(&helper, fs_manager);
                created_handle = nodeBlockCacheHelperCreateNodeEntry(
                    &helper,
                    dir_inode->ino,
                    0,
                    created_node->footer.nid
                );
                nodeBlockCacheHelperDestroy(&helper);
                if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                    nodeBlockCacheEntryHandleDestroy(&level1_handle);
                    goto rollback_new_lpa;
                }

                rtfsDirInodeInitCreatedDirectNode(
                    nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry)
                );
                created_node->in.nid[second_level_slot] =
                    nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry)->footer.nid;
                nodeBlockCacheEntryHandleMarkDirty(&level1_handle);
            }
            nodeBlockCacheEntryHandleDestroy(&level1_handle);
        }

        {
            NodeBlockCacheEntryHandle dind_handle =
                nodeBlockCacheGet(owned_handle->node_cache, dir_inode->disk_inode->i_nid[4]);
            NodeBlockCacheEntryHandle level1_handle;
            NodeBlockCacheEntryHandle direct_handle;

            if (nodeBlockCacheEntryHandleIsEmpty(&dind_handle)) {
                nodeBlockCacheEntryHandleDestroy(&created_handle);
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(dind_handle.entry);
            level1_handle =
                nodeBlockCacheGet(owned_handle->node_cache, created_node->in.nid[first_level_slot]);
            nodeBlockCacheEntryHandleDestroy(&dind_handle);
            if (nodeBlockCacheEntryHandleIsEmpty(&level1_handle)) {
                nodeBlockCacheEntryHandleDestroy(&created_handle);
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(level1_handle.entry);
            direct_handle =
                nodeBlockCacheGet(owned_handle->node_cache, created_node->in.nid[second_level_slot]);
            nodeBlockCacheEntryHandleDestroy(&level1_handle);
            if (nodeBlockCacheEntryHandleIsEmpty(&direct_handle)) {
                nodeBlockCacheEntryHandleDestroy(&created_handle);
                goto rollback_new_lpa;
            }

            created_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
            created_node->dn.addr[dind_local_index] = new_lpa;
            nodeBlockCacheEntryHandleMarkDirty(&direct_handle);
            nodeBlockCacheEntryHandleDestroy(&direct_handle);
        }
        nodeBlockCacheEntryHandleDestroy(&created_handle);
        mapping_attached = true;
    } else {
        sitInvalidateLpa(&sit_op, new_lpa);
        return ENOSPC;
    }

    dir_inode->disk_inode->i_size += BLOCK_BUFFER_SIZE;
    dir_inode->i_size = dir_inode->disk_inode->i_size;
    dir_inode->total_block_count++;
    dir_inode->is_fully_loaded = false;

    ret = rtfsDirInodeRememberLoadedBlock(dir_inode, &new_block, new_block_index, new_lpa);
    if (ret != 0) {
        if (mapping_attached && new_block_index < DEF_ADDRS_PER_INODE) {
            dir_inode->disk_inode->i_addr[new_block_index] = INVALID_LPA;
        }
        dir_inode->disk_inode->i_size -= BLOCK_BUFFER_SIZE;
        dir_inode->i_size = dir_inode->disk_inode->i_size;
        dir_inode->total_block_count--;
        dir_inode->is_fully_loaded =
            dir_inode->loaded_block_count >= dir_inode->total_block_count;
        sitInvalidateLpa(&sit_op, new_lpa);
        return ret;
    }

    if (name == NULL) {
        RtfsLoadedDirBlock *loaded_block =
            &dir_inode->loaded_blocks[dir_inode->loaded_block_count_actual - 1];

        loaded_block->is_dirty = true;
        return 0;
    }

    ret = rtfsDirInodeAddRegularBacking(dir_inode, name, child_ino, child_type);
    if (ret != 0) {
        if (mapping_attached && new_block_index < DEF_ADDRS_PER_INODE) {
            dir_inode->disk_inode->i_addr[new_block_index] = INVALID_LPA;
        }
        dir_inode->disk_inode->i_size -= BLOCK_BUFFER_SIZE;
        dir_inode->i_size = dir_inode->disk_inode->i_size;
        dir_inode->total_block_count--;
        dir_inode->is_fully_loaded =
            dir_inode->loaded_block_count >= dir_inode->total_block_count;
        sitInvalidateLpa(&sit_op, new_lpa);
    }

    return ret;

rollback_new_lpa:
    sitInvalidateLpa(&sit_op, new_lpa);
    return ENOSPC;
}

static void rtfsDirInodeInitCreatedDirectNode(struct RtfsNode *node)
{
    size_t i;

    if (node == NULL) {
        return;
    }

    for (i = 0; i < DEF_ADDRS_PER_BLOCK; ++i) {
        node->dn.addr[i] = INVALID_LPA;
    }
}

static void rtfsDirInodeInitCreatedIndirectNode(struct RtfsNode *node)
{
    size_t i;

    if (node == NULL) {
        return;
    }

    for (i = 0; i < NIDS_PER_BLOCK; ++i) {
        node->in.nid[i] = INVALID_NID;
    }
}

static int rtfsDirInodeWriteDirtyRegularBlocksCow(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
)
{
    struct comm_dev *dev;
    super_manager *sp_manager;
    size_t i;

    if (fs_manager == NULL || dir_inode == NULL) {
        return EINVAL;
    }

    dev = fileSystemManagerGetDevice(fs_manager);
    sp_manager = fileSystemManagerGetSuperManager(fs_manager);
    if (dev == NULL || sp_manager == NULL) {
        return EINVAL;
    }

    for (i = 0; i < dir_inode->loaded_block_count_actual; ++i) {
        RtfsLoadedDirBlock *loaded_block = &dir_inode->loaded_blocks[i];
        BlockBuffer buffer;
        uint32_t new_lpa;

        if (loaded_block->block == NULL || !loaded_block->is_dirty) {
            continue;
        }

        new_lpa = superManagerAllocDataLpa(sp_manager);
        if (new_lpa == INVALID_LPA) {
            return ENOSPC;
        }

        if (g_rtfs_dir_inode_write_block_hook != NULL) {
            int res = g_rtfs_dir_inode_write_block_hook(dev, new_lpa, loaded_block->block);
            if (res != 0) {
                return res;
            }
        } else {
            blockBufferInit(&buffer);
            blockBufferCopyContentFromBuf(&buffer, (const char *)loaded_block->block);
            blockBufferWriteToLpaSync(&buffer, dev, new_lpa);
            blockBufferDestroy(&buffer);
        }

        loaded_block->cow_new_lpa = new_lpa;
        loaded_block->has_pending_cow_relocation = true;
    }

    return 0;
}

static int rtfsDirInodeApplyPendingRegularBlockRelocations(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
)
{
    size_t i;

    if (fs_manager == NULL || dir_inode == NULL) {
        return EINVAL;
    }

    for (i = 0; i < dir_inode->loaded_block_count_actual; ++i) {
        RtfsLoadedDirBlock *loaded_block = &dir_inode->loaded_blocks[i];
        int ret;

        if (!loaded_block->has_pending_cow_relocation ||
            loaded_block->cow_new_lpa == INVALID_LPA) {
            continue;
        }

        ret = rtfsDirInodeCanRelocateOneRegularBlock(
            dir_inode,
            loaded_block->block_index
        );
        if (ret != 0) {
            return ret;
        }
    }

    for (i = 0; i < dir_inode->loaded_block_count_actual; ++i) {
        RtfsLoadedDirBlock *loaded_block = &dir_inode->loaded_blocks[i];
        int ret;

        if (!loaded_block->has_pending_cow_relocation ||
            loaded_block->cow_new_lpa == INVALID_LPA) {
            continue;
        }

        ret = rtfsDirInodeRelocateOneRegularBlock(
            fs_manager,
            dir_inode,
            loaded_block->block_index,
            loaded_block->cow_new_lpa
        );
        if (ret != 0) {
            return ret;
        }

        loaded_block->lpa = loaded_block->cow_new_lpa;
        loaded_block->cow_new_lpa = INVALID_LPA;
        loaded_block->has_pending_cow_relocation = false;
        loaded_block->is_dirty = false;
    }

    return 0;
}

static int rtfsDirInodeCanRelocateOneRegularBlock(
    RtfsDirInode *dir_inode,
    uint32_t block_index
)
{
    RtfsDirInodeNodeHandle *owned_handle;
    NodeBlockCache *node_cache;
    uint32_t direct_block_index;
    uint32_t indirect_block_index;
    uint32_t double_indirect_block_index;
    uint32_t direct_node_slot;
    uint32_t indirect_node_slot;
    uint32_t indirect_node_offset;
    uint32_t first_level_slot;
    uint32_t second_level_slot;
    uint32_t double_indirect_offset;

    if (dir_inode == NULL || dir_inode->disk_inode == NULL || dir_inode->cache_handle == NULL) {
        return EINVAL;
    }

    owned_handle = (RtfsDirInodeNodeHandle *)dir_inode->cache_handle;
    node_cache = owned_handle->node_cache;
    if (node_cache == NULL) {
        return EINVAL;
    }

    if (block_index < DEF_ADDRS_PER_INODE) {
        return 0;
    }

    direct_block_index = block_index - DEF_ADDRS_PER_INODE;
    if (direct_block_index < (2U * DEF_ADDRS_PER_BLOCK)) {
        NodeBlockCacheEntryHandle direct_handle;

        direct_node_slot = direct_block_index / DEF_ADDRS_PER_BLOCK;
        if (dir_inode->disk_inode->i_nid[direct_node_slot] == INVALID_NID) {
            return ENOENT;
        }

        direct_handle = nodeBlockCacheGet(node_cache, dir_inode->disk_inode->i_nid[direct_node_slot]);
        if (nodeBlockCacheEntryHandleIsEmpty(&direct_handle)) {
            return ENOENT;
        }

        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        return 0;
    }

    indirect_block_index = direct_block_index - (2U * DEF_ADDRS_PER_BLOCK);
    if (indirect_block_index < (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        NodeBlockCacheEntryHandle indirect_handle;
        NodeBlockCacheEntryHandle direct_handle;
        struct RtfsNode *indirect_node;

        indirect_node_slot = indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        indirect_node_offset = indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        if (dir_inode->disk_inode->i_nid[2 + indirect_node_slot] == INVALID_NID) {
            return ENOENT;
        }

        indirect_handle = nodeBlockCacheGet(node_cache, dir_inode->disk_inode->i_nid[2 + indirect_node_slot]);
        if (nodeBlockCacheEntryHandleIsEmpty(&indirect_handle)) {
            return ENOENT;
        }

        indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
        direct_node_slot = indirect_node_offset / DEF_ADDRS_PER_BLOCK;
        if (indirect_node->in.nid[direct_node_slot] == INVALID_NID) {
            nodeBlockCacheEntryHandleDestroy(&indirect_handle);
            return ENOENT;
        }

        direct_handle = nodeBlockCacheGet(node_cache, indirect_node->in.nid[direct_node_slot]);
        nodeBlockCacheEntryHandleDestroy(&indirect_handle);
        if (nodeBlockCacheEntryHandleIsEmpty(&direct_handle)) {
            return ENOENT;
        }

        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        return 0;
    }

    double_indirect_block_index = indirect_block_index - (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
    if (double_indirect_block_index < (NIDS_PER_BLOCK * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        NodeBlockCacheEntryHandle dind_handle;
        NodeBlockCacheEntryHandle level1_handle;
        NodeBlockCacheEntryHandle direct_handle;
        struct RtfsNode *dind_node;
        struct RtfsNode *level1_node;

        if (dir_inode->disk_inode->i_nid[4] == INVALID_NID) {
            return ENOENT;
        }

        dind_handle = nodeBlockCacheGet(node_cache, dir_inode->disk_inode->i_nid[4]);
        if (nodeBlockCacheEntryHandleIsEmpty(&dind_handle)) {
            return ENOENT;
        }

        dind_node = nodeBlockCacheEntryGetNodeBlockPtr(dind_handle.entry);
        first_level_slot = double_indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        double_indirect_offset = double_indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        second_level_slot = double_indirect_offset / DEF_ADDRS_PER_BLOCK;
        if (dind_node->in.nid[first_level_slot] == INVALID_NID) {
            nodeBlockCacheEntryHandleDestroy(&dind_handle);
            return ENOENT;
        }

        level1_handle = nodeBlockCacheGet(node_cache, dind_node->in.nid[first_level_slot]);
        nodeBlockCacheEntryHandleDestroy(&dind_handle);
        if (nodeBlockCacheEntryHandleIsEmpty(&level1_handle)) {
            return ENOENT;
        }

        level1_node = nodeBlockCacheEntryGetNodeBlockPtr(level1_handle.entry);
        if (level1_node->in.nid[second_level_slot] == INVALID_NID) {
            nodeBlockCacheEntryHandleDestroy(&level1_handle);
            return ENOENT;
        }

        direct_handle = nodeBlockCacheGet(node_cache, level1_node->in.nid[second_level_slot]);
        nodeBlockCacheEntryHandleDestroy(&level1_handle);
        if (nodeBlockCacheEntryHandleIsEmpty(&direct_handle)) {
            return ENOENT;
        }

        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        return 0;
    }

    return ENOSYS;
}

static int rtfsDirInodeRelocateOneRegularBlock(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode,
    uint32_t block_index,
    uint32_t new_lpa
)
{
    RtfsDirInodeNodeHandle *owned_handle;
    NodeBlockCache *node_cache;
    uint32_t direct_block_index;
    uint32_t indirect_block_index;
    uint32_t double_indirect_block_index;
    uint32_t direct_node_slot;
    uint32_t direct_node_offset;
    uint32_t indirect_node_slot;
    uint32_t indirect_node_offset;
    uint32_t first_level_slot;
    uint32_t second_level_slot;
    uint32_t double_indirect_offset;

    (void)fs_manager;

    if (dir_inode == NULL || dir_inode->disk_inode == NULL || dir_inode->cache_handle == NULL) {
        return EINVAL;
    }

    owned_handle = (RtfsDirInodeNodeHandle *)dir_inode->cache_handle;
    node_cache = owned_handle->node_cache;
    if (node_cache == NULL) {
        return EINVAL;
    }

    if (block_index < DEF_ADDRS_PER_INODE) {
        dir_inode->disk_inode->i_addr[block_index] = new_lpa;
        nodeBlockCacheEntryHandleMarkDirty(&owned_handle->node_handle);
        return 0;
    }

    direct_block_index = block_index - DEF_ADDRS_PER_INODE;
    if (direct_block_index < (2U * DEF_ADDRS_PER_BLOCK)) {
        NodeBlockCacheEntryHandle direct_handle;
        struct RtfsNode *direct_node;

        direct_node_slot = direct_block_index / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = direct_block_index % DEF_ADDRS_PER_BLOCK;
        if (dir_inode->disk_inode->i_nid[direct_node_slot] == INVALID_NID) {
            return ENOENT;
        }

        direct_handle = nodeBlockCacheGet(node_cache, dir_inode->disk_inode->i_nid[direct_node_slot]);
        if (nodeBlockCacheEntryHandleIsEmpty(&direct_handle)) {
            return ENOENT;
        }

        direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
        direct_node->dn.addr[direct_node_offset] = new_lpa;
        nodeBlockCacheEntryHandleMarkDirty(&direct_handle);
        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        return 0;
    }

    indirect_block_index = direct_block_index - (2U * DEF_ADDRS_PER_BLOCK);
    if (indirect_block_index < (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        NodeBlockCacheEntryHandle indirect_handle;
        NodeBlockCacheEntryHandle direct_handle;
        struct RtfsNode *indirect_node;
        struct RtfsNode *direct_node;

        indirect_node_slot = indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        indirect_node_offset = indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        if (dir_inode->disk_inode->i_nid[2 + indirect_node_slot] == INVALID_NID) {
            return ENOENT;
        }

        indirect_handle = nodeBlockCacheGet(node_cache, dir_inode->disk_inode->i_nid[2 + indirect_node_slot]);
        if (nodeBlockCacheEntryHandleIsEmpty(&indirect_handle)) {
            return ENOENT;
        }

        indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
        direct_node_slot = indirect_node_offset / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = indirect_node_offset % DEF_ADDRS_PER_BLOCK;
        if (indirect_node->in.nid[direct_node_slot] == INVALID_NID) {
            nodeBlockCacheEntryHandleDestroy(&indirect_handle);
            return ENOENT;
        }

        direct_handle = nodeBlockCacheGet(node_cache, indirect_node->in.nid[direct_node_slot]);
        nodeBlockCacheEntryHandleDestroy(&indirect_handle);
        if (nodeBlockCacheEntryHandleIsEmpty(&direct_handle)) {
            return ENOENT;
        }

        direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
        direct_node->dn.addr[direct_node_offset] = new_lpa;
        nodeBlockCacheEntryHandleMarkDirty(&direct_handle);
        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        return 0;
    }

    double_indirect_block_index = indirect_block_index - (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
    if (double_indirect_block_index < (NIDS_PER_BLOCK * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        NodeBlockCacheEntryHandle dind_handle;
        NodeBlockCacheEntryHandle level1_handle;
        NodeBlockCacheEntryHandle direct_handle;
        struct RtfsNode *dind_node;
        struct RtfsNode *level1_node;
        struct RtfsNode *direct_node;

        if (dir_inode->disk_inode->i_nid[4] == INVALID_NID) {
            return ENOENT;
        }

        dind_handle = nodeBlockCacheGet(node_cache, dir_inode->disk_inode->i_nid[4]);
        if (nodeBlockCacheEntryHandleIsEmpty(&dind_handle)) {
            return ENOENT;
        }

        dind_node = nodeBlockCacheEntryGetNodeBlockPtr(dind_handle.entry);
        first_level_slot = double_indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        double_indirect_offset = double_indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        second_level_slot = double_indirect_offset / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = double_indirect_offset % DEF_ADDRS_PER_BLOCK;

        if (dind_node->in.nid[first_level_slot] == INVALID_NID) {
            nodeBlockCacheEntryHandleDestroy(&dind_handle);
            return ENOENT;
        }

        level1_handle = nodeBlockCacheGet(node_cache, dind_node->in.nid[first_level_slot]);
        nodeBlockCacheEntryHandleDestroy(&dind_handle);
        if (nodeBlockCacheEntryHandleIsEmpty(&level1_handle)) {
            return ENOENT;
        }

        level1_node = nodeBlockCacheEntryGetNodeBlockPtr(level1_handle.entry);
        if (level1_node->in.nid[second_level_slot] == INVALID_NID) {
            nodeBlockCacheEntryHandleDestroy(&level1_handle);
            return ENOENT;
        }

        direct_handle = nodeBlockCacheGet(node_cache, level1_node->in.nid[second_level_slot]);
        nodeBlockCacheEntryHandleDestroy(&level1_handle);
        if (nodeBlockCacheEntryHandleIsEmpty(&direct_handle)) {
            return ENOENT;
        }

        direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
        direct_node->dn.addr[direct_node_offset] = new_lpa;
        nodeBlockCacheEntryHandleMarkDirty(&direct_handle);
        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        return 0;
    }

    return ENOSYS;
}

static int rtfsDirInodeCollectPendingDataCowOldLpas(
    RtfsDirInode *dir_inode,
    uint32_t *out_array,
    size_t max_count,
    size_t *out_count
)
{
    size_t count = 0;
    size_t i;

    if (dir_inode == NULL || out_array == NULL || out_count == NULL) {
        return EINVAL;
    }

    for (i = 0; i < dir_inode->loaded_block_count_actual; ++i) {
        RtfsLoadedDirBlock *loaded = &dir_inode->loaded_blocks[i];

        if (!loaded->has_pending_cow_relocation ||
            loaded->cow_new_lpa == INVALID_LPA ||
            loaded->lpa == INVALID_LPA) {
            continue;
        }

        if (count >= max_count) {
            return ENOSPC;
        }

        out_array[count++] = loaded->lpa;
    }

    *out_count = count;
    return 0;
}

static JournalContainer *rtfsDirInodeCloneJournalContainer(const JournalContainer *src)
{
    JournalContainer *dst;
    size_t i;

    if (src == NULL) {
        return NULL;
    }

    dst = (JournalContainer *)malloc(sizeof(*dst));
    if (dst == NULL) {
        return NULL;
    }

    journalContainerInit(dst);
    for (i = 0; i < kv_size(src->superBlockJournal); ++i) {
        SuperBlockJournalEntry entry = kv_a(SuperBlockJournalEntry, src->superBlockJournal, i);
        journalContainerAppendSuperBlockJournalEntry(dst, &entry);
    }
    for (i = 0; i < kv_size(src->natJournal); ++i) {
        NatJournalEntry entry = kv_a(NatJournalEntry, src->natJournal, i);
        journalContainerAppendNatJournalEntry(dst, &entry);
    }
    for (i = 0; i < kv_size(src->sitJournal); ++i) {
        SitJournalEntry entry = kv_a(SitJournalEntry, src->sitJournal, i);
        journalContainerAppendSitJournalEntry(dst, &entry);
    }

    return dst;
}

static int rtfsDirInodeSubmitJournal(
    file_system_manager *fs_manager,
    JournalContainer *journal,
    uint64_t *out_tx_id
)
{
    uint64_t tx_id = 0;

    if (journal == NULL) {
        return EINVAL;
    }

    {
        JournalProcessEnv *env = journalProcessEnvGetInstance();
        tx_id = journalProcessEnvAllocTxId(env);
        journalContainerSetTxId(journal, tx_id);
    }

    if (out_tx_id != NULL) {
        *out_tx_id = tx_id;
    }

    if (g_rtfs_dir_inode_journal_commit_hook != NULL) {
        return g_rtfs_dir_inode_journal_commit_hook(journal);
    }

    {
        JournalProcessEnv *env = journalProcessEnvGetInstance();
        journalProcessEnvCommitJournal(env, journal);
    }

    (void)fs_manager;
    return 0;
}
