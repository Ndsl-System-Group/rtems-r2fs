#include "file_inode.h"


#include "cache/block_buffer.h"
#include "cache/node_block_cache.h"
#include "cache/page_cache.h"
#include "communication/memory.h"
#include "fs/cow_reclaim_registry.h"
#include "fs/fs_manager.h"
#include "fs/srmap_utils.h"
#include "fs/sit_utils.h"
#include "fs/super_manager.h"
#include "journal/journal_container.h"
#include "journal/journal_process_env.h"
#include "utils/io_utils.h"
#include "utils/rtfs_log.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define RTFS_FILE_PAGE_CACHE_EXPECT_SIZE 64
#define RTFS_FILE_WRITEBACK_BATCH_PAGES 64
#define RTFS_FILE_DIRTY_PAGES_NODE_CMP(a, b) \
    ((a).blkoff < (b).blkoff ? -1 : ((a).blkoff > (b).blkoff ? 1 : 0))

KBTREE_INIT(ktfipdpn, DirtyPagesNode, RTFS_FILE_DIRTY_PAGES_NODE_CMP)

static rtfs_file_inode_read_block_hook g_rtfs_file_inode_read_block_hook = NULL;
static rtfs_file_inode_write_block_hook g_rtfs_file_inode_write_block_hook = NULL;
static rtfs_file_inode_journal_commit_hook g_rtfs_file_inode_journal_commit_hook = NULL;

/* ===== 内部类型 ===== */

typedef struct RtfsFilePendingDataCowRelocation
{
    uint32_t block_index;
    uint32_t old_lpa;
    uint32_t new_lpa;
} RtfsFilePendingDataCowRelocation;

typedef struct RtfsFileDirtyPageWriteOp
{
    PageEntry *page_entry;
    uint32_t block_index;
    uint32_t old_lpa;
    uint32_t new_lpa;
} RtfsFileDirtyPageWriteOp;

struct RtfsFileInode
{
    /* 普通文件的最小身份信息。 */
    rtfs_ino ino;
    uint8_t file_type;

    /*
     * 从磁盘 inode 缓存出的常用元数据。
     * 普通文件 handler 后续会频繁用这些字段处理 read/write/lseek/fstat/truncate。
     */
    uint64_t i_size;
    uint16_t i_mode;
    uint32_t i_nlink;
    uint64_t i_atime;
    uint64_t i_mtime;
    uint32_t i_atime_nsec;
    uint32_t i_mtime_nsec;
    uint32_t i_ctime_nsec;

    /*
     * 指向 node cache 中的 inode block。
     * disk_inode 不拥有内存，生命周期由 cache_handle 保证。
     */
    struct RtfsInode *disk_inode;
    void *cache_handle;

    /*
     * 普通文件内容页缓存。
     * key 是文件逻辑块号，value 是 4KB page。
     */
    PageCache page_cache;

    /* 串行化同一 inode 上的文件操作和提交路径。 */
    mutex_t op_lock;

    /*
     * dirty data page COW 后暂存的映射切换信息。
     * writeback 阶段只写新 data LPA，relocation 阶段才更新 inode/node tree。
     */
    RtfsFilePendingDataCowRelocation *pending_data_relocations;
    size_t pending_data_relocation_count;
    size_t pending_data_relocation_capacity;

    /*
     * truncate 或 COW 后需要延迟回收的旧 data LPA。
     * 旧块必须等 journal 提交完成后才能交给回收流程。
     */
    uint32_t *pending_truncate_old_lpas;
    size_t pending_truncate_old_lpa_count;
    size_t pending_truncate_old_lpa_capacity;

    /* inode 元数据是否已经被运行时逻辑修改。 */
    bool is_dirty;
};

struct RtfsFileInodeCache
{
    NodeBlockCache *node_cache;
};

typedef struct RtfsFileInodeNodeHandle
{
    NodeBlockCacheEntryHandle node_handle;
    NodeBlockCache *node_cache;
} RtfsFileInodeNodeHandle;

/* ===== 内部辅助函数声明 ===== */

/* 创建一个空的普通文件运行时 inode 对象。 */
static RtfsFileInode *rtfsCreateFileInode(rtfs_ino ino);

/* 释放普通文件运行时 inode 对象及其持有的缓存句柄、page cache 和 pending 状态。 */
static void rtfsDestroyFileInode(RtfsFileInode *file_inode);

/* 从 node cache 中绑定 inode block，并导入普通文件元数据。 */
static int rtfsFileInodeBuildFromCache(
    RtfsFileInode *file_inode,
    RtfsFileInodeCache *cache,
    rtfs_ino ino
);

/* 从磁盘 inode 复制普通文件 handler 常用的元数据字段。 */
static int rtfsFileInodeBuildMetadata(
    RtfsFileInode *file_inode,
    const struct RtfsInode *disk_inode
);

/* 把运行时元数据写回 disk_inode，并把 inode node 标记为 dirty。 */
static int rtfsFileInodeSyncMetadataToDisk(RtfsFileInode *file_inode);

/* 更新普通文件修改时间，并标记 inode 元数据 dirty。 */
static void rtfsFileInodeTouchModifyTime(RtfsFileInode *file_inode);

/* 更新普通文件访问时间，并标记 inode 元数据 dirty。 */
static void rtfsFileInodeTouchAccessTime(RtfsFileInode *file_inode);

/* 根据文件逻辑块号解析当前 data LPA，支持 inode/direct/indirect/double-indirect 路径。 */
static int rtfsFileResolveDataLpaByBlockIndex(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint32_t block_index,
    uint32_t *out_lpa
);

/* 设置文件逻辑块号对应的 data LPA，必要时创建缺失的索引 node。 */
static int rtfsFileSetDataLpaByBlockIndex(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint32_t block_index,
    uint32_t new_lpa,
    bool create_missing_nodes,
    uint32_t *out_old_lpa
);

/* 获取 nid 对应的 node cache entry；cache miss 时后续实现应通过 helper 从 NAT/SSD 读入。 */
static int rtfsFileGetCachedNodeByNid(
    file_system_manager *fs_manager,
    NodeBlockCache *node_cache,
    uint32_t nid,
    uint32_t parent_nid,
    RtfsFileInodeNodeHandle *out_handle
);

/* 初始化新建 direct node 的 addr 数组。 */
static void rtfsFileInodeInitCreatedDirectNode(struct RtfsNode *node);

/* 初始化新建 indirect node 的 nid 数组。 */
static void rtfsFileInodeInitCreatedIndirectNode(struct RtfsNode *node);

/* 为读路径准备 page cache entry；cache miss 时从 data LPA 读入或生成空洞页。 */
static int rtfsFileInodePreparePageForRead(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint32_t block_index,
    PageEntryHandle *out_page
);

/* 为写路径准备 page cache entry；局部覆盖旧块时需要先读旧内容。 */
static int rtfsFileInodePreparePageForWrite(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint32_t block_index,
    bool preserve_existing_content,
    PageEntryHandle *out_page
);

/* 记录一次 data page COW 产生的 old_lpa -> new_lpa 映射。 */
static int rtfsFileInodeAppendPendingDataRelocation(
    RtfsFileInode *file_inode,
    uint32_t block_index,
    uint32_t old_lpa,
    uint32_t new_lpa
);

/* 记录 truncate 释放出的旧 data LPA。 */
static int rtfsFileInodeAppendPendingTruncateOldLpa(
    RtfsFileInode *file_inode,
    uint32_t old_lpa
);

/* 清理一次事务中尚未提交或已经提交完成的 pending COW/truncate 状态。 */
static void rtfsFileInodeClearPendingCowState(RtfsFileInode *file_inode);

/* 将 dirty pages 以 content-COW 方式写入新 data LPA。 */
static int rtfsFileInodeWriteDirtyPagesCow(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
);

/* 将 pending data relocation 应用到 inode/direct/indirect/double-indirect 映射树。 */
static int rtfsFileInodeApplyPendingDataRelocations(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
);

/* truncate 后清理目标大小之后的 page cache 状态。 */
static void rtfsFileInodeInvalidateCachedPagesFrom(
    RtfsFileInode *file_inode,
    uint64_t target_size
);

/* ===== 对外 API ===== */

static JournalContainer *rtfsFileInodeCloneJournalContainer(
    const JournalContainer *src
);

static int rtfsFileInodeSubmitJournal(
    file_system_manager *fs_manager,
    JournalContainer *journal,
    uint64_t *out_tx_id
);

RtfsFileInodeCache *rtfsFileInodeCacheCreate(NodeBlockCache *node_cache)
{
    RtfsFileInodeCache *cache = (RtfsFileInodeCache *)malloc(sizeof(*cache));

    if (cache == NULL) {
        return NULL;
    }

    cache->node_cache = node_cache;
    return cache;
}

void rtfsFileInodeCacheDestroy(RtfsFileInodeCache *cache)
{
    free(cache);
}

int rtfsFileInodeBuild(
    RtfsFileInodeCache *cache,
    rtfs_ino ino,
    RtfsFileInode **out_file_inode
)
{
    RtfsFileInode *file_inode;
    int ret;

    if (out_file_inode == NULL) {
        return EINVAL;
    }

    *out_file_inode = NULL;

    file_inode = rtfsCreateFileInode(ino);
    if (file_inode == NULL) {
        return ENOMEM;
    }

    ret = rtfsFileInodeBuildFromCache(file_inode, cache, ino);
    if (ret != 0) {
        rtfsDestroyFileInode(file_inode);
        return ret;
    }

    *out_file_inode = file_inode;
    return 0;
}

