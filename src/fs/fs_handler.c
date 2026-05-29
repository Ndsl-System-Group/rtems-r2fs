#include "fs_handler.h"

#include <errno.h>
#include <stdint.h>
#include <rtems/libio_.h>
#include <rtems/counter.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "dir_inode/dir_inode.h"
#include "dir_inode/dir_inode_resolver.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "communication/comm_api.h"
#include "fs/cow_reclaim_registry.h"
#include "fs_manager.h"
#include "dir_inode/dir_handler.h"
#include "file_inode/file_handler.h"
#include "inode/inode.h"
#include "inode/inode_loader.h"
#include "journal/journal_process_env.h"
#include "nat_utils.h"
#include "super_manager.h"
#include "utils/rtfs_log.h"


static RtfsRuntimeInodeView *r2fsGetNodeView(
    const rtems_filesystem_location_info_t *loc
)
{
    return loc != NULL ? (RtfsRuntimeInodeView *)loc->node_access : NULL;
}

static const char *r2fsGetNodeName(
    const rtems_filesystem_location_info_t *loc
)
{
    return loc != NULL ? (const char *)loc->node_access_2 : NULL;
}

static char *r2fsDupName(
    const char *name,
    size_t namelen
)
{
    char *copy;

    if (name == NULL) {
        return NULL;
    }

    copy = malloc(namelen + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, name, namelen);
    copy[namelen] = '\0';
    return copy;
}

static void r2fsClearLocationName(
    rtems_filesystem_location_info_t *loc
)
{
    if (loc == NULL) {
        return;
    }

    free(loc->node_access_2);
    loc->node_access_2 = NULL;
}

