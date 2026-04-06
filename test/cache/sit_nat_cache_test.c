#include "rtfs_test.h"

#include "cache/sit_nat_cache.h"


RTFS_TEST(SnceInitDestroyTest)
{
    SitNatCacheEntry entry;

    sitNatCacheEntryInit(&entry, 123);

    TEST_ASSERT_EQUAL(entry.lpa, 123);
    TEST_ASSERT_EQUAL(entry.refCount, 0);

    sitNatCacheEntryDestroy(&entry);
}


RTFS_TEST(SnceHInitDestroy)
{
    SitNatCache cache;
    SitNatCacheEntry entry;

    sitNatCacheInit(&cache, NULL, 16);
    sitNatCacheEntryInit(&entry, 100);

    SitNatCacheEntryHandle handle;
    sitNatCacheEntryHandleInit(&handle, &cache, &entry);

    TEST_ASSERT(handle.cache == &cache);
    TEST_ASSERT(handle.entry == &entry);

    sitNatCacheEntryHandleDestroy(&handle);

    TEST_ASSERT(handle.cache == NULL);
    TEST_ASSERT(handle.entry == NULL);

    sitNatCacheEntryDestroy(&entry);
    sitNatCacheDestroy(&cache);
}

RTFS_TEST(SnceHCopy)
{
    SitNatCache cache;
    SitNatCacheEntry entry;

    sitNatCacheInit(&cache, NULL, 16);
    sitNatCacheEntryInit(&entry, 200);

    SitNatCacheEntryHandle h1;
    sitNatCacheEntryHandleInit(&h1, &cache, &entry);

    SitNatCacheEntryHandle h2;
    sitNatCacheEntryHandleCopy(&h2, &h1);

    TEST_ASSERT(h2.cache == h1.cache);
    TEST_ASSERT(h2.entry == h1.entry);

    sitNatCacheEntryHandleDestroy(&h2);
    sitNatCacheEntryHandleDestroy(&h1);

    sitNatCacheEntryDestroy(&entry);
    sitNatCacheDestroy(&cache);
}

RTFS_TEST(SncGet)
{
    SitNatCache cache;

    sitNatCacheInit(&cache, NULL, 16);

    // 现在缓存并非命中，所以会读取 SSD 读取并构建缓存项。
    SitNatCacheEntryHandle handle = sitNatCacheGet(&cache, 10);

    TEST_ASSERT(handle.entry != NULL);
    TEST_ASSERT(10 == handle.entry->lpa);
    TEST_ASSERT(handle.entry->refCount > 0);

    sitNatCacheEntryHandleDestroy(&handle);

    sitNatCacheDestroy(&cache);
}

RTFS_TEST(SncGetSameLpa)
{
    SitNatCache cache;

    sitNatCacheInit(&cache, NULL, 16);

    // 第一次缓存不命中，会分配 entry 结构，第二次缓存命中了，就直接返回相同的 entry 指针了。
    SitNatCacheEntryHandle h1 = sitNatCacheGet(&cache, 20);
    SitNatCacheEntryHandle h2 = sitNatCacheGet(&cache, 20);

    TEST_ASSERT(h1.entry == h2.entry);

    sitNatCacheEntryHandleDestroy(&h2);
    sitNatCacheEntryHandleDestroy(&h1);

    sitNatCacheDestroy(&cache);
}

// TODO 该部分逻辑等 replacer 的 pin 功能加上以后一起测试。
// RTFS_TEST(SncReplace)
// {
//     SitNatCache cache;

//     sitNatCacheInit(&cache, NULL, 2);

//     SitNatCacheEntryHandle h1 = sitNatCacheGet(&cache, 1);
//     SitNatCacheEntryHandle h2 = sitNatCacheGet(&cache, 2);
//     SitNatCacheEntryHandle h3 = sitNatCacheGet(&cache, 3);

//     TEST_ASSERT(cache.curSize <= cache.expectSize);

//     sitNatCacheEntryHandleDestroy(&h1);
//     sitNatCacheEntryHandleDestroy(&h2);
//     sitNatCacheEntryHandleDestroy(&h3);

//     sitNatCacheDestroy(&cache);
// }
