#ifndef _CACHE_LRU_REPLACER_H_
#define _CACHE_LRU_REPLACER_H_

#include "klib/khash.h"
#include "utils/types.h"


/**
 * @brief LRU 链表节点。
 */
typedef struct LruNode
{
    uint32_t key;
    bool isPinned;

    struct LruNode *prev;
    struct LruNode *next;

} LruNode;

KHASH_MAP_INIT_INT(khclr, struct LruNode *)

/**
 * @brief LRU 置换器。
 */
typedef struct CacheLruReplacer
{
    // 正常的 LRU 链表。头部代表的是最久未访问，尾部代表的是最近访问。
    LruNode *lruHead;

    // pin 住的链表。
    LruNode *pinHead;

    // 因为需要 O(1) 的时间复杂度从 key 定位到链表节点，所以需要额外开辟一个哈希表存储 key 到 LRU 链表的映射关系。
    khash_t(khclr) * table;

    // 当前可置换数量。
    size_t size;
} CacheLruReplacer;


/**
 * @brief 初始化 LRU replacer。
 */
void cacheLruReplacerInit(CacheLruReplacer *this);

/**
 * @brief 销毁 LRU replacer（不释放 key）。
 */
void cacheLruReplacerDestroy(CacheLruReplacer *this);

/**
 * @brief 添加 key（默认视为最近访问）。
 */
void cacheLruReplacerAdd(CacheLruReplacer *this, uint32_t key);

/**
 * @brief 访问 key（刷新 LRU 顺序）。
 */
void cacheLruReplacerAccess(CacheLruReplacer *this, uint32_t key);

/**
 * @brief 是否存在可被置换的 key。
 */
int cacheLruReplacerCanReplace(CacheLruReplacer *this);

/**
 * @brief 弹出一个最久未访问的 key。
 */
uint32_t cacheLruReplacerPop(CacheLruReplacer *this);

/**
 * @brief 手动移除指定 key。
 */
void cacheLruReplacerRemove(CacheLruReplacer *this, uint32_t key);

/**
 * @brief 锁住 key，让其不能被置换。如果已经被锁住，那么什么也不做。
 */
void cacheLruReplacerPin(CacheLruReplacer *this, uint32_t key);

/**
 * @brief 解锁 key，使其可以被置换。unpin 被视为一次访问，会将其放置到 lru 链表尾。如果没有被锁住，什么都不做。
 */
void cacheLruReplacerUnpin(CacheLruReplacer *this, uint32_t key);


#endif
