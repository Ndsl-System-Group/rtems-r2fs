#ifndef _IO_UTILS_H_
#define _IO_UTILS_H_

#include "utils/rtfs_multithread.h"

#include "communication/comm_api.h"

#include <stdatomic.h>


#define LPA_TO_LBA(lpa) ((lpa) * 8)
#define LBA_PER_LPA 8


/**
 * @brief 异步向量 I/O 的同步器。用于需要等待多个非连续缓冲区的异步 I/O 都完成的情景，不允许多个线程同时等待。
 */
typedef struct AsyncVecioSynchronizer
{
    comm_cmd_result ioRes;

    atomic_uint_fast64_t ioNum;

    mutex_t mutex;
    cond_t cond;

    bool isCompleted;
} AsyncVecioSynchronizer;


/**
 * @brief 初始化异步向量 I/O 的同步器。ioNum 为该 I/O 的次数。
 */
void asyncVecioSynchronizerInit(AsyncVecioSynchronizer *this, uint64_t ioNum);

/**
 * @brief 销毁异步向量 I/O 的同步器。
 */
void asyncVecioSynchronizerDestroy(AsyncVecioSynchronizer *this);

/**
 * @brief 完成一次（一般是一个独立缓存区）I/O 时，调用此方法。一般由回调函数调用。
 */
void asyncVecioSynchronizerCpltOnce(AsyncVecioSynchronizer *this, comm_cmd_result ioResult);

/**
 * @brief 等待该异步向量 I/O 完成。
 */
comm_cmd_result asyncVecioSynchronizerWaitCplt(AsyncVecioSynchronizer *this);


/**
 * @brief 提供通信层异步向量 I/O 的通用回调。
 */
void asyncVecioSynchronizerGenericCallback(comm_cmd_result res, void *arg);


#endif
