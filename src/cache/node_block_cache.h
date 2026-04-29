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


/**
 * @brief Node Block 缓存项状态。
 */
typedef enum NodeBlockCacheEntryState
{
    NODE_BLOCK_CACHE_ENTRY_UPTODATE, /**< 数据与 SSD 一致，可直接读取。 */
    NODE_BLOCK_CACHE_ENTRY_DIRTY,    /**< 数据已被修改，尚未回写到 SSD。 */
    NODE_BLOCK_CACHE_ENTRY_DELETED,  /**< 已逻辑删除，待引用计数归零后释放资源。 */
} NodeBlockCacheEntryState;


/**
 * @brief Node Block 缓存项。
 */
typedef struct NodeBlockCacheEntry
{
    /**
     * @brief 当前 node 的 nid。
     */
    uint32_t nid;

    /**
     * @brief 父 node 的 nid，inode 时为 INVALID_NID。
     */
    uint32_t parentNid;

    /**
     * @brief 当前 node 在 SSD 上的位置。
     */
    uint32_t lpa;

    /**
     * @brief node block 数据缓冲区。
     */
    BlockBuffer node;

    /**
     * @brief 引用计数，同时承担 pin/淘汰保护作用。
     */
    uint32_t refCount;

    /**
     * @brief 当前状态。
     */
    NodeBlockCacheEntryState state;
} NodeBlockCacheEntry;


/**
 * @brief 初始化缓存项。
 */
void nodeBlockCacheEntryInit(NodeBlockCacheEntry *this, BlockBuffer *buffer, uint32_t nid, uint32_t parentNid, uint32_t lpa);

/**
 * @brief 销毁缓存项并释放内部资源。
 */
void nodeBlockCacheEntryDestroy(NodeBlockCacheEntry *this);

/**
 * @brief 获取缓存项对应的 lpa。
 */
uint32_t nodeBlockCacheEntryGetLpa(NodeBlockCacheEntry *this);

/**
 * @brief 设置缓存项对应的 lpa。
 */
void nodeBlockCacheEntrySetLpa(NodeBlockCacheEntry *this, uint32_t lpa);

/**
 * @brief 获取缓存项状态。
 */
NodeBlockCacheEntryState nodeBlockCacheEntryGetState(NodeBlockCacheEntry *this);

/**
 * @brief 设置缓存项状态。
 */
void nodeBlockCacheEntrySetState(NodeBlockCacheEntry *this, NodeBlockCacheEntryState state);

/**
 * @brief 获取 node block 数据结构指针。
 */
struct RtfsNode *nodeBlockCacheEntryGetNodeBlockPtr(NodeBlockCacheEntry *this);

/**
 * @brief 获取底层缓冲区对象。
 */
BlockBuffer *nodeBlockCacheEntryGetNodeBuffer(NodeBlockCacheEntry *this);

/**
 * @brief 获取缓存项 nid。
 */
uint32_t nodeBlockCacheEntryGetNid(NodeBlockCacheEntry *this);


/**
 * @brief 缓存项句柄，负责缓存项引用计数的生命周期管理。
 */
typedef struct NodeBlockCacheEntryHandle
{
    struct NodeBlockCache *cache;
    NodeBlockCacheEntry *entry;
} NodeBlockCacheEntryHandle;


/**
 * @brief 初始化句柄。
 */
void nodeBlockCacheEntryHandleInit(NodeBlockCacheEntryHandle *this, struct NodeBlockCache *cache, NodeBlockCacheEntry *entry);

/**
 * @brief 销毁句柄，并减少引用计数。
 */
void nodeBlockCacheEntryHandleDestroy(NodeBlockCacheEntryHandle *this);

/**
 * @brief 复制句柄，并增加引用计数。
 */
void nodeBlockCacheEntryHandleCopy(NodeBlockCacheEntryHandle *this, const NodeBlockCacheEntryHandle *other);

/**
 * @brief 判断句柄是否为空。
 */
bool nodeBlockCacheEntryHandleIsEmpty(NodeBlockCacheEntryHandle *this);

/**
 * @brief 增加 host 侧引用版本。
 */
void nodeBlockCacheEntryHandleAddHostVersion(NodeBlockCacheEntryHandle *this);

/**
 * @brief 增加 SSD 侧版本（等价释放一个引用）。
 */
void nodeBlockCacheEntryHandleAddSsdVersion(NodeBlockCacheEntryHandle *this);

/**
 * @brief 将缓存项标记为 dirty。
 */
void nodeBlockCacheEntryHandleMarkDirty(NodeBlockCacheEntryHandle *this);

/**
 * @brief 删除当前缓存项对应 node。
 */
void nodeBlockCacheEntryHandleDeleteNode(NodeBlockCacheEntryHandle *this);


/**
 * @brief dirty 链表节点。
 */
typedef struct NodeBlockCacheDirtyNode
{
    NodeBlockCacheEntryHandle handle;

    struct NodeBlockCacheDirtyNode *prev;
    struct NodeBlockCacheDirtyNode *next;
} NodeBlockCacheDirtyNode;


