#include "node_block_cache.h"

#include "fs/nat_utils.h"
#include "fs/fs_manager.h"
#include "fs/super_manager.h"
#include "utils/rtfs_exception.h"
#include "utils/rtfs_log.h"

#include <assert.h>


static void nodeBlockCacheEntryHandleDoAddRef(NodeBlockCacheEntryHandle *this);

static void nodeBlockCacheEntryHandleDoSubRef(NodeBlockCacheEntryHandle *this);

static void nodeBlockCacheAddRefCount(NodeBlockCache *this, NodeBlockCacheEntry *entry);

static void nodeBlockCacheSubRefCount(NodeBlockCache *this, NodeBlockCacheEntry *entry);

static void nodeBlockCacheMarkDirty(NodeBlockCache *this, const NodeBlockCacheEntryHandle *handle);

static void nodeBlockCacheRemoveEntry(NodeBlockCache *this, NodeBlockCacheEntry *entry);

static void nodeBlockCacheDoReplace(NodeBlockCache *this);


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

void nodeBlockCacheEntryHandleDestroy(NodeBlockCacheEntryHandle *this)
{
    CEXCEPTION_T e;

    if (NULL != this->entry)
    {
        Try
        {
            nodeBlockCacheSubRefCount(this->cache, this->entry);
        }
        Catch(e)
        {
            RTFS_LOG(RTFS_LOG_WARNING, "exception during sub_refcount of node block cache entry: %d", e);
        }
    }
}

void nodeBlockCacheEntryHandleCopy(NodeBlockCacheEntryHandle *this, const NodeBlockCacheEntryHandle *other)
{
    this->cache = other->cache;
    this->entry = other->entry;

    nodeBlockCacheEntryHandleDoAddRef(this);
}

bool nodeBlockCacheEntryHandleIsEmpty(NodeBlockCacheEntryHandle *this)
{
    return NULL == this->entry;
}

void nodeBlockCacheEntryHandleAddHostVersion(NodeBlockCacheEntryHandle *this)
{
    nodeBlockCacheAddRefCount(this->cache, this->entry);
}

void nodeBlockCacheEntryHandleAddSsdVersion(NodeBlockCacheEntryHandle *this)
{
    nodeBlockCacheSubRefCount(this->cache, this->entry);
}

void nodeBlockCacheEntryHandleMarkDirty(NodeBlockCacheEntryHandle *this)
{
    nodeBlockCacheMarkDirty(this->cache, this);
}

void nodeBlockCacheEntryHandleDeleteNode(NodeBlockCacheEntryHandle *this)
{
    nodeBlockCacheRemoveEntry(this->cache, this->entry);
}


// TODO
void nodeBlockCacheInit(NodeBlockCache *this, struct file_system_manager *fsManager, size_t expectSize)
{
}

void nodeBlockCacheDestroy(NodeBlockCache *this)
{
}

NodeBlockCacheEntryHandle nodeBlockCacheAdd(NodeBlockCache *this, BlockBuffer *buffer, uint32_t nid, uint32_t parentNid, uint32_t lpa)
{
}

NodeBlockCacheEntryHandle nodeBlockCacheGet(NodeBlockCache *this, uint32_t nid)
{
}

NodeBlockCacheDirtyNode *nodeBlockCacheGetAndClearDirtyList(NodeBlockCache *this)
{
}

void nodeBlockCacheForceReplace(NodeBlockCache *this)
{
}


void nodeBlockCacheHelperInit(NodeBlockCacheHelper *this, struct file_system_manager *fsManager)
{
    this->fsManager = fsManager;
    this->natCache = fileSystemManagerGetNatCache(fsManager);
    this->nodeBlockCache = fileSystemManagerGetNodeCache(fsManager);
    // TODO
    // dev = fs_manager->get_device();
}

void nodeBlockCacheHelperDestroy(NodeBlockCacheHelper *this)
{
    this->dev = NULL;
    this->natCache = NULL;
    this->nodeBlockCache = NULL;
    this->fsManager = NULL;
}

