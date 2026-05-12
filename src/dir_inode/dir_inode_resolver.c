#include "dir_inode_resolver.h"

#include "cache/block_buffer.h"
#include "cache/node_block_cache.h"
#include "communication/comm_api.h"
#include "inode/inode_loader.h"
#include "utils/io_utils.h"

#include <stdbool.h>

#define RTFS_DIR_ON_DEMAND_CHUNK_BLOCKS 1

static rtfs_dir_resolver_read_block_hook g_rtfs_dir_resolver_read_block_hook = NULL;

static int rtfsDirInodeResolveOnDemand(
    file_system_manager *fs_manager,
    rtfs_ino ino,
    RtfsDirInode *dir_inode
);

static int rtfsDirInodeLoadNextBlocks(
    file_system_manager *fs_manager,
    rtfs_ino ino,
    RtfsDirInode *dir_inode,
    size_t max_blocks
);

static int rtfsDirResolveDataLpaByBlockIndex(
    file_system_manager *fs_manager,
    uint32_t inode_nid,
    const struct RtfsInode *inode,
    uint32_t block_index,
    uint32_t *out_lpa
);

static int rtfsDirGetCachedNodeByNid(
    file_system_manager *fs_manager,
    uint32_t nid,
    uint32_t parent_nid,
    struct RtfsNode **out_node,
    NodeBlockCacheEntryHandle *out_handle
);

static int rtfsDirResolverReadBlock(
    comm_dev *dev,
    uint32_t lpa,
    void *buffer
);

static bool rtfsDirInodeBuildModeIsSupported(RtfsDirInodeBuildMode mode)
{
    switch (mode) {
    case RTFS_DIR_BUILD_METADATA_ONLY:
    case RTFS_DIR_BUILD_INLINE_IF_POSSIBLE:
    case RTFS_DIR_BUILD_ON_DEMAND:
        return true;
    case RTFS_DIR_BUILD_EAGER:
    default:
        return false;
    }
}

static int rtfsDirInodeResolveCache(
    file_system_manager *fs_manager,
    RtfsDirInodeCache *cache,
    RtfsDirInodeCache **resolved_cache,
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

    *resolved_cache = rtfsDirInodeCacheCreate(node_cache);
    if (*resolved_cache == NULL) {
        return ENOMEM;
    }

    *owns_cache = true;
    return 0;
}

int rtfsDirInodeResolve(
    file_system_manager *fs_manager,
    RtfsDirInodeCache *cache,
    const RtfsDirInodeBuildRequest *request,
    RtfsDirInode **out_dir_inode
)
{
    RtfsDirInodeCache *resolved_cache;
    bool owns_cache;
    int ret;

    if (request == NULL || out_dir_inode == NULL) {
        return EINVAL;
    }

    *out_dir_inode = NULL;

    if (!rtfsDirInodeBuildModeIsSupported(request->mode)) {
        return ENOSYS;
    }

    ret = rtfsInodeLoaderEnsureCached(fs_manager, request->ino);
    if (ret != 0) {
        return ret;
    }

    ret = rtfsDirInodeResolveCache(fs_manager, cache, &resolved_cache, &owns_cache);
    if (ret != 0) {
        return ret;
    }

    ret = rtfsDirInodeBuild(resolved_cache, request->ino, out_dir_inode);

    if (owns_cache) {
        rtfsDirInodeCacheDestroy(resolved_cache);
    }

    if (ret != 0) {
        return ret;
    }

    if (request->mode == RTFS_DIR_BUILD_ON_DEMAND) {
        ret = rtfsDirInodeResolveOnDemand(fs_manager, request->ino, *out_dir_inode);
        if (ret != 0) {
            rtfsDirInodePut(*out_dir_inode);
            *out_dir_inode = NULL;
            return ret;
        }
    }

    return ret;
}

int rtfsDirInodeResolveNext(
    file_system_manager *fs_manager,
    rtfs_ino ino,
    RtfsDirInode *dir_inode
)
{
    return rtfsDirInodeResolveOnDemand(fs_manager, ino, dir_inode);
}

void rtfsDirResolverSetReadBlockHook(rtfs_dir_resolver_read_block_hook hook)
{
    g_rtfs_dir_resolver_read_block_hook = hook;
}

static int rtfsDirInodeResolveOnDemand(
    file_system_manager *fs_manager,
    rtfs_ino ino,
    RtfsDirInode *dir_inode
)
{
    if (fs_manager == NULL || dir_inode == NULL) {
        return EINVAL;
    }

    if (rtfsDirInodeIsFullyLoaded(dir_inode)) {
        return 0;
    }

    return rtfsDirInodeLoadNextBlocks(
        fs_manager,
        ino,
        dir_inode,
        RTFS_DIR_ON_DEMAND_CHUNK_BLOCKS
    );
}

