#include "file_handler.h"

#include "fs/inode.h"

#include <rtems/libio.h>


int rtfsFileOpen(rtems_libio_t *iop, const char *pathname, int oflag, mode_t mode)
{
}

int rtfsFileClose(rtems_libio_t *iop)
{
}

ssize_t rtfsFileRead(rtems_libio_t *iop, void *buffer, size_t count)
{
}

ssize_t rtfsFileWrite(rtems_libio_t *iop, const void *buffer, size_t count)
{
}

off_t rtfsFileLseek(rtems_libio_t *iop, off_t offset, int whence)
{
}

int rtfsFileFtruncate(rtems_libio_t *iop, off_t length)
{
}

int rtfsFileFdatasync(rtems_libio_t *iop)
{
}

int rtfsFileFstat(const rtems_filesystem_location_info_t *pathloc, struct stat *buf)
{
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
