#include "node_block_cache.h"

#include "fs/nat_utils.h"
#include "fs/sit_utils.h"
#include "fs/fs_manager.h"
#include "fs/srmap_utils.h"
#include "fs/super_manager.h"
#include "utils/rtfs_exception.h"
#include "utils/rtfs_log.h"
#include "uthash/utlist.h"

#include <assert.h>
#include <errno.h>


static void nodeBlockCacheEntryHandleDoAddRef(NodeBlockCacheEntryHandle *this);

static void nodeBlockCacheEntryHandleDoSubRef(NodeBlockCacheEntryHandle *this);

static void nodeBlockCacheAddRefCount(NodeBlockCache *this, NodeBlockCacheEntry *entry);

static void nodeBlockCacheSubRefCount(NodeBlockCache *this, NodeBlockCacheEntry *entry);

static void nodeBlockCacheMarkDirty(NodeBlockCache *this, const NodeBlockCacheEntryHandle *handle);

static void nodeBlockCacheRemoveEntry(NodeBlockCache *this, NodeBlockCacheEntry *entry);

static void nodeBlockCacheDoReplace(NodeBlockCache *this);

static node_block_cache_write_block_hook g_node_block_cache_write_block_hook = NULL;
static node_block_cache_read_block_hook g_node_block_cache_read_block_hook = NULL;


void nodeBlockCacheEntryInit(NodeBlockCacheEntry *this, BlockBuffer *buffer, uint32_t nid, uint32_t parentNid, uint32_t lpa)
{
    blockBufferInit(&this->node);
    blockBufferCopy(&this->node, buffer);

    this->nid = nid;
    this->parentNid = parentNid;
    this->lpa = lpa;
    this->cowNewLpa = INVALID_LPA;
    this->hasPendingCowRelocation = false;
    this->refCount = 0;
    this->state = NODE_BLOCK_CACHE_ENTRY_UPTODATE;
}

