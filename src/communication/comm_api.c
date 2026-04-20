#include "comm_api.h"


// TODO
int comm_submit_sync_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count, comm_io_direction dir)
{
}

int comm_submit_async_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count, comm_async_cb_func cb_func, void *cb_arg, comm_io_direction dir)
{
}

int comm_submit_sync_migrate_request(comm_dev *dev, migrate_task *task)
{
}

int comm_submit_async_migrate_request(comm_dev *dev, migrate_task *task, comm_async_cb_func cb_func, void *cb_arg)
{
}

int comm_submit_sync_path_lookup_request(comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res)
{
}

int comm_submit_async_path_lookup_request(comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res, comm_async_cb_func cb_func, void *cb_arg)
{
}

int comm_submit_sync_filemapping_search_request(comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len)
{
}

int comm_submit_async_filemapping_search_request(comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len, comm_async_cb_func cb_func, void *cb_arg)
{
}

int comm_submit_sync_update_metajournal_tail_request(comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num)
{
}

int comm_submit_async_update_metajournal_tail_request(comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num, comm_async_cb_func cb_func, void *cb_arg)
{
}

int comm_submit_sync_get_metajournal_head_request(comm_dev *dev, uint64_t *head_lpa)
{
}

int comm_submit_async_get_metajournal_head_request(comm_dev *dev, uint64_t *head_lpa, comm_async_cb_func cb_func, void *cb_arg)
{
}

int comm_submit_fs_module_init_request(comm_dev *dev)
{
}

int comm_submit_fs_db_init_request(comm_dev *dev)
{
}

int comm_submit_fs_recover_from_db_request(comm_dev *dev)
{
}

int comm_submit_clear_metajournal_request(comm_dev *dev)
{
}

int comm_submit_start_apply_journal_request(comm_dev *dev)
{
}

int comm_submit_stop_apply_journal_request(comm_dev *dev)
{
}
