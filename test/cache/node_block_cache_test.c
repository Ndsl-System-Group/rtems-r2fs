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

RTFS_TEST(NbcehInitTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 16);

    NodeBlockCacheEntry entry;
    NodeBlockCacheEntryHandle handle;

    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);
    nodeBlockCacheEntryHandleInit(&handle, &cache, &entry);


    TEST_ASSERT_EQUAL_PTR(&cache, handle.cache);
    TEST_ASSERT_EQUAL_PTR(&entry, handle.entry);


    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheEntryDestroy(&entry);
    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcehIsEmptyTrueTest)
{
    NodeBlockCacheEntryHandle handle;

    nodeBlockCacheEntryHandleInit(&handle, NULL, NULL);

    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&handle));

    nodeBlockCacheEntryHandleDestroy(&handle);
}

RTFS_TEST(NbcehIsEmptyFalseTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 16);

    NodeBlockCacheEntry entry;
    NodeBlockCacheEntryHandle handle;

    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);
    nodeBlockCacheEntryHandleInit(&handle, &cache, &entry);


    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));


    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheEntryDestroy(&entry);
    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcehDestroyEmptyTest)
{
    NodeBlockCacheEntryHandle handle;

    nodeBlockCacheEntryHandleInit(&handle, NULL, NULL);
    nodeBlockCacheEntryHandleDestroy(&handle);

    TEST_PASS();
}

RTFS_TEST(NbcehCopyEmptyTest)
{
    NodeBlockCacheEntryHandle src;
    NodeBlockCacheEntryHandle dst;

    nodeBlockCacheEntryHandleInit(&src, NULL, NULL);
    memset(&dst, 0xAA, sizeof(dst));

    nodeBlockCacheEntryHandleCopy(&dst, &src);


    TEST_ASSERT_EQUAL_PTR(NULL, dst.cache);
    TEST_ASSERT_EQUAL_PTR(NULL, dst.entry);
    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&dst));


    nodeBlockCacheEntryHandleDestroy(&src);
    nodeBlockCacheEntryHandleDestroy(&dst);
}

RTFS_TEST(NbcehCopyBasicTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 16);

    NodeBlockCacheEntry entry;
    NodeBlockCacheEntryHandle src;
    NodeBlockCacheEntryHandle dst;

    nodeBlockCacheEntryInit(&entry, &buffer, 1, 2, 3);

    entry.refCount = 0;

    nodeBlockCacheEntryHandleInit(&src, &cache, &entry);
    nodeBlockCacheEntryHandleCopy(&dst, &src);


    TEST_ASSERT_EQUAL_PTR(&cache, dst.cache);
    TEST_ASSERT_EQUAL_PTR(&entry, dst.entry);
    TEST_ASSERT_EQUAL_UINT32(1, entry.refCount);


    nodeBlockCacheEntryHandleDestroy(&dst);
    nodeBlockCacheEntryHandleDestroy(&src);

    nodeBlockCacheEntryDestroy(&entry);
    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcInitTest)
{
    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    TEST_ASSERT_EQUAL_UINT32(8, cache.expectSize);
    TEST_ASSERT_EQUAL_UINT32(0, cache.curSize);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.fsManager);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.dirtyListHead);
    TEST_ASSERT_NOT_NULL(cache.dirtyPos);


    nodeBlockCacheDestroy(&cache);
}

RTFS_TEST(NbcDestroyTest)
{
    NodeBlockCache cache;

    nodeBlockCacheInit(&cache, NULL, 8);
    nodeBlockCacheDestroy(&cache);

    TEST_ASSERT_EQUAL_UINT32(0, cache.expectSize);
    TEST_ASSERT_EQUAL_UINT32(0, cache.curSize);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.fsManager);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.dirtyListHead);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.dirtyPos);
}

RTFS_TEST(NbcAddTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    NodeBlockCacheEntryHandle handle = nodeBlockCacheAdd(&cache, &buffer, 100, INVALID_NID, 200);

    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&handle));
    TEST_ASSERT_EQUAL_UINT32(1, cache.curSize);

    TEST_ASSERT_EQUAL_UINT32(100, handle.entry->nid);
    TEST_ASSERT_EQUAL_UINT32(INVALID_NID, handle.entry->parentNid);
    TEST_ASSERT_EQUAL_UINT32(200, handle.entry->lpa);
    TEST_ASSERT_EQUAL_UINT32(1, handle.entry->refCount);
    TEST_ASSERT_EQUAL(NODE_BLOCK_CACHE_ENTRY_UPTODATE, handle.entry->state);


    nodeBlockCacheEntryHandleDestroy(&handle);
    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcGetHitTest)
{
    BlockBuffer buffer;
    blockBufferInit(&buffer);

    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    NodeBlockCacheEntryHandle h1 = nodeBlockCacheAdd(&cache, &buffer, 123, INVALID_NID, 456);

    NodeBlockCacheEntryHandle h2 = nodeBlockCacheGet(&cache, 123);

    TEST_ASSERT_FALSE(nodeBlockCacheEntryHandleIsEmpty(&h2));
    TEST_ASSERT_EQUAL_PTR(h1.entry, h2.entry);
    TEST_ASSERT_EQUAL_UINT32(2, h1.entry->refCount);


    nodeBlockCacheEntryHandleDestroy(&h1);
    nodeBlockCacheEntryHandleDestroy(&h2);

    nodeBlockCacheDestroy(&cache);
    blockBufferDestroy(&buffer);
}

RTFS_TEST(NbcGetMissTest)
{
    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    NodeBlockCacheEntryHandle handle = nodeBlockCacheGet(&cache, 999);

    TEST_ASSERT_TRUE(nodeBlockCacheEntryHandleIsEmpty(&handle));


    nodeBlockCacheDestroy(&cache);
}

RTFS_TEST(NbcForceReplaceNoopTest)
{
    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    nodeBlockCacheForceReplace(&cache);

    TEST_ASSERT_EQUAL_UINT32(0, cache.curSize);


    nodeBlockCacheDestroy(&cache);
}

RTFS_TEST(NbcGetAndClearDirtyListEmptyTest)
{
    NodeBlockCache cache;
    nodeBlockCacheInit(&cache, NULL, 8);


    NodeBlockCacheDirtyNode *list = nodeBlockCacheGetAndClearDirtyList(&cache);

    TEST_ASSERT_EQUAL_PTR(NULL, list);
    TEST_ASSERT_EQUAL_PTR(NULL, cache.dirtyListHead);


    nodeBlockCacheDestroy(&cache);
}

// TODO 后续测试 NodeBlockCacheHelper。
