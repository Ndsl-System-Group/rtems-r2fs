#pragma once

#include <rtems/libio.h>

int rtfsInitialize(
    rtems_filesystem_mount_table_entry_t *mt_entry,
    const void *data);

extern const rtems_filesystem_operations_table rtfsFsHandler;
