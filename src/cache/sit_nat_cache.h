#ifndef _SIT_NAT_CACHE_H_
#define _SIT_NAT_CACHE_H_

#include "utils/types.h"
#include "cache/block_buffer.h"
#include "cache/generic_cache_manager.h"


struct SitNatCache;
struct comm_dev;


typedef struct SitNatCacheEntry
{
    uint32_t lpa;
    uint32_t refCount;
    BlockBuffer cache;
} SitNatCacheEntry;

void sitNatCacheEntryInit(SitNatCacheEntry *this, uint32_t lpa);

void sitNatCacheEntryDestroy(SitNatCacheEntry *this);


typedef struct SitNatCacheEntryHandle
{
    struct SitNatCache *cache;
    SitNatCacheEntry *entry;
} SitNatCacheEntryHandle;

void sitNatCacheEntryHandleInit(SitNatCacheEntryHandle *this, struct SitNatCache *cache, SitNatCacheEntry *entry);

void sitNatCacheEntryHandleDestroy(SitNatCacheEntryHandle *this);

void sitNatCacheEntryHandleCopy(SitNatCacheEntryHandle *this, const SitNatCacheEntryHandle *other);

void sitNatCacheEntryHandleAddHostVersion(SitNatCacheEntryHandle *this);

void sitNatCacheEntryHandleAddSsdVersion(SitNatCacheEntryHandle *this);


/**
 * @brief SIT、NAT 缓存控制器。以 lpa(uint32_t) 作为 key，SitNatCacheEntry 作为缓存项。
 */
typedef struct SitNatCache
{
    GenericCacheManager cacheManager;
    size_t expectSize, curSize;
    struct comm_dev *dev;
} SitNatCache;

void sitNatCacheInit(SitNatCache *this, struct comm_dev *device, size_t expectCacheSize);

void sitNatCacheDestroy(SitNatCache *this);

SitNatCacheEntryHandle sitNatCacheGet(SitNatCache *this, uint32_t lpa);

void sitNatCacheAddSsdVersion(SitNatCache *this, uint32_t lpa);


#endif
