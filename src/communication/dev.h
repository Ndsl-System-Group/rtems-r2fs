#ifndef _DEV_H_
#define _DEV_H_

#include "utils/types.h"

#include <rtems/blkdev.h>


typedef struct comm_dev
{
    rtems_disk_device *diskDevice; // RTEMS 块设备句柄。

    uint32_t blockSize; // 逻辑块大小。

    uint64_t blockCount; // 总块数。
} comm_dev;


#endif
