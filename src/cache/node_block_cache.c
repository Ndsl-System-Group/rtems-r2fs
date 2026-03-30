#include "node_block_cache.h"


static void nodeBlockCacheEntryHandleDoAddRef(NodeBlockCacheEntryHandle *this);

static void nodeBlockCacheEntryHandleDoSubRef(NodeBlockCacheEntryHandle *this);


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


void nodeBlockCacheEntryHandleInit(NodeBlockCacheEntryHandle *this, struct NodeBlockCache *cache, NodeBlockCacheEntry *entry)
{
    this->cache = cache;
    this->entry = entry;
}

// TODO
void nodeBlockCacheEntryHandleDestroy(NodeBlockCacheEntryHandle *this)
{
    // if (entry != nullptr)
    // {
    //     try
    //     {
    //         cache->sub_refcount(entry);
    //     }
    //     catch(const std::exception &e)
    //     {
    //         HSCFS_LOG(HSCFS_LOG_WARNING, "exception during sub_refcount of node block cache entry: "
    //                                      "%s",
    //                   e.what());
    //     }
    // }
}

void nodeBlockCacheEntryHandleCopy(NodeBlockCacheEntryHandle *this, const NodeBlockCacheEntryHandle *other)
{
    this->cache = other->cache;
    this->entry = other->entry;

    // do_addref();
}

bool nodeBlockCacheEntryHandleIsEmpty(NodeBlockCacheEntryHandle *this)
{
    return NULL == this->entry;
}

void nodeBlockCacheEntryHandleAddHostVersion(NodeBlockCacheEntryHandle *this)
{
    // cache->add_refcount(entry);
}

void nodeBlockCacheEntryHandleAddSsdVersion(NodeBlockCacheEntryHandle *this)
{
    // cache->sub_refcount(entry);
}

void nodeBlockCacheEntryHandleMarkDirty(NodeBlockCacheEntryHandle *this)
{
    // cache->mark_dirty(*this);
}

void nodeBlockCacheEntryHandleDeleteNode(NodeBlockCacheEntryHandle *this)
{
    // cache->remove_entry(entry);
}

NodeBlockCacheEntry *nodeBlockCacheEntryHandleGetEntry(NodeBlockCacheEntryHandle *this)
{
    return this->entry;
}


void nodeBlockCacheEntryHandleDoAddRef(NodeBlockCacheEntryHandle *this)
{
    // if (entry != nullptr) cache->add_refcount(entry);
}

void nodeBlockCacheEntryHandleDoSubRef(NodeBlockCacheEntryHandle *this)
{
    // if (entry != nullptr) cache->sub_refcount(entry);
}
