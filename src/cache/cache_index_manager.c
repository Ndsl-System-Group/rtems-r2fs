#include "cache_index_manager.h"

#include <assert.h>


void cacheIndexManagerInit(CacheIndexManager *this)
{
    this->index = NULL;
}

void cacheIndexManagerDestroy(CacheIndexManager *this)
{
    CacheEntry *entry, *tmp;
    HASH_ITER(hh, this->index, entry, tmp)
    {
        HASH_DEL(this->index, entry);
        free(entry->value); // 释放 value
        free(entry);        // 释放条目
    }
    this->index = NULL;
}

void cacheIndexManagerAdd(CacheIndexManager *this, uint32_t key, void *value)
{
    assert(cacheIndexManagerGet(this, key) == NULL); // key 不能重复

    CacheEntry *entry = (CacheEntry *)malloc(sizeof(CacheEntry));
    entry->key = key;
    entry->value = value;

    HASH_ADD_INT(this->index, key, entry);
}

void *cacheIndexManagerGet(CacheIndexManager *this, uint32_t key)
{
    CacheEntry *entry;

    HASH_FIND_INT(this->index, &key, entry);


    return entry ? entry->value : NULL;
}

void *cacheIndexManagerRemove(CacheIndexManager *this, uint32_t key)
{
    CacheEntry *entry;

    HASH_FIND_INT(this->index, &key, entry);
    if (!entry) return NULL;

    void *value = entry->value;
    HASH_DEL(this->index, entry);

    free(entry);


    return value;
}

void cacheIndexManagerErase(CacheIndexManager *this, CacheEntry *cacheEntry)
{
    if (!cacheEntry) return;

    HASH_DEL(this->index, cacheEntry);

    free(cacheEntry->value);
    free(cacheEntry);
}
