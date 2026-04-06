#include "dir_inode.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define RTFS_DIR_STATIC_ENTRY_COUNT 2

typedef struct RtfsMemDirEntry
{
    rtfs_ino ino;
    uint8_t file_type;
    char name[RTFS_NAME_LEN + 1];
} RtfsMemDirEntry;

typedef struct RtfsDirInode
{
    rtfs_ino ino;
    rtfs_ino parent_ino;
    uint8_t file_type;

    uint64_t i_size;
    uint32_t i_dentry_num;
    uint64_t i_mtime;
    uint32_t i_current_depth;

    size_t entry_count;
    size_t capacity;
    RtfsMemDirEntry *entries;

    void *cache_handle;

    bool is_dirty;
} RtfsDirInode;

struct RtfsDirInodeCache
{
    void *opaque;
};

static RtfsDirInode *rtfsCreateDirInode(rtfs_ino ino, rtfs_ino parent_ino);
static void rtfsDestroyDirInode(RtfsDirInode *dir_inode);
static int rtfsDirInodeLoadFromCache(
    RtfsDirInode *dir_inode,
    RtfsDirInodeCache *cache,
    rtfs_ino ino
);
static void rtfsDirBuildViewFromDirInode(
    const RtfsDirInode *dir_inode,
    RtfsRuntimeInodeView *view
);
static int rtfsDirFillDirent(
    struct dirent *entry,
    const RtfsRuntimeInodeView *inode_view,
    const char *name
);
static int rtfsDirGetEntryByIndex(
    const RtfsDirInode *dir_inode,
    size_t index,
    struct dirent *entry
);
static const RtfsMemDirEntry *rtfsDirInodeFindEntry(
    const RtfsDirInode *dir_inode,
    const char *name,
    size_t namelen
);
static int rtfsDirInodeAddEntryInternal(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
);
static int rtfsDirInodeRemoveEntryInternal(
    RtfsDirInode *dir_inode,
    const char *name
);

RtfsDirInode *rtfsDirInodeGet(RtfsDirInodeCache *cache, rtfs_ino ino)
{
    RtfsDirInode *dir_inode = rtfsCreateDirInode(ino, ino);
    if (dir_inode == NULL) {
        return NULL;
    }

    if (rtfsDirInodeLoadFromCache(dir_inode, cache, ino) != 0) {
        rtfsDestroyDirInode(dir_inode);
        return NULL;
    }

    return dir_inode;
}

void rtfsDirInodePut(RtfsDirInode *dir_inode)
{
    rtfsDestroyDirInode(dir_inode);
}

int rtfsDirInodeLookup(
    const RtfsDirInode *dir_inode,
    const char *name,
    size_t namelen,
    RtfsDirLookupResult *result
)
{
    size_t i;

    if (dir_inode == NULL) {
        return ENOTDIR;
    }

    if (name == NULL || result == NULL) {
        return EINVAL;
    }

    if (namelen == 1 && name[0] == '.') {
        rtfsDirBuildViewFromDirInode(dir_inode, &result->inode_view);
        return 0;
    }

    if (namelen == 2 && name[0] == '.' && name[1] == '.') {
        rtfsRuntimeInodeViewInit(
            &result->inode_view,
            dir_inode->parent_ino,
            dir_inode->parent_ino,
            RTFS_FT_DIR
        );
        return 0;
    }

    for (i = 0; i < dir_inode->entry_count; ++i) {
        if (strlen(dir_inode->entries[i].name) == namelen &&
            memcmp(dir_inode->entries[i].name, name, namelen) == 0) {
            rtfsRuntimeInodeViewInit(
                &result->inode_view,
                dir_inode->entries[i].ino,
                dir_inode->ino,
                dir_inode->entries[i].file_type
            );
            return 0;
        }
    }

    return ENOENT;
}

