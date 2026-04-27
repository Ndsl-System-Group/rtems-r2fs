#include "rtfs_test.h"

#include "communication/dev.h"

#include <memory.h>


RTFS_TEST(CdInitTest)
{
    comm_dev dev;
    rtems_disk_device disk;

    int res = commDevInit(&dev, &disk, 4096, 10000, 100, 200);


    TEST_ASSERT_EQUAL(0, res);

    TEST_ASSERT_EQUAL_PTR(&disk, dev.diskDevice);
    TEST_ASSERT_EQUAL_UINT32(4096, dev.blockSize);
    TEST_ASSERT_EQUAL_UINT64(10000, dev.blockCount);

    TEST_ASSERT_EQUAL_UINT64(100, dev.metaJournalStartLpa);
    TEST_ASSERT_EQUAL_UINT64(200, dev.metaJournalEndLpa);
    TEST_ASSERT_EQUAL_UINT64(100, dev.metaJournalHeadLpa);
    TEST_ASSERT_EQUAL_UINT64(100, dev.metaJournalTailLpa);


    commDevDestroy(&dev);
}

RTFS_TEST(CdDestroyTest)
{
    comm_dev dev;
    rtems_disk_device disk;

    memset(&dev, 0xAA, sizeof(dev));

    TEST_ASSERT_EQUAL(0, commDevInit(&dev, &disk, 4096, 10000, 100, 200));
    TEST_ASSERT_EQUAL(0, commDevDestroy(&dev));

    for (size_t i = 0; i < sizeof(dev); i++)
    {
        TEST_ASSERT_EQUAL_UINT8(0, ((uint8_t *)&dev)[i]);
    }
}

RTFS_TEST(CdDestroyNullTest)
{
    TEST_ASSERT_EQUAL(EINVAL, commDevDestroy(NULL));
}
