#include "rtfs_test.h"


struct RtfsTestEntry rtfsTestArray[RTFS_MAX_TESTS] = {};

int rtfsTestCount = 0;


void setUp(void) {}

void tearDown(void) {}

void rtfsRunAllTests(void)
{
    UNITY_BEGIN();

    int failedCases = 0;
    for (int i = 0; i < rtfsTestCount; ++i)
    {
        printf(RTFS_COLOR_GREEN "[ RUN      ] %s" RTFS_COLOR_RESET "\n", rtfsTestArray[i].name);

        // 记录当前测试失败样例的总数。
        failedCases = Unity.TestFailures;

        RUN_TEST(rtfsTestArray[i].func);

        // 如果失败数增加了，说明这个测试失败。
        if (Unity.TestFailures > failedCases)
        {
            printf(RTFS_COLOR_RED "[   FAIL   ] %s" RTFS_COLOR_RESET "\n", rtfsTestArray[i].name);
        }
        else
        {
            printf(RTFS_COLOR_GREEN "[       OK ] %s" RTFS_COLOR_RESET "\n", rtfsTestArray[i].name);
        }
    }

    UNITY_END();
}
