#ifndef _DIR_INODE_H_
#define _DIR_INODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>

#include "fs/fs.h"
#include "inode/inode.h"

typedef struct file_system_manager file_system_manager;
typedef struct comm_dev comm_dev;
typedef struct NodeBlockCache NodeBlockCache;
typedef struct JournalContainer JournalContainer;
typedef struct RtfsDirInode RtfsDirInode;
typedef struct RtfsDirInodeCache RtfsDirInodeCache;

typedef struct RtfsDirLookupResult
{
    RtfsRuntimeInodeView inode_view;
} RtfsDirLookupResult;

/**
 * @brief 创建目录 inode 层使用的轻量 cache 包装器。
 */
RtfsDirInodeCache *rtfsDirInodeCacheCreate(NodeBlockCache *node_cache);

/**
 * @brief 销毁目录 inode cache 包装器本身，不销毁底层 node cache。
 */
void rtfsDirInodeCacheDestroy(RtfsDirInodeCache *cache);

/**
 * @brief 根据给定 ino 构造目录运行时对象，并返回明确错误码。
 * @return 成功返回 0，失败返回错误码值。
 */
int rtfsDirInodeBuild(
    RtfsDirInodeCache *cache,
    rtfs_ino ino,
    RtfsDirInode **out_dir_inode
);

/**
 * @brief 释放一个目录运行时对象。
 */
void rtfsDirInodePut(RtfsDirInode *dir_inode);

/**
 * @brief 将一个普通目录块中的目录项导入到目录运行时对象中。
 * @return 成功返回 0，失败返回错误码值。
 */
int rtfsDirInodeAppendDentryBlock(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block
);

/**
 * @brief 将一个普通目录块中的目录项按指定逻辑块号与 lpa 导入到目录运行时对象中。
 * @return 成功返回 0，失败返回错误码值。
 */
int rtfsDirInodeAppendDentryBlockAt(
    RtfsDirInode *dir_inode,
    const struct RtfsDentryBlock *dentry_block,
    uint32_t block_index,
    uint32_t lpa
);

/**
 * @brief 返回目录对象当前是否已经完成全部目录块装载。
 */
bool rtfsDirInodeIsFullyLoaded(const RtfsDirInode *dir_inode);

/**
 * @brief 返回目录对象当前已经装载的目录逻辑块数量。
 */
size_t rtfsDirInodeGetLoadedBlockCount(const RtfsDirInode *dir_inode);

/**
 * @brief 返回目录对象理论上的目录逻辑块总数。
 */
size_t rtfsDirInodeGetTotalBlockCount(const RtfsDirInode *dir_inode);

/**
 * @brief 更新目录对象的已加载块进度，并自动刷新 fully-loaded 状态。
 */
void rtfsDirInodeSetLoadedBlockCount(
    RtfsDirInode *dir_inode,
    size_t loaded_block_count
);

/**
 * @brief 返回指定 loaded regular block 当前是否仍处于 pending relocation 状态。
 * 该接口仅用于测试和轻量状态观测，不暴露内部结构布局。
 */
bool rtfsDirInodeLoadedBlockHasPendingCowRelocation(
    const RtfsDirInode *dir_inode,
    uint32_t block_index
);

/**
 * @brief 返回指定 loaded regular block 当前记录的 lpa。
 * 若 block 不存在，返回 INVALID_LPA。
 */
uint32_t rtfsDirInodeGetLoadedBlockLpa(
    const RtfsDirInode *dir_inode,
    uint32_t block_index
);

/**
 * @brief 返回指定 loaded regular block 当前记录的 cow_new_lpa。
 * 若 block 不存在，返回 INVALID_LPA。
 */
uint32_t rtfsDirInodeGetLoadedBlockCowNewLpa(
    const RtfsDirInode *dir_inode,
    uint32_t block_index
);

/**
 * @brief 在目录对象中查找指定名字的子项。
 */
int rtfsDirInodeLookup(
    const RtfsDirInode *dir_inode,
    const char *name,
    size_t namelen,
    RtfsDirLookupResult *result
);

/**
 * @brief 以 struct dirent 序列的形式顺序读出目录项。
 */
ssize_t rtfsDirInodeReadEntries(
    const RtfsDirInode *dir_inode,
    off_t *offset,
    void *buffer,
    size_t count
);

/**
 * @brief 向目录对象添加一个目录项。
 */
int rtfsDirInodeAddEntry(
    RtfsDirInode *dir_inode,
    const char *name,
    const RtfsRuntimeInodeView *child_view
);

/**
 * @brief 从目录对象中删除一个目录项。
 */
int rtfsDirInodeRemoveEntry(
    RtfsDirInode *dir_inode,
    const char *name
);

/**
 * @brief 执行第一阶段的 content-COW writeback：为 dirty regular directory
 * blocks 分配新的 data lpa，并将当前 block image 写到新位置。
 * @return 成功返回 0，失败返回错误码值。
 */
int rtfsDirInodeWritebackContentCow(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
);

/**
 * @brief 将已经完成 content-COW 的 pending relocation 应用到 inode/node tree
 * 的内存视图中，即把对应逻辑块地址切换到 cow_new_lpa。
 * @return 成功返回 0，失败返回错误码值。
 */
int rtfsDirInodeApplyPendingCowRelocations(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
);

int rtfsDirInodeCollectPendingDataCowOldLpas(
    RtfsDirInode *dir_inode,
    uint32_t *out_array,
    size_t max_count,
    size_t *out_count
);

typedef int (*rtfs_dir_inode_write_block_hook)(
    comm_dev *dev,
    uint32_t lpa,
    const void *buffer
);

typedef int (*rtfs_dir_inode_journal_commit_hook)(JournalContainer *journal);

void rtfsDirInodeSetWriteBlockHook(rtfs_dir_inode_write_block_hook hook);
void rtfsDirInodeSetJournalCommitHook(rtfs_dir_inode_journal_commit_hook hook);

int rtfsDirInodeCommitCowWritebackWithTxId(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode,
    uint64_t *out_tx_id
);

int rtfsDirInodeCommitCowWriteback(
    file_system_manager *fs_manager,
    RtfsDirInode *dir_inode
);

#endif