static int r2fsReplaceLocationName(
    rtems_filesystem_location_info_t *loc,
    const char *name,
    size_t namelen
)
{
    char *replacement;

    if (loc == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (name == NULL) {
        r2fsClearLocationName(loc);
        return 0;
    }

    replacement = r2fsDupName(name, namelen);
    if (replacement == NULL) {
        errno = ENOMEM;
        return -1;
    }

    free(loc->node_access_2);
    loc->node_access_2 = replacement;
    return 0;
}

static int r2fsResolveTargetInodeHandle(
    const rtems_filesystem_location_info_t *loc,
    RtfsRuntimeInodeView **out_view,
    file_system_manager **out_fs_manager,
    NodeBlockCacheEntryHandle *out_handle,
    struct RtfsNode **out_node
);

static uint64_t r2fsGetTimestampTick(void);
static void r2fsTouchInodeTimes(struct RtfsInode *inode, uint64_t now);
static int r2fsAdjustParentDirectoryNlink(
    const rtems_filesystem_location_info_t *parentloc,
    int delta
);
static int r2fsAdjustParentDirectoryNlinkWithHandle(
    NodeBlockCacheEntryHandle *parent_handle,
    RtfsRuntimeInodeView *parent_view,
    int delta
);
static int r2fsTargetIsRemovableEmptyObject(
    const RtfsRuntimeInodeView *target_view,
    struct RtfsNode *target_node
);
static int r2fsMarkTargetInodeUnlinked(
    const RtfsRuntimeInodeView *target_view,
    NodeBlockCacheEntryHandle *target_handle,
    struct RtfsNode *target_node
);
static void r2fsDestroyHandleArray(
    NodeBlockCacheEntryHandle *handles,
    size_t count
);
static void r2fsDeleteAndDestroyHandleArray(
    NodeBlockCacheEntryHandle *handles,
    size_t count
);
static int r2fsCollectTargetReclaimPlan(
    file_system_manager *fs_manager,
    const RtfsRuntimeInodeView *target_view,
    const struct RtfsNode *target_node,
    uint32_t **out_data_lpas,
    size_t *out_data_count,
    NodeBlockCacheEntryHandle **out_deleted_handles,
    size_t *out_deleted_handle_count
);
static int r2fsAppendUniqueLpasToArray(
    const uint32_t *src,
    size_t src_count,
    uint32_t **dst,
    size_t *dst_count,
    size_t *dst_capacity
);
static int r2fsCollectDirPendingOldDataLpasAppend(
    RtfsDirInode *dir_inode,
    uint32_t **dst,
    size_t *dst_count,
    size_t *dst_capacity
);
static int r2fsCommitDirtyDirsAndNodesWithTxId(
    file_system_manager *fs_manager,
    RtfsDirInode *first_dir,
    RtfsDirInode *second_dir,
    uint64_t *out_tx_id
);
static int r2fsRenameAcrossParents(
    const rtems_filesystem_location_info_t *oldparentloc,
    const rtems_filesystem_location_info_t *oldloc,
    const rtems_filesystem_location_info_t *newparentloc,
    const char *old_name,
    const char *new_name,
    size_t new_namelen,
    const RtfsRuntimeInodeView *source_view,
    const RtfsDirLookupResult *existing_target
);
static int r2fsSubmitJournalContainer(
    JournalContainer *journal,
    uint64_t *out_tx_id
);
static JournalContainer *r2fsCloneJournalContainer(
    const JournalContainer *src
);
static fsfilcnt_t r2fsStatvfsGetTotalFileSlots(
    const RtfsSuperBlock *super_block
);
static fsfilcnt_t r2fsStatvfsCountFreeNids(
    file_system_manager *fs_manager,
    const RtfsSuperBlock *super_block
);

static rtfs_ino r2fsGetRootIno(file_system_manager *fs_manager)
{
    RtfsSuperBlock *super_block = fileSystemManagerGetSuperBlkMem(fs_manager);

    if (super_block != NULL && super_block->root_ino != 0) {
        return super_block->root_ino;
    }

    return 1;
}

static fsfilcnt_t r2fsStatvfsGetTotalFileSlots(
    const RtfsSuperBlock *super_block
)
{
    fsfilcnt_t total;

    if (super_block == NULL || super_block->segment_count_nat == 0) {
        return 0;
    }

    total = (fsfilcnt_t)super_block->segment_count_nat *
        (fsfilcnt_t)BLOCK_PER_SEGMENT *
        (fsfilcnt_t)NAT_ENTRY_PER_BLOCK;

    if (total > 0) {
        total -= 1; /* nid 0 is INVALID_NID, not allocatable */
    }

    return total;
}

static fsfilcnt_t r2fsStatvfsCountFreeNids(
    file_system_manager *fs_manager,
    const RtfsSuperBlock *super_block
)
{
    NatLpaMapping nat_mapping;
    SitNatCache *nat_cache;
    fsfilcnt_t total_slots;
    fsfilcnt_t free_count = 0;
    uint32_t current_nid;

    if (fs_manager == NULL || super_block == NULL) {
        return 0;
    }

    nat_cache = fileSystemManagerGetNatCache(fs_manager);
    if (nat_cache == NULL) {
        return 0;
    }

    total_slots = r2fsStatvfsGetTotalFileSlots(super_block);
    if (total_slots == 0) {
        return 0;
    }

    natLpaMappingInit(&nat_mapping, fs_manager);
    current_nid = super_block->next_free_nid;

    while (current_nid != INVALID_NID && free_count < total_slots) {
        NatNidPos pos;
        SitNatCacheEntryHandle nat_handle;
        struct RtfsNatEntry *nat_entry;
        uint32_t next_nid;

        pos = natGetNidPos(&nat_mapping, current_nid);
        nat_handle = sitNatCacheGet(nat_cache, pos.lpa);
        nat_entry = &sitNatCacheEntryHandleGetNatBlockPtr(&nat_handle)->entries[pos.idx];

        if (nat_entry->ino != INVALID_NID) {
            sitNatCacheEntryHandleDestroy(&nat_handle);
            break;
        }

        next_nid = nat_entry->block_addr;
        sitNatCacheEntryHandleDestroy(&nat_handle);

        free_count++;
        if (next_nid == current_nid) {
            break;
        }
        current_nid = next_nid;
    }

    return free_count;
}

static void r2fsSetHandlers(rtems_filesystem_location_info_t *loc)
{
    RtfsRuntimeInodeView *view = r2fsGetNodeView(loc);

    if (loc == NULL || view == NULL) {
        return;
    }

    if (rtfsInodeIsDirectoryType(view->file_type)) {
        loc->handlers = &rtfsDirhandlers;
    } else {
        loc->handlers = &rtfsFilehandlers;
    }
}

static file_system_manager *r2fsGetFsManagerFromLoc(
    const rtems_filesystem_location_info_t *loc
)
{
    return (loc != NULL && loc->mt_entry != NULL)
        ? (file_system_manager *)loc->mt_entry->fs_info
        : NULL;
}

static super_manager *r2fsGetSuperManagerFromLoc(
    const rtems_filesystem_location_info_t *loc
)
{
    file_system_manager *fs_manager = r2fsGetFsManagerFromLoc(loc);

    return fileSystemManagerGetSuperManager(fs_manager);
}

static int r2fsReplaceNodeView(
    rtems_filesystem_location_info_t *loc,
    const RtfsRuntimeInodeView *view
)
{
    RtfsRuntimeInodeView *replacement;

    if (loc == NULL || view == NULL) {
        errno = EINVAL;
        return -1;
    }

    replacement = rtfsRuntimeInodeViewClone(view);
    if (replacement == NULL) {
        errno = ENOMEM;
        return -1;
    }

    free(loc->node_access);
    loc->node_access = replacement;
    r2fsSetHandlers(loc);
    return 0;
}

static int r2fsSetLocationNode(
    rtems_filesystem_location_info_t *loc,
    const RtfsRuntimeInodeView *view,
    const char *name,
    size_t namelen
)
{
    if (r2fsReplaceNodeView(loc, view) != 0) {
        return -1;
    }

    if (r2fsReplaceLocationName(loc, name, namelen) != 0) {
        free(loc->node_access);
        loc->node_access = NULL;
        loc->handlers = NULL;
        return -1;
    }

    return 0;
}

static int r2fsValidateNodeLoc(
    const rtems_filesystem_location_info_t *loc,
    RtfsRuntimeInodeView **view_out
)
{
    RtfsRuntimeInodeView *view = r2fsGetNodeView(loc);

    if (loc == NULL || view == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (view_out != NULL) {
        *view_out = view;
    }

    return 0;
}

static int r2fsValidateParentDir(
    const rtems_filesystem_location_info_t *parentloc,
    const char *name,
    size_t namelen,
    RtfsRuntimeInodeView **view_out
)
{
    RtfsRuntimeInodeView *parent_view;

    if (r2fsValidateNodeLoc(parentloc, &parent_view) != 0) {
        return -1;
    }

    if (!rtfsInodeIsDirectoryType(parent_view->file_type)) {
        errno = ENOTDIR;
        return -1;
    }

    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (namelen == 0) {
        errno = EINVAL;
        return -1;
    }

    if (namelen > RTFS_NAME_LEN) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if ((namelen == 1 && name[0] == '.') ||
        (namelen == 2 && name[0] == '.' && name[1] == '.')) {
        errno = EINVAL;
        return -1;
    }

    if (view_out != NULL) {
        *view_out = parent_view;
    }

    return 0;
}

static int r2fsResolveParentDirInode(
    const rtems_filesystem_location_info_t *parentloc,
    RtfsDirInode **out_dir_inode,
    RtfsRuntimeInodeView **out_parent_view,
    file_system_manager **out_fs_manager
)
{
    RtfsRuntimeInodeView *parent_view;
    file_system_manager *fs_manager;
    RtfsDirInodeBuildRequest request;
    int ret;

    if (parentloc == NULL || out_dir_inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (r2fsValidateNodeLoc(parentloc, &parent_view) != 0) {
        return -1;
    }

    if (!rtfsInodeIsDirectoryType(parent_view->file_type)) {
        errno = ENOTDIR;
        return -1;
    }

    fs_manager = r2fsGetFsManagerFromLoc(parentloc);
    if (fs_manager == NULL) {
        errno = EIO;
        return -1;
    }

    request.ino = parent_view->ino;
    request.mode = RTFS_DIR_BUILD_ON_DEMAND;

    ret = rtfsDirInodeResolve(fs_manager, NULL, &request, out_dir_inode);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    if (out_parent_view != NULL) {
        *out_parent_view = parent_view;
    }
    if (out_fs_manager != NULL) {
        *out_fs_manager = fs_manager;
    }
    return 0;
}

static int r2fsResolveNameInParent(
    const rtems_filesystem_location_info_t *parentloc,
    const char *name,
    size_t namelen,
    RtfsDirLookupResult *out_result
)
{
    RtfsRuntimeInodeView *parent_view;
    file_system_manager *fs_manager;
    RtfsDirInode *dir_inode;
    int ret;

    if (r2fsValidateParentDir(parentloc, name, namelen, &parent_view) != 0) {
        return -1;
    }

    if (r2fsResolveParentDirInode(parentloc, &dir_inode, NULL, &fs_manager) != 0) {
        return -1;
    }

    do {
        ret = rtfsDirInodeLookup(dir_inode, name, namelen, out_result);
        if (ret != ENOENT || rtfsDirInodeIsFullyLoaded(dir_inode)) {
            break;
        }

        ret = rtfsDirInodeResolveNext(fs_manager, parent_view->ino, dir_inode);
        if (ret != 0) {
            rtfsDirInodePut(dir_inode);
            errno = ret;
            return -1;
        }
    } while (true);

    rtfsDirInodePut(dir_inode);

    if (ret != 0) {
        errno = ret;
        return -1;
    }

    return 0;
}

static int r2fsLookupNameInParent(
    const rtems_filesystem_location_info_t *parentloc,
    const char *name,
    RtfsDirLookupResult *out_result
)
{
    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }

    return r2fsResolveNameInParent(parentloc, name, strlen(name), out_result);
}

static int r2fsNameExistsInParent(
    const rtems_filesystem_location_info_t *parentloc,
    const char *name
)
{
    RtfsDirLookupResult result;

    if (r2fsLookupNameInParent(parentloc, name, &result) == 0) {
        return 1;
    }

    if (errno == ENOENT) {
        return 0;
    }

    return -1;
}

static int r2fsRemoveNameFromParent(
    const rtems_filesystem_location_info_t *parentloc,
    const char *name
)
{
    RtfsDirInode *dir_inode;
    file_system_manager *fs_manager;
    int ret;

    if (name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (r2fsResolveParentDirInode(parentloc, &dir_inode, NULL, &fs_manager) != 0) {
        return -1;
    }

    ret = rtfsDirInodeRemoveEntry(dir_inode, name);
    if (ret == 0) {
        ret = rtfsDirInodeCommitCowWriteback(fs_manager, dir_inode);
    }

    rtfsDirInodePut(dir_inode);

    if (ret != 0) {
        errno = ret;
        return -1;
    }

    return 0;
}

static int r2fsAddNameToParent(
    const rtems_filesystem_location_info_t *parentloc,
    const char *name,
    const RtfsRuntimeInodeView *child_view
)
{
    RtfsDirInode *dir_inode;
    file_system_manager *fs_manager;
    int ret;

    if (name == NULL || name[0] == '\0' || child_view == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (r2fsResolveParentDirInode(parentloc, &dir_inode, NULL, &fs_manager) != 0) {
        return -1;
    }

    ret = rtfsDirInodeAddEntry(dir_inode, name, child_view);
    if (ret == 0) {
        ret = rtfsDirInodeCommitCowWriteback(fs_manager, dir_inode);
    }

    rtfsDirInodePut(dir_inode);

    if (ret != 0) {
        errno = ret;
        return -1;
    }

    return 0;
}

static void r2fsRollbackCreatedInode(
    NodeBlockCacheEntryHandle *inode_handle
)
{
    if (inode_handle == NULL || nodeBlockCacheEntryHandleIsEmpty(inode_handle)) {
        return;
    }

    nodeBlockCacheEntryHandleDeleteNode(inode_handle);
    nodeBlockCacheEntryHandleDestroy(inode_handle);
    inode_handle->cache = NULL;
    inode_handle->entry = NULL;
}

static int r2fsRenameWithinSameParent(
    const rtems_filesystem_location_info_t *parentloc,
    const rtems_filesystem_location_info_t *oldloc,
    const char *old_name,
    const char *new_name,
    size_t new_namelen,
    const RtfsRuntimeInodeView *source_view,
    const RtfsDirLookupResult *existing_target
)
{
    RtfsDirInode *dir_inode;
    file_system_manager *fs_manager;
    RtfsRuntimeInodeView *parent_view = NULL;
    NodeBlockCacheEntryHandle parent_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *parent_node = NULL;
    NodeBlockCacheEntryHandle source_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *source_node;
    NodeBlockCacheEntryHandle replaced_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *replaced_node = NULL;
    uint32_t *replaced_data_lpas = NULL;
    size_t replaced_data_count = 0;
    NodeBlockCacheEntryHandle *replaced_deleted_handles = NULL;
    size_t replaced_deleted_handle_count = 0;
    NodeBlockCacheEntryHandle *deferred_delete_handles = NULL;
    size_t deferred_delete_handle_count = 0;
    bool target_exists = existing_target != NULL;
    bool removed_target = false;
    bool added_new_source = false;
    bool removed_old_source = false;
    uint64_t tx_id = 0;
    int ret;

    if (parentloc == NULL || oldloc == NULL || old_name == NULL || new_name == NULL ||
        source_view == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (r2fsResolveParentDirInode(parentloc, &dir_inode, NULL, &fs_manager) != 0) {
        return -1;
    }

    ret = r2fsResolveTargetInodeHandle(
        parentloc,
        &parent_view,
        NULL,
        &parent_handle,
        &parent_node
    );
    if (ret != 0) {
        rtfsDirInodePut(dir_inode);
        return -1;
    }

    ret = r2fsResolveTargetInodeHandle(
        oldloc,
        NULL,
        NULL,
        &source_handle,
        &source_node
    );
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&parent_handle);
        rtfsDirInodePut(dir_inode);
        return -1;
    }

    if (target_exists) {
        rtems_filesystem_location_info_t target_loc;
        RtfsRuntimeInodeView target_view_copy = existing_target->inode_view;

        memset(&target_loc, 0, sizeof(target_loc));
        target_loc.mt_entry = parentloc->mt_entry;
        target_loc.node_access = &target_view_copy;

        if (rtfsInodeIsDirectoryType(source_view->file_type) &&
            !rtfsInodeIsDirectoryType(existing_target->inode_view.file_type)) {
            nodeBlockCacheEntryHandleDestroy(&parent_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            rtfsDirInodePut(dir_inode);
            errno = ENOTDIR;
            return -1;
        }

        if (!rtfsInodeIsDirectoryType(source_view->file_type) &&
            rtfsInodeIsDirectoryType(existing_target->inode_view.file_type)) {
            nodeBlockCacheEntryHandleDestroy(&parent_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            rtfsDirInodePut(dir_inode);
            errno = EISDIR;
            return -1;
        }

        ret = r2fsResolveTargetInodeHandle(
            &target_loc,
            NULL,
            NULL,
            &replaced_handle,
            &replaced_node
        );
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&parent_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            rtfsDirInodePut(dir_inode);
            return -1;
        }

        ret = r2fsCollectTargetReclaimPlan(
            fs_manager,
            &existing_target->inode_view,
            replaced_node,
            &replaced_data_lpas,
            &replaced_data_count,
            &replaced_deleted_handles,
            &replaced_deleted_handle_count
        );
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&parent_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            rtfsDirInodePut(dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            errno = ret;
            return -1;
        }

        ret = r2fsTargetIsRemovableEmptyObject(&existing_target->inode_view, replaced_node);
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&parent_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            rtfsDirInodePut(dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            errno = ret;
            return -1;
        }

        ret = rtfsDirInodeRemoveEntry(dir_inode, new_name);
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&parent_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            rtfsDirInodePut(dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            errno = ret;
            return -1;
        }
        removed_target = true;
    }

    ret = rtfsDirInodeAddEntry(dir_inode, new_name, source_view);
    if (ret != 0) {
        if (removed_target) {
            (void)rtfsDirInodeAddEntry(dir_inode, new_name, &existing_target->inode_view);
        }
        nodeBlockCacheEntryHandleDestroy(&replaced_handle);
        nodeBlockCacheEntryHandleDestroy(&parent_handle);
        nodeBlockCacheEntryHandleDestroy(&source_handle);
        rtfsDirInodePut(dir_inode);
        free(replaced_data_lpas);
        r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
        errno = ret;
        return -1;
    }
    added_new_source = true;

    ret = rtfsDirInodeRemoveEntry(dir_inode, old_name);
    if (ret != 0) {
        (void)rtfsDirInodeRemoveEntry(dir_inode, new_name);
        if (removed_target) {
            (void)rtfsDirInodeAddEntry(dir_inode, new_name, &existing_target->inode_view);
        }
        nodeBlockCacheEntryHandleDestroy(&replaced_handle);
        nodeBlockCacheEntryHandleDestroy(&parent_handle);
        nodeBlockCacheEntryHandleDestroy(&source_handle);
        rtfsDirInodePut(dir_inode);
        free(replaced_data_lpas);
        r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
        errno = ret;
        return -1;
    }
    removed_old_source = true;

    memset(source_node->i.i_name, 0, sizeof(source_node->i.i_name));
    memcpy(source_node->i.i_name, new_name, new_namelen);
    source_node->i.i_namelen = (uint32_t)new_namelen;
    r2fsTouchInodeTimes(&source_node->i, r2fsGetTimestampTick());
    nodeBlockCacheEntryHandleMarkDirty(&source_handle);

    if (target_exists) {
        if (r2fsMarkTargetInodeUnlinked(&existing_target->inode_view, &replaced_handle, replaced_node) != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&parent_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            rtfsDirInodePut(dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            return -1;
        }

        if (rtfsInodeIsDirectoryType(existing_target->inode_view.file_type) &&
            r2fsAdjustParentDirectoryNlinkWithHandle(&parent_handle, parent_view, -1) != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&parent_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            rtfsDirInodePut(dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            return -1;
        }
    }

    ret = rtfsDirInodeCommitCowWritebackWithTxId(fs_manager, dir_inode, &tx_id);
    rtfsDirInodePut(dir_inode);
    nodeBlockCacheEntryHandleDestroy(&parent_handle);

    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&replaced_handle);
        nodeBlockCacheEntryHandleDestroy(&source_handle);
        free(replaced_data_lpas);
        r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
        errno = ret;
        return -1;
    }

    if (target_exists) {
        if (tx_id != 0) {
            size_t i;

            deferred_delete_handle_count = replaced_deleted_handle_count + 1;
            deferred_delete_handles = (NodeBlockCacheEntryHandle *)calloc(
                deferred_delete_handle_count,
                sizeof(*deferred_delete_handles)
            );
            if (deferred_delete_handles == NULL) {
                nodeBlockCacheEntryHandleDestroy(&replaced_handle);
                nodeBlockCacheEntryHandleDestroy(&source_handle);
                free(replaced_data_lpas);
                r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
                errno = ENOMEM;
                return -1;
            }

            for (i = 0; i < replaced_deleted_handle_count; ++i) {
                nodeBlockCacheEntryHandleCopy(
                    &deferred_delete_handles[i],
                    &replaced_deleted_handles[replaced_deleted_handle_count - 1 - i]
                );
            }
            nodeBlockCacheEntryHandleCopy(
                &deferred_delete_handles[replaced_deleted_handle_count],
                &replaced_handle
            );

            ret = cowReclaimRegistryRegister(
                tx_id,
                replaced_data_lpas,
                replaced_data_count,
                NULL,
                0,
                deferred_delete_handles,
                deferred_delete_handle_count
            );
            r2fsDestroyHandleArray(deferred_delete_handles, deferred_delete_handle_count);
            if (ret != 0) {
                nodeBlockCacheEntryHandleDestroy(&replaced_handle);
                nodeBlockCacheEntryHandleDestroy(&source_handle);
                free(replaced_data_lpas);
                r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
                errno = ret;
                return -1;
            }
        } else {
            r2fsDeleteAndDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            replaced_deleted_handles = NULL;
            replaced_deleted_handle_count = 0;
            nodeBlockCacheEntryHandleDeleteNode(&replaced_handle);
        }
    }

    free(replaced_data_lpas);
    r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
    nodeBlockCacheEntryHandleDestroy(&replaced_handle);
    nodeBlockCacheEntryHandleDestroy(&source_handle);

    (void)added_new_source;
    (void)removed_old_source;
    return 0;
}

static int r2fsResolveTargetInodeHandle(
    const rtems_filesystem_location_info_t *loc,
    RtfsRuntimeInodeView **out_view,
    file_system_manager **out_fs_manager,
    NodeBlockCacheEntryHandle *out_handle,
    struct RtfsNode **out_node
)
{
    RtfsRuntimeInodeView *view;
    file_system_manager *fs_manager;
    NodeBlockCache *node_cache;
    NodeBlockCacheEntryHandle handle = {
        .cache = NULL,
        .entry = NULL
    };
    int ret;

    if (loc == NULL || out_handle == NULL || out_node == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (r2fsValidateNodeLoc(loc, &view) != 0) {
        return -1;
    }

    fs_manager = r2fsGetFsManagerFromLoc(loc);
    node_cache = fileSystemManagerGetNodeCache(fs_manager);
    if (fs_manager == NULL || node_cache == NULL) {
        errno = EIO;
        return -1;
    }

    ret = rtfsInodeLoaderEnsureCached(fs_manager, view->ino);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    handle = nodeBlockCacheGet(node_cache, (uint32_t)view->ino);
    if (nodeBlockCacheEntryHandleIsEmpty(&handle)) {
        errno = ENOENT;
        return -1;
    }

    *out_node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    *out_handle = handle;
    if (out_view != NULL) {
        *out_view = view;
    }
    if (out_fs_manager != NULL) {
        *out_fs_manager = fs_manager;
    }
    return 0;
}

static uint64_t r2fsGetTimestampTick(void)
{
    return (uint64_t)rtems_counter_read();
}

static void r2fsTouchInodeTimes(struct RtfsInode *inode, uint64_t now)
{
    if (inode == NULL) {
        return;
    }

    inode->i_mtime = now;
    inode->i_atime = now;
    inode->i_mtime_nsec = 0;
    inode->i_atime_nsec = 0;
    inode->i_ctime_nsec = 0;
}

static int r2fsAdjustParentDirectoryNlink(
    const rtems_filesystem_location_info_t *parentloc,
    int delta
)
{
    RtfsRuntimeInodeView *parent_view;
    file_system_manager *fs_manager;
    NodeBlockCacheEntryHandle parent_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *parent_node;
    int ret;

    if (parentloc == NULL || delta == 0) {
        errno = EINVAL;
        return -1;
    }

    ret = r2fsResolveTargetInodeHandle(
        parentloc,
        &parent_view,
        &fs_manager,
        &parent_handle,
        &parent_node
    );
    if (ret != 0) {
        return -1;
    }

    ret = r2fsAdjustParentDirectoryNlinkWithHandle(&parent_handle, parent_view, delta);
    nodeBlockCacheEntryHandleDestroy(&parent_handle);
    return ret;
}

static int r2fsAdjustParentDirectoryNlinkWithHandle(
    NodeBlockCacheEntryHandle *parent_handle,
    RtfsRuntimeInodeView *parent_view,
    int delta
)
{
    struct RtfsNode *parent_node;

    if (parent_handle == NULL || parent_view == NULL || delta == 0 ||
        nodeBlockCacheEntryHandleIsEmpty(parent_handle)) {
        errno = EINVAL;
        return -1;
    }

    if (!rtfsInodeIsDirectoryType(parent_view->file_type)) {
        errno = ENOTDIR;
        return -1;
    }

    parent_node = nodeBlockCacheEntryGetNodeBlockPtr(parent_handle->entry);
    if (parent_node == NULL) {
        errno = EIO;
        return -1;
    }

    if (delta < 0) {
        uint32_t decrement = (uint32_t)(-delta);

        if (parent_node->i.i_nlink < decrement) {
            errno = EINVAL;
            return -1;
        }
        parent_node->i.i_nlink -= decrement;
    } else {
        parent_node->i.i_nlink += (uint32_t)delta;
    }

    r2fsTouchInodeTimes(&parent_node->i, r2fsGetTimestampTick());
    nodeBlockCacheEntryHandleMarkDirty(parent_handle);
    return 0;
}

static int r2fsTargetIsRemovableEmptyObject(
    const RtfsRuntimeInodeView *target_view,
    struct RtfsNode *target_node
)
{
    const struct RtfsInode *inode;

    if (target_view == NULL || target_node == NULL) {
        return EINVAL;
    }

    inode = &target_node->i;

    if (rtfsInodeIsDirectoryType(target_view->file_type)) {
        if (inode->i_dentry_num != 0) {
            return ENOTEMPTY;
        }
        return 0;
    }

    if (target_view->file_type == RTFS_FT_REG_FILE) {
        return 0;
    }

    if (target_view->file_type != RTFS_FT_REG_FILE) {
        return ENOTSUP;
    }

    return 0;
}

static int r2fsMarkTargetInodeUnlinked(
    const RtfsRuntimeInodeView *target_view,
    NodeBlockCacheEntryHandle *target_handle,
    struct RtfsNode *target_node
)
{
    if (target_view == NULL || target_handle == NULL || target_node == NULL ||
        nodeBlockCacheEntryHandleIsEmpty(target_handle)) {
        errno = EINVAL;
        return -1;
    }

    target_node->i.i_nlink = 0;
    if (rtfsInodeIsDirectoryType(target_view->file_type)) {
        target_node->i.i_pino = 0;
    }
    r2fsTouchInodeTimes(&target_node->i, r2fsGetTimestampTick());
    nodeBlockCacheEntryHandleMarkDirty(target_handle);
    return 0;
}

static int r2fsAppendUniqueUint32(
    uint32_t value,
    uint32_t **array,
    size_t *count,
    size_t *capacity
)
{
    size_t i;
    uint32_t *new_array;
    size_t new_capacity;

    if (array == NULL || count == NULL || capacity == NULL) {
        return EINVAL;
    }

    if (value == INVALID_LPA || value == INVALID_NID) {
        return 0;
    }

    if (value >= 0x80000000u) {
        return 0;
    }

    for (i = 0; i < *count; ++i) {
        if ((*array)[i] == value) {
            return 0;
        }
    }

    if (*count == *capacity) {
        new_capacity = (*capacity == 0) ? 8 : (*capacity * 2);
        new_array = (uint32_t *)realloc(*array, new_capacity * sizeof(**array));
        if (new_array == NULL) {
            return ENOMEM;
        }
        *array = new_array;
        *capacity = new_capacity;
    }

    (*array)[(*count)++] = value;
    return 0;
}

static void r2fsDestroyHandleArray(
    NodeBlockCacheEntryHandle *handles,
    size_t count
)
{
    size_t i;

    if (handles == NULL) {
        return;
    }

    for (i = 0; i < count; ++i) {
        if (!nodeBlockCacheEntryHandleIsEmpty(&handles[i])) {
            nodeBlockCacheEntryHandleDestroy(&handles[i]);
        }
    }

    free(handles);
}

static void r2fsDeleteAndDestroyHandleArray(
    NodeBlockCacheEntryHandle *handles,
    size_t count
)
{
    size_t i;

    if (handles == NULL) {
        return;
    }

    for (i = 0; i < count; ++i) {
        if (!nodeBlockCacheEntryHandleIsEmpty(&handles[i])) {
            nodeBlockCacheEntryHandleDeleteNode(&handles[i]);
            nodeBlockCacheEntryHandleDestroy(&handles[i]);
        }
    }

    free(handles);
}

static int r2fsAppendUniqueDeletedHandleCopy(
    const NodeBlockCacheEntryHandle *handle,
    NodeBlockCacheEntryHandle **array,
    size_t *count,
    size_t *capacity
)
{
    size_t i;
    NodeBlockCacheEntryHandle *new_array;
    size_t new_capacity;

    if (handle == NULL || array == NULL || count == NULL || capacity == NULL) {
        return EINVAL;
    }

    if (nodeBlockCacheEntryHandleIsEmpty((NodeBlockCacheEntryHandle *)handle)) {
        return 0;
    }

    for (i = 0; i < *count; ++i) {
        if ((*array)[i].entry == handle->entry) {
            return 0;
        }
    }

    if (*count == *capacity) {
        new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        new_array = (NodeBlockCacheEntryHandle *)realloc(
            *array,
            new_capacity * sizeof(**array)
        );
        if (new_array == NULL) {
            return ENOMEM;
        }
        *array = new_array;
        *capacity = new_capacity;
    }

    nodeBlockCacheEntryHandleCopy(&(*array)[*count], handle);
    (*count)++;
    return 0;
}

static int r2fsCollectDirectNodeReclaimPlan(
    NodeBlockCacheHelper *helper,
    uint32_t direct_nid,
    uint32_t parent_nid,
    uint32_t **data_lpas,
    size_t *data_count,
    size_t *data_capacity,
    NodeBlockCacheEntryHandle **deleted_handles,
    size_t *deleted_handle_count,
    size_t *deleted_handle_capacity
)
{
    NodeBlockCacheEntryHandle direct_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *direct_node;
    size_t i;
    int ret;

    if (helper == NULL || direct_nid == INVALID_NID ||
        data_lpas == NULL || data_count == NULL || data_capacity == NULL ||
        deleted_handles == NULL || deleted_handle_count == NULL ||
        deleted_handle_capacity == NULL) {
        return EINVAL;
    }

    ret = nodeBlockCacheHelperGetNodeEntry(
        helper,
        direct_nid,
        parent_nid,
        &direct_handle
    );
    if (ret != 0) {
        return ret;
    }

    ret = r2fsAppendUniqueDeletedHandleCopy(
        &direct_handle,
        deleted_handles,
        deleted_handle_count,
        deleted_handle_capacity
    );
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        return ret;
    }

    direct_node = nodeBlockCacheEntryGetNodeBlockPtr(direct_handle.entry);
    if (direct_node == NULL) {
        nodeBlockCacheEntryHandleDestroy(&direct_handle);
        return EIO;
    }

    for (i = 0; i < DEF_ADDRS_PER_BLOCK; ++i) {
        ret = r2fsAppendUniqueUint32(
            direct_node->dn.addr[i],
            data_lpas,
            data_count,
            data_capacity
        );
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&direct_handle);
            return ret;
        }
    }

    nodeBlockCacheEntryHandleDestroy(&direct_handle);
    return 0;
}

static int r2fsCollectIndirectNodeReclaimPlan(
    NodeBlockCacheHelper *helper,
    uint32_t indirect_nid,
    uint32_t parent_nid,
    uint32_t **data_lpas,
    size_t *data_count,
    size_t *data_capacity,
    NodeBlockCacheEntryHandle **deleted_handles,
    size_t *deleted_handle_count,
    size_t *deleted_handle_capacity
)
{
    NodeBlockCacheEntryHandle indirect_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *indirect_node;
    size_t i;
    int ret;

    if (helper == NULL || indirect_nid == INVALID_NID ||
        data_lpas == NULL || data_count == NULL || data_capacity == NULL ||
        deleted_handles == NULL || deleted_handle_count == NULL ||
        deleted_handle_capacity == NULL) {
        return EINVAL;
    }

    ret = nodeBlockCacheHelperGetNodeEntry(
        helper,
        indirect_nid,
        parent_nid,
        &indirect_handle
    );
    if (ret != 0) {
        return ret;
    }

    ret = r2fsAppendUniqueDeletedHandleCopy(
        &indirect_handle,
        deleted_handles,
        deleted_handle_count,
        deleted_handle_capacity
    );
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&indirect_handle);
        return ret;
    }

    indirect_node = nodeBlockCacheEntryGetNodeBlockPtr(indirect_handle.entry);
    if (indirect_node == NULL) {
        nodeBlockCacheEntryHandleDestroy(&indirect_handle);
        return EIO;
    }

    for (i = 0; i < NIDS_PER_BLOCK; ++i) {
        if (indirect_node->in.nid[i] == INVALID_NID) {
            continue;
        }

        ret = r2fsCollectDirectNodeReclaimPlan(
            helper,
            indirect_node->in.nid[i],
            indirect_nid,
            data_lpas,
            data_count,
            data_capacity,
            deleted_handles,
            deleted_handle_count,
            deleted_handle_capacity
        );
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&indirect_handle);
            return ret;
        }
    }

    nodeBlockCacheEntryHandleDestroy(&indirect_handle);
    return 0;
}

