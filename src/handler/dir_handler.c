#include "dir_handler.h"

#include <errno.h>
#include <rtems/libio_.h>
#include <string.h>

#include "fs/dir_inode.h"
#include "fs/inode.h"


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
    off_t offset;
    ssize_t bytes_read;

    if (iop == NULL || buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    view = rtfsDirGetNodeView(&iop->pathinfo);
    if (view == NULL || !rtfsInodeIsDirectoryType(view->file_type)) {
        errno = ENOTDIR;
        return -1;
    }

    dir_inode = rtfsDirInodeGet(NULL, view->ino);
    if (dir_inode == NULL) {
        errno = ENOMEM;
        return -1;
    }

    offset = iop->offset;
    bytes_read = rtfsDirInodeReadEntries(dir_inode, &offset, buffer, count);
    if (bytes_read >= 0) {
        iop->offset = offset;
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
