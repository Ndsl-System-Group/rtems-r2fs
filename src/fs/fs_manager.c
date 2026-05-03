#include "fs_manager.h"

#include <rtems/thread.h>
#include <stdbool.h>
#include <threads.h>
#include <pthread.h>


#include "sit_utils.h"
#include "nat_utils.h"
#include "cache/node_block_cache.h"
#include "cache/sit_nat_cache.h"
#include "cache/super_cache.h"
#include "cow_reclaim_registry.h"
#include "super_manager.h"
#include "srmap_utils.h"

typedef struct file_system_manager
{
    rtems_recursive_mutex fs_meta_lock_; // 元数据递归互斥锁
    pthread_rwlock_t fs_freeze_lock_;    // 冻结读写锁

    SuperCache super_cache_;               // 超级块缓存
    struct RtfsSuperBlock *super_blk_mem_; // 超级块内存镜像
    super_manager *sp_manager_;            // 超级块管理器
    NodeBlockCache *node_cache_;           // 节点块缓存
    dir_data_block_cache *dir_data_cache_; // 目录数据块缓存

    SrmapUtils *srmap_utils_; // SRMAP
    SitNatCache *sit_cache_;  // 缓存
    SitNatCache *nat_cache_;

    comm_dev *dev_;    // 底层设备抽象
    fd_array *fd_arr_; // 文件描述符数组

    JournalContainer *cur_journal_; // 当前日志容器
    // replace_protect_manager *rp_manager_;    // 替换保护管理器
    bool is_unrecoverable_; // 不可恢复错误标志
} file_system_manager;

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
static rtems_recursive_mutex g_fs_manager_init_lock = RTEMS_RECURSIVE_MUTEX_INITIALIZER("FsManagerInitLock");

// ==================== 内部辅助函数 ====================

static void _internal_destroy(file_system_manager *this);

static int _init_locks(file_system_manager *this)
{
    int ret;

    rtems_recursive_mutex_init(&this->fs_meta_lock_, "FsMetaLock");

    ret = pthread_rwlock_init(&this->fs_freeze_lock_, NULL);
    if (ret != 0)
    {
        rtems_recursive_mutex_destroy(&this->fs_meta_lock_);
        return -ret;
    }

    return 0;
}

static void _destroy_locks(file_system_manager *this)
{
    rtems_recursive_mutex_destroy(&this->fs_meta_lock_);
    pthread_rwlock_destroy(&this->fs_freeze_lock_);
}


static file_system_manager *_internal_create(struct comm_dev *dev)
{
    file_system_manager *this = (file_system_manager *)calloc(1, sizeof(file_system_manager));
    bool locks_inited = false;

    if (!this) return NULL;

    int ret = _init_locks(this);
    if (ret != 0)
    {
        goto fail_before_init;
    }
    locks_inited = true;

    this->dev_ = dev;

    // super block 由 fs_manager 统一装配并持有，后续由 super_manager 消费其内存镜像。
    superCacheInit(&this->super_cache_, dev, super_block_lpa);
    superCacheReadSuperBlock(&this->super_cache_);
    this->super_blk_mem_ = superCacheGet(&this->super_cache_);
    this->sit_cache_ = malloc(sizeof(SitNatCache));
    if (this->sit_cache_ == NULL)
    {
        goto fail_after_init;
    }
    sitNatCacheInit(this->sit_cache_, dev, 100);

    this->nat_cache_ = malloc(sizeof(SitNatCache));
    if (this->nat_cache_ == NULL)
    {
        goto fail_after_init;
    }
    sitNatCacheInit(this->nat_cache_, dev, 100);

    this->node_cache_ = malloc(sizeof(NodeBlockCache));
    if (this->node_cache_ == NULL)
    {
        goto fail_after_init;
    }
    nodeBlockCacheInit(this->node_cache_, this, 100);

    this->sp_manager_ = superManagerCreate(this);
    if (this->sp_manager_ == NULL)
    {
        goto fail_after_init;
    }

    cowReclaimRegistryInit(this);

    return this;

fail_after_init:
    _internal_destroy(this);
    return NULL;

fail_before_init:
    if (locks_inited)
    {
        _destroy_locks(this);
    }
    free(this);
    return NULL;
}

