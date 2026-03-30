#include "node_block_cache.h"


void nodeBlockCacheEntryInit(NodeBlockCacheEntry *this, BlockBuffer *buffer, uint32_t nid, uint32_t parentNid, uint32_t lpa)
{
    blockBufferInit(&this->node);
    blockBufferCopy(&this->node, buffer);

    this->nid = nid;
    this->parentNid = parentNid;
    this->lpa = lpa;
    this->refCount = 0;
    this->state = NODE_BLOCK_CACHE_ENTRY_UPTODATE;
}

void nodeBlockCacheEntryDestroy(NodeBlockCacheEntry *this)
{
    this->state = NODE_BLOCK_CACHE_ENTRY_DELETED;
    this->refCount = 0;
    this->lpa = 0;
    this->parentNid = 0;
    this->nid = 0;

    blockBufferDestroy(&this->node);
}

uint32_t nodeBlockCacheEntryGetLpa(NodeBlockCacheEntry *this)
{
    return this->lpa;
}

void nodeBlockCacheEntrySetLpa(NodeBlockCacheEntry *this, uint32_t lpa)
{
    this->lpa = lpa;
}

NodeBlockCacheEntryState nodeBlockCacheEntryGetState(NodeBlockCacheEntry *this)
{
    return this->state;
}

void nodeBlockCacheEntrySetState(NodeBlockCacheEntry *this, NodeBlockCacheEntryState state)
{
    this->state = state;
}

struct RtfsNode *nodeBlockCacheEntryGetNodeBlockPtr(NodeBlockCacheEntry *this)
{
    return (struct RtfsNode *)blockBufferGetPtr(&this->node);
}

BlockBuffer *nodeBlockCacheEntryGetNodeBuffer(NodeBlockCacheEntry *this)
{
    return &this->node;
}

uint32_t nodeBlockCacheEntryGetNid(NodeBlockCacheEntry *this)
{
    return this->nid;
}
