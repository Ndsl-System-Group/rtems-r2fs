#ifndef _FILE_HANDLER_H_
#define _FILE_HANDLER_H_

#include <rtems/fs.h>

#include <utf8proc/utf8proc.h>

#include <sys/stat.h>


int rtfsFileOpen(rtems_libio_t *iop, const char *pathname, int oflag, mode_t mode);

int rtfsFileClose(rtems_libio_t *iop);

ssize_t rtfsFileRead(rtems_libio_t *iop, void *buffer, size_t count);

ssize_t rtfsFileWrite(rtems_libio_t *iop, const void *buffer, size_t count);

off_t rtfsFileLseek(rtems_libio_t *iop, off_t offset, int whence);

int rtfsFileFtruncate(rtems_libio_t *iop, off_t length);

int rtfsFileFdatasync(rtems_libio_t *iop);

int rtfsFileFstat(const rtems_filesystem_location_info_t *pathloc, struct stat *buf);


#endif
