#include "page_cache.h"

#include "fs/fs.h"
#include "utils/rtfs_log.h"
#include "utils/rtfs_exception.h"


// 对 isDirty 进行 CAS 操作(由 false 更改为 true)，成功返回 true。
static bool pageEntryMarkDirty(PageEntry *this);

static void pageEntryHandleDoAddRef(PageEntryHandle *this);

static void pageEntryHandleDoSubRef(PageEntryHandle *this);

// 调用者需要加 cacheLock，除非能够保证调用时 refCount 不会为 0。
static void pageCacheAddRefCount(PageCache *this, PageEntry *entry);

static void pageCacheSubRefCount(PageCache *this, PageEntry *entry);

static void pageCacheDoReplace(PageCache *this);

// 由 PageEntryHandle 的 markDirty 方法调用。调用时能保证 refCount 不为 0，因为发起调用的 PageEntryHandle 仍有效，所以内部不加 cacheLock 锁。
static void pageCacheAddToDirtyPages(PageCache *this, PageEntryHandle *page);


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
    CEXCEPTION_T e;

    if (NULL != this->entry)
    {
        Try
        {
            pageCacheSubRefCount(this->cache, this->entry);
        }
        Catch(e)
        {
            RTFS_LOG(RTFS_LOG_WARNING, "exception during sub_refcount of page cache entry: %d", e);
        }
    }
}

void pageEntryHandleCopy(PageEntryHandle *this, const PageEntryHandle *other)
{
    this->cache = other->cache;
    this->entry = other->entry;

    // handle 拷贝，能保证 refCount 不为 0。
    pageEntryHandleDoAddRef(this);
}

void pageEntryHandleMakeDirty(PageEntryHandle *this)
{
    // 若返回 true，说明是由本线程将 dirty 置位，因此本线程负责将其加入 cache 的 dirty page set。
    if (pageEntryMarkDirty(this->entry)) pageCacheAddToDirtyPages(this->cache, this);
}


void pageCacheInit(PageCache *this, size_t expectSize)
{
    int res = rtfsSpinInit(&this->cacheLock);
    if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "page cache: init cache spin failed.");

    // 任意对锁初始化失败的异常均为 panic，上层应该捕获异常并终止程序，故此处不再释放成功初始化的锁。
    res = rtfsSpinInit(&this->dirtyPagesLock);
    if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "page cache: init dirty page set spin failed.");

    genericCacheManagerInit(&this->cacheManager);

    this->dirtyPages = kb_init(ktdpn, KB_DEFAULT_SIZE);
    if (NULL == this->dirtyPages) THROW_FATAL_MESSAGE(EXIT_FAILURE, "page cache: init dirty pages tree failed.");

    this->expectSize = expectSize;
    this->curSize = 0;
}

void pageCacheDestroy(PageCache *this)
{
    // page cache 析构内不会并发访问，不加锁。
    if (0 != kb_size(this->dirtyPages)) RTFS_LOG(RTFS_LOG_WARNING, "Page cache still has dirty page while destructed. If delete a file which written but not synchronized, this will be OK.");

    this->curSize = 0;
    this->expectSize = 0;

    kb_destroy(ktdpn, this->dirtyPages);
    this->dirtyPages = NULL;

    genericCacheManagerDestroy(&this->cacheManager);

    rtfsSpinDestroy(&this->dirtyPagesLock);
    rtfsSpinDestroy(&this->cacheLock);
}

// TODO
PageEntryHandle pageCacheGet(PageCache *this, uint32_t blkoff)
{
}

void pageCacheTruncate(PageCache *this, uint32_t maxBlkoff)
{
}

kbtree_t(ktdpn) * pageCacheGetDirtyPages(PageCache *this)
{
    return this->dirtyPages;
}

void pageCacheClearDirtyPages(PageCache *this)
{
    rtfsSpinLock(&this->dirtyPagesLock);

    kbitr_t itr;
    for (kb_itr_first(ktdpn, this->dirtyPages, &itr); kb_itr_valid(&itr); kb_itr_next(ktdpn, this->dirtyPages, &itr))
    {
        DirtyPagesNode *node = &kb_itr_key(DirtyPagesNode, &itr);

        node->handle.entry->isDirty = false;
    }

    // kbtree 未提供 clear 接口。用销毁加重建模拟。
    kb_destroy(ktdpn, this->dirtyPages);
    this->dirtyPages = kb_init(ktdpn, KB_DEFAULT_SIZE);

    rtfsSpinUnlock(&this->dirtyPagesLock);
}


bool pageEntryMarkDirty(PageEntry *this)
{
    bool expect = false;


    return atomic_compare_exchange_strong(&this->isDirty, &expect, true);
}

void pageEntryHandleDoAddRef(PageEntryHandle *this)
{
    if (NULL != this->entry) pageCacheAddRefCount(this->cache, this->entry);
}

void pageEntryHandleDoSubRef(PageEntryHandle *this)
{
    if (NULL != this->entry) pageCacheSubRefCount(this->cache, this->entry);
}

void pageCacheAddRefCount(PageCache *this, PageEntry *entry)
{
}

void pageCacheSubRefCount(PageCache *this, PageEntry *entry)
{
}

void pageCacheDoReplace(PageCache *this)
{
}

void pageCacheAddToDirtyPages(PageCache *this, PageEntryHandle *page)
{
}
