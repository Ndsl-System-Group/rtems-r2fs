#pragma once

// struct super_cache;
struct super_manager;
// struct dentry_cache;
struct node_block_cache;
struct dir_data_block_cache;
struct SIT_cache;
struct NAT_cache;
// struct file_obj_cache;
struct srmap_utils;
struct fd_array;
struct journal_container;
struct replace_protect_manager;
struct server_thread;

struct file_system_manager
{
    // 元数据锁
    rtems_recursive_mutex fs_meta_lock;
    // 读写锁？为什么叫freeze_lock？
    pthread_rwlock_t fs_freeze_lock;

    // super_cache *super;
    super_manager *sp_manager;
    // dentry_cache *d_cache;
    node_block_cache *node_cache;
    dir_data_block_cache *dir_data_cache;
    SIT_cache *sit_cache;
    NAT_cache *nat_cache;
    // file_obj_cache *file_cache;
    srmap_utils *srmap_util;

    // 设备类型
    dev_t *dev;
    // vfs 的 dentry（rtems有无？）
    // dentry_handle root_dentry;
    fd_array *fd_arr;

    journal_container *cur_journal;
    // ? 淘汰保护管理？
    replace_protect_manager *rp_manager;
    // server_thread *server_th; （服务线程？）
    bool is_unrecoverable;

    static file_system_manager *g_fs_manager;

    static uint64_t super_block_lpa;
    static size_t dentry_cache_size;
    static size_t node_cache_size;
    static size_t dir_data_cache_size;
    static size_t sit_cache_size;
    static size_t nat_cache_size;
    static size_t file_cache_size;
    static size_t fd_array_size;
};

// 构造析构
static void InitFileSystemManager(file_system_manager *this, comm_dev *dev);

static void DestroyFileSystemManager(file_system_manager *this);

// 单例实现
static void FileSystemManagerGetInstance();

// 锁
static void FileSystemManagerMetaLock(file_system_manager *this);

static void FileSystemManagerMetaUnlock(file_system_manager *this);

static void FileSystemManagerFreezeLock(file_system_manager *this);

static void FileSystemManagerFreezeLock(file_system_manager *this);

// 获取成员
static super_manager *FileSystemManagerGetSuperManager(file_system_manager *this);

static node_block_cache *FileSystemManagerGetNodeCache(file_system_manager *this);

static dir_data_block_cache *FileSystemManagerGetDirDataCache(file_system_manager *this);

static SIT_cache *FileSystemManagerGetSitCache(file_system_manager *this);

static NAT_cache *FileSystemManagerGetNatCache(file_system_manager *this);

static srmap_utils *FileSystemManagerGetSrmapUtils(file_system_manager *this);

static fd_array *FileSystemManagerGetFdArray(file_system_manager *this);

static journal_container *FileSystemManagerGetCurJournal(file_system_manager *this);