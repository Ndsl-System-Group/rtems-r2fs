#include "super_cache.h"

#include "utils/rtfs_log.h"
#include "utils/rtfs_exception.h"

#include <stdlib.h>


void superCacheInit(SuperCache *this, struct comm_dev *dev, uint64_t superBlockLpa)
{
    this->dev = dev;
    this->sbLpa = superBlockLpa;

    blockBufferInit(&this->superBlock);
}

void superCacheDestroy(SuperCache *this)
{
    blockBufferDestroy(&this->superBlock);
}

void superCacheReadSuperBlock(SuperCache *this)
{
    CEXCEPTION_T e;

    Try
    {
        blockBufferReadFromLpa(&this->superBlock, this->dev, this->sbLpa);
    }
    Catch(e)
    {
        THROW_FATAL_MESSAGE(e, "super cache: read super block error.");
    }
}

struct RtfsSuperBlock *superCacheGet(SuperCache *this)
{
    return (struct RtfsSuperBlock *)blockBufferGetPtr(&this->superBlock);
}
