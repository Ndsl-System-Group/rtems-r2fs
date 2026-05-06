#include "rtfs_test.h"

#include "cache/sit_nat_cache.h"
#include "cache/generic_cache_manager.h"

#include <memory.h>


static int g_sit_nat_read_call_count = 0;

static int sitNatCacheTestReadHook(struct comm_dev *dev, uint32_t lpa, void *buffer)
{
    (void)dev;

    ++g_sit_nat_read_call_count;
    memset(buffer, (int)(lpa & 0xFF), BLOCK_BUFFER_SIZE);

    return 0;
}


RTFS_TEST(SnceInitDestroyTest)
{
    SitNatCacheEntry entry;

    sitNatCacheEntryInit(&entry, 123);

    TEST_ASSERT_EQUAL(entry.lpa, 123);
    TEST_ASSERT_EQUAL(entry.refCount, 0);

    sitNatCacheEntryDestroy(&entry);
}

RTFS_TEST(SnceHInitDestroyTest)
{
    SitNatCache cache;
    SitNatCacheEntry entry;

    sitNatCacheInit(&cache, NULL, 16);
    sitNatCacheEntryInit(&entry, 100);

    SitNatCacheEntryHandle handle;
    sitNatCacheEntryHandleInit(&handle, &cache, &entry);

    entry.refCount = 1;

    TEST_ASSERT(handle.cache == &cache);
    TEST_ASSERT(handle.entry == &entry);

    sitNatCacheEntryHandleDestroy(&handle);

    TEST_ASSERT(handle.cache == NULL);
    TEST_ASSERT(handle.entry == NULL);
    TEST_ASSERT_EQUAL_UINT32(0, entry.refCount);

    sitNatCacheEntryDestroy(&entry);
    sitNatCacheDestroy(&cache);
}

RTFS_TEST(SnceHCopyTest)
{
    SitNatCache cache;
    SitNatCacheEntry entry;

    sitNatCacheInit(&cache, NULL, 16);
    sitNatCacheEntryInit(&entry, 200);

    SitNatCacheEntryHandle h1;
    sitNatCacheEntryHandleInit(&h1, &cache, &entry);
    entry.refCount = 1;

    SitNatCacheEntryHandle h2;
    sitNatCacheEntryHandleCopy(&h2, &h1);

    TEST_ASSERT(h2.cache == h1.cache);
    TEST_ASSERT(h2.entry == h1.entry);
    TEST_ASSERT_EQUAL_UINT32(2, entry.refCount);

    sitNatCacheEntryHandleDestroy(&h2);
    TEST_ASSERT_EQUAL_UINT32(1, entry.refCount);
    sitNatCacheEntryHandleDestroy(&h1);
    TEST_ASSERT_EQUAL_UINT32(0, entry.refCount);

    sitNatCacheEntryDestroy(&entry);
    sitNatCacheDestroy(&cache);
}

RTFS_TEST(SncGetTest)
{
    SitNatCache cache;

    g_sit_nat_read_call_count = 0;
    sitNatCacheSetReadBlockHook(sitNatCacheTestReadHook);
    sitNatCacheInit(&cache, NULL, 16);

    SitNatCacheEntryHandle handle = sitNatCacheGet(&cache, 10);

    TEST_ASSERT_NOT_NULL(handle.entry);
    TEST_ASSERT_EQUAL_UINT32(10, handle.entry->lpa);
    TEST_ASSERT_EQUAL_UINT32(1, handle.entry->refCount);
    TEST_ASSERT_EQUAL(1, g_sit_nat_read_call_count);
    TEST_ASSERT_EQUAL_UINT8(10, *(uint8_t *)blockBufferGetPtr(&handle.entry->cache));

    sitNatCacheEntryHandleDestroy(&handle);
    sitNatCacheDestroy(&cache);
    sitNatCacheSetReadBlockHook(NULL);
}

RTFS_TEST(SncGetSameLpaTest)
{
    SitNatCache cache;

    g_sit_nat_read_call_count = 0;
    sitNatCacheSetReadBlockHook(sitNatCacheTestReadHook);
    sitNatCacheInit(&cache, NULL, 16);

    SitNatCacheEntryHandle h1 = sitNatCacheGet(&cache, 20);
    SitNatCacheEntryHandle h2 = sitNatCacheGet(&cache, 20);

    TEST_ASSERT_EQUAL_PTR(h1.entry, h2.entry);
    TEST_ASSERT_EQUAL_UINT32(2, h1.entry->refCount);
    TEST_ASSERT_EQUAL(1, g_sit_nat_read_call_count);

    sitNatCacheEntryHandleDestroy(&h2);
    sitNatCacheEntryHandleDestroy(&h1);
    sitNatCacheDestroy(&cache);
    sitNatCacheSetReadBlockHook(NULL);
}

RTFS_TEST(SncReplaceTest)
{
    SitNatCache cache;

    g_sit_nat_read_call_count = 0;
    sitNatCacheSetReadBlockHook(sitNatCacheTestReadHook);
    sitNatCacheInit(&cache, NULL, 2);

    SitNatCacheEntryHandle h1 = sitNatCacheGet(&cache, 1);
    SitNatCacheEntryHandle h2 = sitNatCacheGet(&cache, 2);

    TEST_ASSERT_EQUAL_UINT32(2, cache.curSize);

    sitNatCacheEntryHandleDestroy(&h2);
    sitNatCacheEntryHandleDestroy(&h1);

    SitNatCacheEntryHandle h3 = sitNatCacheGet(&cache, 3);

    TEST_ASSERT_NOT_NULL(h3.entry);
    TEST_ASSERT_TRUE(cache.curSize <= cache.expectSize);
    TEST_ASSERT_EQUAL_UINT32(3, h3.entry->lpa);
    TEST_ASSERT_EQUAL(3, g_sit_nat_read_call_count);
    TEST_ASSERT_TRUE(
        NULL == genericCacheManagerGet(&cache.cacheManager, 1, false) ||
        NULL == genericCacheManagerGet(&cache.cacheManager, 2, false)
    );

    sitNatCacheEntryHandleDestroy(&h3);
    sitNatCacheDestroy(&cache);
    sitNatCacheSetReadBlockHook(NULL);
}