static void _internal_destroy(file_system_manager *this)
{
    if (!this) return;

    if (this->node_cache_ != NULL)
    {
        nodeBlockCacheDestroy(this->node_cache_);
        free(this->node_cache_);
        this->node_cache_ = NULL;
    }
    if (this->sit_cache_ != NULL)
    {
        sitNatCacheDestroy(this->sit_cache_);
        free(this->sit_cache_);
        this->sit_cache_ = NULL;
    }
    if (this->nat_cache_ != NULL)
    {
        sitNatCacheDestroy(this->nat_cache_);
        free(this->nat_cache_);
        this->nat_cache_ = NULL;
    }
    if (this->srmap_utils_ != NULL)
    {
        srmapUtilsDestroy(this->srmap_utils_);
        free(this->srmap_utils_);
        this->srmap_utils_ = NULL;
    }
    superManagerDestroy(this->sp_manager_);
    this->sp_manager_ = NULL;

    cowReclaimRegistryDestroy();

    superCacheDestroy(&this->super_cache_);
    this->super_blk_mem_ = NULL;

    _destroy_locks(this);

    free(this);
}

// ==================== 公开 API 实现 ====================

int fileSystemManagerSetup(comm_dev *dev)
{
    int ret = 0;
    rtems_recursive_mutex_lock(&g_fs_manager_init_lock);

    if (g_fs_manager != NULL)
    {
        ret = -1;
        goto out;
    }

    g_fs_manager = _internal_create(dev);
    if (!g_fs_manager)
    {
        ret = -ENOMEM;
    }

out:
    rtems_recursive_mutex_unlock(&g_fs_manager_init_lock);
    return ret;
};

void fileSystemManagerFini(void)
{
    rtems_recursive_mutex_lock(&g_fs_manager_init_lock);

    if (g_fs_manager != NULL)
    {
        _internal_destroy(g_fs_manager);
        g_fs_manager = NULL;
    }

    rtems_recursive_mutex_unlock(&g_fs_manager_init_lock);
};

file_system_manager *fileSystemManagerGetInstance(void)
{
    return g_fs_manager;
};

void fileSystemManagerMetaLock(file_system_manager *this)
{
    if (this) rtems_recursive_mutex_lock(&this->fs_meta_lock_);
};
void fileSystemManagerMetaUnlock(file_system_manager *this)
{
    if (this) rtems_recursive_mutex_unlock(&this->fs_meta_lock_);
};
void fileSystemManagerFreezeLock(file_system_manager *this)
{
    if (this) pthread_rwlock_wrlock(&this->fs_freeze_lock_);
};
void fileSystemManagerFreezeUnLock(file_system_manager *this)
{
    if (this) pthread_rwlock_unlock(&this->fs_freeze_lock_);
};

// ==================== Getter 函数实现 ====================

RtfsSuperBlock *fileSystemManagerGetSuperBlkMem(file_system_manager *this) { return this ? this->super_blk_mem_ : NULL; };
super_manager *fileSystemManagerGetSuperManager(file_system_manager *this) { return this ? this->sp_manager_ : NULL; };
NodeBlockCache *fileSystemManagerGetNodeCache(file_system_manager *this) { return this ? this->node_cache_ : NULL; };
dir_data_block_cache *fileSystemManagerGetDirDataCache(file_system_manager *this) { return this ? this->dir_data_cache_ : NULL; };
SitNatCache *fileSystemManagerGetSitCache(file_system_manager *this) { return this ? this->sit_cache_ : NULL; }
SitNatCache *fileSystemManagerGetNatCache(file_system_manager *this) { return this ? this->nat_cache_ : NULL; }
SrmapUtils *fileSystemManagerGetSrmapUtils(file_system_manager *this) { return this ? this->srmap_utils_ : NULL; };
fd_array *fileSystemManagerGetFdArray(file_system_manager *this) { return this ? this->fd_arr_ : NULL; };
JournalContainer *fileSystemManagerGetCurJournal(file_system_manager *this) { return this ? this->cur_journal_ : NULL; };
comm_dev *fileSystemManagerGetDevice(file_system_manager *this) { return this ? this->dev_ : NULL; }
