#ifndef _SIT_UTILS_H_
#define _SIT_UTILS_H_

#include "utils/types.h"


struct file_system_manager;


/**
 * @brief LPA 在 segment 中的位置。记录一个逻辑页在 segment 中的 segment ID 和 segment 内偏移。
 */
typedef struct
{
    uint32_t segId;  // segment ID。
    uint32_t offset; // segment 内的块偏移。
} SegPos;

/**
 * @brief SIT 表中某个 segment 的位置。用于表示 SIT block LPA 和 block 内 entry 索引。
 */
typedef struct
{
    uint32_t sitLpa; // SIT block 的逻辑页地址。
    uint32_t idx;    // block 内 entry 索引。
} SitPos;

/**
 * @brief SIT LPA 映射器。负责访问 Segment Information Table (SIT)，管理 segment 内块的有效性。提供对 LPA 的验证/失效操作，并可计算 segment 相关位置。
 */
typedef struct
{
    struct file_system_manager *fsManager; // 文件系统管理器。
    uint32_t seg0StartLpa;                 // segment 0 对应的起始 LPA。
    uint32_t segCount;                     // 总 segment 数量。
    uint32_t sitStartLpa;                  // SIT 表起始 LPA。
    uint32_t sitSegmentCnt;                // SIT 占用的 segment 数量。
} SitOperator;


/**
 * @brief 初始化 SIT LPA 映射器。
 */
void sitOperatorInit(SitOperator *this, struct file_system_manager *fsManager, uint32_t seg0StartLpa, uint32_t segCount, uint32_t sitStartLpa, uint32_t sitSegmentCnt);

/**
 * @brief 将 LPA 设置为无效。
 */
void sitInvalidateLpa(SitOperator *this, uint32_t lpa);

/**
 * @brief 将 LPA 设置为有效。
 */
void sitValidateLpa(SitOperator *this, uint32_t lpa);

/**
 * @brief 获取 LPA 对应的 segment 位置。
 */
SegPos sitGetSegPosOfLpa(SitOperator *this, uint32_t lpa);

/**
 * @brief 获取 segment 在 SIT 表中的位置。
 */
SitPos sitGetSegIdPosInSit(SitOperator *this, uint32_t segId);

/**
 * @brief 获取 segment 的第一个 LPA。
 */
uint32_t sitGetFirstLpaOfSegId(SitOperator *this, uint32_t segId);


#endif
