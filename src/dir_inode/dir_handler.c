#include "dir_handler.h"

#include "cache/node_block_cache.h"
#include <dirent.h>
#include <errno.h>
#include <rtems/libio_.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "dir_inode/dir_inode.h"
#include "dir_inode/dir_inode_resolver.h"
#include "fs/fs.h"
#include "fs/fs_manager.h"
#include "inode/inode.h"
#include "inode/inode_loader.h"


static RtfsRuntimeInodeView *rtfsDirGetNodeView(
    const rtems_filesystem_location_info_t *pathloc
)
{
    return pathloc != NULL ? (RtfsRuntimeInodeView *)pathloc->node_access : NULL;
}

static int rtfsDirValidatePathloc(
    const rtems_filesystem_location_info_t *pathloc,
    RtfsRuntimeInodeView **out_view
)
{
    RtfsRuntimeInodeView *view;

    if (pathloc == NULL) {
        return EINVAL;
    }

    view = rtfsDirGetNodeView(pathloc);
    if (view == NULL || !rtfsInodeIsDirectoryType(view->file_type)) {
        return ENOTDIR;
    }

    if (out_view != NULL) {
        *out_view = view;
    }

    return 0;
}

static int rtfsDirValidateIop(
    rtems_libio_t *iop,
    RtfsRuntimeInodeView **out_view
)
{
    if (iop == NULL) {
        return EINVAL;
    }

    return rtfsDirValidatePathloc(&iop->pathinfo, out_view);
}

static int rtfsDirGetFsManager(
    const rtems_filesystem_location_info_t *pathloc,
    file_system_manager **out_fs_manager
)
{
    file_system_manager *fs_manager;

    if (pathloc == NULL || out_fs_manager == NULL || pathloc->mt_entry == NULL) {
        return EINVAL;
    }

    fs_manager = (file_system_manager *)pathloc->mt_entry->fs_info;
    if (fs_manager == NULL) {
        return EINVAL;
    }

    *out_fs_manager = fs_manager;
    return 0;
}

static int rtfsDirGetDiskInode(
    const rtems_filesystem_location_info_t *pathloc,
    struct RtfsInode **out_inode,
    NodeBlockCacheEntryHandle *out_handle
)
{
    file_system_manager *fs_manager;
    NodeBlockCache *node_cache;
    RtfsRuntimeInodeView *view;
    NodeBlockCacheEntryHandle handle = {
        .cache = NULL,
        .entry = NULL
    };
    int ret;

    if (pathloc == NULL || out_inode == NULL || out_handle == NULL) {
        return EINVAL;
    }

    *out_inode = NULL;
    out_handle->cache = NULL;
    out_handle->entry = NULL;

    view = rtfsDirGetNodeView(pathloc);
    fs_manager = (pathloc->mt_entry != NULL)
        ? (file_system_manager *)pathloc->mt_entry->fs_info
        : NULL;
    if (view == NULL || fs_manager == NULL) {
        return EINVAL;
    }

    ret = rtfsInodeLoaderEnsureCached(fs_manager, view->ino);
    if (ret != 0) {
        return ret;
    }

    node_cache = fileSystemManagerGetNodeCache(fs_manager);
    if (node_cache == NULL) {
        return ENOENT;
    }

    handle = nodeBlockCacheGet(node_cache, (uint32_t)view->ino);
    if (nodeBlockCacheEntryHandleIsEmpty(&handle)) {
        return ENOENT;
    }

    *out_inode = &nodeBlockCacheEntryGetNodeBlockPtr(handle.entry)->i;
    *out_handle = handle;
    return 0;
}

static int rtfsDirOpen(rtems_libio_t *iop, const char *path, int oflag, mode_t mode)
{
    int ret;

    (void)path;
    (void)mode;

    ret = rtfsDirValidateIop(iop, NULL);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    if ((oflag & O_ACCMODE) != O_RDONLY) {
        errno = EISDIR;
        return -1;
    }

    return 0;
}