NodeBlockCacheEntryHandle nodeBlockCacheHelperGetNodeEntry(NodeBlockCacheHelper *this, uint32_t nid, uint32_t parentNid)
{
    CEXCEPTION_T e;

    NodeBlockCacheEntryHandle handle = nodeBlockCacheGet(this->nodeBlockCache, nid);
    if (nodeBlockCacheEntryHandleIsEmpty(&handle))
    {
        NatLpaMapping nlp;
        natLpaMappingInit(&nlp, this->fsManager);

        // 从 NAT 表中得到 nid block 的 lpa。
        uint32_t nidLpa = natGetLpaOfNid(&nlp, nid);

        BlockBuffer buffer;
        blockBufferInit(&buffer);

        Try
        {
            blockBufferReadFromLpa(&buffer, this->dev, nidLpa);
        }
        Catch(e)
        {
            THROW_FATAL_MESSAGE(e, "node cache helper: read lpa %u failed.", nidLpa);
        }

        // node_handle = node_cache->add(std::move(buf), nid, parent_nid, nid_lpa);
        handle = nodeBlockCacheAdd(this->nodeBlockCache, &buffer, nid, parentNid, nidLpa);

        blockBufferDestroy(&buffer);
    }

    struct RtfsNode *node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    assert(node->footer.nid == nid);
    if (INVALID_NID == parentNid) assert(node->footer.ino == nid);


    return handle;
}

NodeBlockCacheEntryHandle nodeBlockCacheHelperCreateNodeEntry(NodeBlockCacheHelper *this, uint32_t ino, uint32_t noffset, uint32_t parentNid)
{
    // 分配 nid，创建 node block 缓存项并加入缓存。
    uint32_t newNid = superManagerAllocNid(fileSystemManagerGetSuperManager(this->fsManager), ino, false);

    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntryHandle handle = nodeBlockCacheAdd(this->nodeBlockCache, &buffer, newNid, parentNid, INVALID_LPA);

    struct RtfsNode *node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);

    // 初始化 node footer。
    struct NodeFooter *footer = &node->footer;
    footer->ino = ino;
    footer->nid = newNid;
    footer->offset = noffset;

    // 标记缓存项为 dirty。
    nodeBlockCacheEntryHandleMarkDirty(&handle);

    blockBufferDestroy(&buffer);


    return handle;
}

NodeBlockCacheEntryHandle nodeBlockCacheHelperCreateInodeEntry(NodeBlockCacheHelper *this)
{
    // 分配 nid，创建 inode block 缓存项并加入缓存。
    uint32_t newNid = superManagerAllocNid(fileSystemManagerGetSuperManager(this->fsManager), INVALID_NID, true);

    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntryHandle handle = nodeBlockCacheAdd(this->nodeBlockCache, &buffer, newNid, INVALID_NID, INVALID_LPA);

    struct RtfsNode *node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);

    // 初始化 node footer。
    struct NodeFooter *footer = &node->footer;
    footer->ino = newNid;
    footer->nid = newNid;
    footer->offset = 0;

    // 标记缓存项为 dirty。
    nodeBlockCacheEntryHandleMarkDirty(&handle);

    blockBufferDestroy(&buffer);


    return handle;
}


void nodeBlockCacheEntryHandleDoAddRef(NodeBlockCacheEntryHandle *this)
{
    if (NULL != this->entry) nodeBlockCacheAddRefCount(this->cache, this->entry);
}

void nodeBlockCacheEntryHandleDoSubRef(NodeBlockCacheEntryHandle *this)
{
    if (NULL != this->entry) nodeBlockCacheSubRefCount(this->cache, this->entry);
}

// TODO
void nodeBlockCacheAddRefCount(NodeBlockCache *this, NodeBlockCacheEntry *entry)
{
}

void nodeBlockCacheSubRefCount(NodeBlockCache *this, NodeBlockCacheEntry *entry)
{
}

void nodeBlockCacheMarkDirty(NodeBlockCache *this, const NodeBlockCacheEntryHandle *handle)
{
}

void nodeBlockCacheRemoveEntry(NodeBlockCache *this, NodeBlockCacheEntry *entry)
{
}

void nodeBlockCacheDoReplace(NodeBlockCache *this)
{
}
