#include "node_block_cache.h"

#include "fs/nat_utils.h"
#include "utils/rtfs_exception.h"
#include "utils/rtfs_log.h"

#include <assert.h>


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

void nodeBlockCacheEntryHandleDestroy(NodeBlockCacheEntryHandle *this)
{
    // TODO
    // if (entry != nullptr)
    // {
    //     try
    //     {
    //         cache->sub_refcount(entry);
    //     }
    //     catch(const std::exception &e)
    //     {
    //         RTFS_LOG(RTFS_LOG_WARNING, "exception during sub_refcount of node block cache entry: "
    //                                      "%s",
    //                   e.what());
    //     }
    // }
}

void nodeBlockCacheEntryHandleCopy(NodeBlockCacheEntryHandle *this, const NodeBlockCacheEntryHandle *other)
{
    this->cache = other->cache;
    this->entry = other->entry;

    // TODO
    // do_addref();
}

bool nodeBlockCacheEntryHandleIsEmpty(NodeBlockCacheEntryHandle *this)
{
    return NULL == this->entry;
}

// TODO
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


void nodeBlockCacheHelperInit(NodeBlockCacheHelper *this, struct file_system_manager *fsManager)
{
    // TODO
    // dev = fs_manager->get_device();
    // nat_cache = fs_manager->get_nat_cache();
    // node_cache = fs_manager->get_node_cache();
    // this->fs_manager = fs_manager;
}

void nodeBlockCacheHelperDestroy(NodeBlockCacheHelper *this)
{
    this->dev = NULL;
    this->natCache = NULL;
    this->nodeBlockCache = NULL;
    this->fs_manager = NULL;
}

NodeBlockCacheEntryHandle nodeBlockCacheHelperGetNodeEntry(NodeBlockCacheHelper *this, uint32_t nid, uint32_t parentNid)
{
    CEXCEPTION_T e;

    // TODO
    // NodeBlockCacheEntryHandle handle = nodeBlockCacheGet(nid);
    NodeBlockCacheEntryHandle handle;
    if (nodeBlockCacheEntryHandleIsEmpty(&handle))
    {
        NatLpaMapping nlp;
        natLpaMappingInit(&nlp, this->fs_manager);

        // 从 NAT 表中得到 nid block 的 lpa。
        uint32_t nidLpa = natGetLpaOfNid(&nlp, nid);

        BlockBuffer buf;
        blockBufferInit(&buf);

        Try
        {
            blockBufferReadFromLpa(&buf, this->dev, nidLpa);
        }
        Catch(e)
        {
            THROW_FATAL_MESSAGE(e, "node cache helper: read lpa %u failed.", nidLpa);
        }

        // TODO
        // node_handle = node_cache->add(std::move(buf), nid, parent_nid, nid_lpa);
    }

    struct RtfsNode *node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    assert(node->footer.nid == nid);
    if (INVALID_NID == parentNid) assert(node->footer.ino == nid);


    return handle;
}

NodeBlockCacheEntryHandle nodeBlockCacheHelperCreateNodeEntry(NodeBlockCacheHelper *this, uint32_t ino, uint32_t noffset, uint32_t parentNid)
{
    // TODO
    // 分配 nid，创建 node block 缓存项并加入缓存。
    // uint32_t new_nid = fs_manager->get_super_manager()->alloc_nid(ino);
    // auto handle = node_cache->add(block_buffer(), new_nid, parent_nid, INVALID_LPA);

    uint32_t newNid = 0;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);

    // 初始化 node footer。
    struct NodeFooter *footer = &node->footer;
    footer->ino = ino;
    footer->nid = newNid;
    footer->offset = noffset;

    // 标记缓存项为 dirty。
    nodeBlockCacheEntryHandleMarkDirty(&handle);


    return handle;
}

NodeBlockCacheEntryHandle nodeBlockCacheHelperCreateInodeEntry(NodeBlockCacheHelper *this)
{
    // TODO
    // 分配 nid，创建 inode block 缓存项并加入缓存。
    // uint32_t new_nid = fs_manager->get_super_manager()->alloc_nid(INVALID_NID, true);
    // auto handle = node_cache->add(block_buffer(), new_nid, INVALID_NID, INVALID_LPA);

    uint32_t newNid = 0;
    NodeBlockCacheEntryHandle handle;
    struct RtfsNode *node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);

    // 初始化 node footer。
    struct NodeFooter *footer = &node->footer;
    footer->ino = newNid;
    footer->nid = newNid;
    footer->offset = 0;

    // 标记缓存项为 dirty。
    nodeBlockCacheEntryHandleMarkDirty(&handle);


    return handle;
}


void nodeBlockCacheEntryHandleDoAddRef(NodeBlockCacheEntryHandle *this)
{
    // TODO
    // if (entry != nullptr) cache->add_refcount(entry);
}

void nodeBlockCacheEntryHandleDoSubRef(NodeBlockCacheEntryHandle *this)
{
    // TODO
    // if (entry != nullptr) cache->sub_refcount(entry);
}
