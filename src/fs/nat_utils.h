#ifndef _NAT_UTILS_H_
#define _NAT_UTILS_H_

#include "utils/types.h"


struct file_system_manager;


/**
 * @brief NID 在 NAT 中的位置。
 */
typedef struct
{
    uint32_t lpa; // NAT block 的 LPA。
    uint32_t idx; // 该 block 内的表项索引。
} NatNidPos;

/**
 * @brief NAT LPA 映射器。
 */
typedef struct
{
    uint32_t natStartLpa;   // NAT 表起始 LPA。
    uint32_t natSegmentCnt; // NAT 表占用的 segment 数量。
    struct file_system_manager *fsManager;
} NatLpaMapping;


/**
 * @brief 初始化。
 */
void natLpaMappingInit(NatLpaMapping *this, struct file_system_manager *fsManager, uint32_t natStartLpa, uint32_t natSegmentCnt);

/**
 * @brief 返回 nid 在 NAT 表中的位置：<lpa, idx>。
 */
NatNidPos natGetNidPos(NatLpaMapping *this, uint32_t nid);

/**
 * @brief 访问 NAT 表，得到 nid 对应的 LPA。
 */
uint32_t natGetLpaOfNid(NatLpaMapping *this, uint32_t nid);

/**
 * @brief 修改 NAT 表中 nid 对应项的 LPA。
 */
void natSetLpaOfNid(NatLpaMapping *this, uint32_t nid, uint32_t newLpa);


#endif
