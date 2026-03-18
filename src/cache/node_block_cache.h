#ifndef _NODE_BLOCK_CACHE_H_
#define _NODE_BLOCK_CACHE_H_

#include "cache/generic_cache_manager.h"
#include "cache/block_buffer.h"
#include "fs/fs.h"


struct comm_dev;


typedef enum NodeBlockCacheEntryState
{
    NODE_BLOCK_CACHE_ENTRY_UPTODATE,
    NODE_BLOCK_CACHE_ENTRY_DIRTY,
    NODE_BLOCK_CACHE_ENTRY_DELETED,
} NodeBlockCacheEntryState;


#endif
