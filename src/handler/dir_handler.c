#include "dir_handler.h"

#include <dirent.h>
#include <errno.h>
#include <rtems/libio_.h>
#include <string.h>

#include "fs/dir_inode/dir_inode.h"
#include "fs/dir_inode/dir_inode_resolver.h"
#include "fs/fs_manager.h"
#include "fs/inode/inode.h"


static RtfsRuntimeInodeView *rtfsDirGetNodeView(
    const rtems_filesystem_location_info_t *pathloc
)
{
    return pathloc != NULL ? (RtfsRuntimeInodeView *)pathloc->node_access : NULL;
}

int rtfsDirOpen(rtems_libio_t *iop, const char *path, int oflag, mode_t mode)
{
    (void)iop;
    (void)path;
    (void)oflag;
    (void)mode;
    return 0;
}

int rtfsDirClose(rtems_libio_t *iop)
{
    (void)iop;
    return 0;
}

ssize_t rtfsDirRead(rtems_libio_t *iop, void *buffer, size_t count)
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

    view = rtfsDirGetNodeView(&iop->pathinfo);
    if (view == NULL || !rtfsInodeIsDirectoryType(view->file_type)) {
        errno = ENOTDIR;
        return -1;
    }

    fs_manager = (file_system_manager *)iop->pathinfo.mt_entry->fs_info;
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
        if ((size_t)total_bytes_read >= count || bytes_read > 0) {
            break;
        }

        if (count - (size_t)total_bytes_read < sizeof(struct dirent) ||
            rtfsDirInodeIsFullyLoaded(dir_inode)) {
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

int rtfsDirFstat(const rtems_filesystem_location_info_t *pathloc, struct stat *buf)
{
    RtfsRuntimeInodeView *view;

    if (pathloc == NULL || buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    view = rtfsDirGetNodeView(pathloc);
    if (view == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->st_ino = view->ino;
    buf->st_mode = S_IFDIR | 0755;
    buf->st_nlink = 1;
    buf->st_blksize = 4096;

    return 0;
}


const rtems_filesystem_file_handlers_r rtfsDirhandlers = {
    .open_h = rtfsDirOpen,
    .close_h = rtfsDirClose,
    .read_h = rtfsDirRead,
    .write_h = rtems_filesystem_default_write,
    .ioctl_h = rtems_filesystem_default_ioctl,
    .lseek_h = rtems_filesystem_default_lseek_directory,
    .fstat_h = rtfsDirFstat,
    .ftruncate_h = rtems_filesystem_default_ftruncate,
    .fsync_h = rtems_filesystem_default_fsync_or_fdatasync_success,
    .fdatasync_h = rtems_filesystem_default_fsync_or_fdatasync_success,
    .fcntl_h = rtems_filesystem_default_fcntl,
    .poll_h = rtems_filesystem_default_poll,
    .kqfilter_h = rtems_filesystem_default_kqfilter,
    .readv_h = rtems_filesystem_default_readv,
    .writev_h = rtems_filesystem_default_writev,
    .mmap_h = rtems_filesystem_default_mmap,
};
