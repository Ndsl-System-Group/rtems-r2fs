#include "rtfs_test.h"

#include "utils/rtfs_timer.h"
#include "utils/rtfs_log.h"


static struct timespec makeTs(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;


    return ts;
}


RTFS_TEST(RtfsTimerTest)
{
    RtfsTimer timer;
    TEST_ASSERT_EQUAL(0, rtfsTimerConstructor(&timer, true));


    struct timespec ts = makeTs(500);
    rtfsTimerSet(&timer, &ts, 0);

    TEST_ASSERT_EQUAL(0, rtfsTimerStart(&timer));

    uint64_t overflow = 0;

    // 这里才会卡 10 秒。
    RTFS_LOG(RTFS_LOG_INFO, "start waiting for timer expired.");
    TEST_ASSERT_EQUAL(0, rtfsTimerCheckExpire(&timer, &overflow));
    RTFS_LOG(RTFS_LOG_INFO, "timer has expired, lasting for %d ms.", ts.tv_nsec / 1000000);


    TEST_ASSERT_EQUAL(0, rtfsTimerStop(&timer));
    rtfsTimerDestructor(&timer);
}