static int r2fsCollectDoubleIndirectNodeReclaimPlan(
    NodeBlockCacheHelper *helper,
    uint32_t double_indirect_nid,
    uint32_t parent_nid,
    uint32_t **data_lpas,
    size_t *data_count,
    size_t *data_capacity,
    NodeBlockCacheEntryHandle **deleted_handles,
    size_t *deleted_handle_count,
    size_t *deleted_handle_capacity
)
{
    NodeBlockCacheEntryHandle dind_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *dind_node;
    size_t i;
    int ret;

    if (helper == NULL || double_indirect_nid == INVALID_NID ||
        data_lpas == NULL || data_count == NULL || data_capacity == NULL ||
        deleted_handles == NULL || deleted_handle_count == NULL ||
        deleted_handle_capacity == NULL) {
        return EINVAL;
    }

    ret = nodeBlockCacheHelperGetNodeEntry(
        helper,
        double_indirect_nid,
        parent_nid,
        &dind_handle
    );
    if (ret != 0) {
        return ret;
    }

    ret = r2fsAppendUniqueDeletedHandleCopy(
        &dind_handle,
        deleted_handles,
        deleted_handle_count,
        deleted_handle_capacity
    );
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&dind_handle);
        return ret;
    }

    dind_node = nodeBlockCacheEntryGetNodeBlockPtr(dind_handle.entry);
    if (dind_node == NULL) {
        nodeBlockCacheEntryHandleDestroy(&dind_handle);
        return EIO;
    }

    for (i = 0; i < NIDS_PER_BLOCK; ++i) {
        if (dind_node->in.nid[i] == INVALID_NID) {
            continue;
        }

        ret = r2fsCollectIndirectNodeReclaimPlan(
            helper,
            dind_node->in.nid[i],
            double_indirect_nid,
            data_lpas,
            data_count,
            data_capacity,
            deleted_handles,
            deleted_handle_count,
            deleted_handle_capacity
        );
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&dind_handle);
            return ret;
        }
    }

    nodeBlockCacheEntryHandleDestroy(&dind_handle);
    return 0;
}

