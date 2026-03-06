#include "rtfs_test.h"

#include "cache/cache_lru_replacer.h"


RTFS_TEST(CLRInitTest)
{
    CacheLruReplacer lru;

    cacheLruReplacerInit(&lru);


    TEST_ASSERT_FALSE(cacheLruReplacerCanReplace(&lru));


    cacheLruReplacerDestroy(&lru);
}

RTFS_TEST(CLRAddPopTest1)
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

RTFS_TEST(CLRAddPopTest2)
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

RTFS_TEST(CLRAddPopRefreshTest1)
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

RTFS_TEST(CLRAddPopRefreshTest2)
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

RTFS_TEST(CLRRemoveTest1)
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

RTFS_TEST(CLRRemoveTest2)
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

RTFS_TEST(CLRRemoveTest3)
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

RTFS_TEST(CLRCanReplaceTest)
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

RTFS_TEST(CLRPinTest)
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
