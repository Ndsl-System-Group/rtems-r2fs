#include "sit_nat_cache.h"

#include "cache/block_buffer.h"
#include "utils/rtfs_log.h"
#include "utils/io_utils.h"
#include "utils/rtfs_exception.h"
#include "communication/comm_api.h"

#include <assert.h>


/* 私有静态函数声明 */
static void sitNatCacheEntryHandleDoAddRef(SitNatCacheEntryHandle *this);

static void sitNatCacheEntryHandleDoSubRef(SitNatCacheEntryHandle *this);

// 释放对 entry 的引用。会对缓存项调用 subRefcount。
static void sitNatCachePutCacheEntry(SitNatCache *this, SitNatCacheEntry *entry);

// 将 entry 的引用计数 +1。如果引用计数之前为 0，则 pin 住缓存项。
static void sitNatCacheAddRefcount(SitNatCache *this, SitNatCacheEntry *entry);

// 将 entry 的引用计数 -1。如果引用计数减至 0，则 unpin 缓存项。
static void sitNatCacheSubRefcount(SitNatCache *this, SitNatCacheEntry *entry);

// entry 的主机侧和 SSD 版本号管理。
static void sitNatCacheAddHostVersionForHandle(SitNatCache *this, SitNatCacheEntry *entry);

static void sitNatCacheAddSsdVersionForHandle(SitNatCache *this, SitNatCacheEntry *entry);

// 通过 lpa 在 cache_manager 中查找缓存项，找不到则从 SSD 读取。不增加缓存项的 refCount。
static SitNatCacheEntry *sitNatCacheGetCacheEntryInner(SitNatCache *this, uint32_t lpa, bool isAccess);

static void sitNatCacheDoReplace(SitNatCache *this);

static void sitNatCacheReadLpa(SitNatCache *this, SitNatCacheEntry *entry);

static sit_nat_cache_read_block_hook g_sit_nat_cache_read_block_hook = NULL;


/* 公共函数实现 */
void sitNatCacheEntryInit(SitNatCacheEntry *this, uint32_t lpa)
{
    this->lpa = lpa;
    this->refCount = 0;
    blockBufferInit(&this->cache);
}

void sitNatCacheEntryDestroy(SitNatCacheEntry *this)
{
    if (this->refCount > 0)
    {
        RTFS_LOG(RTFS_LOG_WARNING, "SIT/NAT cache entry has non-zero refCount when destructed, refCount = %u, lpa = %u", this->refCount, this->lpa);
    }
    else
    {
        this->lpa = 0;
        this->refCount = 0;
        blockBufferDestroy(&this->cache);
    }
}


void sitNatCacheEntryHandleInit(SitNatCacheEntryHandle *this, struct SitNatCache *cache, SitNatCacheEntry *entry)
{
    this->cache = cache;
    this->entry = entry;
}

void sitNatCacheEntryHandleDestroy(SitNatCacheEntryHandle *this)
{
    CEXCEPTION_T e;

    if (this->entry)
    {
        Try
        {
            sitNatCachePutCacheEntry(this->cache, this->entry);
        }
        Catch(e)
        {
            RTFS_LOG(RTFS_LOG_WARNING, "exception during sub_refcount of SitNatCacheEntry: %d", e);
        }

        this->entry = NULL;
        this->cache = NULL;
    }
}

void sitNatCacheEntryHandleCopy(SitNatCacheEntryHandle *this, const SitNatCacheEntryHandle *other)
{
    this->cache = other->cache;
    this->entry = other->entry;

    sitNatCacheEntryHandleDoAddRef(this);
}

void sitNatCacheEntryHandleAddHostVersion(SitNatCacheEntryHandle *this)
{
    if (this->entry) sitNatCacheAddHostVersionForHandle(this->cache, this->entry);
}

void sitNatCacheEntryHandleAddSsdVersion(SitNatCacheEntryHandle *this)
{
    if (this->entry) sitNatCacheAddSsdVersionForHandle(this->cache, this->entry);
}

struct RtfsNatBlock *sitNatCacheEntryHandleGetNatBlockPtr(SitNatCacheEntryHandle *this)
{
    return (struct RtfsNatBlock *)blockBufferGetPtr(&this->entry->cache);
}

struct RtfsSitBlock *sitNatCacheEntryHandleGetSitBlockPtr(SitNatCacheEntryHandle *this)
{
    return (struct RtfsSitBlock *)blockBufferGetPtr(&this->entry->cache);
}


void sitNatCacheInit(SitNatCache *this, struct comm_dev *dev, size_t expectCacheSize)
{
    genericCacheManagerInit(&this->cacheManager);

    this->dev = dev;
    this->expectSize = expectCacheSize;
    this->curSize = 0;
}

