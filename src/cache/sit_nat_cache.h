#ifndef _SIT_NAT_CACHE_H_
#define _SIT_NAT_CACHE_H_

#include "utils/types.h"
#include "cache/block_buffer.h"
#include "cache/generic_cache_manager.h"


struct SitNatCache;
struct comm_dev;


/**
 * @brief SIT/NAT 缓存项。
 * @details
 *  - key: lpa (uint32_t)
 *  - refCount: 引用计数，用于 pin / unpin。
 *  - cache: 实际缓存的数据块。
 *
 * 生命周期
 *  - entry 由 SitNatCache 创建和销毁。
 *  - entry 的引用由 SitNatCacheEntryHandle 管理。
 *  - 当 refCount > 0 时，entry 被视为 pinned，不允许被替换。
 */
typedef struct SitNatCacheEntry
{
    uint32_t lpa;
    uint32_t refCount;
    BlockBuffer cache;
} SitNatCacheEntry;


/**
 * @brief 初始化缓存项。
 */
void sitNatCacheEntryInit(SitNatCacheEntry *this, uint32_t lpa);

/**
 * @brief 销毁缓存项。
 * @note refCount 应为 0，否则说明仍有未释放引用。
 */
void sitNatCacheEntryDestroy(SitNatCacheEntry *this);


/**
 * @brief SIT/NAT 缓存项句柄。
 * @details
 *  - Handle 是对 SitNatCacheEntry 的一个引用。
 *  - Handle 不拥有 entry 的所有权。
 *  - entry 的引用计数由 handle 生命周期维护。
 *
 * 生命周期规则
 *  - handle init / copy 会增加 entry 的 refCount。
 *  - handle destroy 会减少 entry 的 refCount。
 */
typedef struct SitNatCacheEntryHandle
{
    struct SitNatCache *cache;
    SitNatCacheEntry *entry;
} SitNatCacheEntryHandle;


/**
 * @brief 初始化 handle。
 * @note 不改变 entry 的 refCount。
 */
void sitNatCacheEntryHandleInit(SitNatCacheEntryHandle *this, struct SitNatCache *cache, SitNatCacheEntry *entry);


/**
 * @brief 销毁 handle。
 * @details
 *  - 若 entry != NULL，则减少 entry 的 refCount。
 */
void sitNatCacheEntryHandleDestroy(SitNatCacheEntryHandle *this);


/**
 * @brief 复制 handle。
 * @details
 *  - 复制 entry 引用。
 *  - entry 的 refCount +1。
 */
void sitNatCacheEntryHandleCopy(SitNatCacheEntryHandle *this, const SitNatCacheEntryHandle *other);


/**
 * @brief 为 entry 添加一个主机侧引用。
 * @details
 *  - refCount +1
 */
void sitNatCacheEntryHandleAddHostVersion(SitNatCacheEntryHandle *this);


/**
 * @brief 为 entry 添加一个 SSD 版本完成标记。
 * @details
 *  - refCount -1
 */
void sitNatCacheEntryHandleAddSsdVersion(SitNatCacheEntryHandle *this);


/**
 * @brief SIT/NAT 缓存控制器。
 * @details
 *  - key 类型: uint32_t (lpa)
 *  - entry 类型: SitNatCacheEntry
 *
 * 内部组件
 *  - index: CacheIndexManager
 *  - replacer: LRU
 *
 * 语义
 *  - entry 所有权由 SitNatCache 管理。
 *  - handle 只持有引用。
 *
 * 替换策略
 *  - 当 curSize > expectSize 时触发 replace。
 *  - 仅允许替换 refCount == 0 的 entry。
 *
 * 典型访问流程
 *
 *   handle = sitNatCacheGet(cache, lpa)
 *   ↓
 *   使用 entry
 *   ↓
 *   sitNatCacheEntryHandleDestroy(handle)
 */
typedef struct SitNatCache
{
    GenericCacheManager cacheManager;

    size_t expectSize;
    size_t curSize;

    struct comm_dev *dev;
} SitNatCache;


/**
 * @brief 初始化 SIT/NAT cache。
 *
 * @param device SSD 设备
 * @param expectCacheSize 期望缓存项数量
 */
void sitNatCacheInit(SitNatCache *this, struct comm_dev *dev, size_t expectCacheSize);


/**
 * @brief 销毁 SIT/NAT cache。
 */
void sitNatCacheDestroy(SitNatCache *this);


/**
 * @brief 获取指定 lpa 的缓存项。
 *
 * @details
 *  - 若缓存命中，返回已有 entry。
 *  - 若缓存未命中，则从 SSD 读取并加入 cache。
 *  - entry 的 refCount +1。
 *
 * @return SitNatCacheEntryHandle
 */
SitNatCacheEntryHandle sitNatCacheGet(SitNatCache *this, uint32_t lpa);


/**
 * @brief 标记 SSD 版本完成。
 *
 * @details
 *  - refCount -1
 */
void sitNatCacheAddSsdVersion(SitNatCache *this, uint32_t lpa);


#endif
