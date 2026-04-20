#ifndef _MEMORY_H_
#define _MEMORY_H_

#include "utils/types.h"
#include "utils/rtfs_log.h"


__attribute__((unused)) void *comm_alloc_dma_mem(size_t size);

__attribute__((unused)) void comm_free_dma_mem(void *buf);


#endif