void rtfsFileInodePut(RtfsFileInode *file_inode)
{
    rtfsDestroyFileInode(file_inode);
}

uint64_t rtfsFileInodeGetSize(const RtfsFileInode *file_inode)
{
    return file_inode != NULL ? file_inode->i_size : 0;
}

ssize_t rtfsFileInodeRead(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    off_t *offset,
    void *buffer,
    size_t count
)
{
    uint64_t cur_offset;
    uint64_t readable;
    size_t bytes_read = 0;

    if (file_inode == NULL || offset == NULL ||
        (buffer == NULL && count != 0)) {
        errno = EINVAL;
        return -1;
    }

    if (*offset < 0) {
        errno = EINVAL;
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    rtfsMutexLock(&file_inode->op_lock);

    cur_offset = (uint64_t)*offset;
    if (cur_offset >= file_inode->i_size) {
        rtfsMutexUnlock(&file_inode->op_lock);
        return 0;
    }

    readable = file_inode->i_size - cur_offset;
    if (readable > count) {
        readable = count;
    }

    while (readable > 0) {
        uint64_t block_index64 = cur_offset / BLOCK_BUFFER_SIZE;
        uint32_t block_index;
        size_t block_offset;
        size_t chunk;
        PageEntryHandle page_handle;
        PageEntry *page_entry;
        char *page_data;
        int ret;

        if (block_index64 > UINT32_MAX) {
            if (bytes_read > 0) {
                break;
            }
            errno = EFBIG;
            rtfsMutexUnlock(&file_inode->op_lock);
            return -1;
        }

        block_index = (uint32_t)block_index64;
        block_offset = (size_t)(cur_offset % BLOCK_BUFFER_SIZE);
        chunk = BLOCK_BUFFER_SIZE - block_offset;
        if ((uint64_t)chunk > readable) {
            chunk = (size_t)readable;
        }

        ret = rtfsFileInodePreparePageForRead(
            fs_manager,
            file_inode,
            block_index,
            &page_handle
        );
        if (ret != 0) {
            if (bytes_read > 0) {
                break;
            }
            errno = ret;
            rtfsMutexUnlock(&file_inode->op_lock);
            return -1;
        }

        page_entry = page_handle.entry;
        rtfsMutexLock(pageEntryGetLock(page_entry));
        page_data = blockBufferGetPtr(pageEntryGetBuffer(page_entry));
        memcpy((char *)buffer + bytes_read, page_data + block_offset, chunk);
        rtfsMutexUnlock(pageEntryGetLock(page_entry));

        pageEntryHandleDestroy(&page_handle);

        cur_offset += chunk;
        bytes_read += chunk;
        readable -= chunk;
    }

    *offset = (off_t)cur_offset;
    if (bytes_read > 0) {
        rtfsFileInodeTouchAccessTime(file_inode);
    }

    rtfsMutexUnlock(&file_inode->op_lock);

    return (ssize_t)bytes_read;
}

ssize_t rtfsFileInodeWrite(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    off_t *offset,
    const void *buffer,
    size_t count
)
{
    uint64_t cur_offset;
    size_t bytes_written = 0;

    if (file_inode == NULL || offset == NULL ||
        (buffer == NULL && count != 0)) {
        errno = EINVAL;
        return -1;
    }

    if (*offset < 0) {
        errno = EINVAL;
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    rtfsMutexLock(&file_inode->op_lock);

    cur_offset = (uint64_t)*offset;
    if (UINT64_MAX - cur_offset < count) {
        errno = EFBIG;
        rtfsMutexUnlock(&file_inode->op_lock);
        return -1;
    }

    while (bytes_written < count) {
        uint64_t block_index64 = cur_offset / BLOCK_BUFFER_SIZE;
        uint32_t block_index;
        size_t block_offset;
        size_t chunk;
        PageEntryHandle page_handle;
        PageEntry *page_entry;
        char *page_data;
        int ret;

        if (block_index64 > UINT32_MAX) {
            if (bytes_written > 0) {
                break;
            }
            errno = EFBIG;
            rtfsMutexUnlock(&file_inode->op_lock);
            return -1;
        }

        block_index = (uint32_t)block_index64;
        block_offset = (size_t)(cur_offset % BLOCK_BUFFER_SIZE);
        chunk = BLOCK_BUFFER_SIZE - block_offset;
        if (chunk > count - bytes_written) {
            chunk = count - bytes_written;
        }

        ret = rtfsFileInodePreparePageForWrite(
            fs_manager,
            file_inode,
            block_index,
            block_offset != 0 || chunk != BLOCK_BUFFER_SIZE,
            &page_handle
        );
        if (ret != 0) {
            if (bytes_written > 0) {
                break;
            }
            errno = ret;
            rtfsMutexUnlock(&file_inode->op_lock);
            return -1;
        }

        page_entry = page_handle.entry;
        rtfsMutexLock(pageEntryGetLock(page_entry));
        page_data = blockBufferGetPtr(pageEntryGetBuffer(page_entry));
        memcpy(page_data + block_offset, (const char *)buffer + bytes_written, chunk);
        pageEntrySetState(page_entry, PAGE_READY);
        rtfsMutexUnlock(pageEntryGetLock(page_entry));

        pageEntryHandleMakeDirty(&page_handle);
        pageEntryHandleDestroy(&page_handle);

        cur_offset += chunk;
        bytes_written += chunk;
        if (cur_offset > file_inode->i_size) {
            file_inode->i_size = cur_offset;
            file_inode->is_dirty = true;
        }
    }

    *offset = (off_t)cur_offset;
    if (bytes_written > 0) {
        rtfsFileInodeTouchModifyTime(file_inode);
    }

    rtfsMutexUnlock(&file_inode->op_lock);

    return (ssize_t)bytes_written;
}

int rtfsFileInodeTruncate(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint64_t target_size
)
{
    uint64_t old_size;
    uint64_t old_block_count;
    uint64_t new_block_count;
    uint64_t block_index;
    int ret;

    if (file_inode == NULL) {
        return EINVAL;
    }

    rtfsMutexLock(&file_inode->op_lock);

    old_size = file_inode->i_size;
    if (target_size == old_size) {
        rtfsMutexUnlock(&file_inode->op_lock);
        return 0;
    }

    if (target_size > old_size) {
        file_inode->i_size = target_size;
        file_inode->is_dirty = true;
        rtfsFileInodeTouchModifyTime(file_inode);
        rtfsMutexUnlock(&file_inode->op_lock);
        return 0;
    }

    old_block_count = SIZE_TO_BLOCK(old_size);
    new_block_count = SIZE_TO_BLOCK(target_size);

    if ((target_size % BLOCK_BUFFER_SIZE) != 0 && new_block_count > 0 &&
        new_block_count <= old_block_count) {
        PageEntryHandle page_handle;
        PageEntry *page_entry;
        size_t keep_bytes = (size_t)(target_size % BLOCK_BUFFER_SIZE);

        if (new_block_count - 1 > UINT32_MAX) {
            rtfsMutexUnlock(&file_inode->op_lock);
            return EFBIG;
        }

        ret = rtfsFileInodePreparePageForWrite(
            fs_manager,
            file_inode,
            (uint32_t)(new_block_count - 1),
            true,
            &page_handle
        );
        if (ret != 0) {
            rtfsMutexUnlock(&file_inode->op_lock);
            return ret;
        }

        page_entry = page_handle.entry;
        rtfsMutexLock(pageEntryGetLock(page_entry));
        memset(
            blockBufferGetPtr(pageEntryGetBuffer(page_entry)) + keep_bytes,
            0,
            BLOCK_BUFFER_SIZE - keep_bytes
        );
        pageEntrySetState(page_entry, PAGE_READY);
        rtfsMutexUnlock(pageEntryGetLock(page_entry));

        pageEntryHandleMakeDirty(&page_handle);
        pageEntryHandleDestroy(&page_handle);
    }

    for (block_index = new_block_count;
         block_index < old_block_count;
         ++block_index) {
        uint32_t old_lpa = INVALID_LPA;

        if (block_index > UINT32_MAX) {
            rtfsMutexUnlock(&file_inode->op_lock);
            return EFBIG;
        }

        ret = rtfsFileSetDataLpaByBlockIndex(
            fs_manager,
            file_inode,
            (uint32_t)block_index,
            INVALID_LPA,
            false,
            &old_lpa
        );
        if (ret != 0) {
            rtfsMutexUnlock(&file_inode->op_lock);
            return ret;
        }

        ret = rtfsFileInodeAppendPendingTruncateOldLpa(file_inode, old_lpa);
        if (ret != 0) {
            rtfsMutexUnlock(&file_inode->op_lock);
            return ret;
        }
    }

    rtfsFileInodeInvalidateCachedPagesFrom(file_inode, target_size);
    file_inode->i_size = target_size;
    file_inode->is_dirty = true;
    rtfsFileInodeTouchModifyTime(file_inode);
    rtfsMutexUnlock(&file_inode->op_lock);

    return 0;
}

int rtfsFileInodeWritebackContentCow(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
)
{
    return rtfsFileInodeWriteDirtyPagesCow(fs_manager, file_inode);
}

int rtfsFileInodeApplyPendingCowRelocations(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
)
{
    return rtfsFileInodeApplyPendingDataRelocations(fs_manager, file_inode);
}

int rtfsFileInodeCollectPendingDataCowOldLpas(
    RtfsFileInode *file_inode,
    uint32_t *out_array,
    size_t max_count,
    size_t *out_count
)
{
    size_t count = 0;
    size_t i;

    if (file_inode == NULL || out_array == NULL || out_count == NULL) {
        return EINVAL;
    }

    for (i = 0; i < file_inode->pending_data_relocation_count; ++i) {
        uint32_t old_lpa = file_inode->pending_data_relocations[i].old_lpa;

        if (old_lpa == INVALID_LPA) {
            continue;
        }

        if (count >= max_count) {
            return ENOSPC;
        }

        out_array[count++] = old_lpa;
    }

    for (i = 0; i < file_inode->pending_truncate_old_lpa_count; ++i) {
        uint32_t old_lpa = file_inode->pending_truncate_old_lpas[i];

        if (old_lpa == INVALID_LPA) {
            continue;
        }

        if (count >= max_count) {
            return ENOSPC;
        }

        out_array[count++] = old_lpa;
    }

    *out_count = count;
    return 0;
}

void rtfsFileInodeSetReadBlockHook(rtfs_file_inode_read_block_hook hook)
{
    g_rtfs_file_inode_read_block_hook = hook;
}

void rtfsFileInodeSetWriteBlockHook(rtfs_file_inode_write_block_hook hook)
{
    g_rtfs_file_inode_write_block_hook = hook;
}

void rtfsFileInodeSetJournalCommitHook(
    rtfs_file_inode_journal_commit_hook hook
)
{
    g_rtfs_file_inode_journal_commit_hook = hook;
}

int rtfsFileInodeCommitCowWritebackWithTxId(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint64_t *out_tx_id
)
{
    NodeBlockCache *node_cache;
    JournalContainer *cur_journal;
    JournalContainer *to_commit = NULL;
    NodeBlockCacheCowRelocation *node_relocations = NULL;
    uint32_t *old_data_lpas = NULL;
    uint32_t *old_node_lpas = NULL;
    size_t old_data_count = 0;
    size_t old_data_capacity = 0;
    size_t old_node_count = 0;
    size_t i;
    uint64_t tx_id = 0;
    bool journal_submitted = false;
    int ret;

    if (fs_manager == NULL || file_inode == NULL) {
        return EINVAL;
    }

    rtfsMutexLock(&file_inode->op_lock);

    node_cache = fileSystemManagerGetNodeCache(fs_manager);
    cur_journal = fileSystemManagerGetCurJournal(fs_manager);
    if (node_cache == NULL || cur_journal == NULL) {
        rtfsMutexUnlock(&file_inode->op_lock);
        return EINVAL;
    }

    ret = cowReclaimRegistryDrainCompleted();
    if (ret != 0) {
        rtfsMutexUnlock(&file_inode->op_lock);
        return ret;
    }

    ret = rtfsFileInodeWritebackContentCow(fs_manager, file_inode);
    if (ret != 0) {
        rtfsMutexUnlock(&file_inode->op_lock);
        return ret;
    }

    old_data_capacity =
        file_inode->pending_data_relocation_count +
        file_inode->pending_truncate_old_lpa_count;
    if (old_data_capacity > 0) {
        old_data_lpas = (uint32_t *)malloc(
            old_data_capacity * sizeof(*old_data_lpas)
        );
        if (old_data_lpas == NULL) {
            rtfsMutexUnlock(&file_inode->op_lock);
            return ENOMEM;
        }

        ret = rtfsFileInodeCollectPendingDataCowOldLpas(
            file_inode,
            old_data_lpas,
            old_data_capacity,
            &old_data_count
        );
        if (ret != 0) {
            free(old_data_lpas);
            rtfsMutexUnlock(&file_inode->op_lock);
            return ret;
        }
    }

    ret = rtfsFileInodeApplyPendingCowRelocations(fs_manager, file_inode);
    if (ret != 0) {
        free(old_data_lpas);
        rtfsMutexUnlock(&file_inode->op_lock);
        return ret;
    }

    if (file_inode->is_dirty) {
        ret = rtfsFileInodeSyncMetadataToDisk(file_inode);
        if (ret != 0) {
            free(old_data_lpas);
            rtfsMutexUnlock(&file_inode->op_lock);
            return ret;
        }
    }

    ret = nodeBlockCacheWritebackDirtyContentCow(node_cache);
    if (ret != 0) {
        free(old_data_lpas);
        rtfsMutexUnlock(&file_inode->op_lock);
        return ret;
    }

    if (node_cache->curSize > 0) {
        node_relocations = (NodeBlockCacheCowRelocation *)malloc(
            node_cache->curSize * sizeof(*node_relocations)
        );
        if (node_relocations == NULL) {
            free(old_data_lpas);
            rtfsMutexUnlock(&file_inode->op_lock);
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
            rtfsMutexUnlock(&file_inode->op_lock);
            return ret;
        }

        if (old_node_count > 0) {
            size_t valid_old_node_count = 0;

            old_node_lpas = (uint32_t *)malloc(
                old_node_count * sizeof(*old_node_lpas)
            );
            if (old_node_lpas == NULL) {
                free(node_relocations);
                free(old_data_lpas);
                rtfsMutexUnlock(&file_inode->op_lock);
                return ENOMEM;
            }

            for (i = 0; i < old_node_count; ++i) {
                if (node_relocations[i].oldLpa == INVALID_LPA) {
                    continue;
                }
                old_node_lpas[valid_old_node_count++] =
                    node_relocations[i].oldLpa;
            }
            old_node_count = valid_old_node_count;
        }
    }

    ret = nodeBlockCacheApplyPendingCowRelocations(node_cache);
    if (ret != 0) {
        free(old_node_lpas);
        free(node_relocations);
        free(old_data_lpas);
        rtfsMutexUnlock(&file_inode->op_lock);
        return ret;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "file commit begin ino=%u dirty=%d journal_empty=%d",
        (unsigned int)file_inode->ino,
        file_inode->is_dirty ? 1 : 0,
        journalContainerIsEmpty(cur_journal) ? 1 : 0
    );

    if (!journalContainerIsEmpty(cur_journal)) {
        to_commit = rtfsFileInodeCloneJournalContainer(cur_journal);
        if (to_commit == NULL) {
            free(old_node_lpas);
            free(node_relocations);
            free(old_data_lpas);
            rtfsMutexUnlock(&file_inode->op_lock);
            return ENOMEM;
        }

        ret = rtfsFileInodeSubmitJournal(fs_manager, to_commit, &tx_id);
        if (ret != 0) {
            journalContainerDestroy(to_commit);
            free(to_commit);
            free(old_node_lpas);
            free(node_relocations);
            free(old_data_lpas);
            rtfsMutexUnlock(&file_inode->op_lock);
            return ret;
        }
        journal_submitted = true;

        journalContainerDestroy(cur_journal);
        journalContainerInit(cur_journal);
    } else {
        RTFS_LOG(
            RTFS_LOG_INFO,
            "file commit skipped submit ino=%u because journal is empty",
            (unsigned int)file_inode->ino
        );
    }

    if (journal_submitted) {
        (void)cowReclaimRegistryRegister(
            tx_id,
            old_data_lpas,
            old_data_count,
            old_node_lpas,
            old_node_count,
            NULL,
            0
        );
    }

    if (out_tx_id != NULL) {
        *out_tx_id = tx_id;
    }

    free(old_node_lpas);
    free(node_relocations);
    free(old_data_lpas);

    rtfsFileInodeClearPendingCowState(file_inode);
    file_inode->is_dirty = false;

    RTFS_LOG(
        RTFS_LOG_INFO,
        "file commit end ino=%u tx_id=%llu submitted=%d",
        (unsigned int)file_inode->ino,
        (unsigned long long)tx_id,
        journal_submitted ? 1 : 0
    );

    rtfsMutexUnlock(&file_inode->op_lock);

    return 0;
}

int rtfsFileInodeCommitCowWriteback(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
)
{
    return rtfsFileInodeCommitCowWritebackWithTxId(
        fs_manager,
        file_inode,
        NULL
    );
}

/* ===== 内部辅助函数骨架 ===== */

static RtfsFileInode *rtfsCreateFileInode(rtfs_ino ino)
{
    RtfsFileInode *file_inode = (RtfsFileInode *)calloc(1, sizeof(*file_inode));

    if (file_inode == NULL) {
        return NULL;
    }

    file_inode->ino = ino;
    file_inode->file_type = RTFS_FT_REG_FILE;
    file_inode->disk_inode = NULL;
    file_inode->cache_handle = NULL;
    pageCacheInit(&file_inode->page_cache, RTFS_FILE_PAGE_CACHE_EXPECT_SIZE);
    rtfsMutexInit(&file_inode->op_lock);

    return file_inode;
}

static void rtfsDestroyFileInode(RtfsFileInode *file_inode)
{
    if (file_inode == NULL) {
        return;
    }

    rtfsFileInodeClearPendingCowState(file_inode);
    free(file_inode->pending_data_relocations);
    free(file_inode->pending_truncate_old_lpas);
    pageCacheDestroy(&file_inode->page_cache);
    rtfsMutexDestroy(&file_inode->op_lock);

    if (file_inode->cache_handle != NULL) {
        RtfsFileInodeNodeHandle *owned_handle =
            (RtfsFileInodeNodeHandle *)file_inode->cache_handle;

        nodeBlockCacheEntryHandleDestroy(&owned_handle->node_handle);
        free(owned_handle);
    }

    free(file_inode);
}

static int rtfsFileInodeBuildFromCache(
    RtfsFileInode *file_inode,
    RtfsFileInodeCache *cache,
    rtfs_ino ino
)
{
    NodeBlockCacheEntryHandle node_handle;
    RtfsFileInodeNodeHandle *owned_handle;
    struct RtfsNode *node;
    int ret;

    if (file_inode == NULL || cache == NULL || cache->node_cache == NULL) {
        return EINVAL;
    }

    /*
     * 这里故意不处理 NodeBlockCache miss。
     * 调用方或上层 resolver 必须先保证 inode block 已经加载到 node cache，
     * 然后再开始普通文件 inode 的运行时对象构建。
     */
    node_handle = nodeBlockCacheGet(cache->node_cache, ino);
    if (nodeBlockCacheEntryHandleIsEmpty(&node_handle)) {
        return ENOENT;
    }

    node = nodeBlockCacheEntryGetNodeBlockPtr(node_handle.entry);
    if (node == NULL || node->footer.ino != ino || node->footer.nid != ino) {
        nodeBlockCacheEntryHandleDestroy(&node_handle);
        return EINVAL;
    }

    if (node->i.i_type != RTFS_FT_REG_FILE) {
        nodeBlockCacheEntryHandleDestroy(&node_handle);
        return EINVAL;
    }

    owned_handle = (RtfsFileInodeNodeHandle *)malloc(sizeof(*owned_handle));
    if (owned_handle == NULL) {
        nodeBlockCacheEntryHandleDestroy(&node_handle);
        return ENOMEM;
    }

    owned_handle->node_cache = cache->node_cache;
    owned_handle->node_handle = node_handle;

    file_inode->disk_inode = &node->i;
    file_inode->cache_handle = owned_handle;

    ret = rtfsFileInodeBuildMetadata(file_inode, file_inode->disk_inode);
    if (ret != 0) {
        file_inode->disk_inode = NULL;
        file_inode->cache_handle = NULL;
        nodeBlockCacheEntryHandleDestroy(&owned_handle->node_handle);
        free(owned_handle);
        return ret;
    }

    return 0;
}

static int rtfsFileInodeBuildMetadata(
    RtfsFileInode *file_inode,
    const struct RtfsInode *disk_inode
)
{
    if (file_inode == NULL || disk_inode == NULL) {
        return EINVAL;
    }

    file_inode->file_type = (uint8_t)disk_inode->i_type;
    file_inode->i_size = disk_inode->i_size;
    file_inode->i_mode = disk_inode->i_mode;
    file_inode->i_nlink = disk_inode->i_nlink;
    file_inode->i_atime = disk_inode->i_atime;
    file_inode->i_mtime = disk_inode->i_mtime;
    file_inode->i_atime_nsec = disk_inode->i_atime_nsec;
    file_inode->i_mtime_nsec = disk_inode->i_mtime_nsec;
    file_inode->i_ctime_nsec = disk_inode->i_ctime_nsec;
    file_inode->is_dirty = false;

    return 0;
}

static int rtfsFileInodeSyncMetadataToDisk(RtfsFileInode *file_inode)
{
    RtfsFileInodeNodeHandle *owned_handle;

    if (file_inode == NULL || file_inode->disk_inode == NULL ||
        file_inode->cache_handle == NULL) {
        return EINVAL;
    }

    file_inode->disk_inode->i_type = file_inode->file_type;
    file_inode->disk_inode->i_size = file_inode->i_size;
    file_inode->disk_inode->i_mode = file_inode->i_mode;
    file_inode->disk_inode->i_nlink = file_inode->i_nlink;
    file_inode->disk_inode->i_atime = file_inode->i_atime;
    file_inode->disk_inode->i_mtime = file_inode->i_mtime;
    file_inode->disk_inode->i_atime_nsec = file_inode->i_atime_nsec;
    file_inode->disk_inode->i_mtime_nsec = file_inode->i_mtime_nsec;
    file_inode->disk_inode->i_ctime_nsec = file_inode->i_ctime_nsec;

    owned_handle = (RtfsFileInodeNodeHandle *)file_inode->cache_handle;
    nodeBlockCacheEntryHandleMarkDirty(&owned_handle->node_handle);
    file_inode->is_dirty = false;

    return 0;
}

static void rtfsFileInodeTouchModifyTime(RtfsFileInode *file_inode)
{
    if (file_inode == NULL) {
        return;
    }

    file_inode->i_mtime++;
    file_inode->i_mtime_nsec = 0;
    file_inode->i_ctime_nsec = 0;
    file_inode->is_dirty = true;
}

static void rtfsFileInodeTouchAccessTime(RtfsFileInode *file_inode)
{
    if (file_inode == NULL) {
        return;
    }

    file_inode->i_atime++;
    file_inode->i_atime_nsec = 0;
    file_inode->is_dirty = true;
}

static int rtfsFileResolveDataLpaByBlockIndex(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint32_t block_index,
    uint32_t *out_lpa
)
{
    RtfsFileInodeNodeHandle *owned_handle;
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

    if (file_inode == NULL || file_inode->disk_inode == NULL ||
        file_inode->cache_handle == NULL || out_lpa == NULL) {
        return EINVAL;
    }

    *out_lpa = INVALID_LPA;

    owned_handle = (RtfsFileInodeNodeHandle *)file_inode->cache_handle;
    node_cache = owned_handle->node_cache;
    if (node_cache == NULL) {
        return EINVAL;
    }

    if (block_index < DEF_ADDRS_PER_INODE) {
        *out_lpa = file_inode->disk_inode->i_addr[block_index];
        return 0;
    }

    direct_block_index = block_index - DEF_ADDRS_PER_INODE;
    if (direct_block_index < (2U * DEF_ADDRS_PER_BLOCK)) {
        RtfsFileInodeNodeHandle direct_handle = {0};
        struct RtfsNode *direct_node;
        int ret;

        direct_node_slot = direct_block_index / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = direct_block_index % DEF_ADDRS_PER_BLOCK;
        if (file_inode->disk_inode->i_nid[direct_node_slot] == INVALID_NID) {
            return 0;
        }

        ret = rtfsFileGetCachedNodeByNid(
            fs_manager,
            node_cache,
            file_inode->disk_inode->i_nid[direct_node_slot],
            (uint32_t)file_inode->ino,
            &direct_handle
        );
        if (ret != 0) {
            return ret;
        }

        direct_node = nodeBlockCacheEntryGetNodeBlockPtr(
            direct_handle.node_handle.entry
        );
        *out_lpa = direct_node->dn.addr[direct_node_offset];
        nodeBlockCacheEntryHandleDestroy(&direct_handle.node_handle);
        return 0;
    }

    indirect_block_index = direct_block_index - (2U * DEF_ADDRS_PER_BLOCK);
    if (indirect_block_index < (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        RtfsFileInodeNodeHandle indirect_handle = {0};
        RtfsFileInodeNodeHandle direct_handle = {0};
        struct RtfsNode *indirect_node;
        struct RtfsNode *direct_node;
        int ret;

        indirect_node_slot =
            indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        indirect_node_offset =
            indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        direct_node_slot = indirect_node_offset / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = indirect_node_offset % DEF_ADDRS_PER_BLOCK;

        if (file_inode->disk_inode->i_nid[2 + indirect_node_slot] ==
            INVALID_NID) {
            return 0;
        }

        ret = rtfsFileGetCachedNodeByNid(
            fs_manager,
            node_cache,
            file_inode->disk_inode->i_nid[2 + indirect_node_slot],
            (uint32_t)file_inode->ino,
            &indirect_handle
        );
        if (ret != 0) {
            return ret;
        }

        indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(
            indirect_handle.node_handle.entry
        );
        if (indirect_node->in.nid[direct_node_slot] == INVALID_NID) {
            nodeBlockCacheEntryHandleDestroy(&indirect_handle.node_handle);
            return 0;
        }

        ret = rtfsFileGetCachedNodeByNid(
            fs_manager,
            node_cache,
            indirect_node->in.nid[direct_node_slot],
            indirect_node->footer.nid,
            &direct_handle
        );
        nodeBlockCacheEntryHandleDestroy(&indirect_handle.node_handle);
        if (ret != 0) {
            return ret;
        }

        direct_node = nodeBlockCacheEntryGetNodeBlockPtr(
            direct_handle.node_handle.entry
        );
        *out_lpa = direct_node->dn.addr[direct_node_offset];
        nodeBlockCacheEntryHandleDestroy(&direct_handle.node_handle);
        return 0;
    }

    double_indirect_block_index =
        indirect_block_index - (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
    if (double_indirect_block_index <
        (NIDS_PER_BLOCK * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        RtfsFileInodeNodeHandle dind_handle = {0};
        RtfsFileInodeNodeHandle level1_handle = {0};
        RtfsFileInodeNodeHandle direct_handle = {0};
        struct RtfsNode *dind_node;
        struct RtfsNode *level1_node;
        struct RtfsNode *direct_node;
        int ret;

        if (file_inode->disk_inode->i_nid[4] == INVALID_NID) {
            return 0;
        }

        first_level_slot =
            double_indirect_block_index /
            (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        double_indirect_offset =
            double_indirect_block_index %
            (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        second_level_slot = double_indirect_offset / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = double_indirect_offset % DEF_ADDRS_PER_BLOCK;

        ret = rtfsFileGetCachedNodeByNid(
            fs_manager,
            node_cache,
            file_inode->disk_inode->i_nid[4],
            (uint32_t)file_inode->ino,
            &dind_handle
        );
        if (ret != 0) {
            return ret;
        }

        dind_node = nodeBlockCacheEntryGetNodeBlockPtr(
            dind_handle.node_handle.entry
        );
        if (dind_node->in.nid[first_level_slot] == INVALID_NID) {
            nodeBlockCacheEntryHandleDestroy(&dind_handle.node_handle);
            return 0;
        }

        ret = rtfsFileGetCachedNodeByNid(
            fs_manager,
            node_cache,
            dind_node->in.nid[first_level_slot],
            dind_node->footer.nid,
            &level1_handle
        );
        nodeBlockCacheEntryHandleDestroy(&dind_handle.node_handle);
        if (ret != 0) {
            return ret;
        }

        level1_node = nodeBlockCacheEntryGetNodeBlockPtr(
            level1_handle.node_handle.entry
        );
        if (level1_node->in.nid[second_level_slot] == INVALID_NID) {
            nodeBlockCacheEntryHandleDestroy(&level1_handle.node_handle);
            return 0;
        }

        ret = rtfsFileGetCachedNodeByNid(
            fs_manager,
            node_cache,
            level1_node->in.nid[second_level_slot],
            level1_node->footer.nid,
            &direct_handle
        );
        nodeBlockCacheEntryHandleDestroy(&level1_handle.node_handle);
        if (ret != 0) {
            return ret;
        }

        direct_node = nodeBlockCacheEntryGetNodeBlockPtr(
            direct_handle.node_handle.entry
        );
        *out_lpa = direct_node->dn.addr[direct_node_offset];
        nodeBlockCacheEntryHandleDestroy(&direct_handle.node_handle);
        return 0;
    }

    return ENOSYS;
}

static int rtfsFileSetDataLpaByBlockIndex(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint32_t block_index,
    uint32_t new_lpa,
    bool create_missing_nodes,
    uint32_t *out_old_lpa
)
{
    RtfsFileInodeNodeHandle *owned_handle;
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

    if (out_old_lpa != NULL) {
        *out_old_lpa = INVALID_LPA;
    }

    if (file_inode == NULL || file_inode->disk_inode == NULL ||
        file_inode->cache_handle == NULL) {
        return EINVAL;
    }

    owned_handle = (RtfsFileInodeNodeHandle *)file_inode->cache_handle;
    node_cache = owned_handle->node_cache;
    if (node_cache == NULL) {
        return EINVAL;
    }

    if (block_index < DEF_ADDRS_PER_INODE) {
        uint32_t old_lpa = file_inode->disk_inode->i_addr[block_index];

        if (out_old_lpa != NULL) {
            *out_old_lpa = old_lpa;
        }

        if (old_lpa != new_lpa) {
            file_inode->disk_inode->i_addr[block_index] = new_lpa;
            nodeBlockCacheEntryHandleMarkDirty(&owned_handle->node_handle);
        }
        return 0;
    }

    direct_block_index = block_index - DEF_ADDRS_PER_INODE;
    if (direct_block_index < (2U * DEF_ADDRS_PER_BLOCK)) {
        RtfsFileInodeNodeHandle direct_handle = {0};
        struct RtfsNode *direct_node;
        int ret;

        direct_node_slot = direct_block_index / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = direct_block_index % DEF_ADDRS_PER_BLOCK;

        if (file_inode->disk_inode->i_nid[direct_node_slot] == INVALID_NID) {
            NodeBlockCacheHelper helper;
            NodeBlockCacheEntryHandle created_handle;

            if (new_lpa == INVALID_LPA) {
                return 0;
            }
            if (!create_missing_nodes) {
                return ENOENT;
            }
            if (fs_manager == NULL) {
                return EINVAL;
            }

            nodeBlockCacheHelperInit(&helper, fs_manager);
            created_handle = nodeBlockCacheHelperCreateNodeEntry(
                &helper,
                (uint32_t)file_inode->ino,
                DEF_ADDRS_PER_INODE + direct_node_slot + 1,
                (uint32_t)file_inode->ino
            );
            nodeBlockCacheHelperDestroy(&helper);
            if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                return ENOSPC;
            }

            direct_node = nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry);
            rtfsFileInodeInitCreatedDirectNode(direct_node);
            file_inode->disk_inode->i_nid[direct_node_slot] =
                direct_node->footer.nid;
            nodeBlockCacheEntryHandleMarkDirty(&owned_handle->node_handle);
            direct_handle.node_cache = node_cache;
            direct_handle.node_handle = created_handle;
        } else {
            ret = rtfsFileGetCachedNodeByNid(
                fs_manager,
                node_cache,
                file_inode->disk_inode->i_nid[direct_node_slot],
                (uint32_t)file_inode->ino,
                &direct_handle
            );
            if (ret != 0) {
                return ret;
            }
        }

        direct_node = nodeBlockCacheEntryGetNodeBlockPtr(
            direct_handle.node_handle.entry
        );
        if (out_old_lpa != NULL) {
            *out_old_lpa = direct_node->dn.addr[direct_node_offset];
        }
        if (direct_node->dn.addr[direct_node_offset] != new_lpa) {
            direct_node->dn.addr[direct_node_offset] = new_lpa;
            nodeBlockCacheEntryHandleMarkDirty(&direct_handle.node_handle);
        }
        nodeBlockCacheEntryHandleDestroy(&direct_handle.node_handle);
        return 0;
    }

    indirect_block_index = direct_block_index - (2U * DEF_ADDRS_PER_BLOCK);
    if (indirect_block_index < (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        RtfsFileInodeNodeHandle indirect_handle = {0};
        RtfsFileInodeNodeHandle direct_handle = {0};
        struct RtfsNode *indirect_node;
        struct RtfsNode *direct_node;
        int ret;

        indirect_node_slot =
            indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        indirect_node_offset =
            indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        direct_node_slot = indirect_node_offset / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = indirect_node_offset % DEF_ADDRS_PER_BLOCK;

        if (file_inode->disk_inode->i_nid[2 + indirect_node_slot] ==
            INVALID_NID) {
            NodeBlockCacheHelper helper;
            NodeBlockCacheEntryHandle created_handle;

            if (new_lpa == INVALID_LPA) {
                return 0;
            }
            if (!create_missing_nodes) {
                return ENOENT;
            }
            if (fs_manager == NULL) {
                return EINVAL;
            }

            nodeBlockCacheHelperInit(&helper, fs_manager);
            created_handle = nodeBlockCacheHelperCreateNodeEntry(
                &helper,
                (uint32_t)file_inode->ino,
                NODE_IND1_BLOCK + indirect_node_slot,
                (uint32_t)file_inode->ino
            );
            nodeBlockCacheHelperDestroy(&helper);
            if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                return ENOSPC;
            }

            indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(
                created_handle.entry
            );
            rtfsFileInodeInitCreatedIndirectNode(indirect_node);
            file_inode->disk_inode->i_nid[2 + indirect_node_slot] =
                indirect_node->footer.nid;
            nodeBlockCacheEntryHandleMarkDirty(&owned_handle->node_handle);
            indirect_handle.node_cache = node_cache;
            indirect_handle.node_handle = created_handle;
        } else {
            ret = rtfsFileGetCachedNodeByNid(
                fs_manager,
                node_cache,
                file_inode->disk_inode->i_nid[2 + indirect_node_slot],
                (uint32_t)file_inode->ino,
                &indirect_handle
            );
            if (ret != 0) {
                return ret;
            }
        }

        indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(
            indirect_handle.node_handle.entry
        );
        if (indirect_node->in.nid[direct_node_slot] == INVALID_NID) {
            NodeBlockCacheHelper helper;
            NodeBlockCacheEntryHandle created_handle;

            if (new_lpa == INVALID_LPA) {
                nodeBlockCacheEntryHandleDestroy(&indirect_handle.node_handle);
                return 0;
            }
            if (!create_missing_nodes) {
                nodeBlockCacheEntryHandleDestroy(&indirect_handle.node_handle);
                return ENOENT;
            }
            if (fs_manager == NULL) {
                nodeBlockCacheEntryHandleDestroy(&indirect_handle.node_handle);
                return EINVAL;
            }

            nodeBlockCacheHelperInit(&helper, fs_manager);
            created_handle = nodeBlockCacheHelperCreateNodeEntry(
                &helper,
                (uint32_t)file_inode->ino,
                0,
                indirect_node->footer.nid
            );
            nodeBlockCacheHelperDestroy(&helper);
            if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                nodeBlockCacheEntryHandleDestroy(&indirect_handle.node_handle);
                return ENOSPC;
            }

            direct_node = nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry);
            rtfsFileInodeInitCreatedDirectNode(direct_node);
            indirect_node->in.nid[direct_node_slot] = direct_node->footer.nid;
            nodeBlockCacheEntryHandleMarkDirty(&indirect_handle.node_handle);
            direct_handle.node_cache = node_cache;
            direct_handle.node_handle = created_handle;
        } else {
            ret = rtfsFileGetCachedNodeByNid(
                fs_manager,
                node_cache,
                indirect_node->in.nid[direct_node_slot],
                indirect_node->footer.nid,
                &direct_handle
            );
            if (ret != 0) {
                nodeBlockCacheEntryHandleDestroy(&indirect_handle.node_handle);
                return ret;
            }
        }
        nodeBlockCacheEntryHandleDestroy(&indirect_handle.node_handle);

        direct_node = nodeBlockCacheEntryGetNodeBlockPtr(
            direct_handle.node_handle.entry
        );
        if (out_old_lpa != NULL) {
            *out_old_lpa = direct_node->dn.addr[direct_node_offset];
        }
        if (direct_node->dn.addr[direct_node_offset] != new_lpa) {
            direct_node->dn.addr[direct_node_offset] = new_lpa;
            nodeBlockCacheEntryHandleMarkDirty(&direct_handle.node_handle);
        }
        nodeBlockCacheEntryHandleDestroy(&direct_handle.node_handle);
        return 0;
    }

    double_indirect_block_index =
        indirect_block_index - (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
    if (double_indirect_block_index <
        (NIDS_PER_BLOCK * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        RtfsFileInodeNodeHandle dind_handle = {0};
        RtfsFileInodeNodeHandle level1_handle = {0};
        RtfsFileInodeNodeHandle direct_handle = {0};
        struct RtfsNode *dind_node;
        struct RtfsNode *level1_node;
        struct RtfsNode *direct_node;
        int ret;

        first_level_slot =
            double_indirect_block_index /
            (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        double_indirect_offset =
            double_indirect_block_index %
            (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        second_level_slot = double_indirect_offset / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = double_indirect_offset % DEF_ADDRS_PER_BLOCK;

        if (file_inode->disk_inode->i_nid[4] == INVALID_NID) {
            NodeBlockCacheHelper helper;
            NodeBlockCacheEntryHandle created_handle;

            if (new_lpa == INVALID_LPA) {
                return 0;
            }
            if (!create_missing_nodes) {
                return ENOENT;
            }
            if (fs_manager == NULL) {
                return EINVAL;
            }

            nodeBlockCacheHelperInit(&helper, fs_manager);
            created_handle = nodeBlockCacheHelperCreateNodeEntry(
                &helper,
                (uint32_t)file_inode->ino,
                NODE_DIND_BLOCK,
                (uint32_t)file_inode->ino
            );
            nodeBlockCacheHelperDestroy(&helper);
            if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                return ENOSPC;
            }

            dind_node = nodeBlockCacheEntryGetNodeBlockPtr(created_handle.entry);
            rtfsFileInodeInitCreatedIndirectNode(dind_node);
            file_inode->disk_inode->i_nid[4] = dind_node->footer.nid;
            nodeBlockCacheEntryHandleMarkDirty(&owned_handle->node_handle);
            dind_handle.node_cache = node_cache;
            dind_handle.node_handle = created_handle;
        } else {
            ret = rtfsFileGetCachedNodeByNid(
                fs_manager,
                node_cache,
                file_inode->disk_inode->i_nid[4],
                (uint32_t)file_inode->ino,
                &dind_handle
            );
            if (ret != 0) {
                return ret;
            }
        }

        dind_node = nodeBlockCacheEntryGetNodeBlockPtr(
            dind_handle.node_handle.entry
        );
        if (dind_node->in.nid[first_level_slot] == INVALID_NID) {
            NodeBlockCacheHelper helper;
            NodeBlockCacheEntryHandle created_handle;

            if (new_lpa == INVALID_LPA) {
                nodeBlockCacheEntryHandleDestroy(&dind_handle.node_handle);
                return 0;
            }
            if (!create_missing_nodes) {
                nodeBlockCacheEntryHandleDestroy(&dind_handle.node_handle);
                return ENOENT;
            }
            if (fs_manager == NULL) {
                nodeBlockCacheEntryHandleDestroy(&dind_handle.node_handle);
                return EINVAL;
            }

            nodeBlockCacheHelperInit(&helper, fs_manager);
            created_handle = nodeBlockCacheHelperCreateNodeEntry(
                &helper,
                (uint32_t)file_inode->ino,
                0,
                dind_node->footer.nid
            );
            nodeBlockCacheHelperDestroy(&helper);
            if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                nodeBlockCacheEntryHandleDestroy(&dind_handle.node_handle);
                return ENOSPC;
            }

            level1_node = nodeBlockCacheEntryGetNodeBlockPtr(
                created_handle.entry
            );
            rtfsFileInodeInitCreatedIndirectNode(level1_node);
            dind_node->in.nid[first_level_slot] = level1_node->footer.nid;
            nodeBlockCacheEntryHandleMarkDirty(&dind_handle.node_handle);
            level1_handle.node_cache = node_cache;
            level1_handle.node_handle = created_handle;
        } else {
            ret = rtfsFileGetCachedNodeByNid(
                fs_manager,
                node_cache,
                dind_node->in.nid[first_level_slot],
                dind_node->footer.nid,
                &level1_handle
            );
            if (ret != 0) {
                nodeBlockCacheEntryHandleDestroy(&dind_handle.node_handle);
                return ret;
            }
        }
        nodeBlockCacheEntryHandleDestroy(&dind_handle.node_handle);

        level1_node = nodeBlockCacheEntryGetNodeBlockPtr(
            level1_handle.node_handle.entry
        );
        if (level1_node->in.nid[second_level_slot] == INVALID_NID) {
            NodeBlockCacheHelper helper;
            NodeBlockCacheEntryHandle created_handle;

            if (new_lpa == INVALID_LPA) {
                nodeBlockCacheEntryHandleDestroy(&level1_handle.node_handle);
                return 0;
            }
            if (!create_missing_nodes) {
                nodeBlockCacheEntryHandleDestroy(&level1_handle.node_handle);
                return ENOENT;
            }
            if (fs_manager == NULL) {
                nodeBlockCacheEntryHandleDestroy(&level1_handle.node_handle);
                return EINVAL;
            }

            nodeBlockCacheHelperInit(&helper, fs_manager);
            created_handle = nodeBlockCacheHelperCreateNodeEntry(
                &helper,
                (uint32_t)file_inode->ino,
                0,
                level1_node->footer.nid
            );
            nodeBlockCacheHelperDestroy(&helper);
            if (nodeBlockCacheEntryHandleIsEmpty(&created_handle)) {
                nodeBlockCacheEntryHandleDestroy(&level1_handle.node_handle);
                return ENOSPC;
            }

            direct_node = nodeBlockCacheEntryGetNodeBlockPtr(
                created_handle.entry
            );
            rtfsFileInodeInitCreatedDirectNode(direct_node);
            level1_node->in.nid[second_level_slot] = direct_node->footer.nid;
            nodeBlockCacheEntryHandleMarkDirty(&level1_handle.node_handle);
            direct_handle.node_cache = node_cache;
            direct_handle.node_handle = created_handle;
        } else {
            ret = rtfsFileGetCachedNodeByNid(
                fs_manager,
                node_cache,
                level1_node->in.nid[second_level_slot],
                level1_node->footer.nid,
                &direct_handle
            );
            if (ret != 0) {
                nodeBlockCacheEntryHandleDestroy(&level1_handle.node_handle);
                return ret;
            }
        }
        nodeBlockCacheEntryHandleDestroy(&level1_handle.node_handle);

        direct_node = nodeBlockCacheEntryGetNodeBlockPtr(
            direct_handle.node_handle.entry
        );
        if (out_old_lpa != NULL) {
            *out_old_lpa = direct_node->dn.addr[direct_node_offset];
        }
        if (direct_node->dn.addr[direct_node_offset] != new_lpa) {
            direct_node->dn.addr[direct_node_offset] = new_lpa;
            nodeBlockCacheEntryHandleMarkDirty(&direct_handle.node_handle);
        }
        nodeBlockCacheEntryHandleDestroy(&direct_handle.node_handle);
        return 0;
    }

    return ENOSYS;
}

static int rtfsFileGetCachedNodeByNid(
    file_system_manager *fs_manager,
    NodeBlockCache *node_cache,
    uint32_t nid,
    uint32_t parent_nid,
    RtfsFileInodeNodeHandle *out_handle
)
{
    NodeBlockCacheEntryHandle node_handle;

    if (node_cache == NULL || out_handle == NULL || nid == INVALID_NID) {
        return EINVAL;
    }

    out_handle->node_cache = NULL;
    out_handle->node_handle.cache = NULL;
    out_handle->node_handle.entry = NULL;

    node_handle = nodeBlockCacheGet(node_cache, nid);
    if (nodeBlockCacheEntryHandleIsEmpty(&node_handle)) {
        NodeBlockCacheHelper helper;

        if (fs_manager == NULL) {
            return ENOENT;
        }

        nodeBlockCacheHelperInit(&helper, fs_manager);
        if (nodeBlockCacheHelperGetNodeEntry(
                &helper,
                nid,
                parent_nid,
                &node_handle
            ) != 0) {
            nodeBlockCacheHelperDestroy(&helper);
            return EIO;
        }
        nodeBlockCacheHelperDestroy(&helper);
    }

    if (nodeBlockCacheEntryHandleIsEmpty(&node_handle)) {
        return ENOENT;
    }

    out_handle->node_cache = node_cache;
    out_handle->node_handle = node_handle;

    return 0;
}

static void rtfsFileInodeInitCreatedDirectNode(struct RtfsNode *node)
{
    size_t i;

    if (node == NULL) {
        return;
    }

    for (i = 0; i < DEF_ADDRS_PER_BLOCK; ++i) {
        node->dn.addr[i] = INVALID_LPA;
    }
}

static void rtfsFileInodeInitCreatedIndirectNode(struct RtfsNode *node)
{
    size_t i;

    if (node == NULL) {
        return;
    }

    for (i = 0; i < NIDS_PER_BLOCK; ++i) {
        node->in.nid[i] = INVALID_NID;
    }
}

static int rtfsFileInodePreparePageForRead(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint32_t block_index,
    PageEntryHandle *out_page
)
{
    PageEntryHandle page_handle;
    PageEntry *page_entry;
    BlockBuffer *page_buffer;
    uint32_t lpa;
    int ret;

    if (file_inode == NULL || out_page == NULL) {
        return EINVAL;
    }

    out_page->cache = NULL;
    out_page->entry = NULL;

    page_handle = pageCacheGet(&file_inode->page_cache, block_index);
    page_entry = page_handle.entry;
    if (page_entry == NULL) {
        return ENOMEM;
    }

    rtfsMutexLock(pageEntryGetLock(page_entry));

    if (pageEntryGetState(page_entry) == PAGE_READY) {
        rtfsMutexUnlock(pageEntryGetLock(page_entry));
        *out_page = page_handle;
        return 0;
    }

    ret = rtfsFileResolveDataLpaByBlockIndex(
        fs_manager,
        file_inode,
        block_index,
        &lpa
    );
    if (ret != 0) {
        rtfsMutexUnlock(pageEntryGetLock(page_entry));
        pageEntryHandleDestroy(&page_handle);
        return ret;
    }

    page_buffer = pageEntryGetBuffer(page_entry);
    if (lpa == INVALID_LPA) {
        memset(blockBufferGetPtr(page_buffer), 0, BLOCK_BUFFER_SIZE);
    } else {
        comm_dev *dev;

        if (fs_manager == NULL) {
            rtfsMutexUnlock(pageEntryGetLock(page_entry));
            pageEntryHandleDestroy(&page_handle);
            return EINVAL;
        }

        dev = fileSystemManagerGetDevice(fs_manager);
        if (dev == NULL) {
            rtfsMutexUnlock(pageEntryGetLock(page_entry));
            pageEntryHandleDestroy(&page_handle);
            return EINVAL;
        }

        if (g_rtfs_file_inode_read_block_hook != NULL) {
            ret = g_rtfs_file_inode_read_block_hook(
                dev,
                lpa,
                blockBufferGetPtr(page_buffer)
            );
            if (ret != 0) {
                rtfsMutexUnlock(pageEntryGetLock(page_entry));
                pageEntryHandleDestroy(&page_handle);
                return ret;
            }
        } else {
            ret = blockBufferReadFromLpa(page_buffer, dev, lpa);
            if (ret != 0) {
                rtfsMutexUnlock(pageEntryGetLock(page_entry));
                pageEntryHandleDestroy(&page_handle);
                return ret;
            }
        }
    }

    pageEntrySetLpa(page_entry, lpa);
    pageEntrySetState(page_entry, PAGE_READY);

    rtfsMutexUnlock(pageEntryGetLock(page_entry));
    *out_page = page_handle;
    return 0;
}

static int rtfsFileInodePreparePageForWrite(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint32_t block_index,
    bool preserve_existing_content,
    PageEntryHandle *out_page
)
{
    PageEntryHandle page_handle;
    PageEntry *page_entry;
    uint32_t lpa;
    int ret;

    if (preserve_existing_content) {
        return rtfsFileInodePreparePageForRead(
            fs_manager,
            file_inode,
            block_index,
            out_page
        );
    }

    if (file_inode == NULL || out_page == NULL) {
        return EINVAL;
    }

    out_page->cache = NULL;
    out_page->entry = NULL;

    page_handle = pageCacheGet(&file_inode->page_cache, block_index);
    page_entry = page_handle.entry;
    if (page_entry == NULL) {
        return ENOMEM;
    }

    rtfsMutexLock(pageEntryGetLock(page_entry));
    if (pageEntryGetState(page_entry) == PAGE_READY) {
        rtfsMutexUnlock(pageEntryGetLock(page_entry));
        *out_page = page_handle;
        return 0;
    }

    if ((uint64_t)block_index >= SIZE_TO_BLOCK(file_inode->i_size)) {
        pageEntrySetLpa(page_entry, INVALID_LPA);
        rtfsMutexUnlock(pageEntryGetLock(page_entry));
        *out_page = page_handle;
        return 0;
    }

    ret = rtfsFileResolveDataLpaByBlockIndex(
        fs_manager,
        file_inode,
        block_index,
        &lpa
    );
    if (ret != 0) {
        rtfsMutexUnlock(pageEntryGetLock(page_entry));
        pageEntryHandleDestroy(&page_handle);
        return ret;
    }

    pageEntrySetLpa(page_entry, lpa);
    rtfsMutexUnlock(pageEntryGetLock(page_entry));
    *out_page = page_handle;
    return 0;
}

static int rtfsFileInodeAppendPendingDataRelocation(
    RtfsFileInode *file_inode,
    uint32_t block_index,
    uint32_t old_lpa,
    uint32_t new_lpa
)
{
    RtfsFilePendingDataCowRelocation *new_items;
    size_t new_capacity;

    if (file_inode == NULL || new_lpa == INVALID_LPA) {
        return EINVAL;
    }

    if (file_inode->pending_data_relocation_count >=
        file_inode->pending_data_relocation_capacity) {
        new_capacity =
            file_inode->pending_data_relocation_capacity == 0 ?
            8 :
            file_inode->pending_data_relocation_capacity * 2;

        new_items = (RtfsFilePendingDataCowRelocation *)realloc(
            file_inode->pending_data_relocations,
            new_capacity * sizeof(*new_items)
        );
        if (new_items == NULL) {
            return ENOMEM;
        }

        file_inode->pending_data_relocations = new_items;
        file_inode->pending_data_relocation_capacity = new_capacity;
    }

    file_inode->pending_data_relocations[
        file_inode->pending_data_relocation_count
    ].block_index = block_index;
    file_inode->pending_data_relocations[
        file_inode->pending_data_relocation_count
    ].old_lpa = old_lpa;
    file_inode->pending_data_relocations[
        file_inode->pending_data_relocation_count
    ].new_lpa = new_lpa;
    file_inode->pending_data_relocation_count++;

    return 0;
}

static int rtfsFileInodeAppendPendingTruncateOldLpa(
    RtfsFileInode *file_inode,
    uint32_t old_lpa
)
{
    uint32_t *new_items;
    size_t new_capacity;

    if (file_inode == NULL) {
        return EINVAL;
    }

    if (old_lpa == INVALID_LPA) {
        return 0;
    }

    if (file_inode->pending_truncate_old_lpa_count >=
        file_inode->pending_truncate_old_lpa_capacity) {
        new_capacity =
            file_inode->pending_truncate_old_lpa_capacity == 0 ?
            8 :
            file_inode->pending_truncate_old_lpa_capacity * 2;

        new_items = (uint32_t *)realloc(
            file_inode->pending_truncate_old_lpas,
            new_capacity * sizeof(*new_items)
        );
        if (new_items == NULL) {
            return ENOMEM;
        }

        file_inode->pending_truncate_old_lpas = new_items;
        file_inode->pending_truncate_old_lpa_capacity = new_capacity;
    }

    file_inode->pending_truncate_old_lpas[
        file_inode->pending_truncate_old_lpa_count++
    ] = old_lpa;

    return 0;
}

static void rtfsFileInodeClearPendingCowState(RtfsFileInode *file_inode)
{
    if (file_inode == NULL) {
        return;
    }

    file_inode->pending_data_relocation_count = 0;
    file_inode->pending_truncate_old_lpa_count = 0;
}

static int rtfsFileInodeWriteDirtyPagesCow(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
)
{
    comm_dev *dev;
    super_manager *sp_manager;
    SitOperator sit_op;
    kbtree_t(ktfipdpn) *dirty_tree;
    kbitr_t itr;
    RtfsFileDirtyPageWriteOp *ops = NULL;
    size_t op_capacity;
    size_t op_count = 0;
    size_t i;
    int ret = 0;

    if (fs_manager == NULL || file_inode == NULL) {
        return EINVAL;
    }

    dev = fileSystemManagerGetDevice(fs_manager);
    sp_manager = fileSystemManagerGetSuperManager(fs_manager);
    if (dev == NULL || sp_manager == NULL) {
        return EINVAL;
    }
    sitOperatorInit(&sit_op, fs_manager);

    dirty_tree = (kbtree_t(ktfipdpn) *)pageCacheGetDirtyPages(
        &file_inode->page_cache
    );
    if (dirty_tree == NULL) {
        return EINVAL;
    }

    op_capacity = kb_size(dirty_tree);
    if (op_capacity == 0) {
        return 0;
    }

    ops = (RtfsFileDirtyPageWriteOp *)calloc(op_capacity, sizeof(*ops));
    if (ops == NULL) {
        return ENOMEM;
    }

    rtfsMutexLock(&file_inode->page_cache.dirtyPagesLock);
    for (kb_itr_first(ktfipdpn, dirty_tree, &itr);
         kb_itr_valid(&itr);
         kb_itr_next(ktfipdpn, dirty_tree, &itr)) {
        DirtyPagesNode *dirty_node = &kb_itr_key(DirtyPagesNode, &itr);
        PageEntry *page_entry = dirty_node->handle.entry;

        if (page_entry == NULL) {
            continue;
        }

        if (!atomic_load(&page_entry->isDirty)) {
            continue;
        }

        rtfsMutexLock(pageEntryGetLock(page_entry));
        if (pageEntryGetState(page_entry) != PAGE_READY) {
            rtfsMutexUnlock(pageEntryGetLock(page_entry));
            ret = EINVAL;
            break;
        }

        ops[op_count].page_entry = page_entry;
        ops[op_count].block_index = dirty_node->blkoff;
        ops[op_count].old_lpa = pageEntryGetLpa(page_entry);
        ops[op_count].new_lpa = superManagerAllocDataLpa(sp_manager);
        if (ops[op_count].new_lpa == INVALID_LPA) {
            rtfsMutexUnlock(pageEntryGetLock(page_entry));
            ret = ENOSPC;
            break;
        }
        ++op_count;
        rtfsMutexUnlock(pageEntryGetLock(page_entry));
    }
    rtfsMutexUnlock(&file_inode->page_cache.dirtyPagesLock);

    if (ret != 0) {
        goto cleanup;
    }

    if (g_rtfs_file_inode_write_block_hook != NULL) {
        for (i = 0; i < op_count; ++i) {
            ret = g_rtfs_file_inode_write_block_hook(
                dev,
                ops[i].new_lpa,
                blockBufferGetPtr(pageEntryGetBuffer(ops[i].page_entry))
            );
            if (ret != 0) {
                goto cleanup;
            }
        }
    } else {
        size_t max_batch_blocks = RTFS_FILE_WRITEBACK_BATCH_PAGES;
        char *batch_buffer;

        batch_buffer = (char *)comm_alloc_dma_mem(
            max_batch_blocks * BLOCK_BUFFER_SIZE
        );
        if (batch_buffer == NULL) {
            ret = ENOMEM;
            goto cleanup;
        }

        for (i = 0; i < op_count;) {
            size_t batch_blocks = 1;
            size_t j;

            while (i + batch_blocks < op_count &&
                   batch_blocks < max_batch_blocks &&
                   ops[i + batch_blocks].block_index ==
                       ops[i].block_index + batch_blocks &&
                   ops[i + batch_blocks].new_lpa ==
                       ops[i].new_lpa + batch_blocks) {
                ++batch_blocks;
            }

            for (j = 0; j < batch_blocks; ++j) {
                memcpy(
                    batch_buffer + j * BLOCK_BUFFER_SIZE,
                    blockBufferGetPtr(
                        pageEntryGetBuffer(ops[i + j].page_entry)
                    ),
                    BLOCK_BUFFER_SIZE
                );
            }

            ret = comm_submit_sync_rw_request(
                dev,
                batch_buffer,
                LPA_TO_LBA(ops[i].new_lpa),
                (uint32_t)(batch_blocks * LBA_PER_LPA),
                COMM_IO_WRITE
            );
            if (ret != 0) {
                comm_free_dma_mem(batch_buffer);
                goto cleanup;
            }

            i += batch_blocks;
        }

        comm_free_dma_mem(batch_buffer);
    }

    for (i = 0; i < op_count; ++i) {
        ret = rtfsFileInodeAppendPendingDataRelocation(
            file_inode,
            ops[i].block_index,
            ops[i].old_lpa,
            ops[i].new_lpa
        );
        if (ret != 0) {
            goto cleanup;
        }
    }

cleanup:
    if (ret != 0) {
        for (i = 0; i < op_count; ++i) {
            sitInvalidateLpa(&sit_op, ops[i].new_lpa);
        }
    }

    free(ops);

    return ret;
}

static int rtfsFileInodeApplyPendingDataRelocations(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
)
{
    size_t i;
    SrmapUtils *srmap_utils;

    if (fs_manager == NULL || file_inode == NULL) {
        return EINVAL;
    }

    if (file_inode->pending_data_relocation_count == 0) {
        return 0;
    }

    srmap_utils = fileSystemManagerGetSrmapUtils(fs_manager);
    for (i = 0; i < file_inode->pending_data_relocation_count; ++i) {
        RtfsFilePendingDataCowRelocation *relocation =
            &file_inode->pending_data_relocations[i];
        uint32_t old_lpa = INVALID_LPA;
        PageEntry *page_entry;
        int ret;

        if (relocation->new_lpa == INVALID_LPA) {
            continue;
        }

        ret = rtfsFileSetDataLpaByBlockIndex(
            fs_manager,
            file_inode,
            relocation->block_index,
            relocation->new_lpa,
            true,
            &old_lpa
        );
        if (ret != 0) {
            return ret;
        }

        if (relocation->old_lpa == INVALID_LPA) {
            relocation->old_lpa = old_lpa;
        }

        if (srmap_utils != NULL) {
            srmapUtilsWriteSrmapOfData(
                srmap_utils,
                relocation->new_lpa,
                file_inode->ino,
                relocation->block_index
            );
        }

        rtfsMutexLock(&file_inode->page_cache.cacheLock);
        page_entry = (PageEntry *)genericCacheManagerGet(
            &file_inode->page_cache.cacheManager,
            relocation->block_index,
            false
        );
        if (page_entry != NULL) {
            rtfsMutexLock(pageEntryGetLock(page_entry));
            pageEntrySetLpa(page_entry, relocation->new_lpa);
            pageEntrySetState(page_entry, PAGE_READY);
            rtfsMutexUnlock(pageEntryGetLock(page_entry));
        }
        rtfsMutexUnlock(&file_inode->page_cache.cacheLock);

        relocation->new_lpa = INVALID_LPA;
    }

    if (srmap_utils != NULL) {
        srmapUtilsWriteDirtySrmapSync(srmap_utils);
    }

    pageCacheClearDirtyPages(&file_inode->page_cache);
    file_inode->pending_data_relocation_count = 0;
    return 0;
}

static void rtfsFileInodeInvalidateCachedPagesFrom(
    RtfsFileInode *file_inode,
    uint64_t target_size
)
{
    uint64_t first_invalid_block;
    khash_t(khcim) *index;
    khiter_t k;

    if (file_inode == NULL) {
        return;
    }

    if (target_size == 0) {
        pageCacheClearDirtyPages(&file_inode->page_cache);
        first_invalid_block = 0;
    } else if ((target_size % BLOCK_BUFFER_SIZE) == 0) {
        first_invalid_block = target_size / BLOCK_BUFFER_SIZE;
        if (first_invalid_block == 0) {
            pageCacheClearDirtyPages(&file_inode->page_cache);
        } else if (first_invalid_block - 1 <= UINT32_MAX) {
            pageCacheTruncate(
                &file_inode->page_cache,
                (uint32_t)(first_invalid_block - 1)
            );
        }
    } else {
        first_invalid_block = (target_size / BLOCK_BUFFER_SIZE) + 1;
        if (target_size / BLOCK_BUFFER_SIZE <= UINT32_MAX) {
            pageCacheTruncate(
                &file_inode->page_cache,
                (uint32_t)(target_size / BLOCK_BUFFER_SIZE)
            );
        }
    }

    if (first_invalid_block > UINT32_MAX) {
        return;
    }

    /*
     * pageCacheTruncate 只清理 dirty pages 集合；这里额外把普通缓存项置 invalid，
     * 避免 truncate 后再扩展文件时读到旧的 clean page 内容。
     */
    rtfsMutexLock(&file_inode->page_cache.cacheLock);
    index = file_inode->page_cache.cacheManager.index.index;
    if (index != NULL) {
        for (k = kh_begin(index); k != kh_end(index); ++k) {
            PageEntry *entry;

            if (!kh_exist(index, k)) {
                continue;
            }

            entry = (PageEntry *)kh_value(index, k);
            if (entry == NULL || entry->blkoff < first_invalid_block) {
                continue;
            }

            rtfsMutexLock(pageEntryGetLock(entry));
            pageEntrySetLpa(entry, INVALID_LPA);
            pageEntrySetState(entry, PAGE_INVALID);
            atomic_store(&entry->isDirty, false);
            rtfsMutexUnlock(pageEntryGetLock(entry));
        }
    }
    rtfsMutexUnlock(&file_inode->page_cache.cacheLock);
}

static JournalContainer *rtfsFileInodeCloneJournalContainer(
    const JournalContainer *src
)
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
        SuperBlockJournalEntry entry =
            kv_A(src->superBlockJournal, i);
        journalContainerAppendSuperBlockJournalEntry(dst, &entry);
    }
    for (i = 0; i < kv_size(src->natJournal); ++i) {
        NatJournalEntry entry = kv_A(src->natJournal, i);
        journalContainerAppendNatJournalEntry(dst, &entry);
    }
    for (i = 0; i < kv_size(src->sitJournal); ++i) {
        SitJournalEntry entry = kv_A(src->sitJournal, i);
        journalContainerAppendSitJournalEntry(dst, &entry);
    }

    return dst;
}

static int rtfsFileInodeSubmitJournal(
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

    RTFS_LOG(
        RTFS_LOG_INFO,
        "file submit journal tx_id=%llu sb=%zu nat=%zu sit=%zu hook=%d",
        (unsigned long long)tx_id,
        kv_size(journal->superBlockJournal),
        kv_size(journal->natJournal),
        kv_size(journal->sitJournal),
        g_rtfs_file_inode_journal_commit_hook != NULL ? 1 : 0
    );

    if (out_tx_id != NULL) {
        *out_tx_id = tx_id;
    }

    if (g_rtfs_file_inode_journal_commit_hook != NULL) {
        return g_rtfs_file_inode_journal_commit_hook(journal);
    }

    {
        JournalProcessEnv *env = journalProcessEnvGetInstance();
        journalProcessEnvCommitJournal(env, journal);
    }

    (void)fs_manager;
    return 0;
}
