#ifndef _SUPER_CACHE_H_
#define _SUPER_CACHE_H_

#include "cache/block_buffer.h"

#include "fs/fs.h"


struct comm_dev;


typedef struct SuperCache
{
    struct comm_dev *dev;

    uint64_t sbLpa;

    BlockBuffer superBlock;
} SuperCache;


void superCacheInit(SuperCache *this, struct comm_dev *dev, uint64_t superBlockLpa);

void superCacheDestroy(SuperCache *this);

int superCacheReadSuperBlock(SuperCache *this);

struct RtfsSuperBlock *superCacheGet(SuperCache *this);


#endif
