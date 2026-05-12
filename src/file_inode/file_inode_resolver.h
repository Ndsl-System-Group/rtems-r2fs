#ifndef _FILE_INODE_RESOLVER_H_
#define _FILE_INODE_RESOLVER_H_

#include "file_inode/file_inode.h"
#include "fs/fs_manager.h"

typedef enum RtfsFileInodeBuildMode
{
    RTFS_FILE_BUILD_METADATA_ONLY = 0,
    RTFS_FILE_BUILD_WITH_PAGE_CACHE
} RtfsFileInodeBuildMode;

typedef struct RtfsFileInodeBuildRequest
{
    rtfs_ino ino;
    RtfsFileInodeBuildMode mode;
} RtfsFileInodeBuildRequest;

/*
 * 解析一个普通文件 inode 构造请求，并返回普通文件运行时 inode 对象。
 *
 * 普通文件的数据页由 file_inode 的 read/write 路径通过 page cache 按需加载，
 * 因此 resolver 只负责确保 inode node 已进入 node cache，并构造运行时对象。
 *
 * 成功返回 0；参数错误返回 EINVAL；不支持的构造模式返回 ENOSYS；
 * inode 无法加载、类型不匹配或构造失败时返回对应 errno 风格错误码。
 */
int rtfsFileInodeResolve(
    file_system_manager *fs_manager,
    RtfsFileInodeCache *cache,
    const RtfsFileInodeBuildRequest *request,
    RtfsFileInode **out_file_inode
);

#endif
