#ifndef _PAGE_CACHE_H_
#define _PAGE_CACHE_H_

#include "cache/block_buffer.h"

#include "utils/types.h"
#include "utils/rtfs_multithread.h"

#include <stdatomic.h>


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
    pthread_mutex_t pageLock;

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

pthread_mutex_t *pageEntryGetLock(PageEntry *this);

BlockBuffer *pageEntryGetBuffer(PageEntry *this);

PageState pageEntryGetState(const PageEntry *this);

void pageEntrySetState(PageEntry *this, PageState state);

uint32_t pageEntryGetBlkoff(const PageEntry *this);

uint32_t pageEntryGetLpaRef(PageEntry *this);

void pageEntrySetLpa(PageEntry *this, uint32_t lpa);


#endif
