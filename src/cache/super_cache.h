#ifndef _SUPER_CACHE_H_
#define _SUPER_CACHE_H_

#include "cache/block_buffer.h"

#include "fs/fs.h"


struct comm_dev;


/**
 * @brief 超级块缓存结构。用于缓存文件系统的 Super Block（超级块），避免频繁从设备读取。
 */
typedef struct SuperCache
{
    struct comm_dev *dev;

    uint64_t sbLpa; // 超级块所在的逻辑块地址（LPA）。初始化后不再修改。

    BlockBuffer superBlock; // 用于缓存超级块内容的块缓冲区。
} SuperCache;


/**
 * @brief 初始化超级块缓存。
 */
void superCacheInit(SuperCache *this, struct comm_dev *dev, uint64_t superBlockLpa);

/**
 * @brief 销毁超级块缓存。
 */
void superCacheDestroy(SuperCache *this);

/**
 * @brief 从设备读取超级块到缓存。
 */
void superCacheReadSuperBlock(SuperCache *this);

/**
 * @brief 获取超级块指针。
 */
struct RtfsSuperBlock *superCacheGet(SuperCache *this);


#endif
