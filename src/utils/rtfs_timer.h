#ifndef _RTFS_TIMER_H_
#define _RTFS_TIMER_H_

#include <stdint.h>
#include <time.h>


typedef struct RtfsTimer
{
    int timerFd;
    struct timespec expirationTime;
    uint8_t isPeriod;
    uint8_t isBlockCheck; // rtfsTimerCheckExpire 时是否阻塞直到到期。
} RtfsTimer;

/* 定时器操作接口。 */

/* isBlockCheck：定时器是阻塞还是非阻塞。 */
int rtfsTimerConstructor(RtfsTimer *this, uint8_t isBlockCheck);
void rtfsTimerDestructor(RtfsTimer *this);
void rtfsTimerSet(RtfsTimer *this, struct timespec *expirationTime, uint8_t isPeriod);
int rtfsTimerStart(RtfsTimer *this);
int rtfsTimerStop(RtfsTimer *this);

/*
 * 检查定时器是否到期。
 * 若定时器设置了 isBlockCheck，则阻塞到到期后返回 0；若发生其它错误，返回 errno。
 * 若定时器未设置 isBlockCheck，直接返回：若到期，返回 0；若未到期，则返回 EAGAIN；若发生其它错误，返回 errno。
 * 若 overflowTimes 不为 NULL，则保存到期次数。
 */
int rtfsTimerCheckExpire(RtfsTimer *this, uint64_t *overflowTimes);


#endif
