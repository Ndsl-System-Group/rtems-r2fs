#ifndef _IO_UTILS_H_
#define _IO_UTILS_H_


#define LPA_TO_LBA(lpa) ((lpa) * 8)
#define LBA_PER_LPA 8


/**
 * @brief 异步向量 I/O 的同步器。用于需要等待多个非连续缓冲区的异步 I/O 都完成的情景，不允许多个线程同时等待。
 */
typedef struct AsyncVecioSynchronizer
{
    // TODO
} AsyncVecioSynchronizer;


#endif