void sitNatCacheDestroy(SitNatCache *this)
{
    genericCacheManagerDestroy(&this->cacheManager);

    this->dev = NULL;
    this->expectSize = 0;
    this->curSize = 0;
}

SitNatCacheEntryHandle sitNatCacheGet(SitNatCache *this, uint32_t lpa)
{
    SitNatCacheEntry *entry = sitNatCacheGetCacheEntryInner(this, lpa, true);

    sitNatCacheAddRefcount(this, entry);
    sitNatCacheDoReplace(this);

    // handle 为普通 struct（仅包含指针），按值返回是安全的。entry 的生命周期由引用计数控制，而非局部变量 handle 控制。
    SitNatCacheEntryHandle handle;
    sitNatCacheEntryHandleInit(&handle, this, entry);


    return handle;
}

void sitNatCacheAddSsdVersion(SitNatCache *this, uint32_t lpa)
{
    SitNatCacheEntry *entry = sitNatCacheGetCacheEntryInner(this, lpa, false);

    sitNatCacheSubRefcount(this, entry);
}

void sitNatCacheSetReadBlockHook(sit_nat_cache_read_block_hook hook)
{
    g_sit_nat_cache_read_block_hook = hook;
}


/* 私有静态函数实现 */
void sitNatCacheEntryHandleDoAddRef(SitNatCacheEntryHandle *this)
{
    if (this->entry) sitNatCacheAddRefcount(this->cache, this->entry);
}

void sitNatCacheEntryHandleDoSubRef(SitNatCacheEntryHandle *this)
{
    if (this->entry) sitNatCacheSubRefcount(this->cache, this->entry);
}

void sitNatCachePutCacheEntry(SitNatCache *this, SitNatCacheEntry *entry)
{
    sitNatCacheSubRefcount(this, entry);
}

void sitNatCacheAddRefcount(SitNatCache *this, SitNatCacheEntry *entry)
{
    ++entry->refCount;

    if (1 == entry->refCount) genericCacheManagerPin(&this->cacheManager, entry->lpa);
}

void sitNatCacheSubRefcount(SitNatCache *this, SitNatCacheEntry *entry)
{
    --entry->refCount;

    if (0 == entry->refCount)
    {
        genericCacheManagerUnpin(&this->cacheManager, entry->lpa);

        sitNatCacheDoReplace(this);
    }
}

void sitNatCacheAddHostVersionForHandle(SitNatCache *this, SitNatCacheEntry *entry)
{
    sitNatCacheAddRefcount(this, entry);
}

void sitNatCacheAddSsdVersionForHandle(SitNatCache *this, SitNatCacheEntry *entry)
{
    sitNatCacheSubRefcount(this, entry);
}

SitNatCacheEntry *sitNatCacheGetCacheEntryInner(SitNatCache *this, uint32_t lpa, bool isAccess)
{
    SitNatCacheEntry *entry = genericCacheManagerGet(&this->cacheManager, lpa, isAccess);

    // 如果缓存不命中，从 SSD 中读取。
    if (!entry)
    {
        entry = malloc(sizeof(SitNatCacheEntry));

        sitNatCacheEntryInit(entry, lpa);

        sitNatCacheReadLpa(this, entry);

        ++this->curSize;

        genericCacheManagerAdd(&this->cacheManager, lpa, entry);
    }


    return entry;
}

void sitNatCacheDoReplace(SitNatCache *this)
{
    if (this->curSize > this->expectSize)
    {
        while (true)
        {
            SitNatCacheEntry *entry = genericCacheManagerReplaceOne(&this->cacheManager);

            if (entry)
            {
                // 正确性检查，被置换出来的缓存项引用计数应该为 0。
                assert(0 == entry->refCount);

                --this->curSize;

                RTFS_LOG(RTFS_LOG_INFO, "relpace SIT/NAT cache entry, lpa = %u", entry->lpa);
            }

            if (entry || this->curSize <= this->expectSize) break;
        }
    }
}

void sitNatCacheReadLpa(SitNatCache *this, SitNatCacheEntry *entry)
{
    if (g_sit_nat_cache_read_block_hook != NULL)
    {
        int res = g_sit_nat_cache_read_block_hook(this->dev, entry->lpa, blockBufferGetPtr(&entry->cache));
        if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "SIT/NAT cache entry: read lpa failed.");

        return;
    }

    int res = comm_submit_sync_rw_request(this->dev, blockBufferGetPtr(&entry->cache), LPA_TO_LBA(entry->lpa), LBA_PER_LPA, COMM_IO_READ);
    if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "SIT/NAT cache entry: read lpa failed.");
}
