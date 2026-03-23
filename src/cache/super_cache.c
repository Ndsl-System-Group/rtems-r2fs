#include "super_cache.h"

#include "utils/rtfs_log.h"

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

int superCacheReadSuperBlock(SuperCache *this)
{
    // 返回 0 成功，非 0 失败。
    int res = blockBufferReadFromLpa(&this->superBlock, this->dev, this->sbLpa);
    if (0 != res)
    {
        RTFS_LOG(RTFS_LOG_ERROR, "super cache: read super block error.");


        exit(EXIT_FAILURE);
    }


    return res;
}

struct RtfsSuperBlock *superCacheGet(SuperCache *this)
{
    return (struct RtfsSuperBlock *)blockBufferGetPtr(&this->superBlock);
}
