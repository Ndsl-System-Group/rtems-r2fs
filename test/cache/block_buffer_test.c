#include "rtfs_test.h"

#include "cache/block_buffer.h"

#include <memory.h>


RTFS_TEST(BbInitTest)
{
    BlockBuffer buf;
    blockBufferInit(&buf);


    TEST_ASSERT_NOT_NULL(buf.buffer);


    blockBufferDestroy(&buf);
}

RTFS_TEST(BbGetPtrTest)
{
    BlockBuffer buf;
    blockBufferInit(&buf);


    char *ptr = blockBufferGetPtr(&buf);

    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_EQUAL_PTR(buf.buffer, ptr);


    blockBufferDestroy(&buf);
}

RTFS_TEST(BbCopyContentFromBufTest)
{
    BlockBuffer buf;
    char src[BLOCK_BUFFER_SIZE];

    memset(src, 0x5A, BLOCK_BUFFER_SIZE);
    blockBufferInit(&buf);


    blockBufferCopyContentFromBuf(&buf, src);

    TEST_ASSERT_EQUAL_MEMORY(src, buf.buffer, BLOCK_BUFFER_SIZE);


    blockBufferDestroy(&buf);
}

RTFS_TEST(BbCopyTest)
{
    BlockBuffer src;
    BlockBuffer dst;

    blockBufferInit(&src);
    blockBufferInit(&dst);


    memset(src.buffer, 0x11, BLOCK_BUFFER_SIZE);
    memset(dst.buffer, 0x22, BLOCK_BUFFER_SIZE);

    blockBufferCopy(&dst, &src);

    TEST_ASSERT_EQUAL_MEMORY(src.buffer, dst.buffer, BLOCK_BUFFER_SIZE);


    blockBufferDestroy(&dst);
    blockBufferDestroy(&src);
}

RTFS_TEST(BbPatternTest)
{
    BlockBuffer buf;
    uint32_t *p;
    int i;

    blockBufferInit(&buf);


    p = (uint32_t *)buf.buffer;

    for (i = 0; i < BLOCK_BUFFER_SIZE / sizeof(uint32_t); ++i) p[i] = (uint32_t)i;

    for (i = 0; i < BLOCK_BUFFER_SIZE / sizeof(uint32_t); ++i) TEST_ASSERT_EQUAL_UINT32((uint32_t)i, p[i]);


    blockBufferDestroy(&buf);
}

// TODO 后续完成 communication 模块后测试 comm_dev 相关接口。
RTFS_TEST(BbDevTest)
{
    TEST_PASS();
}
