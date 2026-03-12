#include "cache_lru_replacer.h"

#include "uthash/utlist.h"

#include <stdlib.h>
#include <assert.h>


void cacheLruReplacerInit(CacheLruReplacer *this)
{
    assert(this);

    this->lruHead = NULL;
    this->pinHead = NULL;
    this->table = NULL;

    this->size = 0;
}

void cacheLruReplacerDestroy(CacheLruReplacer *this)
{
    assert(this);

    LruNode *node, *tmp;

    HASH_ITER(hh, this->table, node, tmp)
    {
        HASH_DEL(this->table, node);
        free(node);
    }

    this->lruHead = NULL;
    this->pinHead = NULL;
    this->table = NULL;

    this->size = 0;
}

void cacheLruReplacerAdd(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    LruNode *node = NULL;

    HASH_FIND_INT(this->table, &key, node);
    assert(node == NULL);

    node = malloc(sizeof(LruNode));
    assert(node);

    node->key = key;
    node->isPinned = false;

    DL_APPEND(this->lruHead, node);

    HASH_ADD_INT(this->table, key, node);

    ++this->size;
}

void cacheLruReplacerAccess(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    LruNode *node = NULL;

    HASH_FIND_INT(this->table, &key, node);
    assert(node);

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

    HASH_DEL(this->table, node);

    free(node);

    --this->size;


    return key;
}

void cacheLruReplacerRemove(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    LruNode *node = NULL;

    HASH_FIND_INT(this->table, &key, node);
    assert(node);

    if (node->isPinned)
    {
        DL_DELETE(this->pinHead, node);
    }
    else
    {
        DL_DELETE(this->lruHead, node);

        --this->size;
    }

    HASH_DEL(this->table, node);

    free(node);
}

void cacheLruReplacerPin(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    LruNode *node = NULL;

    HASH_FIND_INT(this->table, &key, node);

    if (!node) return;

    if (node->isPinned) return;

    DL_DELETE(this->lruHead, node);

    node->isPinned = true;

    DL_APPEND(this->pinHead, node);

    --this->size;
}

void cacheLruReplacerUnpin(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    LruNode *node = NULL;

    HASH_FIND_INT(this->table, &key, node);

    if (!node) return;

    if (!node->isPinned) return;

    DL_DELETE(this->pinHead, node);

    node->isPinned = false;

    DL_APPEND(this->lruHead, node);

    ++this->size;
}
