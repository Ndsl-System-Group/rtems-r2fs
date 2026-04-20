#ifndef _MEMORY_H_
#define _MEMORY_H_

#include "utils/types.h"
#include "utils/rtfs_log.h"


__attribute__((unused)) void *commAllocDmaMem(size_t size);

__attribute__((unused)) void commFreeDmaMem(void *buf);


#endif
