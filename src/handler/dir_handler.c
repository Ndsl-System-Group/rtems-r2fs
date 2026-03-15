#include "dir_handler.h"

#include <rtems/libio.h>


int rtfsDirOpen(rtems_libio_t *iop, const char *path, int oflag, mode_t mode)
{
}

int rtfsDirClose(rtems_libio_t *iop)
{
}

ssize_t rtfsDirRead(rtems_libio_t *iop, void *buffer, size_t count)
{
}

int rtfsDirFstat(const rtems_filesystem_location_info_t *pathloc, struct stat *buf)
{
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
