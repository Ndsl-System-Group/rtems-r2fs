#include "cache_index_manager.h"

#include <assert.h>


void cacheIndexManagerInit(CacheIndexManager *this)
{
    this->index = kh_init(khcim);
}

void cacheIndexManagerDestroy(CacheIndexManager *this)
{
    if (!this->index) return;

    khiter_t k;

    for (k = kh_begin(this->index); k != kh_end(this->index); ++k)
    {
        if (!kh_exist(this->index, k)) continue;

        free(kh_value(this->index, k)); // 释放 value。
    }

    kh_destroy(khcim, this->index);
    this->index = NULL;
}

void cacheIndexManagerAdd(CacheIndexManager *this, uint32_t key, void *value)
{
    int res;
    khiter_t k = kh_put(khcim, this->index, key, &res);

    // key 不能重复。
    assert(res != 0);

    kh_value(this->index, k) = value;
}

void *cacheIndexManagerGet(CacheIndexManager *this, uint32_t key)
{
    khiter_t k = kh_get(khcim, this->index, key);

    if (k == kh_end(this->index)) return NULL;


    return kh_value(this->index, k);
}

void *cacheIndexManagerRemove(CacheIndexManager *this, uint32_t key)
{
    khiter_t k = kh_get(khcim, this->index, key);

    if (k == kh_end(this->index)) return NULL;

    void *value = kh_value(this->index, k);

    kh_del(khcim, this->index, k);


    // 不释放。
    return value;
}

void cacheIndexManagerErase(CacheIndexManager *this, uint32_t key)
{
    khiter_t k = kh_get(khcim, this->index, key);

    if (k == kh_end(this->index)) return;

    free(kh_value(this->index, k));
    kh_del(khcim, this->index, k);
}
