#ifndef _NODE_BLOCK_CACHE_H_
#define _NODE_BLOCK_CACHE_H_

#include "cache/generic_cache_manager.h"
#include "cache/block_buffer.h"
#include "fs/fs.h"
#include "klib/khash.h"


struct NodeBlockCache;
struct SitNatCache;
struct file_system_manager;
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


typedef struct NodeBlockCacheEntryHandle
{
    struct NodeBlockCache *cache;
    NodeBlockCacheEntry *entry;
} NodeBlockCacheEntryHandle;


void nodeBlockCacheEntryHandleInit(NodeBlockCacheEntryHandle *this, struct NodeBlockCache *cache, NodeBlockCacheEntry *entry);

void nodeBlockCacheEntryHandleDestroy(NodeBlockCacheEntryHandle *this);

void nodeBlockCacheEntryHandleCopy(NodeBlockCacheEntryHandle *this, const NodeBlockCacheEntryHandle *other);

bool nodeBlockCacheEntryHandleIsEmpty(NodeBlockCacheEntryHandle *this);

void nodeBlockCacheEntryHandleAddHostVersion(NodeBlockCacheEntryHandle *this);

void nodeBlockCacheEntryHandleAddSsdVersion(NodeBlockCacheEntryHandle *this);

void nodeBlockCacheEntryHandleMarkDirty(NodeBlockCacheEntryHandle *this);

void nodeBlockCacheEntryHandleDeleteNode(NodeBlockCacheEntryHandle *this);


typedef struct NodeBlockCacheDirtyNode
{
    NodeBlockCacheEntryHandle handle;

    struct NodeBlockCacheDirtyNode *prev;
    struct NodeBlockCacheDirtyNode *next;
} NodeBlockCacheDirtyNode;


typedef struct NodeBlockCache
{
    GenericCacheManager cacheManager;

    size_t expectSize;
    size_t curSize;

    NodeBlockCacheDirtyNode *dirtyListHead;

    // TODO std::unordered_map<node_block_cache_entry*, std::list<node_block_cache_entry_handle>::iterator> dirty_pos;

    struct file_system_manager *fs_manager;
} NodeBlockCache;


typedef struct NodeBlockCacheHelper
{
    struct comm_dev *dev;
    struct SitNatCache *natCache;
    NodeBlockCache *nodeBlockCache;
    struct file_system_manager *fs_manager;
} NodeBlockCacheHelper;


void nodeBlockCacheHelperInit(NodeBlockCacheHelper *this, struct file_system_manager *fsManager);

void nodeBlockCacheHelperDestroy(NodeBlockCacheHelper *this);

/**
 * @brief 获取 nid 对应的 node block 缓存项，封装 node block cache 不命中时，从 NAT 表中查找 lpa 并读入缓存的过程。parentNid 为目标 nid 在索引树上的父 node block。如果 nid 为 inode block，则 parentNid 应置为 INVALID_NID。调用者应确保 parentNid 在缓存中，且nid有效。
 */
NodeBlockCacheEntryHandle nodeBlockCacheHelperGetNodeEntry(NodeBlockCacheHelper *this, uint32_t nid, uint32_t parentNid);

/**
 * @brief 分配一个 nid，然后分配一个 node block 缓存项，把该 node block 缓存项和 nid 绑定。新缓存项的 oldLpa 和 newLpa 均为 invalid，状态为 dirty。新 node block 中，node footer 按参数内容初始化，其余内容初始化为 0。
 * @param ino: 该 node 所属文件（此接口用于创建索引树的 node，不用于创建 inode）。
 * @param noffset：该 node 在索引树中的逻辑编号。
 * @param parent_nid：该 node 在索引树上的父结点。
 * @return 返回新 node 缓存项句柄。
 */
NodeBlockCacheEntryHandle nodeBlockCacheHelperCreateNodeEntry(NodeBlockCacheHelper *this, uint32_t ino, uint32_t noffset, uint32_t parentNid);

/**
 * @brief 作用同 nodeBlockCacheHelperCreateNodeEntry，区别在于此函数创建的是 inode，因此不需要额外参数。
 */
NodeBlockCacheEntryHandle nodeBlockCacheHelperCreateInodeEntry(NodeBlockCacheHelper *this);


#endif
