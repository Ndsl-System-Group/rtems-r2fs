#ifndef _DIR_HANDLER_H_
#define _DIR_HANDLER_H_

#include <rtems/fs.h>

#include <utf8proc/utf8proc.h>

#include <sys/stat.h>


int rtfsDirOpen(rtems_libio_t *iop, const char *path, int oflag, mode_t mode);

int rtfsDirClose(rtems_libio_t *iop);

ssize_t rtfsDirRead(rtems_libio_t *iop, void *buffer, size_t count);

int rtfsDirFstat(const rtems_filesystem_location_info_t *pathloc, struct stat *buf);


#endif
