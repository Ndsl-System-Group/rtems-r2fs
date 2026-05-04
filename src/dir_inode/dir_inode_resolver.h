#ifndef _DIR_INODE_RESOLVER_H_
#define _DIR_INODE_RESOLVER_H_

#include "dir_inode/dir_inode.h"
#include "fs/fs_manager.h"

typedef enum RtfsDirInodeBuildMode
{
    RTFS_DIR_BUILD_METADATA_ONLY = 0,
    RTFS_DIR_BUILD_INLINE_IF_POSSIBLE,
    RTFS_DIR_BUILD_ON_DEMAND,
    RTFS_DIR_BUILD_EAGER
} RtfsDirInodeBuildMode;

typedef struct RtfsDirInodeBuildRequest
{
    rtfs_ino ino;
    RtfsDirInodeBuildMode mode;
} RtfsDirInodeBuildRequest;

/**
 * @brief 解析一个目录 inode 构造请求，并返回目录运行时对象。
 * @return 成功返回 0，参数错误返回 EINVAL，尚未支持的构造模式返回 ENOSYS，
 *         inode 无法装载或构造失败时返回对应错误码。
 */
int rtfsDirInodeResolve(
    file_system_manager *fs_manager,
    RtfsDirInodeCache *cache,
    const RtfsDirInodeBuildRequest *request,
    RtfsDirInode **out_dir_inode
);

/**
 * @brief 在已有目录对象上按当前 on-demand 策略继续推进下一批目录块装载。
 * @return 成功返回 0，参数错误返回 EINVAL，无法继续装载时返回对应错误码。
 */
int rtfsDirInodeResolveNext(
    file_system_manager *fs_manager,
    rtfs_ino ino,
    RtfsDirInode *dir_inode
);

typedef int (*rtfs_dir_resolver_read_block_hook)(
    comm_dev *dev,
    uint32_t lpa,
    void *buffer
);

void rtfsDirResolverSetReadBlockHook(rtfs_dir_resolver_read_block_hook hook);

#endif
