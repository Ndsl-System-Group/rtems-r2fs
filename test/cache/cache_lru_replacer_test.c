#include "rtfs_test.h"

#include "cache/cache_lru_replacer.h"


RTFS_TEST(ClrInitTest)
{
    CacheLruReplacer lru;

    cacheLruReplacerInit(&lru);


    TEST_ASSERT_FALSE(cacheLruReplacerCanReplace(&lru));


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(ClrAddPopTest1)
{
    CacheLruReplacer lru;
    uint32_t k1 = 1;

    cacheLruReplacerInit(&lru);


    cacheLruReplacerAdd(&lru, k1);

    TEST_ASSERT_TRUE(cacheLruReplacerCanReplace(&lru));
    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k1);
    TEST_ASSERT_FALSE(cacheLruReplacerCanReplace(&lru));


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(ClrAddPopTest2)
{
    CacheLruReplacer lru;
    uint32_t k1 = 1, k2 = 2, k3 = 3;

    cacheLruReplacerInit(&lru);


    cacheLruReplacerAdd(&lru, k1);
    cacheLruReplacerAdd(&lru, k2);
    cacheLruReplacerAdd(&lru, k3);

    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k1);
    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k2);
    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k3);


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(ClrAddPopRefreshTest1)
{
    CacheLruReplacer lru;
    uint32_t k1 = 1, k2 = 2, k3 = 3;

    cacheLruReplacerInit(&lru);


    cacheLruReplacerAdd(&lru, k1);
    cacheLruReplacerAdd(&lru, k2);
    cacheLruReplacerAdd(&lru, k3);

    cacheLruReplacerAccess(&lru, k1);

    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k2);
    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k3);
    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k1);


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(ClrAddPopRefreshTest2)
{
    CacheLruReplacer lru;
    uint32_t k1 = 1, k2 = 2, k3 = 3;

    cacheLruReplacerInit(&lru);


    cacheLruReplacerAdd(&lru, k1);
    cacheLruReplacerAdd(&lru, k2);
    cacheLruReplacerAdd(&lru, k3);

    cacheLruReplacerAccess(&lru, k1);
    cacheLruReplacerAccess(&lru, k2);

    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k3);
    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k1);
    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k2);


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(ClrRemoveTest1)
{
    CacheLruReplacer lru;
    uint32_t k1 = 1, k2 = 2, k3 = 3;

    cacheLruReplacerInit(&lru);


    cacheLruReplacerAdd(&lru, k1);
    cacheLruReplacerAdd(&lru, k2);
    cacheLruReplacerAdd(&lru, k3);

    cacheLruReplacerRemove(&lru, k2);

    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k1);
    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k3);


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(ClrRemoveTest2)
{
    CacheLruReplacer lru;
    uint32_t k1 = 1, k2 = 2;

    cacheLruReplacerInit(&lru);


    cacheLruReplacerAdd(&lru, k1);
    cacheLruReplacerAdd(&lru, k2);

    cacheLruReplacerRemove(&lru, k1);

    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k2);


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(ClrRemoveTest3)
{
    CacheLruReplacer lru;
    uint32_t k1 = 1, k2 = 2;

    cacheLruReplacerInit(&lru);


    cacheLruReplacerAdd(&lru, k1);
    cacheLruReplacerAdd(&lru, k2);

    cacheLruReplacerRemove(&lru, k2);

    TEST_ASSERT_EQUAL_INT(cacheLruReplacerPop(&lru), k1);


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(ClrCanReplaceTest)
{
    CacheLruReplacer lru;
    uint32_t k1 = 1, k2 = 2;

    cacheLruReplacerInit(&lru);


    cacheLruReplacerAdd(&lru, k1);
    cacheLruReplacerAdd(&lru, k2);

    TEST_ASSERT_TRUE(cacheLruReplacerCanReplace(&lru));

    cacheLruReplacerPop(&lru);
    TEST_ASSERT_TRUE(cacheLruReplacerCanReplace(&lru));

    cacheLruReplacerPop(&lru);
    TEST_ASSERT_FALSE(cacheLruReplacerCanReplace(&lru));


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(ClrPinTest)
{
    CacheLruReplacer lru;
    uint32_t k1 = 1, k2 = 2;

    cacheLruReplacerInit(&lru);


    cacheLruReplacerAdd(&lru, k1);
    cacheLruReplacerAdd(&lru, k2);

    TEST_ASSERT_TRUE(cacheLruReplacerCanReplace(&lru));

    // pin 掉 k1，现在只有 k2 能被替换。
    cacheLruReplacerPin(&lru, k1);
    TEST_ASSERT_TRUE(cacheLruReplacerCanReplace(&lru));

    cacheLruReplacerPop(&lru);
    TEST_ASSERT_FALSE(cacheLruReplacerCanReplace(&lru));

    // unpin 掉 k1，现在 k1 就能被替换了。
    cacheLruReplacerUnpin(&lru, k1);
    TEST_ASSERT_TRUE(cacheLruReplacerCanReplace(&lru));

    cacheLruReplacerPop(&lru);
    TEST_ASSERT_FALSE(cacheLruReplacerCanReplace(&lru));


    cacheLruReplacerDestroy(&lru);
}