static int rtfsDirInodeLoadNextBlocks(
    file_system_manager *fs_manager,
    rtfs_ino ino,
    RtfsDirInode *dir_inode,
    size_t max_blocks
)
{
    NodeBlockCache *node_cache;
    comm_dev *dev;
    NodeBlockCacheEntryHandle node_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *node;
    BlockBuffer buffer;
    int ret = 0;
    size_t i;
    size_t start_block;
    size_t end_block;
    bool buffer_inited = false;

    if (fs_manager == NULL || dir_inode == NULL) {
        return EINVAL;
    }

    node_cache = fileSystemManagerGetNodeCache(fs_manager);
    dev = fileSystemManagerGetDevice(fs_manager);
    if (node_cache == NULL || dev == NULL) {
        return ENOENT;
    }

    node_handle = nodeBlockCacheGet(node_cache, ino);
    if (nodeBlockCacheEntryHandleIsEmpty(&node_handle)) {
        return ENOENT;
    }

    node = nodeBlockCacheEntryGetNodeBlockPtr(node_handle.entry);
    if ((node->i.i_inline & RTFS_INLINE_DENTRY) != 0) {
        goto out;
    }

    start_block = rtfsDirInodeGetLoadedBlockCount(dir_inode);
    if (start_block >= rtfsDirInodeGetTotalBlockCount(dir_inode)) {
        goto out;
    }

    end_block = start_block + max_blocks;
    if (end_block > rtfsDirInodeGetTotalBlockCount(dir_inode)) {
        end_block = rtfsDirInodeGetTotalBlockCount(dir_inode);
    }

    blockBufferInit(&buffer);
    buffer_inited = true;

    for (i = start_block; i < end_block; ++i) {
        uint32_t lpa;
        int io_ret;

        ret = rtfsDirResolveDataLpaByBlockIndex(fs_manager, ino, &node->i, (uint32_t)i, &lpa);
        if (ret != 0) {
            goto out;
        }

        if (lpa == INVALID_LPA) {
            continue;
        }

        io_ret = rtfsDirResolverReadBlock(dev, lpa, blockBufferGetPtr(&buffer));
        if (io_ret != 0) {
            ret = EIO;
            goto out;
        }

        ret = rtfsDirInodeAppendDentryBlockAt(
            dir_inode,
            (const struct RtfsDentryBlock *)blockBufferGetPtr(&buffer),
            (uint32_t)i,
            lpa
        );
        if (ret != 0) {
            goto out;
        }

        rtfsDirInodeSetLoadedBlockCount(dir_inode, i + 1);
    }

out:
    if (buffer_inited) {
        blockBufferDestroy(&buffer);
    }
    nodeBlockCacheEntryHandleDestroy(&node_handle);
    return ret;
}

