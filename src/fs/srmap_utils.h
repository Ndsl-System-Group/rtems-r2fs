#ifndef _SRMAP_UTILS_H_
#define _SRMAP_UTILS_H_

#include "utils/types.h"
#include "cache/block_buffer.h"
#include "uthash/uthash.h"


struct file_system_manager;


/**
 * @brief SrmapCache 哈希表条目。
 */
typedef struct
{
    uint32_t lpa;    // key: srmap block 的逻辑地址。
    BlockBuffer blk; // value: block 缓存。
    UT_hash_handle hh;
} SrmapCacheEntry;

/**
 * @brief 记录 SrmapCache 中，哪些 lpa 是 dirty 的。
 */
typedef struct
{
    uint32_t lpa; // key: dirty 块的 lpa 号。
    UT_hash_handle hh;
} DirtyBlkEntry;

/**
 * @brief Srmap 工具结构体。
 */
typedef struct
{
    struct file_system_manager *fsManager;
    uint32_t srmapStartLpa;

    SrmapCacheEntry *srmapCache;
    DirtyBlkEntry *dirtyBlks;
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
