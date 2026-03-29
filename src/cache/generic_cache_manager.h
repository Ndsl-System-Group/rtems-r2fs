#ifndef _GENERIC_CACHE_MANAGER_H_
#define _GENERIC_CACHE_MANAGER_H_

#include "cache/cache_index_manager.h"
#include "cache/cache_lru_replacer.h"

#include <stdbool.h>


/**
 * @brief 通用缓存管理器。
 * @details 语义说明
 *  - key 类型为 uint32_t。
 *  - entry 为 void*，其所有权由 cache manager 管理。
 *  - get() 返回借用指针，调用者不得释放。
 *  - replace / remove 返回的 entry，其所有权转移给调用者。
 **/
typedef struct GenericCacheManager
{
    CacheIndexManager index;
    CacheLruReplacer replacer;
} GenericCacheManager;


/**
 * @brief 初始化通用缓存管理器。
 */
void genericCacheManagerInit(GenericCacheManager *this);

/**
 * @brief 销毁通用缓存管理器。
 */
void genericCacheManagerDestroy(GenericCacheManager *this);

/**
 * @brief 添加缓存项（转移 entry 所有权）。
 */
void genericCacheManagerAdd(GenericCacheManager *this, uint32_t key, void *entry);

/**
 * @brief 获取缓存项（不转移所有权）。
 */
void *genericCacheManagerGet(GenericCacheManager *this, uint32_t key, bool isAccess);

/**
 * @brief 置换一个缓存项（转移 entry 所有权）。
 */
void *genericCacheManagerReplaceOne(GenericCacheManager *this);

/**
 * @brief 强制移除缓存项（转移 entry 所有权）。
 */
void *genericCacheManagerRemove(GenericCacheManager *this, uint32_t key);

/**
 * @brief pin 住缓存项，标识其不能被置换。
 */
void genericCacheManagerPin(GenericCacheManager *this, uint32_t key);

/**
 * @brief unpin 缓存项，使其可以被置换。
 */
void genericCacheManagerUnpin(GenericCacheManager *this, uint32_t key);


#endif