static int r2fsCollectRegularFileReclaimPlan(
    file_system_manager *fs_manager,
    const struct RtfsNode *target_node,
    uint32_t **out_data_lpas,
    size_t *out_data_count,
    NodeBlockCacheEntryHandle **out_deleted_handles,
    size_t *out_deleted_handle_count
)
{
    NodeBlockCacheHelper helper;
    size_t i;
    uint32_t *data_lpas = NULL;
    size_t data_count = 0;
    size_t data_capacity = 0;
    NodeBlockCacheEntryHandle *deleted_handles = NULL;
    size_t deleted_handle_count = 0;
    size_t deleted_handle_capacity = 0;
    int ret = 0;

    if (fs_manager == NULL || target_node == NULL ||
        out_data_lpas == NULL || out_data_count == NULL ||
        out_deleted_handles == NULL || out_deleted_handle_count == NULL) {
        return EINVAL;
    }

    *out_data_lpas = NULL;
    *out_data_count = 0;
    *out_deleted_handles = NULL;
    *out_deleted_handle_count = 0;

    nodeBlockCacheHelperInit(&helper, fs_manager);

    for (i = 0; i < DEF_ADDRS_PER_INODE; ++i) {
        ret = r2fsAppendUniqueUint32(
            target_node->i.i_addr[i],
            &data_lpas,
            &data_count,
            &data_capacity
        );
        if (ret != 0) {
            goto out;
        }
    }

    for (i = 0; i < 2; ++i) {
        if (target_node->i.i_nid[i] == INVALID_NID) {
            continue;
        }

        ret = r2fsCollectDirectNodeReclaimPlan(
            &helper,
            target_node->i.i_nid[i],
            target_node->footer.nid,
            &data_lpas,
            &data_count,
            &data_capacity,
            &deleted_handles,
            &deleted_handle_count,
            &deleted_handle_capacity
        );
        if (ret != 0) {
            goto out;
        }
    }

    for (i = 2; i < 4; ++i) {
        if (target_node->i.i_nid[i] == INVALID_NID) {
            continue;
        }

        ret = r2fsCollectIndirectNodeReclaimPlan(
            &helper,
            target_node->i.i_nid[i],
            target_node->footer.nid,
            &data_lpas,
            &data_count,
            &data_capacity,
            &deleted_handles,
            &deleted_handle_count,
            &deleted_handle_capacity
        );
        if (ret != 0) {
            goto out;
        }
    }

    if (target_node->i.i_nid[4] != INVALID_NID) {
        ret = r2fsCollectDoubleIndirectNodeReclaimPlan(
            &helper,
            target_node->i.i_nid[4],
            target_node->footer.nid,
            &data_lpas,
            &data_count,
            &data_capacity,
            &deleted_handles,
            &deleted_handle_count,
            &deleted_handle_capacity
        );
        if (ret != 0) {
            goto out;
        }
    }

    *out_data_lpas = data_lpas;
    *out_data_count = data_count;
    *out_deleted_handles = deleted_handles;
    *out_deleted_handle_count = deleted_handle_count;
    ret = 0;

out:
    if (ret != 0) {
        free(data_lpas);
        r2fsDestroyHandleArray(deleted_handles, deleted_handle_count);
    }
    nodeBlockCacheHelperDestroy(&helper);
    return ret;
}

static int r2fsCollectTargetReclaimPlan(
    file_system_manager *fs_manager,
    const RtfsRuntimeInodeView *target_view,
    const struct RtfsNode *target_node,
    uint32_t **out_data_lpas,
    size_t *out_data_count,
    NodeBlockCacheEntryHandle **out_deleted_handles,
    size_t *out_deleted_handle_count
)
{
    int ret;

    if (fs_manager == NULL || target_view == NULL || target_node == NULL ||
        out_data_lpas == NULL || out_data_count == NULL ||
        out_deleted_handles == NULL || out_deleted_handle_count == NULL) {
        return EINVAL;
    }

    *out_data_lpas = NULL;
    *out_data_count = 0;
    *out_deleted_handles = NULL;
    *out_deleted_handle_count = 0;

    if (target_view->file_type != RTFS_FT_REG_FILE) {
        return 0;
    }

    ret = r2fsCollectRegularFileReclaimPlan(
        fs_manager,
        target_node,
        out_data_lpas,
        out_data_count,
        out_deleted_handles,
        out_deleted_handle_count
    );
    return ret;
}

