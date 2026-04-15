#include "rtfs_multithread.h"


int rtfsMutexInit(mutex_t *self)
{
    return pthread_mutex_init(self, NULL);
}

int rtfsMutexLock(mutex_t *self)
{
    return pthread_mutex_lock(self);
}

int rtfsMutexTrylock(mutex_t *self)
{
    return pthread_mutex_trylock(self);
}

int rtfsMutexUnlock(mutex_t *self)
{
    return pthread_mutex_unlock(self);
}

int rtfsMutexDestroy(mutex_t *self)
{
    return pthread_mutex_destroy(self);
}


int rtfsCondInit(cond_t *self)
{
    return pthread_cond_init(self, NULL);
}

int rtfsCondWait(cond_t *self, mutex_t *mtx)
{
    return pthread_cond_wait(self, mtx);
}

int rtfsCondSignal(cond_t *self)
{
    return pthread_cond_signal(self);
}

int rtfsCondBroadcast(cond_t *self)
{
    return pthread_cond_broadcast(self);
}

int rtfsCondDestroy(cond_t *self)
{
    return pthread_cond_destroy(self);
}


// int rtfsSpinInit(spinlock_t *self)
// {
//     return pthread_spin_init(self, PTHREAD_PROCESS_PRIVATE);
// }

// int rtfsSpinLock(spinlock_t *self)
// {
//     return pthread_spin_lock(self);
// }

// int rtfsMutexUnlock(spinlock_t *self)
// {
//     return pthread_spin_unlock(self);
// }

// int rtfsSpinDestroy(spinlock_t *self)
// {
//     return pthread_spin_destroy(self);
// }


// int rtfsRwlockInit(rwlock_t *self)
// {
//     return pthread_rwlock_init(self, NULL);
// }

// int rtfsRwlockRdlock(rwlock_t *self)
// {
//     return pthread_rwlock_rdlock(self);
// }

// int rtfsRwlockWrlock(rwlock_t *self)
// {
//     return pthread_rwlock_wrlock(self);
// }

// int rtfsRwlockUnlock(rwlock_t *self)
// {
//     return pthread_rwlock_unlock(self);
// }

// int rtfsRwlockDestroy(rwlock_t *self)
// {
//     return pthread_rwlock_destroy(self);
// }
