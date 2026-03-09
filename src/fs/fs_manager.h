#pragma once

#include <threads.h>
#include <rtems/thread.h>

typedef struct file_system_manager file_system_manager;
typedef struct comm_dev comm_dev;
typedef struct RtfsSuperBlock RtfsSuperBlock;
typedef struct super_manager super_manager;
typedef struct node_block_cache node_block_cache;
typedef struct dir_data_block_cache dir_data_block_cache;
typedef struct SIT_cache SIT_cache;
typedef struct NAT_cache NAT_cache;
typedef struct srmap_utils srmap_utils;
typedef struct fd_array fd_array;
typedef struct journal_container journal_container;
typedef struct replace_protect_manager replace_protect_manager;

// ==================== 生命周期管理 ====================

int FileSystemManagerSetup(comm_dev *dev);
void FileSystemManagerFini(void);
file_system_manager* FileSystemManagerGetInstance(void);

// ==================== 锁操作 API ====================

void FileSystemManagerMetaLock(file_system_manager *this);
void FileSystemManagerMetaUnlock(file_system_manager *this);
void FileSystemManagerFreezeLock(file_system_manager *this);
void FileSystemManagerFreezeUnLock(file_system_manager *this);

// ==================== 成员访问器 ====================

RtfsSuperBlock*         FileSystemManagerGetSuperBlkMem(file_system_manager *this);
super_manager*          FileSystemManagerGetSuperManager(file_system_manager *this);
node_block_cache*       FileSystemManagerGetNodeCache(file_system_manager *this);
dir_data_block_cache*   FileSystemManagerGetDirDataCache(file_system_manager *this);
SIT_cache*              FileSystemManagerGetSitCache(file_system_manager *this);
NAT_cache*              FileSystemManagerGetNatCache(file_system_manager *this);
srmap_utils*            FileSystemManagerGetSrmapUtils(file_system_manager *this);
fd_array*               FileSystemManagerGetFdArray(file_system_manager *this);
journal_container*      FileSystemManagerGetCurJournal(file_system_manager *this);