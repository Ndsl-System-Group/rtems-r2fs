#ifndef _PAGE_CACHE_H_
#define _PAGE_CACHE_H_

#include "cache/block_buffer.h"
#include "cache/generic_cache_manager.h"

#include "utils/types.h"
#include "utils/rtfs_multithread.h"

#include "klib/kbtree.h"

#include <stdatomic.h>


struct PageCache;


/**
 * @brief Page 状态。
 */
typedef enum PageState
{
    PAGE_INVALID, /**< 页面内容无效。 */
    PAGE_READY    /**< 页面内容已就绪。 */
} PageState;


/**
 * @brief 页缓存条目。表示文件中的一个逻辑块缓存，包括数据缓冲区、状态、引用计数等。
 */
typedef struct PageEntry
{
    /**
     * @brief 文件内块偏移。
     */
    uint32_t blkoff;

    /**
     * @brief 块地址（如果是新创建但没写回，则为 INVALID_LPA）。
     */
    uint32_t lpa;

    /**
     * @brief 页面内容状态。
     */
    PageState contentState;

    /**
     * @brief 实际缓存的数据块。
     */
    BlockBuffer page;

    /**
     * @brief 保护 page、contentState、originLpa 的锁。获得 fileOpLock 独占时，不需要再加此锁。
     */
    mutex_t pageLock;

    /**
     * @brief 引用计数，在调用方与 PageEntryHandle 生命周期绑定，一个 PageEntryHandle 增加 1 引用计数
。page cache 内的 dirty page set 中的 page entry 也增加引用计数。
     * @details 通过 page cache 的 get 获取 PageEntryHandle 时，对 page cache 加 cacheLock 锁后增加 refCount。refCount 为 0 时，一定是 page cache 内部独占访问 pageEntry（内部加了 cacheLock 锁，外部没有句柄，无法访问）。
     * @details PageEntryHandle 拷贝时，对 refCount 原子加，不对 page cache 加锁（此时 refCount 一定大于等于 1）。PageEntryHandle 析构时，调用 page cache 的 subRefcount 方法。
     * @note atomic_uint_least32_t：至少 32 位的最小可用无符号整数类型。位数 ≥ 32，优先占用更少内存。atomic_uint_fast32_t：至少 32 位的最快整数类型。位数 ≥ 32，优先选择 CPU 最快处理的类型。
     */
    atomic_uint_fast32_t refCount;

    /**
     * @brief dirty 标记。
     */
    atomic_bool isDirty;
} PageEntry;


/**
 * @brief 初始化 PageEntry。
 */
void pageEntryInit(PageEntry *this, uint32_t blkoff);

/**
 * @brief 销毁 PageEntry。
 */
void pageEntryDestroy(PageEntry *this);

/**
 * @brief 获取 PageEntry 锁。
 */
mutex_t *pageEntryGetLock(PageEntry *this);

/**
 * @brief 获取页面缓冲区指针。
 */
BlockBuffer *pageEntryGetBuffer(PageEntry *this);

/**
 * @brief 获取页面状态。
 */
PageState pageEntryGetState(const PageEntry *this);

/**
 * @brief 设置页面状态。
 */
void pageEntrySetState(PageEntry *this, PageState state);

/**
 * @brief 获取块偏移。
 */
uint32_t pageEntryGetBlkoff(const PageEntry *this);

/**
 * @brief 获取 LPA。
 */
uint32_t pageEntryGetLpa(PageEntry *this);

/**
 * @brief 设置 LPA。
 */
void pageEntrySetLpa(PageEntry *this, uint32_t lpa);


/**
 * @brief PageEntry 句柄。用于对 PageCache 和 PageEntry 管理，同时处理引用计数，保证生命周期安全。
 */
typedef struct PageEntryHandle
{
    struct PageCache *cache;
    PageEntry *entry;
} PageEntryHandle;


/**
 * @brief 初始化 handle。
 */
void pageEntryHandleInit(PageEntryHandle *this, struct PageCache *cache, PageEntry *entry);

/**
 * @brief 销毁 handle（减少引用计数）。
 */
void pageEntryHandleDestroy(PageEntryHandle *this);

/**
 * @brief 拷贝 handle（增加引用计数）。
 */
void pageEntryHandleCopy(PageEntryHandle *this, const PageEntryHandle *other);

/**
 * @brief 尝试将 page entry 的 dirty 置位。如果是由本线程将 dirty 置位，则加入 page cache  的 dirty pages 集合。
 */
void pageEntryHandleMakeDirty(PageEntryHandle *this);


/**
 * @brief dirty pages 节点，在 kbtree 中存储 dirty 页集合。
 */
typedef struct DirtyPagesNode
{
    uint32_t blkoff;        // key。
    PageEntryHandle handle; // data。
} DirtyPagesNode;

/**
 * @brief 文件页缓存。PageCache 只作为 page 的缓存索引和置换管理器，不关心文件的实际大小。
 */
typedef struct PageCache
{
    /**
     * @brief 缓存管理器。
     */
    GenericCacheManager cacheManager;

    // TODO 原代码里面这两个锁均使用自旋锁 spinlock_t，但飞腾的 bsp 没有 POSIX 标准的自旋锁实现，因此使用 mutex 替代。
    /**
     * @brief 保护 cacheManager 的锁。
     */
    mutex_t cacheLock;

    /**
     * @brief 由于 dirtyPages 有范围 remove 需求，所以用类似 C++ 中的 map<blkoff, PageEntryHandle> 维护。kbtree_t(ktdpn) * 类型不能定义在头文件中，因此这里用 void * 代替。
     */
    void *dirtyPages;

    /**
     * @brief dirtyPages 保护锁。
     */
    mutex_t dirtyPagesLock;

    /**
     * @brief 期望缓存大小。
     */
    size_t expectSize;

    /**
     * @brief 当前缓存大小。
     */
    size_t curSize;
} PageCache;


/**
 * @brief 初始化 PageCache。
 */
void pageCacheInit(PageCache *this, size_t expectSize);

/**
 * @brief 销毁 PageCache。
 */
void pageCacheDestroy(PageCache *this);

/**
 * @brief 获取 blkoff 对应的 pageEntry。若不存在，则构造一个 pageEntry，返回其 handle。此时返回的 pageEntry中，反向映射和 4 KB 缓存是无效值。
 */
PageEntryHandle pageCacheGet(PageCache *this, uint32_t blkoff);

/**
 * @brief 截断文件后调用，将 page cache  内块偏移严格大于 maxBlkoff 的缓存 page 的 dirty 位清除，置位 invalid 状态，但不将它们删除，因为也许之后又会访问。调用者必须持有对应文件的 fileOpLock 独占锁（此时仅有一个线程能操作文件的 page cache ）。
 */
void pageCacheTruncate(PageCache *this, uint32_t maxBlkoff);

/**
 * @brief 获取 dirty pages 集合。调用者如果需要操作 dirty pages，必须持有对应文件的 fileOpLock 独占锁。
 */
void *pageCacheGetDirtyPages(PageCache *this);

/**
 * @brief 将 dirty pages 中所有 page 的 dirty 位清除。
 */
void pageCacheClearDirtyPages(PageCache *this);


#endif