static int r2fsAppendUniqueLpasToArray(
    const uint32_t *src,
    size_t src_count,
    uint32_t **dst,
    size_t *dst_count,
    size_t *dst_capacity
)
{
    size_t i;
    int ret;

    if ((src_count != 0 && src == NULL) || dst == NULL || dst_count == NULL || dst_capacity == NULL) {
        return EINVAL;
    }

    for (i = 0; i < src_count; ++i) {
        ret = r2fsAppendUniqueUint32(src[i], dst, dst_count, dst_capacity);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int r2fsCollectDirPendingOldDataLpasAppend(
    RtfsDirInode *dir_inode,
    uint32_t **dst,
    size_t *dst_count,
    size_t *dst_capacity
)
{
    uint32_t *scratch = NULL;
    size_t scratch_count = 0;
    int ret;

    if (dir_inode == NULL || dst == NULL || dst_count == NULL || dst_capacity == NULL) {
        return EINVAL;
    }

    if (rtfsDirInodeGetLoadedBlockCount(dir_inode) == 0) {
        return 0;
    }

    scratch = (uint32_t *)malloc(
        rtfsDirInodeGetLoadedBlockCount(dir_inode) * sizeof(*scratch)
    );
    if (scratch == NULL) {
        return ENOMEM;
    }

    ret = rtfsDirInodeCollectPendingDataCowOldLpas(
        dir_inode,
        scratch,
        rtfsDirInodeGetLoadedBlockCount(dir_inode),
        &scratch_count
    );
    if (ret == 0) {
        ret = r2fsAppendUniqueLpasToArray(
            scratch,
            scratch_count,
            dst,
            dst_count,
            dst_capacity
        );
    }

    free(scratch);
    return ret;
}

static int r2fsCommitDirtyDirsAndNodesWithTxId(
    file_system_manager *fs_manager,
    RtfsDirInode *first_dir,
    RtfsDirInode *second_dir,
    uint64_t *out_tx_id
)
{
    NodeBlockCache *node_cache;
    JournalContainer *cur_journal;
    JournalContainer *to_commit = NULL;
    NodeBlockCacheCowRelocation *node_relocations = NULL;
    uint32_t *old_data_lpas = NULL;
    size_t old_data_count = 0;
    size_t old_data_capacity = 0;
    uint32_t *old_node_lpas = NULL;
    size_t old_node_count = 0;
    size_t i;
    uint64_t tx_id = 0;
    bool journal_submitted = false;
    int ret;

    if (fs_manager == NULL || first_dir == NULL || second_dir == NULL) {
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

    ret = rtfsDirInodeWritebackContentCow(fs_manager, first_dir);
    if (ret != 0) {
        return ret;
    }

    ret = r2fsCollectDirPendingOldDataLpasAppend(
        first_dir,
        &old_data_lpas,
        &old_data_count,
        &old_data_capacity
    );
    if (ret != 0) {
        free(old_data_lpas);
        return ret;
    }

    ret = rtfsDirInodeWritebackContentCow(fs_manager, second_dir);
    if (ret != 0) {
        free(old_data_lpas);
        return ret;
    }

    ret = r2fsCollectDirPendingOldDataLpasAppend(
        second_dir,
        &old_data_lpas,
        &old_data_count,
        &old_data_capacity
    );
    if (ret != 0) {
        free(old_data_lpas);
        return ret;
    }

    ret = rtfsDirInodeApplyPendingCowRelocations(fs_manager, first_dir);
    if (ret != 0) {
        free(old_data_lpas);
        return ret;
    }

    ret = rtfsDirInodeApplyPendingCowRelocations(fs_manager, second_dir);
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
            size_t valid_old_node_count = 0;

            old_node_lpas = (uint32_t *)malloc(old_node_count * sizeof(*old_node_lpas));
            if (old_node_lpas == NULL) {
                free(node_relocations);
                free(old_data_lpas);
                return ENOMEM;
            }

            for (i = 0; i < old_node_count; ++i) {
                if (node_relocations[i].oldLpa == INVALID_LPA) {
                    continue;
                }
                old_node_lpas[valid_old_node_count++] = node_relocations[i].oldLpa;
            }
            old_node_count = valid_old_node_count;
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
        to_commit = r2fsCloneJournalContainer(cur_journal);
        if (to_commit == NULL) {
            free(old_node_lpas);
            free(node_relocations);
            free(old_data_lpas);
            return ENOMEM;
        }

        ret = r2fsSubmitJournalContainer(to_commit, &tx_id);
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
            old_node_count,
            NULL,
            0
        );
    }

    free(old_node_lpas);
    free(node_relocations);
    free(old_data_lpas);
    if (out_tx_id != NULL) {
        *out_tx_id = tx_id;
    }
    return 0;
}

static int r2fsRenameAcrossParents(
    const rtems_filesystem_location_info_t *oldparentloc,
    const rtems_filesystem_location_info_t *oldloc,
    const rtems_filesystem_location_info_t *newparentloc,
    const char *old_name,
    const char *new_name,
    size_t new_namelen,
    const RtfsRuntimeInodeView *source_view,
    const RtfsDirLookupResult *existing_target
)
{
    RtfsDirInode *old_dir_inode = NULL;
    RtfsDirInode *new_dir_inode = NULL;
    file_system_manager *old_fs_manager = NULL;
    file_system_manager *new_fs_manager = NULL;
    RtfsRuntimeInodeView *old_parent_view = NULL;
    RtfsRuntimeInodeView *new_parent_view = NULL;
    NodeBlockCacheEntryHandle old_parent_handle = {
        .cache = NULL,
        .entry = NULL
    };
    NodeBlockCacheEntryHandle new_parent_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *old_parent_node = NULL;
    struct RtfsNode *new_parent_node = NULL;
    NodeBlockCacheEntryHandle source_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *source_node = NULL;
    NodeBlockCacheEntryHandle replaced_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *replaced_node = NULL;
    uint32_t *replaced_data_lpas = NULL;
    size_t replaced_data_count = 0;
    NodeBlockCacheEntryHandle *replaced_deleted_handles = NULL;
    size_t replaced_deleted_handle_count = 0;
    NodeBlockCacheEntryHandle *deferred_delete_handles = NULL;
    size_t deferred_delete_handle_count = 0;
    bool target_exists = existing_target != NULL;
    uint64_t tx_id = 0;
    int ret;

    if (oldparentloc == NULL || oldloc == NULL || newparentloc == NULL ||
        old_name == NULL || new_name == NULL || source_view == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (r2fsResolveParentDirInode(
            oldparentloc,
            &old_dir_inode,
            &old_parent_view,
            &old_fs_manager
        ) != 0) {
        return -1;
    }

    if (r2fsResolveParentDirInode(
            newparentloc,
            &new_dir_inode,
            &new_parent_view,
            &new_fs_manager
        ) != 0) {
        rtfsDirInodePut(old_dir_inode);
        return -1;
    }

    if (old_fs_manager != new_fs_manager) {
        rtfsDirInodePut(new_dir_inode);
        rtfsDirInodePut(old_dir_inode);
        errno = EXDEV;
        return -1;
    }

    ret = r2fsResolveTargetInodeHandle(
        oldparentloc,
        NULL,
        NULL,
        &old_parent_handle,
        &old_parent_node
    );
    if (ret != 0) {
        rtfsDirInodePut(new_dir_inode);
        rtfsDirInodePut(old_dir_inode);
        return -1;
    }

    ret = r2fsResolveTargetInodeHandle(
        newparentloc,
        NULL,
        NULL,
        &new_parent_handle,
        &new_parent_node
    );
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
        rtfsDirInodePut(new_dir_inode);
        rtfsDirInodePut(old_dir_inode);
        return -1;
    }

    ret = r2fsResolveTargetInodeHandle(
        oldloc,
        NULL,
        NULL,
        &source_handle,
        &source_node
    );
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
        nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
        rtfsDirInodePut(new_dir_inode);
        rtfsDirInodePut(old_dir_inode);
        return -1;
    }

    if (target_exists) {
        rtems_filesystem_location_info_t target_loc;
        RtfsRuntimeInodeView target_view_copy = existing_target->inode_view;

        memset(&target_loc, 0, sizeof(target_loc));
        target_loc.mt_entry = newparentloc->mt_entry;
        target_loc.node_access = &target_view_copy;

        if (rtfsInodeIsDirectoryType(source_view->file_type) &&
            !rtfsInodeIsDirectoryType(existing_target->inode_view.file_type)) {
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
            nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
            rtfsDirInodePut(new_dir_inode);
            rtfsDirInodePut(old_dir_inode);
            errno = ENOTDIR;
            return -1;
        }

        if (!rtfsInodeIsDirectoryType(source_view->file_type) &&
            rtfsInodeIsDirectoryType(existing_target->inode_view.file_type)) {
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
            nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
            rtfsDirInodePut(new_dir_inode);
            rtfsDirInodePut(old_dir_inode);
            errno = EISDIR;
            return -1;
        }

        ret = r2fsResolveTargetInodeHandle(
            &target_loc,
            NULL,
            NULL,
            &replaced_handle,
            &replaced_node
        );
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
            nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
            rtfsDirInodePut(new_dir_inode);
            rtfsDirInodePut(old_dir_inode);
            return -1;
        }

        ret = r2fsCollectTargetReclaimPlan(
            old_fs_manager,
            &existing_target->inode_view,
            replaced_node,
            &replaced_data_lpas,
            &replaced_data_count,
            &replaced_deleted_handles,
            &replaced_deleted_handle_count
        );
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
            nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
            rtfsDirInodePut(new_dir_inode);
            rtfsDirInodePut(old_dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            errno = ret;
            return -1;
        }

        ret = r2fsTargetIsRemovableEmptyObject(&existing_target->inode_view, replaced_node);
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
            nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
            rtfsDirInodePut(new_dir_inode);
            rtfsDirInodePut(old_dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            errno = ret;
            return -1;
        }

        ret = rtfsDirInodeRemoveEntry(new_dir_inode, new_name);
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
            nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
            rtfsDirInodePut(new_dir_inode);
            rtfsDirInodePut(old_dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            errno = ret;
            return -1;
        }
    }

    ret = rtfsDirInodeAddEntry(new_dir_inode, new_name, source_view);
    if (ret != 0) {
        if (target_exists) {
            (void)rtfsDirInodeAddEntry(new_dir_inode, new_name, &existing_target->inode_view);
        }
        nodeBlockCacheEntryHandleDestroy(&replaced_handle);
        nodeBlockCacheEntryHandleDestroy(&source_handle);
        nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
        nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
        rtfsDirInodePut(new_dir_inode);
        rtfsDirInodePut(old_dir_inode);
        free(replaced_data_lpas);
        r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
        errno = ret;
        return -1;
    }

    ret = rtfsDirInodeRemoveEntry(old_dir_inode, old_name);
    if (ret != 0) {
        (void)rtfsDirInodeRemoveEntry(new_dir_inode, new_name);
        if (target_exists) {
            (void)rtfsDirInodeAddEntry(new_dir_inode, new_name, &existing_target->inode_view);
        }
        nodeBlockCacheEntryHandleDestroy(&replaced_handle);
        nodeBlockCacheEntryHandleDestroy(&source_handle);
        nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
        nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
        rtfsDirInodePut(new_dir_inode);
        rtfsDirInodePut(old_dir_inode);
        free(replaced_data_lpas);
        r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
        errno = ret;
        return -1;
    }

    if (rtfsInodeIsDirectoryType(source_view->file_type)) {
        ret = r2fsAdjustParentDirectoryNlinkWithHandle(
            &old_parent_handle,
            old_parent_view,
            -1
        );
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
            nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
            rtfsDirInodePut(new_dir_inode);
            rtfsDirInodePut(old_dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            return -1;
        }

        ret = r2fsAdjustParentDirectoryNlinkWithHandle(
            &new_parent_handle,
            new_parent_view,
            1
        );
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
            nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
            rtfsDirInodePut(new_dir_inode);
            rtfsDirInodePut(old_dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            return -1;
        }
    }

    if (target_exists) {
        if (r2fsMarkTargetInodeUnlinked(&existing_target->inode_view, &replaced_handle, replaced_node) != 0) {
            nodeBlockCacheEntryHandleDestroy(&replaced_handle);
            nodeBlockCacheEntryHandleDestroy(&source_handle);
            nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
            nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
            rtfsDirInodePut(new_dir_inode);
            rtfsDirInodePut(old_dir_inode);
            free(replaced_data_lpas);
            r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            return -1;
        }

        if (rtfsInodeIsDirectoryType(existing_target->inode_view.file_type)) {
            ret = r2fsAdjustParentDirectoryNlinkWithHandle(
                &new_parent_handle,
                new_parent_view,
                -1
            );
            if (ret != 0) {
                nodeBlockCacheEntryHandleDestroy(&replaced_handle);
                nodeBlockCacheEntryHandleDestroy(&source_handle);
                nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
                nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
                rtfsDirInodePut(new_dir_inode);
                rtfsDirInodePut(old_dir_inode);
                free(replaced_data_lpas);
                r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
                return -1;
            }
        }
    }

    source_node->i.i_pino = (uint32_t)new_parent_view->ino;
    memset(source_node->i.i_name, 0, sizeof(source_node->i.i_name));
    memcpy(source_node->i.i_name, new_name, new_namelen);
    source_node->i.i_namelen = (uint32_t)new_namelen;
    r2fsTouchInodeTimes(&source_node->i, r2fsGetTimestampTick());
    nodeBlockCacheEntryHandleMarkDirty(&source_handle);

    ret = r2fsCommitDirtyDirsAndNodesWithTxId(
        old_fs_manager,
        old_dir_inode,
        new_dir_inode,
        &tx_id
    );

    nodeBlockCacheEntryHandleDestroy(&source_handle);
    nodeBlockCacheEntryHandleDestroy(&new_parent_handle);
    nodeBlockCacheEntryHandleDestroy(&old_parent_handle);
    rtfsDirInodePut(new_dir_inode);
    rtfsDirInodePut(old_dir_inode);

    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&replaced_handle);
        free(replaced_data_lpas);
        r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
        errno = ret;
        return -1;
    }

    if (target_exists) {
        if (tx_id != 0) {
            size_t i;

            deferred_delete_handle_count = replaced_deleted_handle_count + 1;
            deferred_delete_handles = (NodeBlockCacheEntryHandle *)calloc(
                deferred_delete_handle_count,
                sizeof(*deferred_delete_handles)
            );
            if (deferred_delete_handles == NULL) {
                nodeBlockCacheEntryHandleDestroy(&replaced_handle);
                free(replaced_data_lpas);
                r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
                errno = ENOMEM;
                return -1;
            }

            for (i = 0; i < replaced_deleted_handle_count; ++i) {
                nodeBlockCacheEntryHandleCopy(
                    &deferred_delete_handles[i],
                    &replaced_deleted_handles[replaced_deleted_handle_count - 1 - i]
                );
            }
            nodeBlockCacheEntryHandleCopy(
                &deferred_delete_handles[replaced_deleted_handle_count],
                &replaced_handle
            );

            ret = cowReclaimRegistryRegister(
                tx_id,
                replaced_data_lpas,
                replaced_data_count,
                NULL,
                0,
                deferred_delete_handles,
                deferred_delete_handle_count
            );
            r2fsDestroyHandleArray(deferred_delete_handles, deferred_delete_handle_count);
            if (ret != 0) {
                nodeBlockCacheEntryHandleDestroy(&replaced_handle);
                free(replaced_data_lpas);
                r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
                errno = ret;
                return -1;
            }
        } else {
            r2fsDeleteAndDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
            replaced_deleted_handles = NULL;
            replaced_deleted_handle_count = 0;
            nodeBlockCacheEntryHandleDeleteNode(&replaced_handle);
        }
    }

    free(replaced_data_lpas);
    r2fsDestroyHandleArray(replaced_deleted_handles, replaced_deleted_handle_count);
    nodeBlockCacheEntryHandleDestroy(&replaced_handle);
    (void)tx_id;
    return 0;
}