ssize_t rtfsDirInodeReadEntries(
    const RtfsDirInode *dir_inode,
    off_t *offset,
    void *buffer,
    size_t count
)
{
    size_t entry_index;
    size_t bytes_read;
    struct dirent current;

    if (dir_inode == NULL) {
        errno = ENOTDIR;
        return -1;
    }

    if (offset == NULL || buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    entry_index = (size_t)(*offset / (off_t)sizeof(struct dirent));
    count = (count / sizeof(struct dirent)) * sizeof(struct dirent);
    bytes_read = 0;

    while (count >= sizeof(struct dirent)) {
        int ret = rtfsDirGetEntryByIndex(dir_inode, entry_index, &current);
        if (ret == ENOENT) {
            break;
        }
        if (ret != 0) {
            errno = ret;
            return -1;
        }

        current.d_off = (off_t)((entry_index + 1) * sizeof(struct dirent));
        memcpy((char *)buffer + bytes_read, &current, sizeof(current));

        ++entry_index;
        bytes_read += sizeof(struct dirent);
        count -= sizeof(struct dirent);
    }

    *offset = (off_t)(entry_index * sizeof(struct dirent));
    return (ssize_t)bytes_read;
}

int rtfsDirInodeAddEntry(
    RtfsDirInode *dir_inode,
    const char *name,
    const RtfsRuntimeInodeView *child_view
)
{
    if (dir_inode == NULL || name == NULL || child_view == NULL) {
        return EINVAL;
    }

    return rtfsDirInodeAddEntryInternal(
        dir_inode,
        name,
        child_view->ino,
        child_view->file_type
    );
}

int rtfsDirInodeRemoveEntry(
    RtfsDirInode *dir_inode,
    const char *name
)
{
    if (dir_inode == NULL || name == NULL) {
        return EINVAL;
    }

    return rtfsDirInodeRemoveEntryInternal(dir_inode, name);
}

static RtfsDirInode *rtfsCreateDirInode(rtfs_ino ino, rtfs_ino parent_ino)
{
    RtfsDirInode *dir_inode = malloc(sizeof(*dir_inode));
    if (dir_inode == NULL) {
        return NULL;
    }

    memset(dir_inode, 0, sizeof(*dir_inode));
    dir_inode->ino = ino;
    dir_inode->parent_ino = parent_ino;
    dir_inode->file_type = RTFS_FT_DIR;

    return dir_inode;
}

static void rtfsDestroyDirInode(RtfsDirInode *dir_inode)
{
    if (dir_inode == NULL) {
        return;
    }

    free(dir_inode->entries);
    dir_inode->entries = NULL;
    dir_inode->cache_handle = NULL;

    free(dir_inode);
}

static int rtfsDirInodeLoadFromCache(
    RtfsDirInode *dir_inode,
    RtfsDirInodeCache *cache,
    rtfs_ino ino
)
{
    (void)cache;
    (void)ino;

    if (dir_inode == NULL) {
        return EINVAL;
    }

    // TODO: 等 dir_cache / block_cache 接口明确后，在这里完成真正的转换装载。
    // 当前先保留一个可编译的转换入口，dir_inode 的实际内容仍为空模型。
    dir_inode->cache_handle = NULL;
    dir_inode->is_dirty = false;
    return 0;
}

static void rtfsDirBuildViewFromDirInode(
    const RtfsDirInode *dir_inode,
    RtfsRuntimeInodeView *view
)
{
    if (dir_inode == NULL || view == NULL) {
        return;
    }

    rtfsRuntimeInodeViewInit(
        view,
        dir_inode->ino,
        dir_inode->parent_ino,
        dir_inode->file_type
    );
}

static int rtfsDirFillDirent(
    struct dirent *entry,
    const RtfsRuntimeInodeView *inode_view,
    const char *name
)
{
    size_t namelen;

    if (entry == NULL || inode_view == NULL || name == NULL) {
        return EINVAL;
    }

    namelen = strnlen(name, RTFS_NAME_LEN + 1);
    if (namelen == 0 || namelen > RTFS_NAME_LEN) {
        return EINVAL;
    }

    memset(entry, 0, sizeof(*entry));
    entry->d_ino = (ino_t)inode_view->ino;
    entry->d_reclen = sizeof(*entry);
    entry->d_namlen = namelen;
#ifdef DT_DIR
    entry->d_type = rtfsInodeIsDirectoryType(inode_view->file_type) ? DT_DIR : DT_REG;
#else
    (void)inode_view;
#endif
    memcpy(entry->d_name, name, namelen);
    entry->d_name[namelen] = '\0';

    return 0;
}

static int rtfsDirGetEntryByIndex(
    const RtfsDirInode *dir_inode,
    size_t index,
    struct dirent *entry
)
{
    RtfsRuntimeInodeView inode_view;

    if (dir_inode == NULL || entry == NULL) {
        return EINVAL;
    }

    rtfsDirBuildViewFromDirInode(dir_inode, &inode_view);

    if (index == 0) {
        return rtfsDirFillDirent(entry, &inode_view, ".");
    }

    if (index == 1) {
        RtfsRuntimeInodeView parent_view;

        rtfsRuntimeInodeViewInit(
            &parent_view,
            dir_inode->parent_ino,
            dir_inode->parent_ino,
            RTFS_FT_DIR
        );
        return rtfsDirFillDirent(entry, &parent_view, "..");
    }

    index -= RTFS_DIR_STATIC_ENTRY_COUNT;
    if (index >= dir_inode->entry_count) {
        return ENOENT;
    }

    return rtfsDirFillDirent(
        entry,
        &(RtfsRuntimeInodeView){
            .ino = dir_inode->entries[index].ino,
            .parent_ino = dir_inode->ino,
            .file_type = dir_inode->entries[index].file_type,
        },
        dir_inode->entries[index].name
    );
}

static const RtfsMemDirEntry *rtfsDirInodeFindEntry(
    const RtfsDirInode *dir_inode,
    const char *name,
    size_t namelen
)
{
    size_t i;

    if (dir_inode == NULL || name == NULL) {
        return NULL;
    }

    for (i = 0; i < dir_inode->entry_count; ++i) {
        if (strlen(dir_inode->entries[i].name) == namelen &&
            memcmp(dir_inode->entries[i].name, name, namelen) == 0) {
            return &dir_inode->entries[i];
        }
    }

    return NULL;
}

static int rtfsDirInodeAddEntryInternal(
    RtfsDirInode *dir_inode,
    const char *name,
    rtfs_ino child_ino,
    uint8_t child_type
)
{
    RtfsMemDirEntry *new_entries;
    size_t namelen;

    if (dir_inode == NULL || name == NULL) {
        return EINVAL;
    }

    namelen = strnlen(name, RTFS_NAME_LEN + 1);
    if (namelen == 0 || namelen > RTFS_NAME_LEN) {
        return ENAMETOOLONG;
    }

    if (rtfsDirInodeFindEntry(dir_inode, name, namelen) != NULL) {
        return EEXIST;
    }

    if (dir_inode->entry_count == dir_inode->capacity) {
        size_t new_capacity = dir_inode->capacity == 0 ? 4 : dir_inode->capacity * 2;
        new_entries = realloc(dir_inode->entries, new_capacity * sizeof(*new_entries));
        if (new_entries == NULL) {
            return ENOMEM;
        }
        dir_inode->entries = new_entries;
        dir_inode->capacity = new_capacity;
    }

    memset(&dir_inode->entries[dir_inode->entry_count], 0,
           sizeof(dir_inode->entries[dir_inode->entry_count]));
    dir_inode->entries[dir_inode->entry_count].ino = child_ino;
    dir_inode->entries[dir_inode->entry_count].file_type = child_type;
    memcpy(dir_inode->entries[dir_inode->entry_count].name, name, namelen);
    dir_inode->entries[dir_inode->entry_count].name[namelen] = '\0';

    dir_inode->entry_count++;
    dir_inode->i_dentry_num++;
    dir_inode->is_dirty = true;

    return 0;
}

static int rtfsDirInodeRemoveEntryInternal(
    RtfsDirInode *dir_inode,
    const char *name
)
{
    size_t i;
    size_t namelen;

    if (dir_inode == NULL || name == NULL) {
        return EINVAL;
    }

    namelen = strnlen(name, RTFS_NAME_LEN + 1);
    if (namelen == 0 || namelen > RTFS_NAME_LEN) {
        return ENAMETOOLONG;
    }

    for (i = 0; i < dir_inode->entry_count; ++i) {
        if (strlen(dir_inode->entries[i].name) == namelen &&
            memcmp(dir_inode->entries[i].name, name, namelen) == 0) {
            if (i + 1 < dir_inode->entry_count) {
                dir_inode->entries[i] = dir_inode->entries[dir_inode->entry_count - 1];
            }

            dir_inode->entry_count--;
            if (dir_inode->i_dentry_num > 0) {
                dir_inode->i_dentry_num--;
            }
            dir_inode->is_dirty = true;
            return 0;
        }
    }

    return ENOENT;
}
