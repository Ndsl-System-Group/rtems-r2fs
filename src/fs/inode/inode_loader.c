#include "inode_loader.h"

#include "cache/node_block_cache.h"

#include <stdbool.h>

static int rtfsInodeLoaderEnsureCachedInNodeCache(NodeBlockCache *node_cache, rtfs_ino ino)
{
    NodeBlockCacheEntryHandle handle;

    if (node_cache == NULL || ino == INVALID_NID) {
        return EINVAL;
    }

    handle = nodeBlockCacheGet(node_cache, ino);
    if (nodeBlockCacheEntryHandleIsEmpty(&handle)) {
        return ENOENT;
    }

    nodeBlockCacheEntryHandleDestroy(&handle);
    return 0;
}

int rtfsInodeLoaderEnsureCached(file_system_manager *fs_manager, rtfs_ino ino)
{
    NodeBlockCache *node_cache;
    NodeBlockCacheHelper helper;
    NodeBlockCacheEntryHandle handle = {
        .cache = NULL,
        .entry = NULL
    };
    bool helper_inited = false;

    if (fs_manager == NULL) {
        return EINVAL;
    }

    node_cache = fileSystemManagerGetNodeCache(fs_manager);
    if (node_cache == NULL || ino == INVALID_NID) {
        return EINVAL;
    }

    if (rtfsInodeLoaderEnsureCachedInNodeCache(node_cache, ino) == 0) {
        return 0;
    }

    nodeBlockCacheHelperInit(&helper, fs_manager);
    helper_inited = true;

    if (helper.nodeBlockCache == NULL || helper.natCache == NULL || helper.dev == NULL) {
        goto fail;
    }

    handle = nodeBlockCacheHelperGetNodeEntry(&helper, ino, INVALID_NID);
    if (nodeBlockCacheEntryHandleIsEmpty(&handle)) {
        goto fail;
    }

    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheHelperDestroy(&helper);
    return 0;

fail:
    if (helper_inited) {
        nodeBlockCacheHelperDestroy(&helper);
    }
    return ENOENT;
}
