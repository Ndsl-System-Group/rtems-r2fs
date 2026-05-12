#ifndef _FILE_INODE_H_
#define _FILE_INODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "fs/fs.h"
#include "inode/inode.h"

typedef struct file_system_manager file_system_manager;
typedef struct comm_dev comm_dev;
typedef struct NodeBlockCache NodeBlockCache;
typedef struct JournalContainer JournalContainer;
typedef struct RtfsFileInode RtfsFileInode;
typedef struct RtfsFileInodeCache RtfsFileInodeCache;

/*
 * 创建普通文件 inode 层使用的轻量 cache 包装器。
 * 该包装器不拥有也不销毁底层 node cache。
 */
RtfsFileInodeCache *rtfsFileInodeCacheCreate(NodeBlockCache *node_cache);

void rtfsFileInodeCacheDestroy(RtfsFileInodeCache *cache);

/*
 * 根据 node cache 中已有的 inode node 构造普通文件运行时 inode 对象。
 * 成功返回 0，失败返回 errno 风格的错误码。
 */
int rtfsFileInodeBuild(
    RtfsFileInodeCache *cache,
    rtfs_ino ino,
    RtfsFileInode **out_file_inode
);

/**
 * 释放一个目录运行时对象。
 */
void rtfsFileInodePut(RtfsFileInode *file_inode);

uint64_t rtfsFileInodeGetSize(const RtfsFileInode *file_inode);

/*
 * 通过普通文件 inode 的 page cache 读写文件内容。
 * offset 会按照实际传输的字节数推进。失败时返回 -1 并设置 errno。
 */
ssize_t rtfsFileInodeRead(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    off_t *offset,
    void *buffer,
    size_t count
);

ssize_t rtfsFileInodeWrite(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    off_t *offset,
    const void *buffer,
    size_t count
);

/*
 * 调整普通文件大小。
 * 扩大文件时只修改元数据，新增长度保持为空洞；缩小文件时清理目标大小
 * 之后的映射，并把旧块留给后续 COW 提交流程回收。
 */
int rtfsFileInodeTruncate(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint64_t target_size
);

/*
 * 数据 content-COW 写回的第一阶段：为 dirty pages 分配新的 data LPA，
 * 并把 page 内容写到新位置。此阶段暂不切换 inode/node tree 中的映射。
 */
int rtfsFileInodeWritebackContentCow(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
);

/*
 * 将已经完成 content-COW 的 pending data-page relocation 应用到内存中的
 * inode/node tree。被修改的 node block 会标记为 dirty，后续由 node cache
 * 的 COW 路径提交。
 */
int rtfsFileInodeApplyPendingCowRelocations(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
);

int rtfsFileInodeCollectPendingDataCowOldLpas(
    RtfsFileInode *file_inode,
    uint32_t *out_array,
    size_t max_count,
    size_t *out_count
);

typedef int (*rtfs_file_inode_read_block_hook)(
    comm_dev *dev,
    uint32_t lpa,
    void *buffer
);

typedef int (*rtfs_file_inode_write_block_hook)(
    comm_dev *dev,
    uint32_t lpa,
    const void *buffer
);

typedef int (*rtfs_file_inode_journal_commit_hook)(JournalContainer *journal);

void rtfsFileInodeSetReadBlockHook(rtfs_file_inode_read_block_hook hook);

void rtfsFileInodeSetWriteBlockHook(rtfs_file_inode_write_block_hook hook);

void rtfsFileInodeSetJournalCommitHook(
    rtfs_file_inode_journal_commit_hook hook
);

/*
 * 完成普通文件 COW 写回事务：写 dirty pages、切换数据映射、写 dirty nodes、
 * 应用 NAT relocation、提交 journal，并注册旧 LPA 等待延迟回收。
 */
int rtfsFileInodeCommitCowWritebackWithTxId(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode,
    uint64_t *out_tx_id
);

int rtfsFileInodeCommitCowWriteback(
    file_system_manager *fs_manager,
    RtfsFileInode *file_inode
);

#endif
