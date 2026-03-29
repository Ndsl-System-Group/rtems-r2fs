#ifndef _SRMAP_UTILS_H_
#define _SRMAP_UTILS_H_

#include "utils/types.h"
#include "cache/block_buffer.h"
#include "klib/khash.h"


struct file_system_manager;


KHASH_MAP_INIT_INT(khsc, BlockBuffer)

KHASH_SET_INIT_INT(khdb)

/**
 * @brief Srmap 工具结构体。
 */
typedef struct
{
    struct file_system_manager *fsManager;
    uint32_t srmapStartLpa;

    khash_t(khsc) * srmapCache;
    khash_t(khdb) * dirtyBlks;
} SrmapUtils;


/**
 * @brief 初始化 Srmap 工具。
 */
void srmapUtilsInit(SrmapUtils *this, struct file_system_manager *fsManager);

/**
 * @brief 销毁 Srmap 工具。
 */
void srmapUtilsDestroy(SrmapUtils *this);

/**
 * @brief 写入数据块 Srmap 信息。
 */
void srmapUtilsWriteSrmapOfData(SrmapUtils *this, uint32_t dataLpa, uint32_t ino, uint32_t blkoff);

/**
 * @brief 写入 node 块 Srmap 信息。
 */
void srmapUtilsWriteSrmapOfNode(SrmapUtils *this, uint32_t nodeLpa, uint32_t nid);

/**
 * @brief 将所有 dirty 的 srmap block 同步写回原位。
 */
void srmapUtilsWriteDirtySrmapSync(SrmapUtils *this);

/**
 * @brief 清空 Srmap 缓存。
 */
void srmapUtilsClearCache(SrmapUtils *this);


#endif
