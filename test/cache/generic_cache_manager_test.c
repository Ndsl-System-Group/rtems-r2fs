#include "rtfs_test.h"

#include "cache/generic_cache_manager.h"


typedef struct
{
    int value;
} GcmTestEntry;

static GcmTestEntry *newTestEntry(int v)
{
    GcmTestEntry *e = (GcmTestEntry *)malloc(sizeof(GcmTestEntry));
    e->value = v;


    return e;
}


RTFS_TEST(GcmInitTest)
{
    GenericCacheManager gcm;

    genericCacheManagerInit(&gcm);


    // 空 cache，无法 replace。
    TEST_ASSERT_NULL(genericCacheManagerReplaceOne(&gcm));


    genericCacheManagerDestroy(&gcm);
}

RTFS_TEST(GcmAddGetTest)
{
    GenericCacheManager gcm;
    uint32_t k1 = 1;

    genericCacheManagerInit(&gcm);


    GcmTestEntry *e1 = newTestEntry(100);
    genericCacheManagerAdd(&gcm, k1, e1);

    GcmTestEntry *res = (GcmTestEntry *)genericCacheManagerGet(&gcm, k1, true);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQUAL_INT(res->value, 100);


    genericCacheManagerDestroy(&gcm);
}

RTFS_TEST(GcmReplaceOrderTest1)
{
    GenericCacheManager gcm;
    uint32_t k1 = 1, k2 = 2, k3 = 3;

    genericCacheManagerInit(&gcm);


    genericCacheManagerAdd(&gcm, k1, newTestEntry(1));
    genericCacheManagerAdd(&gcm, k2, newTestEntry(2));
    genericCacheManagerAdd(&gcm, k3, newTestEntry(3));

    GcmTestEntry *e;

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 1);
    free(e);

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 2);
    free(e);

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 3);
    free(e);


    genericCacheManagerDestroy(&gcm);
}

RTFS_TEST(GcmAccessRefreshTest)
{
    GenericCacheManager gcm;
    uint32_t k1 = 1, k2 = 2, k3 = 3;

    genericCacheManagerInit(&gcm);


    genericCacheManagerAdd(&gcm, k1, newTestEntry(1));
    genericCacheManagerAdd(&gcm, k2, newTestEntry(2));
    genericCacheManagerAdd(&gcm, k3, newTestEntry(3));

    // 访问 k1，刷新到 LRU 尾。
    genericCacheManagerGet(&gcm, k1, true);

    GcmTestEntry *e;

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 2);
    free(e);

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 3);
    free(e);

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 1);
    free(e);


    genericCacheManagerDestroy(&gcm);
}

RTFS_TEST(GcmNoAccessRefreshTest)
{
    GenericCacheManager gcm;
    uint32_t k1 = 1, k2 = 2, k3 = 3;

    genericCacheManagerInit(&gcm);


    genericCacheManagerAdd(&gcm, k1, newTestEntry(1));
    genericCacheManagerAdd(&gcm, k2, newTestEntry(2));
    genericCacheManagerAdd(&gcm, k3, newTestEntry(3));

    // 只是窥视，不算访问。
    genericCacheManagerGet(&gcm, k1, false);

    GcmTestEntry *e;

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 1);
    free(e);

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 2);
    free(e);

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 3);
    free(e);


    genericCacheManagerDestroy(&gcm);
}

RTFS_TEST(GcmRemoveTest)
{
    GenericCacheManager gcm;
    uint32_t k1 = 1, k2 = 2, k3 = 3;

    genericCacheManagerInit(&gcm);


    genericCacheManagerAdd(&gcm, k1, newTestEntry(1));
    genericCacheManagerAdd(&gcm, k2, newTestEntry(2));
    genericCacheManagerAdd(&gcm, k3, newTestEntry(3));

    GcmTestEntry *e = (GcmTestEntry *)genericCacheManagerRemove(&gcm, k2);
    TEST_ASSERT_EQUAL_INT(e->value, 2);
    free(e);

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 1);
    free(e);

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 3);
    free(e);


    genericCacheManagerDestroy(&gcm);
}

RTFS_TEST(GcmPinTest)
{
    GenericCacheManager gcm;
    uint32_t k1 = 1, k2 = 2, k3 = 3;

    genericCacheManagerInit(&gcm);


    genericCacheManagerAdd(&gcm, k2, newTestEntry(2));
    genericCacheManagerAdd(&gcm, k1, newTestEntry(1));
    genericCacheManagerAdd(&gcm, k3, newTestEntry(3));

    // pin 住 k1。
    genericCacheManagerPin(&gcm, k1);

    GcmTestEntry *e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 2);
    free(e);

    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 3);
    free(e);

    TEST_ASSERT_EQUAL(genericCacheManagerReplaceOne(&gcm), NULL);

    // unpin 住 k1。
    genericCacheManagerUnpin(&gcm, k1);
    e = (GcmTestEntry *)genericCacheManagerReplaceOne(&gcm);
    TEST_ASSERT_EQUAL_INT(e->value, 1);
    free(e);


    genericCacheManagerDestroy(&gcm);
}
