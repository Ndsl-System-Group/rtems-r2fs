#include "page_cache.h"

#include "fs/fs.h"
#include "utils/rtfs_log.h"
#include "utils/rtfs_exception.h"

#include <assert.h>


#define DIRTY_PAGES_NODE_CMP(a, b) ((a).blkoff < (b).blkoff ? -1 : ((a).blkoff > (b).blkoff ? 1 : 0))

KBTREE_INIT(ktdpn, DirtyPagesNode, DIRTY_PAGES_NODE_CMP)

static kbtree_t(ktdpn) * getDirtyPagesTree(PageCache *this);


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

    if (false != atomic_load(&this->isDirty)) RTFS_LOG(RTFS_LOG_WARNING, "page cache entry(blkoff = %u): still dirty while destructed.", this->blkoff);
}

mutex_t *pageEntryGetLock(PageEntry *this)
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
    int res = rtfsMutexInit(&this->cacheLock);
    if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "page cache: init cache mutex failed.");

    // 任意对锁初始化失败的异常均为 panic，上层应该捕获异常并终止程序，故此处不再释放成功初始化的锁。
    res = rtfsMutexInit(&this->dirtyPagesLock);
    if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "page cache: init dirty page set mutex failed.");

    genericCacheManagerInit(&this->cacheManager);

    this->dirtyPages = kb_init(ktdpn, KB_DEFAULT_SIZE);
    if (NULL == this->dirtyPages) THROW_FATAL_MESSAGE(EXIT_FAILURE, "page cache: init dirty pages tree failed.");

    this->expectSize = expectSize;
    this->curSize = 0;
}

void pageCacheDestroy(PageCache *this)
{
    // page cache 析构内不会并发访问，不加锁。
    if (0 != kb_size(getDirtyPagesTree(this))) RTFS_LOG(RTFS_LOG_WARNING, "Page cache still has dirty page while destructed. If delete a file which written but not synchronized, this will be OK.");

    this->curSize = 0;
    this->expectSize = 0;

    kb_destroy(ktdpn, getDirtyPagesTree(this));
    this->dirtyPages = NULL;

    genericCacheManagerDestroy(&this->cacheManager);

    rtfsMutexDestroy(&this->dirtyPagesLock);
    rtfsMutexDestroy(&this->cacheLock);
}

PageEntryHandle pageCacheGet(PageCache *this, uint32_t blkoff)
{
    rtfsMutexLock(&this->cacheLock);

    PageEntry *entry = (PageEntry *)genericCacheManagerGet(&this->cacheManager, blkoff, true);

    // 缓存项不存在，则需要新建一个。
    if (NULL == entry)
    {
        // 如果当前缓存数量已达到阈值，则尝试置换一个，直接把置换出的 pageEntry 资源移动给新缓存项。如果无法置换，再新建。防止多次分配和释放 4 KB block。
        if (this->curSize >= this->expectSize)
        {
            PageEntry *victim = (PageEntry *)genericCacheManagerReplaceOne(&this->cacheManager);
            if (NULL != victim)
            {
                assert(0 == victim->refCount && false == atomic_load(&victim->isDirty));

                RTFS_LOG(RTFS_LOG_INFO, "replace page cache entry, blkoff = %u", victim->blkoff);

                victim->blkoff = blkoff;
                victim->contentState = PAGE_INVALID;

                entry = victim;

                genericCacheManagerAdd(&this->cacheManager, blkoff, victim);
                pageCacheAddRefCount(this, entry);

                // 尝试将先前无法淘汰的缓存项淘汰掉，否则 pageCache 中缓存项数量单调不减。
                pageCacheDoReplace(this);
            }
        }

        // 到此处，说明要么缓存容量充足，要么找不到能置换的缓存项，则直接增加一项。
        if (NULL == entry)
        {
            RTFS_LOG(RTFS_LOG_DEBUG, "sizeof PageEntry: %d", sizeof(PageEntry));
            PageEntry *newEntry = (PageEntry *)malloc(sizeof(PageEntry));
            RTFS_LOG(RTFS_LOG_DEBUG, "newEntry by malloc: %p", newEntry);
            assert(NULL != newEntry);

            pageEntryInit(newEntry, blkoff);
            entry = newEntry;

            genericCacheManagerAdd(&this->cacheManager, blkoff, entry);
            pageCacheAddRefCount(this, entry);
            ++this->curSize;
        }
    }
    // 缓存项存在，添加引用计数即可。
    else
    {
        pageCacheAddRefCount(this, entry);
    }

    rtfsMutexUnlock(&this->cacheLock);

    PageEntryHandle res = {.cache = this, .entry = entry};


    return res;
}

