#ifndef _DEV_H_
#define _DEV_H_

#include "utils/types.h"
#include "utils/rtfs_multithread.h"

#include <stddef.h>
#include <rtems/blkdev.h>

typedef struct comm_recovered_reclaim_record
{
    uint32_t *data_lpas;
    size_t data_count;
    uint32_t *node_lpas;
    size_t node_count;
    struct comm_recovered_reclaim_record *next;
} comm_recovered_reclaim_record;

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
    comm_recovered_reclaim_record *recoveredReclaimHead;
} comm_dev;


int commDevInit(comm_dev *dev, rtems_disk_device *diskDevice, uint32_t blockSize, uint64_t blockCount, uint64_t metaJournalStartLpa, uint64_t metaJournalEndLpa);

int commDevDestroy(comm_dev *this);

void commDevClearRecoveredReclaimRecords(comm_dev *dev);


#endif
