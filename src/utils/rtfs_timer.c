#include "rtfs_timer.h"
#include "rtfs_log.h"

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>


// TODO
int rtfsTimerConstructor(RtfsTimer *this, uint8_t isBlockCheck)
{
}

void rtfsTimerDestructor(RtfsTimer *this)
{
}

void rtfsTimerSet(RtfsTimer *this, struct timespec *expirationTime, uint8_t isPeriod)
{
}

int rtfsTimerStart(RtfsTimer *this)
{
}

int rtfsTimerStop(RtfsTimer *this)
{
}

int rtfsTimerCheckExpire(RtfsTimer *this, uint64_t *overflowTimes)
{
}
