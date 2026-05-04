#include "inode.h"

#include <stddef.h>
#include <stdlib.h>

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

RtfsRuntimeInodeView *rtfsRuntimeInodeViewCreate(
    rtfs_ino ino,
    rtfs_ino parent_ino,
    uint8_t file_type
)
{
    RtfsRuntimeInodeView *view = malloc(sizeof(*view));

    if (view == NULL) {
        return NULL;
    }

    rtfsRuntimeInodeViewInit(view, ino, parent_ino, file_type);
    return view;
}

RtfsRuntimeInodeView *rtfsRuntimeInodeViewClone(
    const RtfsRuntimeInodeView *source
)
{
    if (source == NULL) {
        return NULL;
    }

    return rtfsRuntimeInodeViewCreate(
        source->ino,
        source->parent_ino,
        source->file_type
    );
}

bool rtfsInodeIsDirectoryType(uint8_t file_type)
{
    return file_type == RTFS_FT_DIR;
}
