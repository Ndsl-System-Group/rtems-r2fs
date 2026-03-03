#ifndef _SIT_NAT_CACHE_H_
#define _SIT_NAT_CACHE_H_

#include "utils/types.h"
#include "cache/block_buffer.h"


typedef struct
{
    uint32_t lpa;
    uint32_t refCount;
    BlockBuffer cache;
} SitNatCacheEntry;

void sitNatCacheEntryInit(SitNatCacheEntry *this, uint32_t lpa);

void sitNatCacheEntryDestroy(SitNatCacheEntry *this);


struct SitNatCache;

typedef struct
{
    struct SitNatCache *cache;
    SitNatCacheEntry *entry;
} SitNatCacheEntryHandle;

void sitNatCacheEntryHandleInit(SitNatCacheEntryHandle *this, struct SitNatCache *cache, SitNatCacheEntry *entry);

void sitNatCacheEntryHandleDestroy(SitNatCacheEntryHandle *this);

void sitNatCacheEntryHandleCopy(SitNatCacheEntryHandle *this, const SitNatCacheEntryHandle *other);

void sitNatCacheEntryHandleAddHostVersion(SitNatCacheEntryHandle *this);

void sitNatCacheEntryHandleAddSsdVersion(SitNatCacheEntryHandle *this);


typedef struct SitNatCache
{
};


#endif
