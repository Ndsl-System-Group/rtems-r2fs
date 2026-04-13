#include "rtfs_test.h"

#include "utils/rtfs_timer.h"
#include "utils/rtfs_log.h"

#include <errno.h>


static struct timespec makeTs(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;


    return ts;
}


RTFS_TEST(RtfsTimerBlockingOnceTest)
{
    RtfsTimer timer;
    TEST_ASSERT_EQUAL(0, rtfsTimerConstructor(&timer, true));


    struct timespec ts = makeTs(200);
    rtfsTimerSet(&timer, &ts, 0);

    TEST_ASSERT_EQUAL(0, rtfsTimerStart(&timer));

    uint64_t overflow = 0;

    // 这里才会卡住 200 毫秒。
    RTFS_LOG(RTFS_LOG_INFO, "start waiting for timer expired.");
    TEST_ASSERT_EQUAL(0, rtfsTimerCheckExpire(&timer, &overflow));
    RTFS_LOG(RTFS_LOG_INFO, "timer has expired, lasting for %d ms.", ts.tv_nsec / 1000000);

    TEST_ASSERT_TRUE(overflow >= 1);


    TEST_ASSERT_EQUAL(0, rtfsTimerStop(&timer));
    rtfsTimerDestructor(&timer);
}

RTFS_TEST(RtfsTimerNonBlockingNotReadyTest)
{
    RtfsTimer timer;
    TEST_ASSERT_EQUAL(0, rtfsTimerConstructor(&timer, false));


    struct timespec ts = makeTs(200);
    rtfsTimerSet(&timer, &ts, 0);

    TEST_ASSERT_EQUAL(0, rtfsTimerStart(&timer));

    uint64_t overflow = 0;

    // 立即检查，应未到期。
    TEST_ASSERT_EQUAL(EAGAIN, rtfsTimerCheckExpire(&timer, &overflow));


    TEST_ASSERT_EQUAL(0, rtfsTimerStop(&timer));
    rtfsTimerDestructor(&timer);
}

RTFS_TEST(RtfsTimerNonBlockingReadyTest)
{
    RtfsTimer timer;
    TEST_ASSERT_EQUAL(0, rtfsTimerConstructor(&timer, false));


    struct timespec ts = makeTs(200);
    rtfsTimerSet(&timer, &ts, 0);

    TEST_ASSERT_EQUAL(0, rtfsTimerStart(&timer));

    // 等一段时间（依赖 tick）。
    rtems_task_wake_after(50);

    uint64_t overflow = 0;

    int res;
    do
    {
        res = rtfsTimerCheckExpire(&timer, &overflow);
    } while (EAGAIN == res);

    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_TRUE(overflow >= 1);


    TEST_ASSERT_EQUAL(0, rtfsTimerStop(&timer));
    rtfsTimerDestructor(&timer);
}

RTFS_TEST(RtfsTimerIsPeriodTest)
{
    RtfsTimer timer;
    TEST_ASSERT_EQUAL(0, rtfsTimerConstructor(&timer, true));


    struct timespec ts = makeTs(200);
    rtfsTimerSet(&timer, &ts, 1);

    TEST_ASSERT_EQUAL(0, rtfsTimerStart(&timer));

    uint64_t overflow = 0;

    TEST_ASSERT_EQUAL(0, rtfsTimerCheckExpire(&timer, &overflow));
    TEST_ASSERT_TRUE(overflow >= 1);

    TEST_ASSERT_EQUAL(0, rtfsTimerCheckExpire(&timer, &overflow));
    TEST_ASSERT_TRUE(overflow >= 1);


    TEST_ASSERT_EQUAL(0, rtfsTimerStop(&timer));
    rtfsTimerDestructor(&timer);
}

RTFS_TEST(RtfsTimerOverflowAccumulationTest)
{
    RtfsTimer timer;
    TEST_ASSERT_EQUAL(0, rtfsTimerConstructor(&timer, true));


    struct timespec ts = makeTs(200);
    rtfsTimerSet(&timer, &ts, 1);

    TEST_ASSERT_EQUAL(0, rtfsTimerStart(&timer));

    // 等多个周期（不消费）。
    rtems_task_wake_after(50);

    uint64_t overflow = 0;

    TEST_ASSERT_EQUAL(0, rtfsTimerCheckExpire(&timer, &overflow));

    // 应该累计多个。
    TEST_ASSERT_TRUE(overflow >= 2);


    TEST_ASSERT_EQUAL(0, rtfsTimerStop(&timer));
    rtfsTimerDestructor(&timer);
}

RTFS_TEST(RtfsTimerStopTest)
{
    RtfsTimer timer;
    TEST_ASSERT_EQUAL(0, rtfsTimerConstructor(&timer, false));


    struct timespec ts = makeTs(200);
    rtfsTimerSet(&timer, &ts, 1);

    TEST_ASSERT_EQUAL(0, rtfsTimerStart(&timer));

    // 先等一次触发。
    rtems_task_wake_after(50);

    uint64_t overflow = 0;
    int res;

    do
    {
        res = rtfsTimerCheckExpire(&timer, &overflow);
    } while (EAGAIN == res);

    TEST_ASSERT_EQUAL(0, res);

    // 停止。
    TEST_ASSERT_EQUAL(0, rtfsTimerStop(&timer));

    // 再等一段时间，不应该再触发。
    rtems_task_wake_after(50);

    res = rtfsTimerCheckExpire(&timer, &overflow);
    TEST_ASSERT_EQUAL(EAGAIN, res);


    TEST_ASSERT_EQUAL(0, rtfsTimerStop(&timer));
    rtfsTimerDestructor(&timer);
}
