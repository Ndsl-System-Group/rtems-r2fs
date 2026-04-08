#ifndef _DIR_INODE_H_
#define _DIR_INODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>

#include "fs.h"
#include "inode.h"

typedef struct file_system_manager file_system_manager;
typedef struct RtfsDirInode RtfsDirInode;
typedef struct RtfsDirInodeCache RtfsDirInodeCache;

typedef struct RtfsDirLookupResult
{
    RtfsRuntimeInodeView inode_view;
} RtfsDirLookupResult;

RtfsDirInode *rtfsDirInodeGet(RtfsDirInodeCache *cache, rtfs_ino ino);

void rtfsDirInodePut(RtfsDirInode *dir_inode);

int rtfsDirInodeLookup(
    const RtfsDirInode *dir_inode,
    const char *name,
    size_t namelen,
    RtfsDirLookupResult *result
);

ssize_t rtfsDirInodeReadEntries(
    const RtfsDirInode *dir_inode,
    off_t *offset,
    void *buffer,
    size_t count
);

int rtfsDirInodeAddEntry(
    RtfsDirInode *dir_inode,
    const char *name,
    const RtfsRuntimeInodeView *child_view
);

int rtfsDirInodeRemoveEntry(
    RtfsDirInode *dir_inode,
    const char *name
);

#endif
