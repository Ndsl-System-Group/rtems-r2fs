#include "memory.h"

#include <stdlib.h>


void *commAllocDmaMem(size_t size)
{
    // TODO DMA 内存对应甲方具体板子内部的实现，先使用 malloc() 或 posix_memalign() 函数模拟。posix_memalign() 是支持内存对齐版本的 malloc()，但是飞腾这个 BSP 没有实现。
    void *res = malloc(size);
    if (NULL == res) RTFS_LOG(RTFS_LOG_ERROR, "commAllocDmaMem: alloc dma memory failed.");


    return res;
}

void commFreeDmaMem(void *buf)
{
    free(buf);
}
