#include "rtfs_test.h"

#include "communication/memory.h"

#include <memory.h>


RTFS_TEST(MemBasicTest)
{
    void *buf = comm_alloc_dma_mem(1024);


    TEST_ASSERT_NOT_NULL(buf);


    comm_free_dma_mem(buf);
}

RTFS_TEST(MemReadWriteTest)
{
    uint8_t *buf = (uint8_t *)comm_alloc_dma_mem(256);


    TEST_ASSERT_NOT_NULL(buf);

    memset(buf, 0x5A, 256);

    for (int i = 0; i < 256; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(0x5A, buf[i]);
    }


    comm_free_dma_mem(buf);
}

RTFS_TEST(MemZeroSizeTest)
{
    void *buf = comm_alloc_dma_mem(0);


    // malloc(0) 允许返回 NULL 或有效指针，两者都合法。
    if (NULL != buf) comm_free_dma_mem(buf);


    TEST_PASS();
}

RTFS_TEST(MemMultiAllocTest)
{
    void *buf1 = comm_alloc_dma_mem(128);
    void *buf2 = comm_alloc_dma_mem(256);
    void *buf3 = comm_alloc_dma_mem(512);


    TEST_ASSERT_NOT_NULL(buf1);
    TEST_ASSERT_NOT_NULL(buf2);
    TEST_ASSERT_NOT_NULL(buf3);

    TEST_ASSERT_NOT_EQUAL(buf1, buf2);
    TEST_ASSERT_NOT_EQUAL(buf1, buf3);
    TEST_ASSERT_NOT_EQUAL(buf2, buf3);


    comm_free_dma_mem(buf3);
    comm_free_dma_mem(buf2);
    comm_free_dma_mem(buf1);
}

RTFS_TEST(MemFreeNullTest)
{
    // free(NULL) 标准允许，应该安全。
    comm_free_dma_mem(NULL);

    TEST_PASS();
}
