#include "rtfs_test.h"

#include <rtems/counter.h>


struct RtfsTestEntry rtfsTestArray[RTFS_MAX_TESTS] = {};

int rtfsTestCount = 0;

static uint64_t rtfsCounterFreq = 0;


void setUp(void) {}

void tearDown(void) {}


// 获取当前硬件计数器值。
static uint64_t rtfsGetCounter();

// 计数器差值转换为微秒。
static uint64_t rtfsCounterToUs(uint64_t diff);

// 输出耗时，自动切换 us/ms/s。
static void rtfsPrintElapsedTime(uint64_t us);


void rtfsRunAllTests(void)
{
    UNITY_BEGIN();


    int failedCases = 0;
    rtfsCounterFreq = rtems_counter_frequency();
    uint64_t totalBegin = rtfsGetCounter();

    for (int i = 0; i < rtfsTestCount; ++i)
    {
        printf(RTFS_COLOR_GREEN "[ RUN      ] %s" RTFS_COLOR_RESET "\n", rtfsTestArray[i].name);

        // 记录当前测试失败样例的总数。
        failedCases = Unity.TestFailures;

        uint64_t begin = rtfsGetCounter();
        RUN_TEST(rtfsTestArray[i].func);
        uint64_t end = rtfsGetCounter();

        uint64_t costUs = rtfsCounterToUs(end - begin);

        // 如果失败数增加了，说明这个测试失败。
        if (Unity.TestFailures > failedCases)
        {
            printf(RTFS_COLOR_RED "[   FAIL   ] %s (", rtfsTestArray[i].name);
            rtfsPrintElapsedTime(costUs);
            printf(")" RTFS_COLOR_RESET "\n");
        }
        else
        {
            printf(RTFS_COLOR_GREEN "[       OK ] %s (", rtfsTestArray[i].name);
            rtfsPrintElapsedTime(costUs);
            printf(")" RTFS_COLOR_RESET "\n");
        }
    }

    uint64_t totalEnd = rtfsGetCounter();
    uint64_t totalUs = rtfsCounterToUs(totalEnd - totalBegin);

    printf(RTFS_COLOR_YELLOW "\n[ TOTAL TIME ] ");
    rtfsPrintElapsedTime(totalUs);
    printf(RTFS_COLOR_RESET "\n");


    UNITY_END();
}


uint64_t rtfsGetCounter()
{
    return rtems_counter_read();
}

uint64_t rtfsCounterToUs(uint64_t diff)
{
    return (diff * 1000000ULL) / rtfsCounterFreq;
}

void rtfsPrintElapsedTime(uint64_t us)
{
    if (us < 1000ULL)
    {
        printf("%llu us", (unsigned long long)us);
    }
    else if (us < 1000000ULL)
    {
        printf("%.3f ms", (double)us / 1000.0);
    }
    else
    {
        printf("%.3f s", (double)us / 1000000.0);
    }
}
