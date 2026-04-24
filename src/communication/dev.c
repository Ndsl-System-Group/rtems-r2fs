#include "dev.h"

#include <memory.h>


int commDevInit(comm_dev *dev, rtems_disk_device *diskDevice, uint32_t blockSize, uint64_t blockCount, uint64_t metaJournalStartLpa, uint64_t metaJournalEndLpa)
{
    if (NULL == dev) return EINVAL;
    if (NULL == diskDevice) return EINVAL;
    if (0 == blockSize) return EINVAL;
    if (0 == blockCount) return EINVAL;
    if (metaJournalStartLpa >= metaJournalEndLpa) return EINVAL;
    if (metaJournalEndLpa > blockCount) return EINVAL;

    memset(dev, 0, sizeof(comm_dev));

    dev->diskDevice = diskDevice;
    dev->blockSize = blockSize;
    dev->blockCount = blockCount;

    dev->metaJournalStartLpa = metaJournalStartLpa;
    dev->metaJournalEndLpa = metaJournalEndLpa;
    dev->metaJournalHeadLpa = metaJournalStartLpa;
    dev->metaJournalTailLpa = metaJournalStartLpa;

    int res = rtfsMutexInit(&dev->metaJournalMutex);
    if (0 != res)
    {
        memset(dev, 0, sizeof(comm_dev));


        return res;
    }

    return 0;
}

int commDevDestroy(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;

    rtfsMutexDestroy(&dev->metaJournalMutex);
    memset(dev, 0, sizeof(comm_dev));


    return 0;
}
