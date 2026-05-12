#pragma once

#include <rtems/libio.h>

int r2fsInitialize(
    rtems_filesystem_mount_table_entry_t *mt_entry,
    const void *data
);

extern const rtems_filesystem_operations_table r2fsFsHandler;
