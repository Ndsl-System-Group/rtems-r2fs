#ifndef _BLOCK_BUFFER_H_
#define _BLOCK_BUFFER_H_

#include "utils/types.h"


struct comm_dev;


#define BLOCK_BUFFER_SIZE 4096


/**
 * @struct BlockBuffer
 * @brief 表示一个固定大小（4KB）的块级 DMA 缓冲区，用于与块设备进行读写。
 **/
typedef struct
{
    char *buffer;
} BlockBuffer;


/**
 * @brief 初始化一个 blockBuffer，并分配 DMA-safe 的 4KB 缓冲区。
 */
int blockBufferInit(BlockBuffer *this);

/**
 * @brief 释放 blockBuffer 持有的 DMA 缓冲区。
 */
void blockBufferDestroy(BlockBuffer *this);

/**
 * @brief 获取 blockBuffer 内部 DMA 缓冲区指针。
 */
char *blockBufferGetPtr(BlockBuffer *this);

/**
 * @brief 将外部缓冲区中的 4KB 数据拷贝到 blockBuffer 中。
 */
void blockBufferCopyContentFromBuf(BlockBuffer *this, const char *src);

/**
 * @brief 从块设备读取指定 LPA 的数据到 blockBuffer（同步）。
 */
int blockBufferReadFromLpa(BlockBuffer *this, struct comm_dev *dev, uint32_t lpa);

/**
 * @brief 将 blockBuffer 中的数据写入指定 LPA（同步）。
 */
int blockBufferWriteToLpaSync(BlockBuffer *this, struct comm_dev *dev, uint32_t lpa);

/**
 * @brief 将 blockBuffer 中的数据写入指定 LPA（异步）。
 */
// int blockBufferWriteToLpaAsync(BlockBuffer *this, struct comm_dev *dev, uint32_t lpa, comm_async_cb_func cbFunc, void *cbArg);


#endif
