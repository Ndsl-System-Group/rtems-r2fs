#include "fs_handler.h"

#include <errno.h>
#include <stdint.h>
#include <rtems/libio_.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "dir_inode/dir_inode.h"
#include "dir_inode/dir_inode_resolver.h"
#include "fs_manager.h"
#include "dir_inode/dir_handler.h"
#include "handler/file_handler.h"
#include "inode/inode.h"
#include "nat_utils.h"
#include "super_manager.h"
#include "utils/rtfs_log.h"


static RtfsRuntimeInodeView *r2fsGetNodeView(
    const rtems_filesystem_location_info_t *loc
)
{
    return loc != NULL ? (RtfsRuntimeInodeView *)loc->node_access : NULL;
}

static rtfs_ino r2fsGetRootIno(file_system_manager *fs_manager)
{
    RtfsSuperBlock *super_block = fileSystemManagerGetSuperBlkMem(fs_manager);

    if (super_block != NULL && super_block->root_ino != 0) {
        return super_block->root_ino;
    }

    return 1;
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

    if (r2fsReplaceNodeView(currentloc, &lookup_result.inode_view) != 0) {
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
    uint32_t new_ino;
    uint32_t new_lpa;
    NatLpaMapping nat_mapping;
    bool is_dir;

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
    if (fs_manager == NULL || sp_manager == NULL) {
        errno = EIO;
        RTFS_ERRNO_LOG(RTFS_LOG_ERROR, errno, "mknod cannot get fs_manager/super_manager");
        return -1;
    }

    is_dir = S_ISDIR(mode);
    new_ino = superManagerAllocNid(sp_manager, 0, true);
    if (new_ino == INVALID_NID) {
        errno = ENOSPC;
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "mknod failed to allocate inode nid");
        return -1;
    }

    new_lpa = superManagerAllocNodeLpa(sp_manager);
    if (new_lpa == INVALID_LPA) {
        RTFS_LOG(
            RTFS_LOG_WARNING,
            "mknod rollback allocated ino=%u because node lpa allocation failed",
            new_ino
        );
        superManagerFreeNid(sp_manager, new_ino);
        errno = ENOSPC;
        return -1;
    }

    natLpaMappingInit(&nat_mapping, fs_manager);
    natSetLpaOfNid(&nat_mapping, new_ino, new_lpa);

    RTFS_LOG(
        RTFS_LOG_INFO,
        "mknod allocated ino=%u lpa=%u under parent ino=%llu, name=%.*s, mode=%u",
        new_ino,
        new_lpa,
        (unsigned long long)parent_view->ino,
        (int)namelen,
        name,
        (unsigned int)mode
    );

    // TODO: 基于 new_ino/new_lpa 初始化 inode 的磁盘/缓存表示。
    // TODO: 对目录类型创建 "." 和 ".." 的目录初始内容。
    // TODO: 将新 inode 对应的目录项写入 parent 目录。
    // TODO: 为 inode 创建和目录项创建记录元数据日志。
    RTFS_LOG(
        RTFS_LOG_INFO,
        "mknod rollback allocated ino=%u lpa=%u because inode/cache/dentry create path is not connected yet",
        new_ino,
        new_lpa
    );
    natSetLpaOfNid(&nat_mapping, new_ino, INVALID_LPA);
    superManagerFreeNid(sp_manager, new_ino);

    // TODO: 当 inode 创建与目录项落盘接通后，移除这里的回滚并返回成功。
    errno = ENOTSUP;
    return -1;
}

int r2fsRmnod(
    const rtems_filesystem_location_info_t *parentloc,
    const rtems_filesystem_location_info_t *loc
)
{
    RtfsRuntimeInodeView *parent_view;
    RtfsRuntimeInodeView *target_view;

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

    RTFS_LOG(
        RTFS_LOG_INFO,
        "rmnod requested for target ino=%llu under parent ino=%llu",
        (unsigned long long)target_view->ino,
        (unsigned long long)parent_view->ino
    );

    // TODO: 当前 location 中没有稳定保存 basename，无法直接定位父目录中的名字。
    // TODO: 接入 dentry/dir_cache 后，通过父目录 + 名字删除目录项。
    // TODO: 接入 inode/nlink 生命周期与删除日志。
    errno = ENOTSUP;
    return -1;
}

int r2fsFchmod(const rtems_filesystem_location_info_t *loc, mode_t mode)
{
    RtfsRuntimeInodeView *view;

    if (r2fsValidateNodeLoc(loc, &view) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "fchmod input invalid");
        return -1;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "fchmod requested for ino=%llu, mode=%u",
        (unsigned long long)view->ino,
        (unsigned int)mode
    );

    // TODO: RtfsRuntimeInodeView 当前不承载 mode。
    // TODO: 接入 inode cache 后更新磁盘 inode 的权限字段。
    // TODO: 记录 chmod 元数据日志。
    errno = ENOTSUP;
    return -1;
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

    loc->node_access = clone;
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
    if (temp_mt_entry == NULL) {
        return;
    }

    RTFS_LOG(RTFS_LOG_INFO, "unmount file system instance");

    if (temp_mt_entry->mt_fs_root != NULL) {
        r2fsFreeNode(&temp_mt_entry->mt_fs_root->location);
        temp_mt_entry->mt_fs_root->location.node_access = NULL;
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

    // TODO: RtfsRuntimeInodeView 当前不承载时间戳。
    // TODO: 接入 inode cache 后更新 atime/mtime。
    // TODO: 记录 utimens 元数据日志。
    errno = ENOTSUP;
    return -1;
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

    if (r2fsValidateNodeLoc(oldparentloc, &old_parent_view) != 0 ||
        r2fsValidateNodeLoc(oldloc, &target_view) != 0 ||
        r2fsValidateParentDir(newparentloc, name, namelen, &new_parent_view) != 0) {
        RTFS_ERRNO_LOG(RTFS_LOG_WARNING, errno, "rename input invalid");
        return -1;
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "rename requested: target ino=%llu, old parent ino=%llu, new parent ino=%llu, new name=%.*s",
        (unsigned long long)target_view->ino,
        (unsigned long long)old_parent_view->ino,
        (unsigned long long)new_parent_view->ino,
        (int)namelen,
        name
    );

    // TODO: 当前 location 中没有稳定保存旧名字，无法完成父目录中旧目录项删除。
    // TODO: 接入 dir_cache/dentry 层后完成 old remove + new add 的原子重命名。
    // TODO: 处理跨目录 rename、覆盖、目录 rename 的语义细节。
    // TODO: 记录 rename 元数据日志。
    errno = ENOTSUP;
    return -1;
}

int r2fsStatvfs(const rtems_filesystem_location_info_t *loc, struct statvfs *buf)
{
    file_system_manager *fs_manager;
    RtfsSuperBlock *super_block;

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
    }

    RTFS_LOG(
        RTFS_LOG_INFO,
        "statvfs requested: blocks=%llu, free_blocks=%llu",
        (unsigned long long)buf->f_blocks,
        (unsigned long long)buf->f_bfree
    );

    // TODO: 接入 inode/file 计数后补充 f_files / f_ffree。
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
