#include "rtfs_test.h"

#include "cache/node_block_cache.h"

#include <memory.h>


RTFS_TEST(NbceInitTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    memset(&entry, 0xAA, sizeof(entry));

    nodeBlockCacheEntryInit(&entry, &buffer, 100, 200, 300);


    TEST_ASSERT_EQUAL_UINT32(100, entry.nid);
    TEST_ASSERT_EQUAL_UINT32(200, entry.parentNid);
    TEST_ASSERT_EQUAL_UINT32(300, entry.lpa);
    TEST_ASSERT_EQUAL_UINT32(0, entry.refCount);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, entry.state);


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceDestroyTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 10, 20, 30);

    nodeBlockCacheEntryDestroy(&entry);


    TEST_ASSERT_EQUAL_UINT32(0, entry.nid);
    TEST_ASSERT_EQUAL_UINT32(0, entry.parentNid);
    TEST_ASSERT_EQUAL_UINT32(0, entry.lpa);
    TEST_ASSERT_EQUAL_UINT32(0, entry.refCount);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_DELETED, entry.state);


    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceLpaTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);


    TEST_ASSERT_EQUAL_UINT32(3, nodeBlockCacheEntryGetLpa(&entry));

    nodeBlockCacheEntrySetLpa(&entry, 999);

    TEST_ASSERT_EQUAL_UINT32(999, nodeBlockCacheEntryGetLpa(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceStateTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);


    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, nodeBlockCacheEntryGetState(&entry));

    nodeBlockCacheEntrySetState(&entry, NODE_BLOCK_CACHE_ENTRY_DIRTY);

    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_DIRTY, nodeBlockCacheEntryGetState(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceBufferTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);


    TEST_ASSERT_EQUAL_PTR(&entry.node, nodeBlockCacheEntryGetNodeBuffer(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceNidTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 777, 2, 3);


    TEST_ASSERT_EQUAL_UINT32(777, nodeBlockCacheEntryGetNid(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbceNodeBlockPtrTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCacheEntry entry;
    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);


    TEST_ASSERT_EQUAL_PTR(blockBufferGetPtr(&entry.node), nodeBlockCacheEntryGetNodeBlockPtr(&entry));


    nodeBlockCacheEntryDestroy(&entry);
    blockBufferDestroy(&buffer);
}
