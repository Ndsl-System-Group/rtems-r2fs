#pragma once

#include "srmap_utils.h"

typedef struct file_system_manager file_system_manager;
typedef struct comm_dev comm_dev;
typedef struct RtfsSuperBlock RtfsSuperBlock;
typedef struct super_manager super_manager;
typedef struct NodeBlockCache NodeBlockCache;
typedef struct SitNatCache SitNatCache;
typedef struct JournalContainer JournalContainer;

// ==================== 生命周期管理 ====================

int fileSystemManagerSetup(comm_dev *dev);
void fileSystemManagerFini(void);
file_system_manager *fileSystemManagerGetInstance(void);
int fileSystemManagerFlushForUnmount(file_system_manager *this);

// ==================== 锁操作 API ====================

void fileSystemManagerMetaLock(file_system_manager *this);
void fileSystemManagerMetaUnlock(file_system_manager *this);
void fileSystemManagerFreezeLock(file_system_manager *this);
void fileSystemManagerFreezeUnLock(file_system_manager *this);

// ==================== 成员访问器 ====================

RtfsSuperBlock *fileSystemManagerGetSuperBlkMem(file_system_manager *this);
super_manager *fileSystemManagerGetSuperManager(file_system_manager *this);
NodeBlockCache *fileSystemManagerGetNodeCache(file_system_manager *this);
SitNatCache *fileSystemManagerGetSitCache(file_system_manager *this);
SitNatCache *fileSystemManagerGetNatCache(file_system_manager *this);
SrmapUtils *fileSystemManagerGetSrmapUtils(file_system_manager *this);
JournalContainer *fileSystemManagerGetCurJournal(file_system_manager *this);
comm_dev *fileSystemManagerGetDevice(file_system_manager *this);

/**
 * @brief 测试用故障注入：指定 Setup 期间在哪一步返回失败。0 表示关闭注入。
 */
void fileSystemManagerSetSetupFailureStepForTest(uint32_t step);
