#include "file_handler.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <rtems/libio_.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cache/node_block_cache.h"
#include "file_inode/file_inode.h"
#include "file_inode/file_inode_resolver.h"
#include "fs/fs.h"
#include "fs/fs_manager.h"
#include "inode/inode.h"
#include "inode/inode_loader.h"
#include "utils/rtfs_log.h"


typedef struct RtfsFileHandle
{
    file_system_manager *fs_manager;
    RtfsFileInode *file_inode;
    int oflag;
    bool sync_failed;
} RtfsFileHandle;

static RtfsRuntimeInodeView *rtfsFileGetNodeView(
    const rtems_filesystem_location_info_t *pathloc
)
{
    return pathloc != NULL ? (RtfsRuntimeInodeView *)pathloc->node_access : NULL;
}

static int rtfsFileValidatePathloc(
    const rtems_filesystem_location_info_t *pathloc,
    RtfsRuntimeInodeView **out_view
)
{
    RtfsRuntimeInodeView *view;

    if (pathloc == NULL) {
        return EINVAL;
    }

    view = rtfsFileGetNodeView(pathloc);
    if (view == NULL) {
        return EINVAL;
    }

    if (rtfsInodeIsDirectoryType(view->file_type)) {
        return EISDIR;
    }

    if (view->file_type != RTFS_FT_REG_FILE) {
        return EINVAL;
    }

    if (out_view != NULL) {
        *out_view = view;
    }

    return 0;
}

static int rtfsFileGetFsManager(
    const rtems_filesystem_location_info_t *pathloc,
    file_system_manager **out_fs_manager
)
{
    file_system_manager *fs_manager;

    if (pathloc == NULL || pathloc->mt_entry == NULL ||
        out_fs_manager == NULL) {
        return EINVAL;
    }

    fs_manager = (file_system_manager *)pathloc->mt_entry->fs_info;
    if (fs_manager == NULL) {
        return EINVAL;
    }

    *out_fs_manager = fs_manager;
    return 0;
}

static int rtfsFileGetHandle(
    rtems_libio_t *iop,
    RtfsFileHandle **out_handle
)
{
    RtfsFileHandle *handle;

    if (iop == NULL || out_handle == NULL) {
        return EINVAL;
    }

    handle = (RtfsFileHandle *)iop->data1;
    if (handle == NULL || handle->file_inode == NULL ||
        handle->fs_manager == NULL) {
        return EBADF;
    }

    *out_handle = handle;
    return 0;
}

static int rtfsFileGetDiskInode(
    const rtems_filesystem_location_info_t *pathloc,
    struct RtfsInode **out_inode,
    NodeBlockCacheEntryHandle *out_handle
)
{
    RtfsRuntimeInodeView *view;
    file_system_manager *fs_manager;
    NodeBlockCache *node_cache;
    NodeBlockCacheEntryHandle handle = {
        .cache = NULL,
        .entry = NULL
    };
    int ret;

    if (pathloc == NULL || out_inode == NULL || out_handle == NULL) {
        return EINVAL;
    }

    *out_inode = NULL;
    out_handle->cache = NULL;
    out_handle->entry = NULL;

    ret = rtfsFileValidatePathloc(pathloc, &view);
    if (ret != 0) {
        return ret;
    }

    ret = rtfsFileGetFsManager(pathloc, &fs_manager);
    if (ret != 0) {
        return ret;
    }

    ret = rtfsInodeLoaderEnsureCached(fs_manager, view->ino);
    if (ret != 0) {
        return ret;
    }

    node_cache = fileSystemManagerGetNodeCache(fs_manager);
    if (node_cache == NULL) {
        return ENOENT;
    }

    handle = nodeBlockCacheGet(node_cache, (uint32_t)view->ino);
    if (nodeBlockCacheEntryHandleIsEmpty(&handle)) {
        return ENOENT;
    }

    *out_inode = &nodeBlockCacheEntryGetNodeBlockPtr(handle.entry)->i;
    *out_handle = handle;
    return 0;
}

static int rtfsFileComputeSeekOffset(
    off_t base,
    off_t delta,
    off_t *out_offset
)
{
    off_t result;

    if (out_offset == NULL || base < 0) {
        return EINVAL;
    }

    if (delta >= 0) {
        if (base > (off_t)(LLONG_MAX - delta)) {
            return EOVERFLOW;
        }
        result = base + delta;
    } else {
        uint64_t distance = (uint64_t)(-(delta + 1)) + 1U;

        if ((uint64_t)base < distance) {
            return EINVAL;
        }
        result = (off_t)((uint64_t)base - distance);
    }

    *out_offset = result;
    return 0;
}

