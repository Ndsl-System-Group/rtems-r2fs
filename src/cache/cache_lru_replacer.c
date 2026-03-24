#include "cache_lru_replacer.h"

#include "uthash/utlist.h"

#include <stdlib.h>
#include <assert.h>


void cacheLruReplacerInit(CacheLruReplacer *this)
{
    assert(this);

    this->lruHead = NULL;
    this->pinHead = NULL;
    this->table = kh_init(khclr);

    this->size = 0;
}

void cacheLruReplacerDestroy(CacheLruReplacer *this)
{
    assert(this);

    khiter_t k;

    for (k = kh_begin(this->table); kh_end(this->table) != k; ++k)
    {
        if (kh_exist(this->table, k)) free(kh_value(this->table, k));
    }

    kh_destroy(khclr, this->table);

    this->lruHead = NULL;
    this->pinHead = NULL;
    this->table = NULL;

    this->size = 0;
}

void cacheLruReplacerAdd(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    khiter_t k = kh_get(khclr, this->table, key);
    assert(kh_end(this->table) == k);

    LruNode *node = malloc(sizeof(LruNode));
    assert(node);

    node->key = key;
    node->isPinned = false;

    DL_APPEND(this->lruHead, node);

    int res;
    k = kh_put(khclr, this->table, key, &res);
    kh_value(this->table, k) = node;

    ++this->size;
}

void cacheLruReplacerAccess(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    khiter_t k = kh_get(khclr, this->table, key);
    assert(kh_end(this->table) != k);

    LruNode *node = kh_value(this->table, k);

    if (node->isPinned)
    {
        if (this->pinHead && node == this->pinHead->prev) return;

        DL_DELETE(this->pinHead, node);
        DL_APPEND(this->pinHead, node);
    }
    else
    {
        if (this->lruHead && node == this->lruHead->prev) return;

        DL_DELETE(this->lruHead, node);
        DL_APPEND(this->lruHead, node);
    }
}

int cacheLruReplacerCanReplace(CacheLruReplacer *this)
{
    assert(this);

    return this->size > 0;
}

uint32_t cacheLruReplacerPop(CacheLruReplacer *this)
{
    assert(this);
    assert(this->size > 0);

    LruNode *node = this->lruHead;

    uint32_t key = node->key;

    DL_DELETE(this->lruHead, node);

    khiter_t k = kh_get(khclr, this->table, key);
    kh_del(khclr, this->table, k);

    free(node);

    --this->size;


    return key;
}

void cacheLruReplacerRemove(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    khiter_t k = kh_get(khclr, this->table, key);
    assert(kh_end(this->table) != k);

    LruNode *node = kh_value(this->table, k);

    if (node->isPinned)
    {
        DL_DELETE(this->pinHead, node);
    }
    else
    {
        DL_DELETE(this->lruHead, node);

        --this->size;
    }

    kh_del(khclr, this->table, k);

    free(node);
}

void cacheLruReplacerPin(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    khiter_t k = kh_get(khclr, this->table, key);
    if (kh_end(this->table) == k) return;

    LruNode *node = kh_value(this->table, k);

    if (node->isPinned) return;

    DL_DELETE(this->lruHead, node);

    node->isPinned = true;

    DL_APPEND(this->pinHead, node);

    --this->size;
}

void cacheLruReplacerUnpin(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    khiter_t k = kh_get(khclr, this->table, key);
    if (kh_end(this->table) == k) return;

    LruNode *node = kh_value(this->table, k);

    if (!node->isPinned) return;

    DL_DELETE(this->pinHead, node);

    node->isPinned = false;

    DL_APPEND(this->lruHead, node);

    ++this->size;
}
