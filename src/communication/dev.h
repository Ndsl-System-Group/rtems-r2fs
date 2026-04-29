#ifndef _DEV_H_
#define _DEV_H_

#include "utils/types.h"
#include "utils/rtfs_multithread.h"

#include <rtems/blkdev.h>


typedef struct comm_dev
{
    rtems_disk_device *diskDevice; // RTEMS 块设备句柄。

    uint32_t blockSize; // 逻辑块大小。

    uint64_t blockCount; // 总块数。

    uint64_t metaJournalStartLpa;
    uint64_t metaJournalEndLpa;
    uint64_t metaJournalHeadLpa;
    uint64_t metaJournalTailLpa;

    mutex_t metaJournalMutex;
} comm_dev;


int commDevInit(comm_dev *dev, rtems_disk_device *diskDevice, uint32_t blockSize, uint64_t blockCount, uint64_t metaJournalStartLpa, uint64_t metaJournalEndLpa);

int commDevDestroy(comm_dev *this);


#endif