void pageCacheTruncate(PageCache *this, uint32_t maxBlkoff)
{
    // 由于调用者已经加了 fileOpLock，内部不用加任何锁了。
    // 找到大于等于 maxBlkoff 的第一个元素。
    DirtyPagesNode query = {.blkoff = maxBlkoff};
    DirtyPagesNode *lower = NULL;
    DirtyPagesNode *upper = NULL;

    // kbtree 插入的数据不允许 key 重复，若重复插入相同的 key 数据，后续的数据会被丢弃。
    // lower 找到的是最后一个 <= query 的元素。upper 找到的是第一个 >= query 的元素。如果存在等于 query 的 key，lower 会直接指向该元素，同理 upper。
    kb_interval(ktdpn, getDirtyPagesTree(this), query, &lower, &upper);
    if (!upper) return;

    // 如果命中等于 maxBlkoff，需要跳到下一个（即 > maxBlkoff）
    if (upper->blkoff <= maxBlkoff)
    {
        query.blkoff = 1 + maxBlkoff;
        kb_interval(ktdpn, getDirtyPagesTree(this), query, &lower, &upper);
    }

    // 和 kbtree_test KbtreeRangeEraseTest 的算法相同，因为树的结构会发生改变，不能依赖原来树的指针。
    while (upper)
    {
        DirtyPagesNode node = *upper;
        uint32_t k = node.blkoff;

        // 将范围外的所有 page 的 dirty 标记清除，标记为 invalid。
        atomic_store(&node.handle.entry->isDirty, false);
        node.handle.entry->contentState = PAGE_INVALID;

        // 范围外的 page 将被移除，所以递减 curSize。
        --this->curSize;

        // 从 dirty pages 集合中移除范围外的所有 page。
        kb_del(ktdpn, getDirtyPagesTree(this), node);

        // 查找后继（严格大于 k）。
        query.blkoff = k;
        kb_interval(ktdpn, getDirtyPagesTree(this), query, &lower, &upper);
    }
}

void *pageCacheGetDirtyPages(PageCache *this)
{
    return this->dirtyPages;
}

void pageCacheClearDirtyPages(PageCache *this)
{
    rtfsMutexLock(&this->dirtyPagesLock);

    kbitr_t itr;
    for (kb_itr_first(ktdpn, getDirtyPagesTree(this), &itr); kb_itr_valid(&itr); kb_itr_next(ktdpn, getDirtyPagesTree(this), &itr))
    {
        DirtyPagesNode *node = &kb_itr_key(DirtyPagesNode, &itr);

        atomic_store(&node->handle.entry->isDirty, false);
    }

    // kbtree 未提供 clear 接口。用销毁加重建模拟。
    kb_destroy(ktdpn, getDirtyPagesTree(this));
    this->dirtyPages = kb_init(ktdpn, KB_DEFAULT_SIZE);

    rtfsMutexUnlock(&this->dirtyPagesLock);
}


kbtree_t(ktdpn) * getDirtyPagesTree(PageCache *this)
{
    return (kbtree_t(ktdpn) *)this->dirtyPages;
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
    if (0 == atomic_fetch_add(&entry->refCount, 1))
    {
        // 先前引用计数为 0，不可能是 dirty 状态。此时 refCount 由 0 增至 1，且加了 cacheLock，assert 访问 entry 是安全的（此时只可能在此处访问）。
        atomic_store(&entry->isDirty, false);
        assert(false == atomic_load(&entry->isDirty));

        // 可能出现 refCount 减到0，但减少 refCount 的线程还没来得及 unpin 的情况（见 subRefcount）。由于 GenericCacheManager 使用的 lruReplacer 允许重复调用 pin，所以不会造成影响。
        genericCacheManagerPin(&this->cacheManager, entry->blkoff);
    }
}

void pageCacheSubRefCount(PageCache *this, PageEntry *entry)
{
    // 由调用者线程将引用计数减为 0，此时应当考虑 unpin。
    if (1 == atomic_fetch_sub(&entry->refCount, 1))
    {
        // 加 cacheLock，再次检查引用计数。成功获取锁后，如果 refCount 仍为 0，则不可能有其它线程能够修改 refCount，因为 refCount 从 0 增加到 1，一定是通过调用 PageCache 的 get 方法，而该方法在加 refCount 前需要加 cacheLock。此时将其 unpin，然后解锁。
        rtfsMutexLock(&this->cacheLock);

        if (0 == atomic_load(&entry->refCount))
        {
            // 若引用计数减为 0，不可能是 dirty 状态。此时 refCount 由 1 减至 0，且加了 cacheLock，访问 entry 是安全的。
            atomic_store(&entry->isDirty, false);
            assert(false == atomic_load(&entry->isDirty));

            genericCacheManagerUnpin(&this->cacheManager, entry->blkoff);
        }

        // 如果 refCount 此时不为 0，说明加锁前有其它线程再次通过 get 获取引用计数，所以放弃 unpin。

        rtfsMutexUnlock(&this->cacheLock);
    }
}

void pageCacheDoReplace(PageCache *this)
{
    if (this->curSize > this->expectSize)
    {
        while (true)
        {
            PageEntry *entry = (PageEntry *)genericCacheManagerReplaceOne(&this->cacheManager);

            if (NULL != entry)
            {
                assert(0 == entry->refCount && false == atomic_load(&entry->isDirty));

                --this->curSize;

                RTFS_LOG(RTFS_LOG_INFO, "replace page cache entry, blkoff = %u", entry->blkoff);
            }

            if (NULL == entry || this->curSize <= this->expectSize) break;
        }
    }
}

void pageCacheAddToDirtyPages(PageCache *this, PageEntryHandle *page)
{
    rtfsMutexLock(&this->dirtyPagesLock);

    assert(atomic_load(&page->entry->refCount) >= 1);

    DirtyPagesNode node = {.blkoff = page->entry->blkoff, .handle = *page};

    kb_put(ktdpn, getDirtyPagesTree(this), node);

    rtfsMutexUnlock(&this->dirtyPagesLock);
}
