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


typedef struct NodeBlockCacheEntry
{
    uint32_t nid;
    uint32_t parentNid;
    uint32_t lpa;

    BlockBuffer node;

    uint32_t refCount;
    NodeBlockCacheEntryState state;
} NodeBlockCacheEntry;


void nodeBlockCacheEntryInit(NodeBlockCacheEntry *this, BlockBuffer *buffer, uint32_t nid, uint32_t parentNid, uint32_t lpa);

void nodeBlockCacheEntryDestroy(NodeBlockCacheEntry *this);

uint32_t nodeBlockCacheEntryGetLpa(NodeBlockCacheEntry *this);

void nodeBlockCacheEntrySetLpa(NodeBlockCacheEntry *this, uint32_t lpa);

NodeBlockCacheEntryState nodeBlockCacheEntryGetState(NodeBlockCacheEntry *this);

void nodeBlockCacheEntrySetState(NodeBlockCacheEntry *this, NodeBlockCacheEntryState state);

struct RtfsNode *nodeBlockCacheEntryGetNodeBlockPtr(NodeBlockCacheEntry *this);

BlockBuffer *nodeBlockCacheEntryGetNodeBuffer(NodeBlockCacheEntry *this);

uint32_t nodeBlockCacheEntryGetNid(NodeBlockCacheEntry *this);


#endif
