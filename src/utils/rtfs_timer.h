#ifndef _RTFS_TIMER_H_
#define _RTFS_TIMER_H_

#include <rtems.h>


/**
 * @brief RTFS 定时器，封装了 RTEMS 原生定时器（rtems_timer_*），并通过 event 机制模拟 Linux timerfd 的“可读 + 阻塞/非阻塞检查”语义。
 *
 * @details 特性
 *  1. 支持单次 / 周期定时器。
 *  2. 支持阻塞 / 非阻塞检查到期（类似 read(timerfd)）。
 *  3. 支持累计到期次数（简化版 overrun 统计）。
 *
 * @note
 *  1. 当前实现默认一个 timer 对应一个 task（ownerTask）。
 *  2. 多 timer 共享同一 event bit 时需要额外区分机制。
 */
typedef struct RtfsTimer
{
    /**
     * @brief RTEMS 定时器对象。
     */
    rtems_id timerId;

    /**
     * @brief 是否为周期定时器。
     */
    bool isPeriod;

    /**
     * @brief checkExpire 时是否阻塞直到到期。
     */
    bool isBlockCheck;

    /**
     * @brief < 已累计的到期次数（类似 timerfd read 返回值）。每次触发 +1，在 checkExpire 中读取并清零。
     */
    volatile uint64_t pendingExpirations;

    /**
     * @brief 定时器周期（单位：RTEMS ticks）。
     */
    rtems_interval ticks;

    /**
     * @brief 接收定时器事件的任务 ID。
     */
    rtems_id ownerTask;
} RtfsTimer;


/**
 * @brief 构造定时器。
 * @param isBlockCheck 定时器是阻塞还是非阻塞。true 表示阻塞直到定时器到期。false 表示非阻塞（未到期返回 EAGAIN）。
 * @return 0 成功，非 0 为错误码。
 */
int rtfsTimerConstructor(RtfsTimer *this, bool isBlockCheck);

/**
 * @brief 析构定时器。
 */
void rtfsTimerDestructor(RtfsTimer *this);

/**
 * @brief 设置定时器参数。不会立即启动定时器，需调用 rtfsTimerStart 生效。
 * @param expirationTime timespec 类型标准的定时时间。
 * @param isPeriod 是否为周期定时器。
 */
void rtfsTimerSet(RtfsTimer *this, struct timespec *expirationTime, bool isPeriod);

/**
 * @brief 启动定时器。使用 rtems_timer_fire_after 启动定时器。到期后触发回调，发送 event 给 ownerTask。
 * @note 需配合 rtfsTimerCheckExpire 函数一起使用，具体见函数注释。
 * @return 0 成功，非 0 为错误码。
 */
int rtfsTimerStart(RtfsTimer *this);

/**
 * @brief 停止定时器。取消已启动的定时器，不会清空 pendingExpirations。
 * @return 0 成功，非 0 为错误码。
 */
int rtfsTimerStop(RtfsTimer *this);

/**
 * @brief 检查定时器是否到期。
 * @note 若定时器设置了 isBlockCheck，则阻塞到到期后返回 0；若发生其它错误，返回 errno。若定时器未设置 isBlockCheck，直接返回：若到期，返回 0；若未到期，则返回 EAGAIN；若发生其它错误，返回 errno。若 overflowTimes 不为 NULL，则保存到期次数。
 * @details 定时器在启动后会在后台按时触发，并通过 RTEMS 的 event 机制向所属任务发送通知，同时累计触发次数；但这些通知不会自动影响业务逻辑，只有在调用 rtfsTimerCheckExpire 时才会主动“消费”这些事件并读取到期信息，因此如果不调用该函数，就无法在代码层面感知到定时器已经触发。
 */
int rtfsTimerCheckExpire(RtfsTimer *this, uint64_t *overflowTimes);


#endif
