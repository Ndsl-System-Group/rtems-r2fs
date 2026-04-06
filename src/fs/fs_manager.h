#pragma once

#include "srmap_utils.h" // TODO(刘): 应该 struct srmap_utils

typedef struct file_system_manager file_system_manager;
typedef struct comm_dev comm_dev;
typedef struct RtfsSuperBlock RtfsSuperBlock;
typedef struct super_manager super_manager;
typedef struct node_block_cache node_block_cache;
typedef struct dir_data_block_cache dir_data_block_cache;
typedef struct SitNatCache SitNatCache;
// typedef struct SrmapUtils SrmapUtils;
typedef struct fd_array fd_array;
typedef struct JournalContainer JournalContainer;
// typedef struct replace_protect_manager replace_protect_manager;

// ==================== 生命周期管理 ====================

int fileSystemManagerSetup(comm_dev *dev);
void fileSystemManagerFini(void);
file_system_manager *fileSystemManagerGetInstance(void);

// ==================== 锁操作 API ====================

void fileSystemManagerMetaLock(file_system_manager *this);
void fileSystemManagerMetaUnlock(file_system_manager *this);
void fileSystemManagerFreezeLock(file_system_manager *this);
void fileSystemManagerFreezeUnLock(file_system_manager *this);

// ==================== 成员访问器 ====================

RtfsSuperBlock *fileSystemManagerGetSuperBlkMem(file_system_manager *this);
super_manager *fileSystemManagerGetSuperManager(file_system_manager *this);
node_block_cache *fileSystemManagerGetNodeCache(file_system_manager *this);
dir_data_block_cache *fileSystemManagerGetDirDataCache(file_system_manager *this);
SitNatCache *fileSystemManagerGetSitCache(file_system_manager *this);
SitNatCache *fileSystemManagerGetNatCache(file_system_manager *this);
SrmapUtils *fileSystemManagerGetSrmapUtils(file_system_manager *this);
fd_array *fileSystemManagerGetFdArray(file_system_manager *this);
JournalContainer *fileSystemManagerGetCurJournal(file_system_manager *this);