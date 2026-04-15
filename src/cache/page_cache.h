#ifndef _PAGE_CACHE_H_
#define _PAGE_CACHE_H_

#include "cache/block_buffer.h"
#include "cache/generic_cache_manager.h"

#include "utils/types.h"
#include "utils/rtfs_multithread.h"

#include "klib/kbtree.h"

#include <stdatomic.h>


struct PageCache;


typedef enum PageState
{
    PAGE_INVALID,
    PAGE_READY
} PageState;


typedef struct PageEntry
{
    // 文件内块偏移。
    uint32_t blkoff;

    // 块地址（如果是新创建但没写回，则为 INVALID_LPA）。
    uint32_t lpa;

    PageState contentState;
    BlockBuffer page;

    // 保护 page、contentState、originLpa 的锁。获得 fileOpLock 独占时，不需要再加此锁。
    mutex_t pageLock;

    // atomic_uint_least32_t：至少 32 位的最小可用无符号整数类型。位数 ≥ 32，优先占用更少内存。
    // atomic_uint_fast32_t：至少 32 位的最快整数类型。位数 ≥ 32，优先选择 CPU 最快处理的类型。
    /*
     * 引用计数，在调用方与page_entry_handle生命周期绑定，一个page_entry_handle增加1引用计数
     * page cache内的dirty page set中的page entry也增加引用计数
     *
     * 通过page_cache.get获取page_entry_handle时，对page_cache加cache_lock锁后增加ref_count
     * ref_count为0时，一定是page cache内部独占访问page_entry(内部加了cache_lock锁，外部没有句柄，无法访问)
     *
     * page_entry_handle拷贝时，对ref_count原子加，不对page_cache加锁(此时ref_count一定大于等于1)
     * page_entry_handle析构时，调用page_cache的sub_refcount方法
     */
    atomic_uint_fast32_t refCount;

    // dirty 标记。
    atomic_bool isDirty;
} PageEntry;


void pageEntryInit(PageEntry *this, uint32_t blkoff);

void pageEntryDestroy(PageEntry *this);

mutex_t *pageEntryGetLock(PageEntry *this);

BlockBuffer *pageEntryGetBuffer(PageEntry *this);

PageState pageEntryGetState(const PageEntry *this);

void pageEntrySetState(PageEntry *this, PageState state);

uint32_t pageEntryGetBlkoff(const PageEntry *this);

uint32_t pageEntryGetLpaRef(PageEntry *this);

void pageEntrySetLpa(PageEntry *this, uint32_t lpa);


typedef struct PageEntryHandle
{
    struct PageCache *cache;
    PageEntry *entry;
} PageEntryHandle;


void pageEntryHandleInit(PageEntryHandle *this, struct PageCache *cache, PageEntry *entry);

void pageEntryHandleDestroy(PageEntryHandle *this);

void pageEntryHandleCopy(PageEntryHandle *this, const PageEntryHandle *other);

/**
 * @brief 尝试将 page entry 的 dirty 置位。如果是由本线程将 dirty 置位，则加入 page cache 的 dirty pages 集合。
 */
void pageEntryHandleMakeDirty(PageEntryHandle *this);


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
    GenericCacheManager cacheManager;

    // TODO 原代码里面这两个锁均使用自旋锁 spinlock_t，但飞腾的 bsp 没有 POSIX 标准的自旋锁实现，因此使用 mutex 替代。
    // 保护 cacheManager。
    mutex_t cacheLock;

    // 由于 dirtyPages 有范围 remove 需求，所以用类似 C++ 中的 map<blkoff, PageEntryHandle> 维护。
    // kbtree_t(ktdpn) * 类型不能定义在头文件中，因此这里用 void * 代替。
    void *dirtyPages;

    mutex_t dirtyPagesLock;

    size_t expectSize, curSize;
} PageCache;


void pageCacheInit(PageCache *this, size_t expectSize);

void pageCacheDestroy(PageCache *this);

/**
 * @brief 获取 blkoff 对应的 pageEntry。若不存在，则构造一个 pageEntry，返回其 handle。此时返回的 pageEntry中，反向映射和 4 KB 缓存是无效值。
 */
PageEntryHandle pageCacheGet(PageCache *this, uint32_t blkoff);

/**
 * @brief 截断文件后调用，将 page cache 内块偏移严格大于 maxBlkoff 的缓存 page 的 dirty 位清除，置位 invalid 状态，但不将它们删除，因为也许之后又会访问。调用者必须持有对应文件的 fileOpLock 独占锁（此时仅有一个线程能操作文件的 page cache）。
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