static JournalContainer *r2fsCloneJournalContainer(
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
        SuperBlockJournalEntry entry = kv_A(src->superBlockJournal, i);
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

static int r2fsUnlinkFromParentAndCommit(
    const rtems_filesystem_location_info_t *parentloc,
    const char *name,
    uint64_t *out_tx_id
)
{
    RtfsDirInode *dir_inode;
    file_system_manager *fs_manager;
    int ret;

    if (parentloc == NULL || name == NULL || out_tx_id == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (r2fsResolveParentDirInode(parentloc, &dir_inode, NULL, &fs_manager) != 0) {
        return -1;
    }

    ret = rtfsDirInodeRemoveEntry(dir_inode, name);
    if (ret != 0) {
        rtfsDirInodePut(dir_inode);
        errno = ret;
        return -1;
    }
    RTFS_LOG(RTFS_LOG_INFO, "unlink commit begin name=%s", name);
    ret = rtfsDirInodeCommitCowWritebackWithTxId(fs_manager, dir_inode, out_tx_id);
    rtfsDirInodePut(dir_inode);
    if (ret != 0) {
        errno = ret;
        return -1;
    }
    RTFS_LOG(
        RTFS_LOG_INFO,
        "unlink commit end name=%s tx_id=%llu",
        name,
        (unsigned long long)(out_tx_id != NULL ? *out_tx_id : 0)
    );
    return 0;
}

static int r2fsSubmitJournalContainer(
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

    {
        JournalProcessEnv *env = journalProcessEnvGetInstance();
        journalProcessEnvCommitJournal(env, journal);
    }

    return 0;
}

static int r2fsCommitDirtyNodeOnlyWithTxId(
    file_system_manager *fs_manager,
    uint64_t *out_tx_id
)
{
    NodeBlockCache *node_cache;
    JournalContainer *cur_journal;
    JournalContainer *to_commit = NULL;
    NodeBlockCacheCowRelocation *node_relocations = NULL;
    uint32_t *old_node_lpas = NULL;
    size_t old_node_count = 0;
    size_t i;
    uint64_t tx_id = 0;
    int ret;

    if (fs_manager == NULL) {
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

    ret = nodeBlockCacheWritebackDirtyContentCow(node_cache);
    if (ret != 0) {
        return ret;
    }

    if (node_cache->curSize > 0) {
        node_relocations = (NodeBlockCacheCowRelocation *)malloc(
            node_cache->curSize * sizeof(*node_relocations)
        );
        if (node_relocations == NULL) {
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
            return ret;
        }

        if (old_node_count > 0) {
            old_node_lpas = (uint32_t *)malloc(old_node_count * sizeof(*old_node_lpas));
            if (old_node_lpas == NULL) {
                free(node_relocations);
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
        return ret;
    }

    if (!journalContainerIsEmpty(cur_journal)) {
        to_commit = r2fsCloneJournalContainer(cur_journal);
        if (to_commit == NULL) {
            free(old_node_lpas);
            free(node_relocations);
            return ENOMEM;
        }

        ret = r2fsSubmitJournalContainer(to_commit, &tx_id);
        if (ret != 0) {
            journalContainerDestroy(to_commit);
            free(to_commit);
            free(old_node_lpas);
            free(node_relocations);
            return ret;
        }
        journalContainerDestroy(cur_journal);
        journalContainerInit(cur_journal);

        (void)cowReclaimRegistryRegister(
            tx_id,
            NULL,
            0,
            old_node_lpas,
            old_node_count,
            NULL,
            0
        );
    }

    free(old_node_lpas);
    free(node_relocations);
    if (out_tx_id != NULL) {
        *out_tx_id = tx_id;
    }
    return 0;
}

static int r2fsCommitDirtyNodeOnly(file_system_manager *fs_manager)
{
    return r2fsCommitDirtyNodeOnlyWithTxId(fs_manager, NULL);
}

static void r2fsInitCreatedInode(
    struct RtfsNode *node,
    rtfs_ino parent_ino,
    const char *name,
    size_t namelen,
    mode_t mode,
    bool is_dir
)
{
    struct RtfsInode *inode = &node->i;
    uint32_t nid;

    if (node == NULL) {
        return;
    }

    nid = node->footer.nid;

    memset(node, 0, sizeof(*node));

    inode->i_mode = (uint16_t)(mode & 0777);
    inode->i_type = is_dir ? RTFS_FT_DIR : RTFS_FT_REG_FILE;
    inode->i_nlink = is_dir ? 2u : 1u;
    inode->i_pino = (uint32_t)parent_ino;
    inode->i_namelen = (uint32_t)namelen;
    inode->i_size = 0;
    inode->i_blocks = 0;
    inode->i_dentry_num = 0;
    inode->i_current_depth = 0;
    if (is_dir) {
        inode->i_inline = RTFS_INLINE_DENTRY;
    }
    if (name != NULL && namelen > 0) {
        memcpy(inode->i_name, name, namelen);
    }

    node->footer.nid = nid;
    node->footer.ino = nid;
}

// ****************************** Initialize API ******************************

int r2fsInitialize(
    rtems_filesystem_mount_table_entry_t *mt_entry,
    const void *data
)
{
    comm_dev *dev;
    file_system_manager *fs_manager;
    RtfsRuntimeInodeView *root_view;
    rtfs_ino root_ino;
    int ret;

    if (mt_entry == NULL || mt_entry->mt_fs_root == NULL) {
        errno = EINVAL;
        return -1;
    }

    dev = (comm_dev *)(data != NULL ? data : mt_entry->dev);
    ret = comm_submit_fs_recover_from_db_request(dev);
    if (ret != 0) {
        errno = ret == ENOMEM ? ENOMEM : EBUSY;
        return -1;
    }

    ret = fileSystemManagerSetup(dev);
    if (ret != 0) {
        errno = ret == -ENOMEM ? ENOMEM : EBUSY;
        return -1;
    }

    fs_manager = fileSystemManagerGetInstance();
    if (fs_manager == NULL) {
        errno = EIO;
        return -1;
    }

    root_ino = r2fsGetRootIno(fs_manager);
    root_view = rtfsRuntimeInodeViewCreate(root_ino, root_ino, RTFS_FT_DIR);
    if (root_view == NULL) {
        fileSystemManagerFini();
        errno = ENOMEM;
        return -1;
    }

    mt_entry->fs_info = fs_manager;
    mt_entry->ops = &r2fsFsHandler;
    mt_entry->mt_fs_root->location.node_access = root_view;
    mt_entry->mt_fs_root->location.node_access_2 = NULL;
    mt_entry->mt_fs_root->location.handlers = &rtfsDirhandlers;

    return 0;
}

// ****************************** Handler API ******************************

void r2fsLock(const rtems_filesystem_mount_table_entry_t *mt_entry)
{
    file_system_manager *fs_manager = mt_entry->fs_info;

    fileSystemManagerMetaLock(fs_manager);
}

void r2fsUnlock(const rtems_filesystem_mount_table_entry_t *mt_entry)
{
    file_system_manager *fs_manager = mt_entry->fs_info;

    fileSystemManagerMetaUnlock(fs_manager);
}

static bool r2fsAreNodesEqual(
    const rtems_filesystem_location_info_t *a,
    const rtems_filesystem_location_info_t *b
)
{
    const RtfsRuntimeInodeView *a_view = r2fsGetNodeView(a);
    const RtfsRuntimeInodeView *b_view = r2fsGetNodeView(b);

    if (a == NULL || b == NULL) {
        return 0;
    }

    if (a->mt_entry != b->mt_entry) {
        return 0;
    }

    if (a_view == NULL || b_view == NULL) {
        return a_view == b_view;
    }

    return a_view->ino == b_view->ino;
}

// ****************** Handler EvalPath API *******************

static bool r2fsFsIsDirectory(
    rtems_filesystem_eval_path_context_t *ctx,
    void *arg
)
{
    rtems_filesystem_location_info_t *currentloc;
    RtfsRuntimeInodeView *view;

    (void)arg;

    currentloc = rtems_filesystem_eval_path_get_currentloc(ctx);
    view = r2fsGetNodeView(currentloc);
    return view != NULL && rtfsInodeIsDirectoryType(view->file_type);
}

static rtems_filesystem_eval_path_generic_status r2fsFsEvalToken(
    rtems_filesystem_eval_path_context_t *ctx,
    void *arg,
    const char *token,
    size_t tokenlen
)
{
    rtems_filesystem_location_info_t *currentloc;
    RtfsRuntimeInodeView *current_view;
    RtfsDirInode *dir_inode;
    RtfsDirInodeBuildRequest request;
    file_system_manager *fs_manager;
    RtfsDirLookupResult lookup_result;
    int ret;

    (void)arg;

    currentloc = rtems_filesystem_eval_path_get_currentloc(ctx);
    current_view = r2fsGetNodeView(currentloc);
    fs_manager = r2fsGetFsManagerFromLoc(currentloc);

    if (current_view == NULL) {
        rtems_filesystem_eval_path_error(ctx, EIO);
        return RTEMS_FILESYSTEM_EVAL_PATH_GENERIC_DONE;
    }

    if (rtems_filesystem_is_current_directory(token, tokenlen)) {
        rtems_filesystem_eval_path_clear_token(ctx);
        return RTEMS_FILESYSTEM_EVAL_PATH_GENERIC_CONTINUE;
    }

    request.ino = current_view->ino;
    request.mode = RTFS_DIR_BUILD_ON_DEMAND;

    ret = rtfsDirInodeResolve(fs_manager, NULL, &request, &dir_inode);
    if (ret != 0) {
        rtems_filesystem_eval_path_error(ctx, ret);
        return RTEMS_FILESYSTEM_EVAL_PATH_GENERIC_DONE;
    }

    do {
        ret = rtfsDirInodeLookup(dir_inode, token, tokenlen, &lookup_result);
        if (ret != ENOENT || rtfsDirInodeIsFullyLoaded(dir_inode)) {
            break;
        }

        ret = rtfsDirInodeResolveNext(fs_manager, current_view->ino, dir_inode);
        if (ret != 0) {
            rtfsDirInodePut(dir_inode);
            rtems_filesystem_eval_path_error(ctx, ret);
            return RTEMS_FILESYSTEM_EVAL_PATH_GENERIC_DONE;
        }
    } while (true);

    rtfsDirInodePut(dir_inode);

    if (ret == ENOENT) {
        return RTEMS_FILESYSTEM_EVAL_PATH_GENERIC_NO_ENTRY;
    }

    if (ret != 0) {
        rtems_filesystem_eval_path_error(ctx, ret);
        return RTEMS_FILESYSTEM_EVAL_PATH_GENERIC_DONE;
    }

    if (r2fsSetLocationNode(currentloc, &lookup_result.inode_view, token, tokenlen) != 0) {
        rtems_filesystem_eval_path_error(ctx, errno);
        return RTEMS_FILESYSTEM_EVAL_PATH_GENERIC_DONE;
    }

    rtems_filesystem_eval_path_clear_token(ctx);
    if (rtems_filesystem_eval_path_has_path(ctx)) {
        return RTEMS_FILESYSTEM_EVAL_PATH_GENERIC_CONTINUE;
    }

    return RTEMS_FILESYSTEM_EVAL_PATH_GENERIC_DONE;
}

static const rtems_filesystem_eval_path_generic_config r2fsFsEvalConfig = {
    .is_directory = r2fsFsIsDirectory,
    .eval_token = r2fsFsEvalToken
};

static void r2fsEvalPath(rtems_filesystem_eval_path_context_t *ctx)
{
    rtems_filesystem_eval_path_generic(ctx, NULL, &r2fsFsEvalConfig);
}

// ****************** Handler Other API *******************

int r2fsMknod(
    const rtems_filesystem_location_info_t *parentloc,
    const char *name,
    size_t namelen,
    mode_t mode,
    dev_t dev
)
{
    RtfsRuntimeInodeView *parent_view;
    file_system_manager *fs_manager;
    super_manager *sp_manager;
    NodeBlockCache *node_cache;
    NodeBlockCacheHelper helper;
    NodeBlockCacheEntryHandle inode_handle = {
        .cache = NULL,
        .entry = NULL
    };
    NodeBlockCacheEntryHandle parent_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *parent_node = NULL;
    struct RtfsNode *created_inode;
    RtfsRuntimeInodeView child_view;
    bool is_dir;
    bool parent_nlink_adjusted = false;
    int ret;

    (void)dev;

    if (r2fsValidateParentDir(parentloc, name, namelen, &parent_view) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "mknod input invalid");
        return -1;
    }

    if (!S_ISDIR(mode) && !S_ISREG(mode)) {
        errno = ENOTSUP;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "mknod mode unsupported: %u", (unsigned int)mode);
        return -1;
    }

    fs_manager = r2fsGetFsManagerFromLoc(parentloc);
    sp_manager = r2fsGetSuperManagerFromLoc(parentloc);
    node_cache = fileSystemManagerGetNodeCache(fs_manager);
    if (fs_manager == NULL || sp_manager == NULL || node_cache == NULL) {
        errno = EIO;
        RTFS_ERRNO_LOG(RTFS_LOG_ERROR, errno, "mknod cannot get fs_manager/super_manager/node_cache");
        return -1;
    }

    is_dir = S_ISDIR(mode);
    ret = r2fsNameExistsInParent(parentloc, name);
    if (ret > 0) {
        errno = EEXIST;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "mknod target already exists");
        return -1;
    }
    if (ret < 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "mknod failed to probe target existence");
        return -1;
    }

    nodeBlockCacheHelperInit(&helper, fs_manager);
    inode_handle = nodeBlockCacheHelperCreateInodeEntry(&helper);
    nodeBlockCacheHelperDestroy(&helper);
    if (nodeBlockCacheEntryHandleIsEmpty(&inode_handle)) {
        errno = ENOSPC;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "mknod failed to create inode entry");
        return -1;
    }

    created_inode = nodeBlockCacheEntryGetNodeBlockPtr(inode_handle.entry);
    r2fsInitCreatedInode(created_inode, parent_view->ino, name, namelen, mode, is_dir);
    nodeBlockCacheEntryHandleMarkDirty(&inode_handle);

    if (is_dir) {
        ret = r2fsResolveTargetInodeHandle(
            parentloc,
            NULL,
            NULL,
            &parent_handle,
            &parent_node
        );
        if (ret != 0) {
            r2fsRollbackCreatedInode(&inode_handle);
            return -1;
        }

        if (r2fsAdjustParentDirectoryNlinkWithHandle(&parent_handle, parent_view, 1) != 0) {
            nodeBlockCacheEntryHandleDestroy(&parent_handle);
            r2fsRollbackCreatedInode(&inode_handle);
            return -1;
        }
        parent_nlink_adjusted = true;
    }

    rtfsRuntimeInodeViewInit(
        &child_view,
        created_inode->footer.ino,
        parent_view->ino,
        is_dir ? RTFS_FT_DIR : RTFS_FT_REG_FILE
    );

    RTFS_LOG(
        RTFS_LOG_INFO,
        "mknod created ino=%u under parent ino=%llu, name=%.*s, mode=%u",
        created_inode->footer.ino,
        (unsigned long long)parent_view->ino,
        (int)namelen,
        name,
        (unsigned int)mode
    );

    ret = r2fsAddNameToParent(parentloc, name, &child_view);
    if (ret != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "mknod failed to add parent dentry");
        if (parent_nlink_adjusted) {
            (void)r2fsAdjustParentDirectoryNlinkWithHandle(&parent_handle, parent_view, -1);
        }
        nodeBlockCacheEntryHandleDestroy(&parent_handle);
        r2fsRollbackCreatedInode(&inode_handle);
        return -1;
    }

    nodeBlockCacheEntryHandleDestroy(&parent_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);
    return 0;
}

