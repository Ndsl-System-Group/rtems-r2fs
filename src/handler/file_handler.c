#include "file_handler.h"

#include <errno.h>
#include <rtems/libio_.h>
#include <string.h>

#include "fs/inode/inode.h"


static RtfsRuntimeInodeView *rtfsFileGetNodeView(
    const rtems_filesystem_location_info_t *pathloc
)
{
    return pathloc != NULL ? (RtfsRuntimeInodeView *)pathloc->node_access : NULL;
}

int rtfsFileOpen(rtems_libio_t *iop, const char *pathname, int oflag, mode_t mode)
{
    (void)iop;
    (void)pathname;
    (void)oflag;
    (void)mode;
    return 0;
}

int rtfsFileClose(rtems_libio_t *iop)
{
    (void)iop;
    return 0;
}

ssize_t rtfsFileRead(rtems_libio_t *iop, void *buffer, size_t count)
{
    (void)iop;
    (void)buffer;
    (void)count;
    errno = ENOTSUP;
    return -1;
}

ssize_t rtfsFileWrite(rtems_libio_t *iop, const void *buffer, size_t count)
{
    (void)iop;
    (void)buffer;
    (void)count;
    errno = ENOTSUP;
    return -1;
}

off_t rtfsFileLseek(rtems_libio_t *iop, off_t offset, int whence)
{
    (void)iop;
    (void)offset;
    (void)whence;
    errno = ENOTSUP;
    return -1;
}

int rtfsFileFtruncate(rtems_libio_t *iop, off_t length)
{
    (void)iop;
    (void)length;
    errno = ENOTSUP;
    return -1;
}

int rtfsFileFdatasync(rtems_libio_t *iop)
{
    (void)iop;
    errno = ENOTSUP;
    return -1;
}

int rtfsFileFstat(const rtems_filesystem_location_info_t *pathloc, struct stat *buf)
{
    RtfsRuntimeInodeView *view;

    if (pathloc == NULL || buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    view = rtfsFileGetNodeView(pathloc);
    if (view == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->st_ino = view->ino;
    buf->st_mode = S_IFREG | 0644;
    buf->st_nlink = 1;
    buf->st_blksize = 4096;

    return 0;
}


const rtems_filesystem_file_handlers_r rtfsFilehandlers = {
    .open_h = rtfsFileOpen,
    .close_h = rtfsFileClose,
    .read_h = rtfsFileRead,
    .write_h = rtfsFileWrite,
    .ioctl_h = rtems_filesystem_default_ioctl,
    .lseek_h = rtfsFileLseek,
    .fstat_h = rtfsFileFstat,
    .ftruncate_h = rtfsFileFtruncate,
    .fsync_h = rtfsFileFdatasync,
    .fdatasync_h = rtfsFileFdatasync,
    .fcntl_h = rtems_filesystem_default_fcntl,
    .poll_h = rtems_filesystem_default_poll,
    .kqfilter_h = rtems_filesystem_default_kqfilter,
    .readv_h = rtems_filesystem_default_readv,
    .writev_h = rtems_filesystem_default_writev,
    .mmap_h = rtems_filesystem_default_mmap,
};