KHASH_MAP_INIT_PTR(khdp, NodeBlockCacheDirtyNode *)


/**
 * @brief Node block 缓存管理器。
 */
typedef struct NodeBlockCache
{
    /**
     * @brief 通用缓存替换管理器。
     */
    GenericCacheManager cacheManager;

    /**
     * @brief 期望缓存大小。
     */
    size_t expectSize;

    /**
     * @brief 当前缓存项数量。
     */
    size_t curSize;

    /**
     * @brief dirty 链表头。
     */
    NodeBlockCacheDirtyNode *dirtyListHead;

    /**
     * @brief dirty 项快速定位表。
     */
    khash_t(khdp) * dirtyPos;

    /**
     * @brief 全局文件系统管理器。
     */
    struct file_system_manager *fsManager;
} NodeBlockCache;


/**
 * @brief 初始化 node block cache。
 */
void nodeBlockCacheInit(NodeBlockCache *this, struct file_system_manager *fsManager, size_t expectSize);

/**
 * @brief 销毁 node block cache。
 */
void nodeBlockCacheDestroy(NodeBlockCache *this);

/**
 * @brief 将一个 node block 加入缓存，返回缓存项句柄。加入时，调用者需保证加入的 buffer 满足：refCount为 0（不存在读写引用，双版本号相同），语义上为 uptodate 状态。通常，只应当在缓存未命中时，从 SSD 读取或执行 file mapping 任务，并 add 结果。调用 add 后，buffer 的资源被移动到缓存项中，调用者若要使用 buffer，应通过返回的 handle 获取 buffer。parentNid 为索引树上的父 node block，若此 node 为 inode，则 parentNid 应置为 INVALID_NID。调用者应确保 parentNid 在缓存中。
 */
NodeBlockCacheEntryHandle nodeBlockCacheAdd(NodeBlockCache *this, BlockBuffer *buffer, uint32_t nid, uint32_t parentNid, uint32_t lpa);

/**
 * @brief 查找 nid 对应的缓存项。若不存在，则句柄的 isEmpty 方法返回 true。视作对缓存项的一次访问。
 */
NodeBlockCacheEntryHandle nodeBlockCacheGet(NodeBlockCache *this, uint32_t nid);

/**
 * @brief 清除 dirty list 中的 node 缓存项的 dirty 标记，清空 dirty list，并返回原先的 dirty list 用作淘汰保护。返回的 dirty list 中的元素，已经不带脏标记。
 */
NodeBlockCacheDirtyNode *nodeBlockCacheGetAndClearDirtyList(NodeBlockCache *this);

/**
 * @brief 强制执行一次缓存替换。
 */
void nodeBlockCacheForceReplace(NodeBlockCache *this);


/**
 * @brief Node block cache 辅助操作器。封装 NAT 查询、SSD 读取、nid 分配等流程。
 */
typedef struct NodeBlockCacheHelper
{
    struct comm_dev *dev;
    struct SitNatCache *natCache;
    NodeBlockCache *nodeBlockCache;
    struct file_system_manager *fsManager;
} NodeBlockCacheHelper;


/**
 * @brief 初始化辅助操作器。
 */
void nodeBlockCacheHelperInit(NodeBlockCacheHelper *this, struct file_system_manager *fsManager);

/**
 * @brief 销毁辅助操作器。
 */
void nodeBlockCacheHelperDestroy(NodeBlockCacheHelper *this);

/**
 * @brief 获取 nid 对应的 node block 缓存项，封装 node block cache 不命中时，从 NAT 表中查找 lpa 并读入缓存的过程。parentNid 为目标 nid 在索引树上的父 node block。如果 nid 为 inode block，则 parentNid 应置为 INVALID_NID。调用者应确保 parentNid 在缓存中，且nid有效。
 */
NodeBlockCacheEntryHandle nodeBlockCacheHelperGetNodeEntry(NodeBlockCacheHelper *this, uint32_t nid, uint32_t parentNid);

/**
 * @brief 分配一个 nid，然后分配一个 node block 缓存项，把该 node block 缓存项和 nid 绑定。新缓存项的 oldLpa 和 newLpa 均为 invalid，状态为 dirty。新 node block 中，node footer 按参数内容初始化，其余内容初始化为 0。
 * @param ino 该 node 所属文件（此接口用于创建索引树的 node，不用于创建 inode）。
 * @param noffset 该 node 在索引树中的逻辑编号。
 * @param parentNid 该 node 在索引树上的父结点。
 * @return 返回新 node 缓存项句柄。
 */
NodeBlockCacheEntryHandle nodeBlockCacheHelperCreateNodeEntry(NodeBlockCacheHelper *this, uint32_t ino, uint32_t noffset, uint32_t parentNid);

/**
 * @brief 作用同 nodeBlockCacheHelperCreateNodeEntry，区别在于此函数创建的是 inode，因此不需要额外参数。
 */
NodeBlockCacheEntryHandle nodeBlockCacheHelperCreateInodeEntry(NodeBlockCacheHelper *this);


#endif
