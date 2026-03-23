#include "page_cache.h"

#include "fs/fs.h"
#include "utils/rtfs_log.h"


// 对 isDirty 进行 CAS 操作(由 false 更改为 true)，成功返回 true。
static bool pageEntryMarkDirty(PageEntry *this);


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


bool pageEntryMarkDirty(PageEntry *this)
{
    bool expect = false;


    return atomic_compare_exchange_strong(&this->isDirty, &expect, true);
}
