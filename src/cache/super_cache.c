#include "super_cache.h"

#include "utils/rtfs_log.h"
#include "utils/rtfs_exception.h"

#include <stdlib.h>

static super_cache_read_block_hook g_super_cache_read_block_hook = NULL;

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
        if (g_super_cache_read_block_hook != NULL)
        {
            int ret = g_super_cache_read_block_hook(
                this->dev,
                (uint32_t)this->sbLpa,
                blockBufferGetPtr(&this->superBlock)
            );
            if (ret != 0)
            {
                THROW_FATAL_MESSAGE(EXIT_FAILURE, "super cache: test hook read super block failed.");
            }
        }
        else
        {
            blockBufferReadFromLpa(&this->superBlock, this->dev, this->sbLpa);
        }
    }
    Catch(e)
    {
        THROW_FATAL_MESSAGE(e, "super cache: read super block error.");
    }
}

void superCacheSetReadBlockHook(super_cache_read_block_hook hook)
{
    g_super_cache_read_block_hook = hook;
}

struct RtfsSuperBlock *superCacheGet(SuperCache *this)
{
    return (struct RtfsSuperBlock *)blockBufferGetPtr(&this->superBlock);
}