static ssize_t rtfsDirRead(rtems_libio_t *iop, void *buffer, size_t count)
{
    RtfsRuntimeInodeView *view;
    RtfsDirInode *dir_inode;
    RtfsDirInodeBuildRequest request;
    file_system_manager *fs_manager;
    off_t offset;
    ssize_t bytes_read;
    ssize_t total_bytes_read;
    int ret;

    if (iop == NULL || buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    ret = rtfsDirValidateIop(iop, &view);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    ret = rtfsDirGetFsManager(&iop->pathinfo, &fs_manager);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    request.ino = view->ino;
    request.mode = RTFS_DIR_BUILD_ON_DEMAND;

    ret = rtfsDirInodeResolve(fs_manager, NULL, &request, &dir_inode);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    offset = iop->offset;
    total_bytes_read = 0;

    do {
        bytes_read = rtfsDirInodeReadEntries(
            dir_inode,
            &offset,
            (char *)buffer + total_bytes_read,
            count - (size_t)total_bytes_read
        );
        if (bytes_read < 0) {
            break;
        }

        total_bytes_read += bytes_read;
        if ((size_t)total_bytes_read >= count) {
            break;
        }

        if (bytes_read > 0 &&
            (count - (size_t)total_bytes_read < sizeof(struct dirent) ||
             rtfsDirInodeIsFullyLoaded(dir_inode))) {
            break;
        }

        ret = rtfsDirInodeResolveNext(fs_manager, view->ino, dir_inode);
        if (ret != 0) {
            errno = ret;
            bytes_read = -1;
            break;
        }
    } while (true);

    if (bytes_read >= 0) {
        iop->offset = offset;
        bytes_read = total_bytes_read;
    }

    rtfsDirInodePut(dir_inode);
    return bytes_read;
}

static int rtfsDirFstat(const rtems_filesystem_location_info_t *pathloc, struct stat *buf)
{
    RtfsRuntimeInodeView *view;
    struct RtfsInode *disk_inode;
    NodeBlockCacheEntryHandle handle = {
        .cache = NULL,
        .entry = NULL
    };
    mode_t mode_bits;
    int ret;

    if (pathloc == NULL || buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    ret = rtfsDirValidatePathloc(pathloc, &view);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    ret = rtfsDirGetDiskInode(pathloc, &disk_inode, &handle);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->st_ino = view->ino;
    mode_bits = (mode_t)disk_inode->i_mode;
    if (mode_bits == 0) {
        mode_bits = 0755;
    }
    buf->st_mode = S_IFDIR | mode_bits;
    buf->st_nlink = disk_inode->i_nlink != 0 ? disk_inode->i_nlink : 1;
    buf->st_size = (off_t)disk_inode->i_size;
    buf->st_blocks = (blkcnt_t)SIZE_TO_BLOCK(disk_inode->i_size);
    buf->st_blksize = 4096;
    buf->st_atime = (time_t)disk_inode->i_atime;
    buf->st_mtime = (time_t)disk_inode->i_mtime;
    buf->st_ctime = (time_t)disk_inode->i_mtime;

    nodeBlockCacheEntryHandleDestroy(&handle);

    return 0;
}


const rtems_filesystem_file_handlers_r rtfsDirhandlers = {
    .open_h = rtfsDirOpen,
    .close_h = rtems_filesystem_default_close,
    .read_h = rtfsDirRead,
    .write_h = rtems_filesystem_default_write,
    .ioctl_h = rtems_filesystem_default_ioctl,
    .lseek_h = rtems_filesystem_default_lseek_directory,
    .fstat_h = rtfsDirFstat,
    .ftruncate_h = rtems_filesystem_default_ftruncate_directory,
    .fsync_h = rtems_filesystem_default_fsync_or_fdatasync_success,
    .fdatasync_h = rtems_filesystem_default_fsync_or_fdatasync_success,
    .fcntl_h = rtems_filesystem_default_fcntl,
    .poll_h = rtems_filesystem_default_poll,
    .kqfilter_h = rtems_filesystem_default_kqfilter,
    .readv_h = rtems_filesystem_default_readv,
    .writev_h = rtems_filesystem_default_writev,
    .mmap_h = rtems_filesystem_default_mmap,
};
