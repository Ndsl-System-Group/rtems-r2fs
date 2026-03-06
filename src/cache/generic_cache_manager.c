#include "generic_cache_manager.h"

#include <assert.h>


void genericCacheManagerInit(GenericCacheManager *this)
{
    assert(this);

    cacheIndexManagerInit(&this->index);
    cacheLruReplacerInit(&this->replacer);
}

void genericCacheManagerDestroy(GenericCacheManager *this)
{
    assert(this);

    cacheIndexManagerDestroy(&this->index);
    cacheLruReplacerDestroy(&this->replacer);
}

void genericCacheManagerAdd(GenericCacheManager *this, uint32_t key, void *entry)
{
    assert(this);
    assert(entry);

    cacheIndexManagerAdd(&this->index, key, entry);
    cacheLruReplacerAdd(&this->replacer, key);
}

void *genericCacheManagerGet(GenericCacheManager *this, uint32_t key, bool is_access)
{
    assert(this);

    void *entry = cacheIndexManagerGet(&this->index, key);
    if (entry && is_access) cacheLruReplacerAccess(&this->replacer, key);


    return entry;
}

void *genericCacheManagerReplaceOne(GenericCacheManager *this)
{
    assert(this);

    if (!cacheLruReplacerCanReplace(&this->replacer)) return NULL;

    uint32_t key = cacheLruReplacerPop(&this->replacer);


    return cacheIndexManagerRemove(&this->index, key);
}

void *genericCacheManagerRemove(GenericCacheManager *this, uint32_t key)
{
    assert(this);

    cacheLruReplacerRemove(&this->replacer, key);


    return cacheIndexManagerRemove(&this->index, key);
}

void genericCacheManagerPin(GenericCacheManager *this, uint32_t key)
{
    assert(this);

    cacheLruReplacerPin(&this->replacer, key);
}

void genericCacheManagerUnpin(GenericCacheManager *this, uint32_t key)
{

    assert(this);

    cacheLruReplacerUnpin(&this->replacer, key);
}
