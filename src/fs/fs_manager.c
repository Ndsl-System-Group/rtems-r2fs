#include "fs_manager.h"

#include <rtems/thread.h>
#include <stdbool.h>

typedef struct file_system_manager
{
    rtems_recursive_mutex fs_meta_lock;     // 元数据递归互斥锁
    pthread_rwlock_t fs_freeze_lock;        // 冻结读写锁

    struct RtfsSuperBlock *super_blk_mem;   // 超级块内存镜像
    super_manager *sp_manager;              // 超级块管理器
    node_block_cache *node_cache;           // 节点块缓存
    dir_data_block_cache *dir_data_cache;   // 目录数据块缓存
    SIT_cache *sit_cache;                   // 段信息表缓存
    NAT_cache *nat_cache;                   // 节点地址表缓存
    srmap_utils *srmap_util;                // 空间回收映射工具

    dev_t *dev;                             // 底层设备抽象
    fd_array *fd_arr;                       // 文件描述符数组

    journal_container *cur_journal;         // 当前日志容器
    replace_protect_manager *rp_manager;    // 替换保护管理器
    bool is_unrecoverable;                  // 不可恢复错误标志
}file_system_manager;

static uint64_t super_block_lpa;
static size_t dentry_cache_size;
static size_t node_cache_size;
static size_t dir_data_cache_size;
static size_t sit_cache_size;
static size_t nat_cache_size;
static size_t file_cache_size;
static size_t fd_array_size;

// ==================== 全局静态变量 ====================

static file_system_manager *g_fs_manager = NULL;
static rtems_mutex g_fs_manager_init_lock = RTEMS_RECURSIVE_MUTEX_INITIALIZER("FsManagerInitLock");

// ==================== 内部辅助函数 ====================

static int _init_locks(file_system_manager *this)
{
    int ret;
    
    rtems_recursive_mutex_init(&this->fs_meta_lock, "FsMetaLock");
    
    ret = pthread_rwlock_init(&this->fs_freeze_lock, NULL);
    if (ret != 0) {
        rtems_recursive_mutex_destroy(&this->fs_meta_lock);
        return -ret;
    }
    
    return 0;
}

static void _destroy_locks(file_system_manager *this)
{
    rtems_recursive_mutex_destroy(&this->fs_meta_lock);
    pthread_rwlock_destroy(&this->fs_freeze_lock);
}


static file_system_manager* _internal_create(struct comm_dev *dev) {
    file_system_manager *this = (file_system_manager*)calloc(1, sizeof(file_system_manager));
    if (!this) return NULL;

    int ret = _init_locks(this);
    if (ret != 0) {
        free(this);
        return NULL;
    }
    
    this->dev = dev;
    this->is_unrecoverable = false;
    // TODO: 从设备读取超级块到内存
    // ret = blockBufferReadFromLpa(&this->super_buf, dev, g_super_block_lpa);
    // if (ret != 0) {
    //     _destroy_locks(this);
    //     free(this);
    //     return NULL;
    // }
    // this->super_blk_mem = (struct RtfsSuperBlock*)blockBufferGetPtr(&this->super_buf);
    
    // TODO: 初始化各个子模块
    // this->sp_manager = SuperManagerCreate(this);
    // this->node_cache = NodeCacheCreate(this);
    // ...

    return this;
}

static void _internal_destroy(file_system_manager *this) {
    if (!this) return;

    // 1. 销毁子模块 (按依赖逆序)
    // TODO: 添加子模块销毁调用
    // DestroyReplaceProtectManager(this->rp_manager);
    // DestroyJournalContainer(this->cur_journal);
    // DestroyFdArray(this->fd_arr);
    // DestroySrmapUtils(this->srmap_util);
    // DestroyNatCache(this->nat_cache);
    // DestroySitCache(this->sit_cache);
    // DestroyDirDataCache(this->dir_data_cache);
    // DestroyNodeCache(this->node_cache);
    // DestroySuperManager(this->sp_manager);
    
    // 2. 释放超级块内存 (如果已分配)
    if (this->super_blk_mem) {
        free(this->super_blk_mem);
    }

    // 3. 销毁同步对象
    _destroy_locks(this);

    // 4. 释放主结构体
    free(this);
}

// ==================== 公开 API 实现 ====================

int FileSystemManagerSetup(comm_dev *dev){
    int ret = 0;
    rtems_mutex_lock(&g_fs_manager_init_lock);

    if (g_fs_manager != NULL) {
        ret = -1;
        goto out;
    }

    g_fs_manager = _internal_create(dev);
    if (!g_fs_manager) {
        ret = -ENOMEM;
    }

out:
    rtems_mutex_unlock(&g_fs_manager_init_lock);
    return ret;
};

void FileSystemManagerFini(void){
    rtems_mutex_lock(&g_fs_manager_init_lock);

    if (g_fs_manager != NULL) {
        _internal_destroy(g_fs_manager);
        g_fs_manager = NULL;
    }

    rtems_mutex_unlock(&g_fs_manager_init_lock);
};

file_system_manager* FileSystemManagerGetInstance(void){
    return g_fs_manager;
};

void FileSystemManagerMetaLock(file_system_manager *this){
    if (this) rtems_recursive_mutex_lock(&this->fs_meta_lock);
};
void FileSystemManagerMetaUnlock(file_system_manager *this){
    if (this) rtems_recursive_mutex_unlock(&this->fs_meta_lock);
};
void FileSystemManagerFreezeLock(file_system_manager *this){
    if (this) pthread_rwlock_wrlock(&this->fs_freeze_lock);
};
void FileSystemManagerFreezeUnLock(file_system_manager *this){
    if (this) pthread_rwlock_unlock(&this->fs_freeze_lock);
};

// ==================== Getter 函数实现 ====================

RtfsSuperBlock*         FileSystemManagerGetSuperBlkMem(file_system_manager *this)      { return this ? this->super_blk_mem : NULL; };
super_manager*          FileSystemManagerGetSuperManager(file_system_manager *this)     { return this ? this->sp_manager : NULL; };
node_block_cache*       FileSystemManagerGetNodeCache(file_system_manager *this)        { return this ? this->node_cache : NULL; };
dir_data_block_cache*   FileSystemManagerGetDirDataCache(file_system_manager *this)     { return this ? this->dir_data_cache : NULL; };
SIT_cache*              FileSystemManagerGetSitCache(file_system_manager *this)         { return this ? this->sit_cache : NULL; };
NAT_cache*              FileSystemManagerGetNatCache(file_system_manager *this)         { return this ? this->nat_cache : NULL; };
srmap_utils*            FileSystemManagerGetSrmapUtils(file_system_manager *this)       { return this ? this->srmap_util : NULL; };
fd_array*               FileSystemManagerGetFdArray(file_system_manager *this)          { return this ? this->fd_arr : NULL; };
journal_container*      FileSystemManagerGetCurJournal(file_system_manager *this)       { return this ? this->cur_journal : NULL; };