static int rtfsDirResolveDataLpaByBlockIndex(
    file_system_manager *fs_manager,
    uint32_t inode_nid,
    const struct RtfsInode *inode,
    uint32_t block_index,
    uint32_t *out_lpa
)
{
    uint32_t direct_block_index;
    uint32_t direct_node_slot;
    uint32_t direct_node_offset;
    uint32_t indirect_block_index;
    uint32_t indirect_node_slot;
    uint32_t indirect_node_offset;
    uint32_t double_indirect_block_index;
    uint32_t first_level_slot;
    uint32_t second_level_slot;
    uint32_t double_indirect_offset;
    NodeBlockCacheEntryHandle direct_node_handle = {
        .cache = NULL,
        .entry = NULL
    };
    NodeBlockCacheEntryHandle indirect_node_handle = {
        .cache = NULL,
        .entry = NULL
    };
    NodeBlockCacheEntryHandle first_indirect_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *direct_node;
    struct RtfsNode *indirect_node;
    struct RtfsNode *first_indirect_node;
    int ret;

    if (fs_manager == NULL || inode_nid == INVALID_NID || inode == NULL || out_lpa == NULL) {
        return EINVAL;
    }

    if (block_index < DEF_ADDRS_PER_INODE) {
        *out_lpa = inode->i_addr[block_index];
        return 0;
    }

    direct_block_index = block_index - DEF_ADDRS_PER_INODE;
    if (direct_block_index < (2U * DEF_ADDRS_PER_BLOCK)) {
        direct_node_slot = direct_block_index / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = direct_block_index % DEF_ADDRS_PER_BLOCK;

        if (inode->i_nid[direct_node_slot] == INVALID_NID) {
            *out_lpa = INVALID_LPA;
            return 0;
        }

        ret = rtfsDirGetCachedNodeByNid(
            fs_manager,
            inode->i_nid[direct_node_slot],
            inode_nid,
            &direct_node,
            &direct_node_handle
        );
        if (ret != 0) {
            return ret;
        }

        *out_lpa = direct_node->dn.addr[direct_node_offset];
        nodeBlockCacheEntryHandleDestroy(&direct_node_handle);
        return 0;
    }

    indirect_block_index = direct_block_index - (2U * DEF_ADDRS_PER_BLOCK);
    if (indirect_block_index < (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        indirect_node_slot = indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        indirect_node_offset = indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);

        if (inode->i_nid[2 + indirect_node_slot] == INVALID_NID) {
            *out_lpa = INVALID_LPA;
            return 0;
        }

        ret = rtfsDirGetCachedNodeByNid(
            fs_manager,
            inode->i_nid[2 + indirect_node_slot],
            inode_nid,
            &indirect_node,
            &indirect_node_handle
        );
        if (ret != 0) {
            return ret;
        }

        direct_node_slot = indirect_node_offset / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = indirect_node_offset % DEF_ADDRS_PER_BLOCK;

        if (indirect_node->in.nid[direct_node_slot] == INVALID_NID) {
            *out_lpa = INVALID_LPA;
            nodeBlockCacheEntryHandleDestroy(&indirect_node_handle);
            return 0;
        }

        ret = rtfsDirGetCachedNodeByNid(
            fs_manager,
            indirect_node->in.nid[direct_node_slot],
            inode->i_nid[2 + indirect_node_slot],
            &direct_node,
            &direct_node_handle
        );
        nodeBlockCacheEntryHandleDestroy(&indirect_node_handle);
        if (ret != 0) {
            return ret;
        }

        *out_lpa = direct_node->dn.addr[direct_node_offset];
        nodeBlockCacheEntryHandleDestroy(&direct_node_handle);
        return 0;
    }

    double_indirect_block_index = indirect_block_index - (2U * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
    if (double_indirect_block_index < (NIDS_PER_BLOCK * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK)) {
        if (inode->i_nid[4] == INVALID_NID) {
            *out_lpa = INVALID_LPA;
            return 0;
        }

        ret = rtfsDirGetCachedNodeByNid(
            fs_manager,
            inode->i_nid[4],
            inode_nid,
            &indirect_node,
            &indirect_node_handle
        );
        if (ret != 0) {
            return ret;
        }

        first_level_slot = double_indirect_block_index / (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        double_indirect_offset = double_indirect_block_index % (NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK);
        second_level_slot = double_indirect_offset / DEF_ADDRS_PER_BLOCK;
        direct_node_offset = double_indirect_offset % DEF_ADDRS_PER_BLOCK;

        if (indirect_node->in.nid[first_level_slot] == INVALID_NID) {
            *out_lpa = INVALID_LPA;
            nodeBlockCacheEntryHandleDestroy(&indirect_node_handle);
            return 0;
        }

        ret = rtfsDirGetCachedNodeByNid(
            fs_manager,
            indirect_node->in.nid[first_level_slot],
            inode->i_nid[4],
            &first_indirect_node,
            &first_indirect_handle
        );
        nodeBlockCacheEntryHandleDestroy(&indirect_node_handle);
        if (ret != 0) {
            return ret;
        }

        if (first_indirect_node->in.nid[second_level_slot] == INVALID_NID) {
            *out_lpa = INVALID_LPA;
            nodeBlockCacheEntryHandleDestroy(&first_indirect_handle);
            return 0;
        }

        ret = rtfsDirGetCachedNodeByNid(
            fs_manager,
            first_indirect_node->in.nid[second_level_slot],
            first_indirect_node->footer.nid,
            &direct_node,
            &direct_node_handle
        );
        nodeBlockCacheEntryHandleDestroy(&first_indirect_handle);
        if (ret != 0) {
            return ret;
        }

        *out_lpa = direct_node->dn.addr[direct_node_offset];
        nodeBlockCacheEntryHandleDestroy(&direct_node_handle);
        return 0;
    }

    return ENOSYS;
}

static int rtfsDirGetCachedNodeByNid(
    file_system_manager *fs_manager,
    uint32_t nid,
    uint32_t parent_nid,
    struct RtfsNode **out_node,
    NodeBlockCacheEntryHandle *out_handle
)
{
    NodeBlockCacheHelper helper;
    NodeBlockCacheEntryHandle handle = {
        .cache = NULL,
        .entry = NULL
    };

    if (fs_manager == NULL || nid == INVALID_NID || out_node == NULL || out_handle == NULL) {
        return EINVAL;
    }

    *out_node = NULL;
    out_handle->cache = NULL;
    out_handle->entry = NULL;

    nodeBlockCacheHelperInit(&helper, fs_manager);
    if (helper.nodeBlockCache == NULL || helper.natCache == NULL || helper.dev == NULL) {
        nodeBlockCacheHelperDestroy(&helper);
        return ENOENT;
    }

    handle = nodeBlockCacheHelperGetNodeEntry(&helper, nid, parent_nid);
    nodeBlockCacheHelperDestroy(&helper);

    if (nodeBlockCacheEntryHandleIsEmpty(&handle)) {
        return ENOENT;
    }

    *out_node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    *out_handle = handle;
    return 0;
}

static int rtfsDirResolverReadBlock(
    comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    if (g_rtfs_dir_resolver_read_block_hook != NULL) {
        return g_rtfs_dir_resolver_read_block_hook(dev, lpa, buffer);
    }

    return comm_submit_sync_rw_request(
        dev,
        buffer,
        LPA_TO_LBA(lpa),
        LBA_PER_LPA,
        COMM_IO_READ
    );
}
