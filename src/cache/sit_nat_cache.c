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
