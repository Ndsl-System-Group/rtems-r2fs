#include "rtfs_timer.h"
#include "rtfs_log.h"

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <memory.h>


static rtems_interval timespecToTicks(struct timespec *ts);

static void rtfsTimerService(rtems_id id, void *arg);


int rtfsTimerConstructor(RtfsTimer *this, bool isBlockCheck)
{
    memset(this, 0, sizeof(RtfsTimer));

    this->isBlockCheck = isBlockCheck;
    this->ownerTask = rtems_task_self();

    rtems_status_code sc = rtems_timer_create(rtems_build_name('T', 'M', 'R', '0'), &this->timerId);

    if (RTEMS_SUCCESSFUL != sc) return EINVAL;

    this->isCreated = 1;


    return 0;
}

void rtfsTimerDestructor(RtfsTimer *this)
{
    if (!this->isCreated) return;

    rtems_timer_delete(this->timerId);

    this->isCreated = 0;
}

void rtfsTimerSet(RtfsTimer *this, struct timespec *expirationTime, uint8_t isPeriod)
{
    this->expirationTime = *expirationTime;
    this->isPeriod = isPeriod;

    this->ticks = timespecToTicks(expirationTime);
}

int rtfsTimerStart(RtfsTimer *this)
{
    rtems_status_code sc = rtems_timer_fire_after(this->timerId, this->ticks, rtfsTimerService, this);
    if (RTEMS_SUCCESSFUL != sc)
    {
        RTFS_LOG(RTFS_LOG_ERROR, "timer start failed: %d", sc);


        return EINVAL;
    }


    return 0;
}

int rtfsTimerStop(RtfsTimer *this)
{
    rtems_status_code sc = rtems_timer_cancel(this->timerId);
    if (RTEMS_SUCCESSFUL != sc) return EINVAL;


    return 0;
}

int rtfsTimerCheckExpire(RtfsTimer *this, uint64_t *overflowTimes)
{
    rtems_event_set out;

    if (this->isBlockCheck)
    {
        rtems_event_receive(RTEMS_EVENT_1, RTEMS_WAIT, RTEMS_NO_TIMEOUT, &out);
    }
    else
    {
        rtems_status_code sc = rtems_event_receive(RTEMS_EVENT_1, RTEMS_NO_WAIT, 0, &out);

        if (RTEMS_UNSATISFIED == sc)
        {
            return EAGAIN;
        }
        else
        {
            return EINVAL;
        }
    }

    if (overflowTimes) *overflowTimes = this->pendingExpirations;

    this->pendingExpirations = 0;


    return 0;
}


rtems_interval timespecToTicks(struct timespec *ts)
{
    uint64_t ns = ts->tv_sec * 1000000000ULL + ts->tv_nsec;

    rtems_interval ticks_per_sec = rtems_clock_get_ticks_per_second();

    uint64_t ticks = (ns * ticks_per_sec) / 1000000000ULL;

    if (ticks == 0) ticks = 1; // 至少 1 tick。

    return (rtems_interval)ticks;
}

void rtfsTimerService(rtems_id id, void *arg)
{
    RtfsTimer *t = (RtfsTimer *)arg;

    ++t->pendingExpirations;

    rtems_event_send(t->ownerTask, RTEMS_EVENT_1);

    // 周期 timer：重新调度。
    if (t->isPeriod)
    {
        rtems_timer_fire_after(t->timerId, t->ticks, rtfsTimerService, t);
    }
}
