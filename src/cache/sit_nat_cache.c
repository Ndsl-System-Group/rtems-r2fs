#include "sit_nat_cache.h"
#include "utils/rtfs_log.h"
#include "utils/io_utils.h"

#include <assert.h>


/* 私有静态函数声明 */
static void sitNatCacheEntryHandleDoAddRef(SitNatCacheEntryHandle *this);

static void sitNatCacheEntryHandleDoSubRef(SitNatCacheEntryHandle *this);

static SitNatCacheEntry *sitNatCacheGetCacheEntryInner(SitNatCache *this, uint32_t lpa, bool isAccess);

static void sitNatCacheAddRefcount(SitNatCache *this, SitNatCacheEntry *entry);

static void sitNatCacheSubRefcount(SitNatCache *this, SitNatCacheEntry *entry);

static void sitNatCacheDoReplace(SitNatCache *this);

static void sitNatCacheReadLpa(SitNatCache *this, SitNatCacheEntry *entry);

static void sitNatCacheAddHostVersionForHandle(SitNatCache *this, SitNatCacheEntry *entry);

static void sitNatCacheAddSsdVersionForHandle(SitNatCache *this, SitNatCacheEntry *entry);


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
    if (this->entry)
    {
        // if (this->cache->putCacheEntry(this->cache, this->entry))
        // {
        //     RTFS_LOG(RTFS_LOG_WARNING, "exception during sub_refcount of SitNatCacheEntry");
        // }

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

void sitNatCacheInit(SitNatCache *this, struct comm_dev *device, size_t expectCacheSize)
{
    genericCacheManagerInit(&this->cacheManager);

    this->dev = device;
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


/* 私有静态函数实现 */
void sitNatCacheEntryHandleDoAddRef(SitNatCacheEntryHandle *this)
{
    if (this->entry) sitNatCacheAddRefcount(this->cache, this->entry);
}

void sitNatCacheEntryHandleDoSubRef(SitNatCacheEntryHandle *this)
{
    if (this->entry) sitNatCacheSubRefcount(this->cache, this->entry);
}

// 通过 lpa 在 cache_manager 中查找缓存项，找不到则从 SSD 读取。不增加缓存项的 refCount。
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

void sitNatCacheAddRefcount(SitNatCache *this, SitNatCacheEntry *entry)
{
    ++entry->refCount;

    // TODO
    //  if (1 == entry->refCount) this->cacheManager.pin(entry->lpa_);
}

void sitNatCacheSubRefcount(SitNatCache *this, SitNatCacheEntry *entry)
{
    --entry->refCount;

    // TODO
    // if (0 == entry->refCount)
    // {
    //     cache_manager.unpin(entry->lpa_);
    //     do_replace();
    // }
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
    // TODO
    // int res = comm_submit_sync_rw_request(dev, entry->cache.get_ptr(), LPA_TO_LBA(entry->lpa_), LBA_PER_LPA, COMM_IO_READ);
    // if (res != 0) throw io_error("SIT/NAT cache entry: read lpa failed.");
}

// entry 的主机侧 和 SSD 版本号管理。
void sitNatCacheAddHostVersionForHandle(SitNatCache *this, SitNatCacheEntry *entry)
{
    sitNatCacheAddRefcount(this, entry);
}

void sitNatCacheAddSsdVersionForHandle(SitNatCache *this, SitNatCacheEntry *entry)
{
    sitNatCacheSubRefcount(this, entry);
}