int r2fsRmnod(
    const rtems_filesystem_location_info_t *parentloc,
    const rtems_filesystem_location_info_t *loc
)
{
    RtfsRuntimeInodeView *parent_view;
    RtfsRuntimeInodeView *target_view;
    file_system_manager *fs_manager;
    uint32_t *target_data_lpas = NULL;
    size_t target_data_count = 0;
    NodeBlockCacheEntryHandle *target_deleted_handles = NULL;
    size_t target_deleted_handle_count = 0;
    NodeBlockCacheEntryHandle target_handle = {
        .cache = NULL,
        .entry = NULL
    };
    NodeBlockCacheEntryHandle *deferred_delete_handles = NULL;
    size_t deferred_delete_handle_count = 0;
    struct RtfsNode *target_node;
    const char *target_name;
    uint64_t tx_id = 0;
    int ret;

    if (r2fsValidateNodeLoc(parentloc, &parent_view) != 0 ||
        r2fsValidateNodeLoc(loc, &target_view) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod input invalid");
        return -1;
    }

    if (!rtfsInodeIsDirectoryType(parent_view->file_type)) {
        errno = ENOTDIR;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod parent is not a directory");
        return -1;
    }

    if (parent_view->ino == target_view->ino) {
        errno = EBUSY;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod refuses to remove mount/root node");
        return -1;
    }

    target_name = r2fsGetNodeName(loc);
    if (target_name == NULL || target_name[0] == '\0') {
        errno = EINVAL;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod target name is unavailable");
        return -1;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "rmnod requested for target ino=%llu under parent ino=%llu, name=%s",
        (unsigned long long)target_view->ino,
        (unsigned long long)parent_view->ino,
        target_name
    );

    if (r2fsResolveTargetInodeHandle(
            loc,
            &target_view,
            &fs_manager,
            &target_handle,
            &target_node
        ) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod failed to resolve target inode");
        return -1;
    }

    ret = r2fsCollectTargetReclaimPlan(
        fs_manager,
        target_view,
        target_node,
        &target_data_lpas,
        &target_data_count,
        &target_deleted_handles,
        &target_deleted_handle_count
    );
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&target_handle);
        free(target_data_lpas);
        r2fsDestroyHandleArray(target_deleted_handles, target_deleted_handle_count);
        errno = ret;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod failed to collect reclaim plan for target");
        return -1;
    }

    ret = r2fsTargetIsRemovableEmptyObject(target_view, target_node);
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&target_handle);
        free(target_data_lpas);
        r2fsDestroyHandleArray(target_deleted_handles, target_deleted_handle_count);
        errno = ret;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod target is not removable by current model");
        return -1;
    }

    if (r2fsMarkTargetInodeUnlinked(target_view, &target_handle, target_node) != 0) {
        nodeBlockCacheEntryHandleDestroy(&target_handle);
        free(target_data_lpas);
        r2fsDestroyHandleArray(target_deleted_handles, target_deleted_handle_count);
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod failed to mark target inode unlinked");
        return -1;
    }

    if (rtfsInodeIsDirectoryType(target_view->file_type) &&
        r2fsAdjustParentDirectoryNlink(parentloc, -1) != 0) {
        nodeBlockCacheEntryHandleDestroy(&target_handle);
        free(target_data_lpas);
        r2fsDestroyHandleArray(target_deleted_handles, target_deleted_handle_count);
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod failed to adjust parent directory nlink");
        return -1;
    }

    ret = r2fsUnlinkFromParentAndCommit(parentloc, target_name, &tx_id);
    if (ret != 0) {
        nodeBlockCacheEntryHandleDestroy(&target_handle);
        free(target_data_lpas);
        r2fsDestroyHandleArray(target_deleted_handles, target_deleted_handle_count);
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod failed to unlink from parent");
        return -1;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "rmnod commit result target ino=%llu tx_id=%llu",
        (unsigned long long)target_view->ino,
        (unsigned long long)tx_id
    );

    if (tx_id != 0) {
        deferred_delete_handle_count = target_deleted_handle_count + 1;
        deferred_delete_handles = (NodeBlockCacheEntryHandle *)calloc(
            deferred_delete_handle_count,
            sizeof(*deferred_delete_handles)
        );
        if (deferred_delete_handles == NULL) {
            nodeBlockCacheEntryHandleDestroy(&target_handle);
            free(target_data_lpas);
            r2fsDestroyHandleArray(target_deleted_handles, target_deleted_handle_count);
            errno = ENOMEM;
            RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod failed to allocate deferred delete handles");
            return -1;
        }

        {
            size_t i;

            for (i = 0; i < target_deleted_handle_count; ++i) {
                nodeBlockCacheEntryHandleCopy(
                    &deferred_delete_handles[i],
                    &target_deleted_handles[target_deleted_handle_count - 1 - i]
                );
            }
        }
        nodeBlockCacheEntryHandleCopy(
            &deferred_delete_handles[target_deleted_handle_count],
            &target_handle
        );

        ret = cowReclaimRegistryRegister(
            tx_id,
            target_data_lpas,
            target_data_count,
            NULL,
            0,
            deferred_delete_handles,
            deferred_delete_handle_count
        );
        r2fsDestroyHandleArray(deferred_delete_handles, deferred_delete_handle_count);
        if (ret != 0) {
            nodeBlockCacheEntryHandleDestroy(&target_handle);
            free(target_data_lpas);
            r2fsDestroyHandleArray(target_deleted_handles, target_deleted_handle_count);
            errno = ret;
            RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rmnod failed to register deferred reclaim");
            return -1;
        }
    } else {
        r2fsDeleteAndDestroyHandleArray(target_deleted_handles, target_deleted_handle_count);
        nodeBlockCacheEntryHandleDeleteNode(&target_handle);
    }

    free(target_data_lpas);
    if (tx_id != 0) {
        r2fsDestroyHandleArray(target_deleted_handles, target_deleted_handle_count);
    }
    nodeBlockCacheEntryHandleDestroy(&target_handle);
    return 0;
}

int r2fsFchmod(const rtems_filesystem_location_info_t *loc, mode_t mode)
{
    RtfsRuntimeInodeView *view;
    file_system_manager *fs_manager;
    NodeBlockCacheEntryHandle inode_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *inode_node;
    int ret;

    if (r2fsValidateNodeLoc(loc, &view) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "fchmod input invalid");
        return -1;
    }

    fs_manager = r2fsGetFsManagerFromLoc(loc);
    ret = r2fsResolveTargetInodeHandle(
        loc,
        NULL,
        NULL,
        &inode_handle,
        &inode_node
    );
    if (ret != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "fchmod failed to resolve target inode");
        return -1;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "fchmod requested for ino=%llu, mode=%u",
        (unsigned long long)view->ino,
        (unsigned int)mode
    );

    inode_node->i.i_mode = (uint16_t)(mode & 07777);
    r2fsTouchInodeTimes(&inode_node->i, r2fsGetTimestampTick());
    nodeBlockCacheEntryHandleMarkDirty(&inode_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    ret = r2fsCommitDirtyNodeOnly(fs_manager);
    if (ret != 0) {
        errno = ret;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "fchmod failed to commit dirty inode");
        return -1;
    }

    return 0;
}