int rtfsFileOpen(rtems_libio_t *iop, const char *pathname, int oflag, mode_t mode)
{
    RtfsRuntimeInodeView *view;
    file_system_manager *fs_manager;
    RtfsFileInodeBuildRequest request;
    RtfsFileHandle *handle;
    RtfsFileInode *file_inode = NULL;
    int ret;

    (void)pathname;
    (void)mode;

    if (iop == NULL) {
        errno = EINVAL;
        RTFS_LOG(RTFS_LOG_WARNING, "file open invalid iop");
        return -1;
    }

    if ((rtems_libio_iop_flags(iop) & LIBIO_FLAGS_OPEN) != 0) {
        errno = EBUSY;
        RTFS_LOG(
            RTFS_LOG_WARNING,
            "file open rejects reused iop path=%s",
            pathname != NULL ? pathname : "(null)"
        );
        return -1;
    }

    ret = rtfsFileValidatePathloc(&iop->pathinfo, &view);
    if (ret != 0) {
        RTFS_LOG(
            RTFS_LOG_WARNING,
            "file open validate path failed ret=%d path=%s",
            ret,
            pathname != NULL ? pathname : "(null)"
        );
        errno = ret;
        return -1;
    }

    ret = rtfsFileGetFsManager(&iop->pathinfo, &fs_manager);
    if (ret != 0) {
        RTFS_LOG(
            RTFS_LOG_WARNING,
            "file open get fs manager failed ret=%d path=%s",
            ret,
            pathname != NULL ? pathname : "(null)"
        );
        errno = ret;
        return -1;
    }

    if ((oflag & O_TRUNC) != 0 &&
        (oflag & O_ACCMODE) == O_RDONLY) {
        errno = EACCES;
        RTFS_LOG(
            RTFS_LOG_WARNING,
            "file open rejects readonly truncation path=%s",
            pathname != NULL ? pathname : "(null)"
        );
        return -1;
    }

    handle = (RtfsFileHandle *)calloc(1, sizeof(*handle));
    if (handle == NULL) {
        errno = ENOMEM;
        RTFS_LOG(
            RTFS_LOG_WARNING,
            "file open alloc handle failed path=%s",
            pathname != NULL ? pathname : "(null)"
        );
        return -1;
    }

    request.ino = view->ino;
    request.mode = RTFS_FILE_BUILD_WITH_PAGE_CACHE;

    ret = rtfsFileInodeResolve(fs_manager, NULL, &request, &file_inode);
    if (ret != 0) {
        free(handle);
        RTFS_LOG(
            RTFS_LOG_WARNING,
            "file open resolve inode failed ret=%d ino=%u path=%s",
            ret,
            (unsigned int)view->ino,
            pathname != NULL ? pathname : "(null)"
        );
        errno = ret;
        return -1;
    }

    if ((oflag & O_TRUNC) != 0) {
        ret = rtfsFileInodeTruncate(fs_manager, file_inode, 0);
        if (ret == 0) {
            ret = rtfsFileInodeCommitCowWriteback(fs_manager, file_inode);
        }
        if (ret != 0) {
            rtfsFileInodePut(file_inode);
            free(handle);
            RTFS_LOG(
                RTFS_LOG_WARNING,
                "file open truncate commit failed ret=%d ino=%u path=%s",
                ret,
                (unsigned int)view->ino,
                pathname != NULL ? pathname : "(null)"
            );
            errno = ret;
            return -1;
        }
    }

    handle->fs_manager = fs_manager;
    handle->file_inode = file_inode;
    handle->oflag = oflag;
    iop->data1 = handle;

    if ((oflag & O_APPEND) != 0) {
        iop->offset = (off_t)rtfsFileInodeGetSize(file_inode);
    }

    return 0;
}

int rtfsFileClose(rtems_libio_t *iop)
{
    RtfsFileHandle *handle;
    int ret = 0;

    if (iop == NULL) {
        errno = EINVAL;
        return -1;
    }

    handle = (RtfsFileHandle *)iop->data1;
    if (handle == NULL) {
        return 0;
    }

    if (handle->file_inode != NULL && handle->fs_manager != NULL) {
        if (!handle->sync_failed) {
            ret = rtfsFileInodeCommitCowWriteback(
                handle->fs_manager,
                handle->file_inode
            );
        }
    }

    if (handle->file_inode != NULL) {
        rtfsFileInodePut(handle->file_inode);
    }

    free(handle);
    iop->data1 = NULL;

    if (ret != 0) {
        errno = ret;
        return -1;
    }

    return 0;
}

ssize_t rtfsFileRead(rtems_libio_t *iop, void *buffer, size_t count)
{
    RtfsFileHandle *handle;
    off_t offset;
    ssize_t bytes_read;
    int ret;

    if (buffer == NULL && count != 0) {
        errno = EINVAL;
        return -1;
    }

    ret = rtfsFileGetHandle(iop, &handle);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    if ((handle->oflag & O_ACCMODE) == O_WRONLY) {
        errno = EBADF;
        return -1;
    }

    offset = iop->offset;
    bytes_read = rtfsFileInodeRead(
        handle->fs_manager,
        handle->file_inode,
        &offset,
        buffer,
        count
    );
    if (bytes_read < 0) {
        return -1;
    }

    iop->offset = offset;
    return bytes_read;
}

