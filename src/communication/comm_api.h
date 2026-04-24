#ifndef _COMM_API_H_
#define _COMM_API_H_

#include "communication/vendor_cmds.h"


struct comm_dev;


typedef enum comm_cmd_result
{
    COMM_CMD_SUCCESS,
    COMM_CMD_CQE_ERROR,
    COMM_CMD_TID_QUERY_ERROR
} comm_cmd_result;

// 通信层异步接口的回调函数。
typedef void (*comm_async_cb_func)(comm_cmd_result, void *);

typedef enum comm_io_direction
{
    COMM_IO_READ,
    COMM_IO_WRITE
} comm_io_direction;


int comm_submit_sync_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_io_direction dir);
int comm_submit_async_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_async_cb_func cbFunc, void *cbArg, comm_io_direction dir);

// int comm_submit_sync_migrate_request(struct comm_dev *dev, migrate_task *task);
// int comm_submit_async_migrate_request(struct comm_dev *dev, migrate_task *task, comm_async_cb_func cbFunc, void *cbArg);

// int comm_submit_sync_path_lookup_request(struct comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res);
// int comm_submit_async_path_lookup_request(struct comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res, comm_async_cb_func cbFunc, void *cbArg);

// int comm_submit_sync_filemapping_search_request(struct comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len);
// int comm_submit_async_filemapping_search_request(struct comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len, comm_async_cb_func cbFunc, void *cbArg);

// 更新元数据日志尾指针命令。原位置是 originLpa，新写入了 writeBlockNum 个 block 的日志。
int comm_submit_sync_update_metajournal_tail_request(struct comm_dev *dev, uint64_t originLpa, uint32_t writeBlockNum);
int comm_submit_async_update_metajournal_tail_request(struct comm_dev *dev, uint64_t originLpa, uint32_t writeBlockNum, comm_async_cb_func cbFunc, void *cbArg);

// 获取元数据日志头指针命令。结果存放在 headLpa 中，head_lpa 必须是可 DMA 的内存。需要修改，实际上用此命令同时获取头尾指针，不仅是头指针。
int comm_submit_sync_get_metajournal_head_request(struct comm_dev *dev, uint64_t *headLpa);
int comm_submit_async_get_metajournal_head_request(struct comm_dev *dev, uint64_t *headLpa, comm_async_cb_func cbFunc, void *cbArg);

// 文件系统模块初始化，此接口为同步接口。
int comm_submit_fs_module_init_request(struct comm_dev *dev);

// 文件系统 SSD DB 区域初始化，同步接口。
int comm_submit_fs_db_init_request(struct comm_dev *dev);

// 用 DB 内容恢复文件系统超级块，同步接口。
int comm_submit_fs_recover_from_db_request(struct comm_dev *dev);

// 清空元数据日志，同步接口。
int comm_submit_clear_metajournal_request(struct comm_dev *dev);

// 启动元数据日志应用，同步接口。
int comm_submit_start_apply_journal_request(struct comm_dev *dev);

// 挂起元数据日志应用，同步接口。
int comm_submit_stop_apply_journal_request(struct comm_dev *dev);


#endif
