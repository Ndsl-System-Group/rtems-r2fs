#include "sit_nat_cache.h"
#include "utils/rtfs_log.h"


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


static void sitNatCacheEntryHandleDoAddRef(SitNatCacheEntryHandle *this)
{
    // if (this->entry)
    //     this->cache->addRefcount(this->cache, this->entry);
}

static void sitNatCacheEntryHandleDoSubRef(SitNatCacheEntryHandle *this)
{
    // if (this->entry)
    //     this->cache->subRefcount(this->cache, this->entry);
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
    // if (this->entry) this->cache->addHostVersion(this->cache, this->entry);
}

void sitNatCacheEntryHandleAddSsdVersion(SitNatCacheEntryHandle *this)
{
    // if (this->entry) this->cache->addSsdVersion(this->cache, this->entry);
}
