#ifndef _CACHE_INDEX_MANAGER_H_
#define _CACHE_INDEX_MANAGER_H_

#include "uthash/uthash.h"


/**
 * @brief 缓存条目。
 * @details CacheEntry 的 key 设计。
 * 当前实现：
 *  - key 类型为 uint32_t，按“值”存储在哈希表中，而非指针或对象引用。
 *  - 哈希比较基于 key 的数值本身，与 key 的存储位置和生命周期无关。
 *  - 管理器不托管 key（因为是值类型）；value 由管理器托管并负责释放。
 *  - 调用者可安全地使用栈变量、全局变量或临时值作为 key。
 *
 * 设计优势：
 *  1. key 无生命周期问题，不存在悬空指针或 use-after-free。
 *  2. 哈希行为稳定、可预测，适合索引号 / ID / 表项编号等场景。
 *  3. 接口语义清晰，易于理解和维护。
 *
 * 适用场景：
 *  - key 为逻辑编号（如块号、节点号、索引号等）。
 *  - key 能自然映射为 uint32_t，且不需要表达复杂结构。
 *
 * 可选方案对比：
 *  - 方案 A：当前实现（uint32_t 按值存储）
 *       优点：安全、简单、高效，支持栈变量，无需额外内存管理
 *       缺点：仅支持固定类型 key，不够泛型
 *
 *  - 方案 B：托管任意 key（内部 malloc + memcpy）
 *       优点：支持任意复杂 key，对调用者友好
 *       缺点：接口复杂，需要 key 大小，增加内存与拷贝开销
 *
 *  - 方案 C：指针型 key（不托管 key，仅存地址）
 *       优点：灵活，可支持任意对象类型
 *       缺点：依赖调用者保证 key 生命周期，容易产生隐蔽错误
 *
 * 当前版本选择方案 A，是在“安全性、可维护性、实现复杂度”之间的权衡结果。
 */
typedef struct CacheEntry
{
    uint32_t key;      // uint32_t 类型 key。
    void *value;       // 任意类型 value（类似 unique_ptr，index 拥有）。
    UT_hash_handle hh; // uthash 必须要求需要的字段。
} CacheEntry;

/**
 * @brief 缓存索引管理器。
 */
typedef struct CacheIndexManager
{
    CacheEntry *index; // 哈希表头指针。
} CacheIndexManager;


/**
 * @brief 初始化缓存索引。
 */
void cacheIndexManagerInit(CacheIndexManager *this);

/**
 * @brief 销毁缓存索引，释放所有 value 内存。
 */
void cacheIndexManagerDestroy(CacheIndexManager *this);

/**
 * @brief 添加缓存项。
 */
void cacheIndexManagerAdd(CacheIndexManager *this, uint32_t key, void *value);

/**
 * @brief 获取缓存项。
 */
void *cacheIndexManagerGet(CacheIndexManager *this, uint32_t key);

/**
 * @brief 移除缓存项，返回 value，由调用者决定是否释放。
 */
void *cacheIndexManagerRemove(CacheIndexManager *this, uint32_t key);

/**
 * @brief 删除指定条目（迭代器风格）。
 */
void cacheIndexManagerErase(CacheIndexManager *this, CacheEntry *cacheEntry);


#endif
