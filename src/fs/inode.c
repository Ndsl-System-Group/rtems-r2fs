#include "inode.h"

#include <stddef.h>

void rtfsRuntimeInodeViewInit(
    RtfsRuntimeInodeView *view,
    rtfs_ino ino,
    rtfs_ino parent_ino,
    uint8_t file_type
)
{
    if (view == NULL) {
        return;
    }

    view->ino = ino;
    view->parent_ino = parent_ino;
    view->file_type = file_type;
}

bool rtfsInodeIsDirectoryType(uint8_t file_type)
{
    return file_type == RTFS_FT_DIR;
}