ssize_t rtfsFileWrite(rtems_libio_t *iop, const void *buffer, size_t count)
{
    RtfsFileHandle *handle;
    off_t offset;
    ssize_t bytes_written;
    int ret;

    if (buffer == NULL && count != 0) {
        errno = EINVAL;
        return -1;
    }

    ret = rtfsFileGetHandle(iop, &handle);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    if ((handle->oflag & O_ACCMODE) == O_RDONLY) {
        errno = EBADF;
        return -1;
    }

    if ((handle->oflag & O_APPEND) != 0) {
        offset = (off_t)rtfsFileInodeGetSize(handle->file_inode);
    } else {
        offset = iop->offset;
    }

    bytes_written = rtfsFileInodeWrite(
        handle->fs_manager,
        handle->file_inode,
        &offset,
        buffer,
        count
    );
    if (bytes_written < 0) {
        return -1;
    }

    iop->offset = offset;
    return bytes_written;
}

off_t rtfsFileLseek(rtems_libio_t *iop, off_t offset, int whence)
{
    RtfsFileHandle *handle;
    off_t base;
    off_t new_offset;
    int ret;

    ret = rtfsFileGetHandle(iop, &handle);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = iop->offset;
        break;
    case SEEK_END:
        base = (off_t)rtfsFileInodeGetSize(handle->file_inode);
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    ret = rtfsFileComputeSeekOffset(base, offset, &new_offset);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    iop->offset = new_offset;
    return new_offset;
}

int rtfsFileFtruncate(rtems_libio_t *iop, off_t length)
{
    RtfsFileHandle *handle;
    int ret;

    if (length < 0) {
        errno = EINVAL;
        return -1;
    }

    ret = rtfsFileGetHandle(iop, &handle);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    if ((handle->oflag & O_ACCMODE) == O_RDONLY) {
        errno = EBADF;
        return -1;
    }

    ret = rtfsFileInodeTruncate(
        handle->fs_manager,
        handle->file_inode,
        (uint64_t)length
    );
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    return 0;
}

int rtfsFileFdatasync(rtems_libio_t *iop)
{
    RtfsFileHandle *handle;
    int ret;

    ret = rtfsFileGetHandle(iop, &handle);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    ret = rtfsFileInodeFdatasync(
        handle->fs_manager,
        handle->file_inode
    );
    if (ret != 0) {
        handle->sync_failed = true;
        errno = ret;
        return -1;
    }

    handle->sync_failed = false;
    return 0;
}

int rtfsFileFsync(rtems_libio_t *iop)
{
    RtfsFileHandle *handle;
    int ret;

    ret = rtfsFileGetHandle(iop, &handle);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    ret = rtfsFileInodeCommitCowWriteback(
        handle->fs_manager,
        handle->file_inode
    );
    if (ret != 0) {
        handle->sync_failed = true;
        errno = ret;
        return -1;
    }

    handle->sync_failed = false;
    return 0;
}

int rtfsFileFstat(const rtems_filesystem_location_info_t *pathloc, struct stat *buf)
{
    RtfsRuntimeInodeView *view;
    struct RtfsInode *disk_inode;
    NodeBlockCacheEntryHandle handle = {
        .cache = NULL,
        .entry = NULL
    };
    mode_t mode_bits;
    int ret;

    if (pathloc == NULL || buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    ret = rtfsFileValidatePathloc(pathloc, &view);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    ret = rtfsFileGetDiskInode(pathloc, &disk_inode, &handle);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->st_ino = view->ino;
    mode_bits = (mode_t)disk_inode->i_mode;
    if (mode_bits == 0) {
        mode_bits = 0644;
    }
    buf->st_mode = S_IFREG | mode_bits;
    buf->st_nlink = disk_inode->i_nlink != 0 ? disk_inode->i_nlink : 1;
    buf->st_size = (off_t)disk_inode->i_size;
    buf->st_blocks = (blkcnt_t)SIZE_TO_BLOCK(disk_inode->i_size);
    buf->st_blksize = 4096;
    buf->st_atime = (time_t)disk_inode->i_atime;
    buf->st_mtime = (time_t)disk_inode->i_mtime;
    buf->st_ctime = (time_t)disk_inode->i_mtime;

    nodeBlockCacheEntryHandleDestroy(&handle);

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
    .fsync_h = rtfsFileFsync,
    .fdatasync_h = rtfsFileFdatasync,
    .fcntl_h = rtems_filesystem_default_fcntl,
    .poll_h = rtems_filesystem_default_poll,
    .kqfilter_h = rtems_filesystem_default_kqfilter,
    .readv_h = rtems_filesystem_default_readv,
    .writev_h = rtems_filesystem_default_writev,
    .mmap_h = rtems_filesystem_default_mmap,
};