void nodeBlockCacheEntryDestroy(NodeBlockCacheEntry *this)
{
    this->state = NODE_BLOCK_CACHE_ENTRY_DELETED;
    this->refCount = 0;
    this->lpa = 0;
    this->cowNewLpa = 0;
    this->hasPendingCowRelocation = false;
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


void nodeBlockCacheInit(NodeBlockCache *this, struct file_system_manager *fsManager, size_t expectSize)
{
    this->expectSize = expectSize;
    this->curSize = 0;
    this->fsManager = fsManager;

    this->dirtyListHead = NULL;

    genericCacheManagerInit(&this->cacheManager);

    this->dirtyPos = kh_init(khdp);
}

void nodeBlockCacheDestroy(NodeBlockCache *this)
{
    if (NULL != this->dirtyListHead) RTFS_LOG(RTFS_LOG_WARNING, "node block cache still has dirty block while destructed.");

    kh_destroy(khdp, this->dirtyPos);
    this->dirtyPos = NULL;

    genericCacheManagerDestroy(&this->cacheManager);

    this->dirtyListHead = NULL;

    this->fsManager = NULL;
    this->curSize = 0;
    this->expectSize = 0;
}

NodeBlockCacheEntryHandle nodeBlockCacheAdd(NodeBlockCache *this, BlockBuffer *buffer, uint32_t nid, uint32_t parentNid, uint32_t lpa)
{
    assert(NULL == genericCacheManagerGet(&this->cacheManager, nid, false));

    // 构造新的缓存项。
    NodeBlockCacheEntry *entry = (NodeBlockCacheEntry *)malloc(sizeof(NodeBlockCacheEntry));
    nodeBlockCacheEntryInit(entry, buffer, nid, parentNid, lpa);

    // 将 parentNid 的引用计数加 1。
    if (INVALID_NID != parentNid)
    {
        NodeBlockCacheEntry *parentEntry = (NodeBlockCacheEntry *)genericCacheManagerGet(&this->cacheManager, parentNid, false);
        assert(NULL != parentEntry);

        nodeBlockCacheAddRefCount(this, parentEntry);
    }

    // 将缓存项加入 cacheManager，并尝试进行置换（若加入后的缓存数量加入阈值）。
    genericCacheManagerAdd(&this->cacheManager, nid, entry);
    nodeBlockCacheAddRefCount(this, entry);
    ++this->curSize;
    nodeBlockCacheDoReplace(this);


    NodeBlockCacheEntryHandle res = {.cache = this, .entry = entry};


    return res;
}

NodeBlockCacheEntryHandle nodeBlockCacheGet(NodeBlockCache *this, uint32_t nid)
{
    NodeBlockCacheEntry *entry = (NodeBlockCacheEntry *)genericCacheManagerGet(&this->cacheManager, nid, true);
    if (NULL != entry) nodeBlockCacheAddRefCount(this, entry);

    NodeBlockCacheEntryHandle res = {.cache = this, .entry = entry};


    return res;
}

NodeBlockCacheDirtyNode *nodeBlockCacheGetAndClearDirtyList(NodeBlockCache *this)
{
    NodeBlockCacheDirtyNode *cur = NULL, *tmp = NULL;

    // 遍历 dirty list，修改状态。
    DL_FOREACH_SAFE(this->dirtyListHead, cur, tmp)
    {
        NodeBlockCacheEntryHandle *handle = &cur->handle;

        assert(NODE_BLOCK_CACHE_ENTRY_DIRTY == handle->entry->state && handle->entry->refCount >= 1);

        handle->entry->state = NODE_BLOCK_CACHE_ENTRY_UPTODATE;
    }

    // swap 直接摘链表，清空 dirty list，并返回原先的 dirty list 用作淘汰保护。
    NodeBlockCacheDirtyNode *res = this->dirtyListHead;
    this->dirtyListHead = NULL;


    return res;
}

void nodeBlockCacheForceReplace(NodeBlockCache *this)
{
    nodeBlockCacheDoReplace(this);
}

void nodeBlockCacheSetWriteBlockHook(node_block_cache_write_block_hook hook)
{
    g_node_block_cache_write_block_hook = hook;
}

void nodeBlockCacheSetReadBlockHook(node_block_cache_read_block_hook hook)
{
    g_node_block_cache_read_block_hook = hook;
}

int nodeBlockCacheWritebackDirtyContentCow(NodeBlockCache *this)
{
    NodeBlockCacheDirtyNode *cur = NULL;
    struct file_system_manager *fs_manager;
    super_manager *sp_manager;
    struct comm_dev *dev;

    if (this == NULL || this->fsManager == NULL) {
        return EINVAL;
    }

    fs_manager = this->fsManager;
    sp_manager = fileSystemManagerGetSuperManager(fs_manager);
    dev = fileSystemManagerGetDevice(fs_manager);
    if (sp_manager == NULL || dev == NULL) {
        return EINVAL;
    }

    DL_FOREACH(this->dirtyListHead, cur)
    {
        NodeBlockCacheEntry *entry = cur->handle.entry;
        uint32_t new_lpa;

        if (entry == NULL || entry->state != NODE_BLOCK_CACHE_ENTRY_DIRTY) {
            continue;
        }

        new_lpa = superManagerAllocNodeLpa(sp_manager);
        if (new_lpa == INVALID_LPA) {
            return ENOSPC;
        }

        if (g_node_block_cache_write_block_hook != NULL) {
            int res = g_node_block_cache_write_block_hook(dev, new_lpa, blockBufferGetPtr(&entry->node));
            if (res != 0) {
                return res;
            }
        } else {
            blockBufferWriteToLpaSync(&entry->node, dev, new_lpa);
        }
        entry->cowNewLpa = new_lpa;
        entry->hasPendingCowRelocation = true;
    }

    return 0;
}

int nodeBlockCacheCollectPendingCowRelocations(
    NodeBlockCache *this,
    NodeBlockCacheCowRelocation *out_array,
    size_t max_count,
    size_t *out_count
)
{
    size_t count = 0;
    khiter_t k;

    if (this == NULL || out_array == NULL || out_count == NULL) {
        return EINVAL;
    }

    for (k = kh_begin(this->cacheManager.index.index);
         k != kh_end(this->cacheManager.index.index);
         ++k)
    {
        NodeBlockCacheEntry *entry;

        if (!kh_exist(this->cacheManager.index.index, k)) {
            continue;
        }

        entry = (NodeBlockCacheEntry *)kh_val(this->cacheManager.index.index, k);
        if (entry == NULL || !entry->hasPendingCowRelocation || entry->cowNewLpa == INVALID_LPA) {
            continue;
        }

        if (count >= max_count) {
            return ENOSPC;
        }

        out_array[count].nid = entry->nid;
        out_array[count].oldLpa = entry->lpa;
        out_array[count].newLpa = entry->cowNewLpa;
        count++;
    }

    *out_count = count;
    return 0;
}

int nodeBlockCacheApplyPendingCowRelocations(NodeBlockCache *this)
{
    khiter_t k;
    NatLpaMapping nat_mapping;
    SrmapUtils *srmap_utils;

    if (this == NULL || this->fsManager == NULL) {
        return EINVAL;
    }

    natLpaMappingInit(&nat_mapping, this->fsManager);
    srmap_utils = fileSystemManagerGetSrmapUtils(this->fsManager);
    for (k = kh_begin(this->cacheManager.index.index);
         k != kh_end(this->cacheManager.index.index);
         ++k)
    {
        NodeBlockCacheEntry *entry;

        if (!kh_exist(this->cacheManager.index.index, k)) {
            continue;
        }

        entry = (NodeBlockCacheEntry *)kh_val(this->cacheManager.index.index, k);
        if (entry == NULL || !entry->hasPendingCowRelocation || entry->cowNewLpa == INVALID_LPA) {
            continue;
        }

        natSetLpaOfNid(&nat_mapping, entry->nid, entry->cowNewLpa);
        if (srmap_utils != NULL) {
            srmapUtilsWriteSrmapOfNode(srmap_utils, entry->cowNewLpa, entry->nid);
        }
        entry->lpa = entry->cowNewLpa;
        entry->cowNewLpa = INVALID_LPA;
        entry->hasPendingCowRelocation = false;
        if (entry->state == NODE_BLOCK_CACHE_ENTRY_DIRTY) {
            khiter_t dirty_k = kh_get(khdp, this->dirtyPos, entry);
            if (dirty_k != kh_end(this->dirtyPos)) {
                NodeBlockCacheDirtyNode *dirty_node = kh_value(this->dirtyPos, dirty_k);
                DL_DELETE(this->dirtyListHead, dirty_node);
                kh_del(khdp, this->dirtyPos, dirty_k);
                free(dirty_node);
            }
            entry->state = NODE_BLOCK_CACHE_ENTRY_UPTODATE;
        }
    }

    if (srmap_utils != NULL) {
        srmapUtilsWriteDirtySrmapSync(srmap_utils);
    }

    return 0;
}


void nodeBlockCacheHelperInit(NodeBlockCacheHelper *this, struct file_system_manager *fsManager)
{
    this->fsManager = fsManager;
    this->natCache = fileSystemManagerGetNatCache(fsManager);
    this->nodeBlockCache = fileSystemManagerGetNodeCache(fsManager);
    this->dev = fileSystemManagerGetDevice(fsManager);
}

void nodeBlockCacheHelperDestroy(NodeBlockCacheHelper *this)
{
    this->dev = NULL;
    this->natCache = NULL;
    this->nodeBlockCache = NULL;
    this->fsManager = NULL;
}

int nodeBlockCacheHelperGetNodeEntry(
    NodeBlockCacheHelper *this,
    uint32_t nid,
    uint32_t parentNid,
    NodeBlockCacheEntryHandle *out_handle
)
{
    NodeBlockCacheEntryHandle handle;

    if (this == NULL || out_handle == NULL) {
        return EINVAL;
    }

    out_handle->cache = NULL;
    out_handle->entry = NULL;

    handle = nodeBlockCacheGet(this->nodeBlockCache, nid);
    if (nodeBlockCacheEntryHandleIsEmpty(&handle))
    {
        NatLpaMapping nlp;
        natLpaMappingInit(&nlp, this->fsManager);

        // 从 NAT 表中得到 nid block 的 lpa。
        uint32_t nidLpa = natGetLpaOfNid(&nlp, nid);

        BlockBuffer buffer;
        blockBufferInit(&buffer);

        if (g_node_block_cache_read_block_hook != NULL)
        {
            int res = g_node_block_cache_read_block_hook(this->dev, nidLpa, blockBufferGetPtr(&buffer));
            if (0 != res) {
                blockBufferDestroy(&buffer);
                return res;
            }
        }
        else
        {
            int ret = blockBufferReadFromLpa(&buffer, this->dev, nidLpa);
            if (ret != 0) {
                blockBufferDestroy(&buffer);
                return ret;
            }
        }

        // node_handle = node_cache->add(std::move(buf), nid, parentNid, nid_lpa);
        handle = nodeBlockCacheAdd(this->nodeBlockCache, &buffer, nid, parentNid, nidLpa);

        blockBufferDestroy(&buffer);
    }

    struct RtfsNode *node = nodeBlockCacheEntryGetNodeBlockPtr(handle.entry);
    assert(node->footer.nid == nid);
    if (INVALID_NID == parentNid) assert(node->footer.ino == nid);

    *out_handle = handle;
    return 0;
}

NodeBlockCacheEntryHandle nodeBlockCacheHelperCreateNodeEntry(NodeBlockCacheHelper *this, uint32_t ino, uint32_t noffset, uint32_t parentNid)
{
    // 分配 nid，创建 node block 缓存项并加入缓存。
    uint32_t newNid = superManagerAllocNid(fileSystemManagerGetSuperManager(this->fsManager), ino, false);
    NodeBlockCacheEntryHandle handle = {
        .cache = NULL,
        .entry = NULL
    };

    if (newNid == INVALID_NID) {
        return handle;
    }

    BlockBuffer buffer;
    blockBufferInit(&buffer);

    handle = nodeBlockCacheAdd(this->nodeBlockCache, &buffer, newNid, parentNid, INVALID_LPA);

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
    NodeBlockCacheEntryHandle handle = {
        .cache = NULL,
        .entry = NULL
    };

    if (newNid == INVALID_NID) {
        return handle;
    }

    BlockBuffer buffer;
    blockBufferInit(&buffer);

    handle = nodeBlockCacheAdd(this->nodeBlockCache, &buffer, newNid, INVALID_NID, INVALID_LPA);

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

void nodeBlockCacheAddRefCount(NodeBlockCache *this, NodeBlockCacheEntry *entry)
{
    ++entry->refCount;

    // 引用计数同时维护了淘汰保护、脏、被引用、读写等状态。只要引用计数不为 0，就需要 pin。
    if (1 == entry->refCount) genericCacheManagerPin(&this->cacheManager, entry->nid);
}

void nodeBlockCacheSubRefCount(NodeBlockCache *this, NodeBlockCacheEntry *entry)
{
    --entry->refCount;

    if (0 == entry->refCount)
    {
        genericCacheManagerUnpin(&this->cacheManager, entry->nid);

        // 如果该 node block 需要删除，则减少其父结点的引用计数，释放它的 FS 资源，把它移除缓存。
        if (NODE_BLOCK_CACHE_ENTRY_DELETED == entry->state)
        {
            // 释放它的 nid。
            superManagerFreeNid(fileSystemManagerGetSuperManager(this->fsManager), entry->nid);
            RTFS_LOG(RTFS_LOG_INFO, "delete node %u.", entry->nid);

            // 将它占有的 lpa 标记为垃圾块。
            if (INVALID_LPA != entry->lpa)
            {
                SitOperator sitOp;
                sitOperatorInit(&sitOp, this->fsManager);

                sitInvalidateLpa(&sitOp, entry->lpa);

                RTFS_LOG(RTFS_LOG_INFO, "the lpa of nid [%u] is [%u], will be invalidated.", entry->nid, entry->lpa);
            }

            // 将缓存项移除。
            uint32_t nid = entry->nid;
            uint32_t parentNid = entry->parentNid;
            genericCacheManagerRemove(&this->cacheManager, nid);
            --this->curSize;

            // 减少父结点的引用计数。
            if (INVALID_NID != parentNid)
            {
                NodeBlockCacheEntry *parentEntry = (NodeBlockCacheEntry *)genericCacheManagerGet(&this->cacheManager, parentNid, false);
                assert(NULL != parentEntry);

                nodeBlockCacheSubRefCount(this, parentEntry);
            }
        }
    }
}

void nodeBlockCacheMarkDirty(NodeBlockCache *this, const NodeBlockCacheEntryHandle *handle)
{
    // 保证 node block 缓存项如果是 dirty 状态，一定至少有一个引用计数。
    if (handle->entry->state != NODE_BLOCK_CACHE_ENTRY_DIRTY)
    {
        assert(handle->entry->state == NODE_BLOCK_CACHE_ENTRY_UPTODATE);

        handle->entry->state = NODE_BLOCK_CACHE_ENTRY_DIRTY;

        NodeBlockCacheDirtyNode *node = (NodeBlockCacheDirtyNode *)malloc(sizeof(NodeBlockCacheDirtyNode));
        assert(NULL != node);

        node->handle = *handle;
        node->prev = node->next = NULL;

        DL_APPEND(this->dirtyListHead, node);

        int res;
        khiter_t k = kh_put(khdp, this->dirtyPos, handle->entry, &res);
        assert(-1 != res);

        kh_value(this->dirtyPos, k) = node;
    }
}

void nodeBlockCacheRemoveEntry(NodeBlockCache *this, NodeBlockCacheEntry *entry)
{
    if (NODE_BLOCK_CACHE_ENTRY_DIRTY == entry->state)
    {
        khiter_t k = kh_get(khdp, this->dirtyPos, entry);
        assert(kh_end(this->dirtyPos) != k);

        NodeBlockCacheDirtyNode *node = kh_value(this->dirtyPos, k);
        assert(node->handle.entry == entry);

        DL_DELETE(this->dirtyListHead, node);

        kh_del(khdp, this->dirtyPos, k);

        free(node);
    }

    entry->state = NODE_BLOCK_CACHE_ENTRY_DELETED;
}

void nodeBlockCacheDoReplace(NodeBlockCache *this)
{
    if (this->curSize > this->expectSize)
    {
        while (true)
        {
            NodeBlockCacheEntry *entry = (NodeBlockCacheEntry *)genericCacheManagerReplaceOne(&this->cacheManager);

            if (NULL != entry)
            {
                assert(0 == entry->refCount);
                RTFS_LOG(RTFS_LOG_INFO, "replace node block cache entry, nid = %u", entry->nid);

                --this->curSize;

                // 将 parent 的引用计数 -1。
                uint32_t parentNid = entry->parentNid;
                if (INVALID_NID != parentNid)
                {
                    NodeBlockCacheEntry *parentEntry = (NodeBlockCacheEntry *)genericCacheManagerGet(&this->cacheManager, parentNid, false);
                    assert(NULL != parentEntry);

                    nodeBlockCacheSubRefCount(this, parentEntry);
                }
            }

            if (NULL == entry || this->curSize <= this->expectSize) break;
        }
    }
}
