#ifndef _RTFS_MULTITHREAD_H_
#define _RTFS_MULTITHREAD_H_

// 用户态、内核多线程同步和互斥工具的兼容层。
#include <pthread.h>


typedef pthread_mutex_t mutex_t;
// TODO 飞腾的 BSP 并未实现 POSIX 标准的自旋锁 spinlock_t 和读写锁 rwlock_t。
// typedef pthread_spinlock_t spinlock_t;
// typedef pthread_rwlock_t rwlock_t;
typedef pthread_cond_t cond_t;


__attribute__((unused)) int rtfsMutexInit(mutex_t *self);

__attribute__((unused)) int rtfsMutexLock(mutex_t *self);

__attribute__((unused)) int rtfsMutexTrylock(mutex_t *self);

__attribute__((unused)) int rtfsMutexUnlock(mutex_t *self);

__attribute__((unused)) int rtfsMutexDestroy(mutex_t *self);

// __attribute__((unused)) int rtfsSpinInit(spinlock_t *self);

// __attribute__((unused)) int rtfsSpinLock(spinlock_t *self);

// __attribute__((unused)) int rtfsMutexUnlock(spinlock_t *self);

// __attribute__((unused)) int rtfsSpinDestroy(spinlock_t *self);

// __attribute__((unused)) int rtfsRwlockInit(rwlock_t *self);

// __attribute__((unused)) int rtfsRwlockRdlock(rwlock_t *self);

// __attribute__((unused)) int rtfsRwlockWrlock(rwlock_t *self);

// __attribute__((unused)) int rtfsRwlockUnlock(rwlock_t *self);

// __attribute__((unused)) int rtfsRwlockDestroy(rwlock_t *self);

__attribute__((unused)) int rtfsCondInit(cond_t *self);

__attribute__((unused)) int rtfsCondWait(cond_t *self, mutex_t *mtx);

__attribute__((unused)) int rtfsCondSignal(cond_t *self);

__attribute__((unused)) int rtfsCondBroadcast(cond_t *self);

__attribute__((unused)) int rtfsCondDestroy(cond_t *self);


#endif
