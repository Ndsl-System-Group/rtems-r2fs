#include "rtfs_test.h"


RTFS_TEST(TestRun1)
{
    TEST_PASS();
}

RTFS_TEST(TestRun2)
{
    TEST_ASSERT_EQUAL(1 + 1, 2);
}
