#include "page_cache.h"

#include "fs/fs.h"
#include "utils/rtfs_log.h"


/* 私有静态函数声明 */

// 对 isDirty 进行 CAS 操作(由 false 更改为 true)，成功返回 true。
static bool pageEntryMarkDirty(PageEntry *this);

static void pageEntryHandleDoAddRef(PageEntryHandle *this);

static void pageEntryHandleDoSubRef(PageEntryHandle *this);


void pageEntryInit(PageEntry *this, uint32_t blkoff)
{
    this->blkoff = blkoff;
    this->lpa = INVALID_LPA;
    this->contentState = PAGE_INVALID;

    atomic_store(&this->refCount, 0);
    atomic_store(&this->isDirty, false);

    rtfsMutexInit(&this->pageLock);
    blockBufferInit(&this->page);
}

void pageEntryDestroy(PageEntry *this)
{
    rtfsMutexDestroy(&this->pageLock);
    blockBufferDestroy(&this->page);

    if (0 != this->refCount) RTFS_LOG(RTFS_LOG_WARNING, "page cache entry(blkoff = %u): refcount = %u while destructed.", this->blkoff, atomic_load(&this->refCount));

    if (0 != this->isDirty) RTFS_LOG(RTFS_LOG_WARNING, "page cache entry(blkoff = %u): still dirty while destructed.", this->blkoff);
}

pthread_mutex_t *pageEntryGetLock(PageEntry *this)
{
    return &this->pageLock;
}

BlockBuffer *pageEntryGetBuffer(PageEntry *this)
{
    return &this->page;
}

PageState pageEntryGetState(const PageEntry *this)
{
    return this->contentState;
}

void pageEntrySetState(PageEntry *this, PageState state)
{
    this->contentState = state;
}

uint32_t pageEntryGetBlkoff(const PageEntry *this)
{
    return this->blkoff;
}

uint32_t pageEntryGetLpaRef(PageEntry *this)
{
    return this->lpa;
}

void pageEntrySetLpa(PageEntry *this, uint32_t lpa)
{
    this->lpa = lpa;
}


void pageEntryHandleInit(PageEntryHandle *this, struct PageCache *cache, PageEntry *entry)
{
    this->cache = cache;
    this->entry = entry;
}

void pageEntryHandleDestroy(PageEntryHandle *this)
{
    // if (entry != nullptr)
    // {
    //     try
    //     {
    //         cache->sub_refcount(entry);
    //     }
    //     catch(const std::exception &e)
    //     {
    //         HSCFS_LOG(HSCFS_LOG_WARNING, "exception during sub_refcount of page cache entry: %s", e.what());
    //     }
    // }
}

void pageEntryHandleCopy(PageEntryHandle *this, const PageEntryHandle *other)
{
}

void pageEntryHandleMakeDirty(PageEntryHandle *this)
{
    // 若返回 true，说明是由本线程将 dirty 置位，因此本线程负责将其加入 cache 的 dirty page set。
    // if (entry->mark_dirty()) cache->add_to_dirty_pages(*this);
}


bool pageEntryMarkDirty(PageEntry *this)
{
    bool expect = false;


    return atomic_compare_exchange_strong(&this->isDirty, &expect, true);
}

void pageEntryHandleDoAddRef(PageEntryHandle *this)
{
    // if (entry != nullptr) cache->add_refcount(entry);
}

void pageEntryHandleDoSubRef(PageEntryHandle *this)
{
    // if (entry != nullptr) cache->sub_refcount(entry);
}
