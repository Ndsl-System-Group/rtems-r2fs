#include "cache_lru_replacer.h"

#include <stdlib.h>
#include <assert.h>


static void detachFromLru(CacheLruReplacer *this, LruNode *node);

static void attachToLruTail(CacheLruReplacer *this, LruNode *node);

static void detachFromPin(CacheLruReplacer *this, LruNode *node);

static void attachToPinTail(CacheLruReplacer *this, LruNode *node);


void cacheLruReplacerInit(CacheLruReplacer *this)
{
    assert(this);

    this->head = NULL;
    this->tail = NULL;

    this->pinHead = NULL;
    this->pinTail = NULL;

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

    this->head = NULL;
    this->tail = NULL;

    this->pinHead = NULL;
    this->pinTail = NULL;

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
    node->isPinned = 0;
    node->prev = node->next = NULL;

    attachToLruTail(this, node);

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
        if (node == this->pinTail) return;

        detachFromPin(this, node);
        attachToPinTail(this, node);
    }
    else
    {
        if (node == this->tail) return;

        detachFromLru(this, node);
        attachToLruTail(this, node);
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

    LruNode *node = this->head;

    uint32_t key = node->key;

    detachFromLru(this, node);

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
        detachFromPin(this, node);
    }
    else
    {
        detachFromLru(this, node);
        this->size--;
    }

    HASH_DEL(this->table, node);

    free(node);
}

void cacheLruReplacerPin(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    LruNode *node = NULL;

    HASH_FIND_INT(this->table, &key, node);

    if (!node)
        return;

    if (node->isPinned)
        return;

    detachFromLru(this, node);

    node->isPinned = 1;

    attachToPinTail(this, node);

    --this->size;
}

void cacheLruReplacerUnpin(CacheLruReplacer *this, uint32_t key)
{
    assert(this);

    LruNode *node = NULL;

    HASH_FIND_INT(this->table, &key, node);

    if (!node) return;

    if (!node->isPinned) return;

    detachFromPin(this, node);

    node->isPinned = 0;

    attachToLruTail(this, node);

    ++this->size;
}


static void detachFromLru(CacheLruReplacer *this, LruNode *node)
{
    if (node->prev)
    {
        node->prev->next = node->next;
    }
    else
    {
        this->head = node->next;
    }

    if (node->next)
    {
        node->next->prev = node->prev;
    }
    else
    {
        this->tail = node->prev;
    }

    node->prev = node->next = NULL;
}

static void attachToLruTail(CacheLruReplacer *this, LruNode *node)
{
    node->prev = this->tail;
    node->next = NULL;

    if (this->tail)
    {
        this->tail->next = node;
    }
    else
    {
        this->head = node;
    }

    this->tail = node;
}

static void detachFromPin(CacheLruReplacer *this, LruNode *node)
{
    if (node->prev)
    {
        node->prev->next = node->next;
    }
    else
    {
        this->pinHead = node->next;
    }

    if (node->next)
    {
        node->next->prev = node->prev;
    }
    else
    {
        this->pinTail = node->prev;
    }

    node->prev = node->next = NULL;
}

static void attachToPinTail(CacheLruReplacer *this, LruNode *node)
{
    node->prev = this->pinTail;
    node->next = NULL;

    if (this->pinTail)
    {
        this->pinTail->next = node;
    }
    else
    {
        this->pinHead = node;
    }

    this->pinTail = node;
}