int r2fsChown(
    const rtems_filesystem_location_info_t *loc,
    uid_t owner,
    gid_t group
)
{
    RtfsRuntimeInodeView *view;

    if (r2fsValidateNodeLoc(loc, &view) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "chown input invalid");
        return -1;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "chown requested for ino=%llu, owner=%u, group=%u",
        (unsigned long long)view->ino,
        (unsigned int)owner,
        (unsigned int)group
    );

    // TODO: RtfsRuntimeInodeView 当前不承载 owner/group。
    // TODO: 接入 inode cache 后更新磁盘 inode 的属主字段。
    // TODO: 记录 chown 元数据日志。
    errno = ENOTSUP;
    return -1;
}

int r2fsCloneNode(rtems_filesystem_location_info_t *loc)
{
    RtfsRuntimeInodeView *view = r2fsGetNodeView(loc);
    RtfsRuntimeInodeView *clone;
    const char *name;
    char *name_clone = NULL;

    if (view == NULL) {
        errno = EINVAL;
        return -1;
    }

    clone = rtfsRuntimeInodeViewClone(view);
    if (clone == NULL) {
        errno = ENOMEM;
        RTFS_ERRNO_LOG(RTFS_LOG_ERROR, errno, "clone node failed");
        return -1;
    }

    name = r2fsGetNodeName(loc);
    if (name != NULL) {
        name_clone = r2fsDupName(name, strlen(name));
        if (name_clone == NULL) {
            free(clone);
            errno = ENOMEM;
            return -1;
        }
    }

    loc->node_access = clone;
    loc->node_access_2 = name_clone;
    r2fsSetHandlers(loc);
    return 0;
}

void r2fsFreeNode(const rtems_filesystem_location_info_t *loc)
{
    if (loc == NULL) {
        return;
    }

    RTFS_LOG(RTFS_LOG_DEBUG, "free node_access");
    free(loc->node_access);
    free((void *)loc->node_access_2);
}

int r2fsMount(rtems_filesystem_mount_table_entry_t *mt_entry)
{
    RTFS_LOG(RTFS_LOG_INFO, "mount requested for nested file system");
    return rtems_filesystem_default_mount(mt_entry);
}

int r2fsUnmount(rtems_filesystem_mount_table_entry_t *mt_entry)
{
    RTFS_LOG(RTFS_LOG_INFO, "unmount requested for nested file system");
    return rtems_filesystem_default_unmount(mt_entry);
}

void r2fsUnmountMe(rtems_filesystem_mount_table_entry_t *temp_mt_entry)
{
    file_system_manager *fs_manager;

    if (temp_mt_entry == NULL) {
        return;
    }

    RTFS_LOG(RTFS_LOG_INFO, "unmount file system instance");

    fs_manager = (file_system_manager *)temp_mt_entry->fs_info;
    if (fs_manager != NULL) {
        (void)fileSystemManagerFlushForUnmount(fs_manager);
    }

    if (temp_mt_entry->mt_fs_root != NULL) {
        r2fsFreeNode(&temp_mt_entry->mt_fs_root->location);
        temp_mt_entry->mt_fs_root->location.node_access = NULL;
        temp_mt_entry->mt_fs_root->location.node_access_2 = NULL;
        temp_mt_entry->mt_fs_root->location.handlers = NULL;
    }

    temp_mt_entry->fs_info = NULL;
    fileSystemManagerFini();
}

int r2fsUtimens(
    const rtems_filesystem_location_info_t *loc,
    struct timespec times[2]
)
{
    RtfsRuntimeInodeView *view;
    file_system_manager *fs_manager;
    NodeBlockCacheEntryHandle inode_handle = {
        .cache = NULL,
        .entry = NULL
    };
    struct RtfsNode *inode_node;
    int ret;
    uint64_t atime;
    uint64_t mtime;

    if (r2fsValidateNodeLoc(loc, &view) != 0 || times == NULL) {
        if (errno == 0) {
            errno = EINVAL;
        }
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "utimens input invalid");
        return -1;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "utimens requested for ino=%llu, atime=%lld, mtime=%lld",
        (unsigned long long)view->ino,
        (long long)times[0].tv_sec,
        (long long)times[1].tv_sec
    );

    fs_manager = r2fsGetFsManagerFromLoc(loc);
    ret = r2fsResolveTargetInodeHandle(
        loc,
        NULL,
        NULL,
        &inode_handle,
        &inode_node
    );
    if (ret != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "utimens failed to resolve target inode");
        return -1;
    }

    atime = (uint64_t)times[0].tv_sec;
    mtime = (uint64_t)times[1].tv_sec;
    inode_node->i.i_atime = atime;
    inode_node->i.i_mtime = mtime;
    inode_node->i.i_atime_nsec = (uint32_t)times[0].tv_nsec;
    inode_node->i.i_mtime_nsec = (uint32_t)times[1].tv_nsec;
    inode_node->i.i_ctime_nsec = 0;
    nodeBlockCacheEntryHandleMarkDirty(&inode_handle);
    nodeBlockCacheEntryHandleDestroy(&inode_handle);

    ret = r2fsCommitDirtyNodeOnly(fs_manager);
    if (ret != 0) {
        errno = ret;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "utimens failed to commit dirty inode");
        return -1;
    }

    return 0;
}

int r2fsSymlink(
    const rtems_filesystem_location_info_t *parentloc,
    const char *name,
    size_t namelen,
    const char *target
)
{
    RtfsRuntimeInodeView *parent_view;

    if (r2fsValidateParentDir(parentloc, name, namelen, &parent_view) != 0 ||
        target == NULL || target[0] == '\0') {
        if (errno == 0) {
            errno = EINVAL;
        }
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "symlink input invalid");
        return -1;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "symlink requested under parent ino=%llu, name=%.*s, target=%s",
        (unsigned long long)parent_view->ino,
        (int)namelen,
        name,
        target
    );

    // TODO: 接入 symlink inode/data 表达。
    // TODO: 接入目录项创建与符号链接内容持久化。
    // TODO: 记录 symlink 元数据日志。
    errno = ENOTSUP;
    return -1;
}

ssize_t r2fsReadlink(
    const rtems_filesystem_location_info_t *loc,
    char *buf,
    size_t bufsize
)
{
    RtfsRuntimeInodeView *view;

    if (r2fsValidateNodeLoc(loc, &view) != 0 || buf == NULL || bufsize == 0) {
        if (errno == 0) {
            errno = EINVAL;
        }
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "readlink input invalid");
        return -1;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "readlink requested for ino=%llu, bufsize=%zu",
        (unsigned long long)view->ino,
        bufsize
    );

    // TODO: 接入 symlink 数据读取路径。
    // TODO: 读取符号链接内容时记录必要访问日志。
    errno = ENOTSUP;
    return -1;
}

int r2fsRename(
    const rtems_filesystem_location_info_t *oldparentloc,
    const rtems_filesystem_location_info_t *oldloc,
    const rtems_filesystem_location_info_t *newparentloc,
    const char *name,
    size_t namelen
)
{
    RtfsRuntimeInodeView *old_parent_view;
    RtfsRuntimeInodeView *new_parent_view;
    RtfsRuntimeInodeView *target_view;
    const char *old_name;
    RtfsDirLookupResult existing_target = {0};

    if (r2fsValidateNodeLoc(oldparentloc, &old_parent_view) != 0 ||
        r2fsValidateNodeLoc(oldloc, &target_view) != 0 ||
        r2fsValidateParentDir(newparentloc, name, namelen, &new_parent_view) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rename input invalid");
        return -1;
    }

    old_name = r2fsGetNodeName(oldloc);
    if (old_name == NULL || old_name[0] == '\0') {
        errno = EINVAL;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rename old name is unavailable");
        return -1;
    }

    if (old_parent_view->ino == new_parent_view->ino &&
        strlen(old_name) == namelen &&
        memcmp(old_name, name, namelen) == 0) {
        return 0;
    }

    {
        int lookup_ret = r2fsResolveNameInParent(newparentloc, name, namelen, &existing_target);
        if (lookup_ret != 0 && errno != ENOENT) {
            RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rename failed to probe target existence");
            return -1;
        }
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "rename requested: target ino=%llu, old parent ino=%llu, old name=%s, new parent ino=%llu, new name=%.*s",
        (unsigned long long)target_view->ino,
        (unsigned long long)old_parent_view->ino,
        old_name,
        (unsigned long long)new_parent_view->ino,
        (int)namelen,
        name
    );

    if (old_parent_view->ino != new_parent_view->ino) {
        if (r2fsRenameAcrossParents(
                oldparentloc,
                oldloc,
                newparentloc,
                old_name,
                name,
                namelen,
                target_view,
                (existing_target.inode_view.ino == 0) ? NULL : &existing_target
            ) != 0) {
            RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "cross-directory rename failed");
            return -1;
        }

        if (r2fsReplaceLocationName((rtems_filesystem_location_info_t *)oldloc, name, namelen) != 0) {
            RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "cross-directory rename succeeded but failed to update cached name");
            return -1;
        }

        return 0;
    }

    if (r2fsRenameWithinSameParent(
            oldparentloc,
            oldloc,
            old_name,
            name,
            namelen,
            target_view,
            (existing_target.inode_view.ino == 0) ? NULL : &existing_target
        ) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rename failed within parent directory");
        return -1;
    }

    if (r2fsReplaceLocationName((rtems_filesystem_location_info_t *)oldloc, name, namelen) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rename succeeded but failed to update cached name");
        return -1;
    }

    return 0;
}

int r2fsStatvfs(const rtems_filesystem_location_info_t *loc, struct statvfs *buf)
{
    file_system_manager *fs_manager;
    RtfsSuperBlock *super_block;
    fsfilcnt_t total_files = 0;
    fsfilcnt_t free_files = 0;

    if (loc == NULL || buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    fs_manager = loc->mt_entry != NULL ? loc->mt_entry->fs_info : NULL;
    super_block = fileSystemManagerGetSuperBlkMem(fs_manager);

    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_namemax = RTFS_NAME_LEN;
    buf->f_fsid = (unsigned long)(uintptr_t)fs_manager;

    if (super_block != NULL) {
        buf->f_blocks = super_block->block_count;
        buf->f_bfree = (fsblkcnt_t)super_block->free_segment_count * BLOCK_PER_SEGMENT;
        buf->f_bavail = buf->f_bfree;
        total_files = r2fsStatvfsGetTotalFileSlots(super_block);
        free_files = r2fsStatvfsCountFreeNids(fs_manager, super_block);
        if (free_files > total_files) {
            free_files = total_files;
        }
        buf->f_files = total_files;
        buf->f_ffree = free_files;
        buf->f_favail = free_files;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "statvfs requested: blocks=%llu, free_blocks=%llu, files=%llu, free_files=%llu",
        (unsigned long long)buf->f_blocks,
        (unsigned long long)buf->f_bfree,
        (unsigned long long)buf->f_files,
        (unsigned long long)buf->f_ffree
    );

    // TODO: 如果后续引入保留块策略，补充 f_bavail 与 f_bfree 的差异。
    return 0;
}

// ****************************** Register API ******************************

const rtems_filesystem_operations_table r2fsFsHandler = {
    .lock_h = r2fsLock,
    .unlock_h = r2fsUnlock,
    .eval_path_h = r2fsEvalPath,
    .link_h = rtems_filesystem_default_link,
    .are_nodes_equal_h = r2fsAreNodesEqual,
    .mknod_h = r2fsMknod,
    .rmnod_h = r2fsRmnod,
    .fchmod_h = r2fsFchmod,
    .chown_h = r2fsChown,
    .clonenod_h = r2fsCloneNode,
    .freenod_h = r2fsFreeNode,
    .mount_h = r2fsMount,
    .unmount_h = r2fsUnmount,
    .fsunmount_me_h = r2fsUnmountMe,
    .utimens_h = r2fsUtimens,
    .symlink_h = r2fsSymlink,
    .readlink_h = r2fsReadlink,
    .rename_h = r2fsRename,
    .statvfs_h = r2fsStatvfs
};
