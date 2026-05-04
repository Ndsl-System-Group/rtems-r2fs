#ifndef _INODE_LOADER_H_
#define _INODE_LOADER_H_

#include <errno.h>

#include "fs/fs.h"
#include "fs/fs_manager.h"
#include "inode/inode.h"

/**
 * @brief 确保指定 ino 的 inode block 已经位于 NodeBlockCache 中。
 * @return 成功返回 0；若当前无法装载或缓存缺失则返回 ENOENT；参数错误返回 EINVAL。
 */
int rtfsInodeLoaderEnsureCached(file_system_manager *fs_manager, rtfs_ino ino);

#endif
