#include "rtfs_test.h"

#include "cache/super_cache.h"


RTFS_TEST(ScInitTest)
{
    SuperCache sc;
    // 仅做测试，实际不能这样使用。
    superCacheInit(&sc, NULL, 10086);


    TEST_ASSERT_EQUAL_PTR(sc.dev, NULL);
    TEST_ASSERT_EQUAL(sc.sbLpa, 10086);


    superCacheDestroy(&sc);
}
