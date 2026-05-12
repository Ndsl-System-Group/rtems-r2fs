#include "file_inode_resolver.h"

#include "cache/node_block_cache.h"
#include "inode/inode_loader.h"

#include <errno.h>
#include <stdbool.h>

static bool rtfsFileInodeBuildModeIsSupported(RtfsFileInodeBuildMode mode)
{
    switch (mode) {
    case RTFS_FILE_BUILD_METADATA_ONLY:
    case RTFS_FILE_BUILD_WITH_PAGE_CACHE:
        return true;
    default:
        return false;
    }
}

static int rtfsFileInodeResolveCache(
    file_system_manager *fs_manager,
    RtfsFileInodeCache *cache,
    RtfsFileInodeCache **resolved_cache,
    bool *owns_cache
)
{
    NodeBlockCache *node_cache;

    if (resolved_cache == NULL || owns_cache == NULL) {
        return EINVAL;
    }

    *resolved_cache = NULL;
    *owns_cache = false;

    if (cache != NULL) {
        *resolved_cache = cache;
        return 0;
    }

    if (fs_manager == NULL) {
        return EINVAL;
    }

    node_cache = fileSystemManagerGetNodeCache(fs_manager);
    if (node_cache == NULL) {
        return ENOENT;
    }

    *resolved_cache = rtfsFileInodeCacheCreate(node_cache);
    if (*resolved_cache == NULL) {
        return ENOMEM;
    }

    *owns_cache = true;
    return 0;
}

int rtfsFileInodeResolve(
    file_system_manager *fs_manager,
    RtfsFileInodeCache *cache,
    const RtfsFileInodeBuildRequest *request,
    RtfsFileInode **out_file_inode
)
{
    RtfsFileInodeCache *resolved_cache;
    bool owns_cache;
    int ret;

    if (request == NULL || out_file_inode == NULL) {
        return EINVAL;
    }

    *out_file_inode = NULL;

    if (!rtfsFileInodeBuildModeIsSupported(request->mode)) {
        return ENOSYS;
    }

    ret = rtfsInodeLoaderEnsureCached(fs_manager, request->ino);
    if (ret != 0) {
        return ret;
    }

    ret = rtfsFileInodeResolveCache(
        fs_manager,
        cache,
        &resolved_cache,
        &owns_cache
    );
    if (ret != 0) {
        return ret;
    }

    ret = rtfsFileInodeBuild(resolved_cache, request->ino, out_file_inode);

    if (owns_cache) {
        rtfsFileInodeCacheDestroy(resolved_cache);
    }

    if (ret != 0) {
        *out_file_inode = NULL;
    }

    return ret;
}
