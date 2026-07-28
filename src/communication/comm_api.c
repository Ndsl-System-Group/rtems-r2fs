#include "comm_api.h"

#include "cache/block_buffer.h"
#include "communication/dev.h"
#include "communication/memory.h"
#include "fs/fs.h"
#include "journal/journal_type.h"
#include "utils/io_utils.h"
#include "utils/rtfs_log.h"

#include <errno.h>
#include <stdlib.h>
#include <memory.h>
#include <stdatomic.h>


typedef struct comm_sync_rw_ctx
{
    rtems_id sem;
    rtems_status_code status;
    atomic_bool timed_out;
} comm_sync_rw_ctx;

typedef struct comm_async_ctx
{
    comm_async_cb_func user_cb;
    void *user_arg;
} comm_async_ctx;

static comm_test_sync_rw_hook g_comm_test_sync_rw_hook = NULL;
static comm_test_async_rw_hook g_comm_test_async_rw_hook = NULL;
static comm_test_get_metajournal_head_hook g_comm_test_get_metajournal_head_hook = NULL;
static comm_test_update_metajournal_tail_hook g_comm_test_update_metajournal_tail_hook = NULL;
static comm_test_fs_recover_hook g_comm_test_fs_recover_hook = NULL;

typedef struct comm_recovery_block
{
    uint32_t lpa;
    bool dirty;
    unsigned char data[BLOCK_BUFFER_SIZE];
    struct comm_recovery_block *next;
} comm_recovery_block;

typedef struct comm_recovery_lpa_vector
{
    uint32_t *items;
    size_t count;
    size_t capacity;
} comm_recovery_lpa_vector;

typedef struct comm_recovery_tx_state
{
    struct RtfsSuperBlock super_block;
    comm_recovery_block *blocks;
    comm_recovery_lpa_vector reclaim_data_lpas;
    comm_recovery_lpa_vector reclaim_node_lpas;
} comm_recovery_tx_state;


static void comm_sync_rw_done(rtems_blkdev_request *req, rtems_status_code status);

static void comm_async_rw_done(rtems_blkdev_request *req, rtems_status_code status);

// syncCtx 是同步请求时使用的上下文，asyncCtx 是异步请求时使用的上下文。
static int comm_submit_rw_request_common(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_io_direction dir, comm_sync_rw_ctx *syncCtx, comm_async_ctx **asyncCtx);

static int comm_recovery_read_lpa(
    struct comm_dev *dev,
    uint32_t lpa,
    void *buffer
);

static int comm_recovery_write_lpa(
    struct comm_dev *dev,
    uint32_t lpa,
    const void *buffer
);

static uint64_t comm_recovery_next_journal_lpa(
    uint64_t cur_lpa,
    uint64_t journal_start_lpa,
    uint64_t journal_end_lpa
);

static void comm_recovery_tx_state_init(
    comm_recovery_tx_state *state,
    const struct RtfsSuperBlock *super_block
);

static void comm_recovery_tx_state_destroy(comm_recovery_tx_state *state);

static void comm_recovery_lpa_vector_destroy(
    comm_recovery_lpa_vector *vec
);

static int comm_recovery_lpa_vector_append_unique(
    comm_recovery_lpa_vector *vec,
    uint32_t lpa
);

static int comm_recovery_tx_get_block(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    uint32_t lpa,
    comm_recovery_block **out_block
);

static int comm_recovery_apply_super_entries(
    comm_recovery_tx_state *state,
    const SuperBlockJournalEntry *entries,
    size_t count
);

static int comm_recovery_apply_nat_entries(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    const NatJournalEntry *entries,
    size_t count
);

static int comm_recovery_collect_inode_data_reclaim(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    uint32_t old_inode_lpa,
    uint32_t new_inode_lpa
);

static int comm_recovery_record_nat_reclaim(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    uint32_t nid,
    const struct RtfsNatEntry *old_entry,
    const struct RtfsNatEntry *new_entry
);

static int comm_recovery_apply_sit_entries(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    const SitJournalEntry *entries,
    size_t count
);

static int comm_recovery_commit_tx(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    struct RtfsSuperBlock *out_super_block
);

static int comm_recovery_append_reclaim_record(
    struct comm_dev *dev,
    comm_recovery_tx_state *state
);

static int comm_recovery_replay_one_transaction(
    struct comm_dev *dev,
    const struct RtfsSuperBlock *base_super_block,
    uint64_t start_lpa,
    uint64_t journal_start_lpa,
    uint64_t journal_end_lpa,
    uint64_t *out_end_lpa,
    struct RtfsSuperBlock *out_super_block,
    bool *out_committed
);


int comm_submit_sync_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_io_direction dir)
{
    if (g_comm_test_sync_rw_hook != NULL) {
        return g_comm_test_sync_rw_hook(dev, buffer, lba, lbaCount, dir);
    }

    if (0 == lbaCount) return 0;

    comm_sync_rw_ctx *syncCtx = (comm_sync_rw_ctx *)calloc(1, sizeof(*syncCtx));
    if (NULL == syncCtx) return ENOMEM;

    int res = comm_submit_rw_request_common(dev, buffer, lba, lbaCount, dir, syncCtx, NULL);
    // RTFS_LOG(RTFS_LOG_DEBUG, "comm_submit_sync_rw_request res: %d", res);
    if (0 != res)
    {
        free(syncCtx);
        return res;
    }

    rtems_status_code wait_status = rtems_semaphore_obtain(
        syncCtx->sem,
        RTEMS_WAIT,
        RTEMS_MILLISECONDS_TO_TICKS(5000));
    if (RTEMS_SUCCESSFUL != wait_status)
    {
        atomic_store_explicit(&syncCtx->timed_out, true, memory_order_release);
        if (RTEMS_SUCCESSFUL == rtems_semaphore_obtain(syncCtx->sem, RTEMS_NO_WAIT, 0))
        {
            res = (RTEMS_SUCCESSFUL == syncCtx->status) ? 0 : EIO;
            rtems_semaphore_delete(syncCtx->sem);
            free(syncCtx);
            return res;
        }

        RTFS_LOG(
            RTFS_LOG_WARNING,
            "sync rw timed out dev=%p disk=%p lba=%llu lbaCount=%u dir=%d",
            (void *)dev,
            dev != NULL ? (void *)dev->diskDevice : NULL,
            (unsigned long long)lba,
            (unsigned int)lbaCount,
            (int)dir);
        return ETIMEDOUT;
    }

    res = (RTEMS_SUCCESSFUL == syncCtx->status) ? 0 : EIO;
    rtems_semaphore_delete(syncCtx->sem);
    free(syncCtx);

    return res;
}

int comm_submit_async_rw_request(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_async_cb_func cbFunc, void *cbArg, comm_io_direction dir)
{
    if (g_comm_test_async_rw_hook != NULL) {
        return g_comm_test_async_rw_hook(dev, buffer, lba, lbaCount, cbFunc, cbArg, dir);
    }

    if (0 == lbaCount)
    {
        if (NULL != cbFunc) cbFunc(COMM_CMD_SUCCESS, cbArg);
        return 0;
    }

    comm_async_ctx *ctx = (comm_async_ctx *)malloc(sizeof(comm_async_ctx));
    if (NULL == ctx) return ENOMEM;

    ctx->user_cb = cbFunc;
    ctx->user_arg = cbArg;

    int res = comm_submit_rw_request_common(dev, buffer, lba, lbaCount, dir, NULL, &ctx);
    RTFS_LOG(RTFS_LOG_DEBUG, "comm_submit_async_rw_request res: %d", res);
    if (0 != res)
    {
        RTFS_LOG(
            RTFS_LOG_WARNING,
            "async rw submit failed res=%d dev=%p disk=%p lba=%llu lbaCount=%u dir=%d",
            res,
            (void *)dev,
            dev != NULL ? (void *)dev->diskDevice : NULL,
            (unsigned long long)lba,
            (unsigned int)lbaCount,
            (int)dir
        );
        free(ctx);


        return res;
    }


    return 0;
}

// int comm_submit_sync_migrate_request(struct comm_dev *dev, migrate_task *task)
// {
// }

// int comm_submit_async_migrate_request(struct comm_dev *dev, migrate_task *task, comm_async_cb_func cbFunc, void *cbArg)
// {
// }

// int comm_submit_sync_path_lookup_request(struct comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res)
// {
// }

// int comm_submit_async_path_lookup_request(struct comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res, comm_async_cb_func cbFunc, void *cbArg)
// {
// }

// int comm_submit_sync_filemapping_search_request(struct comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len)
// {
// }

// int comm_submit_async_filemapping_search_request(struct comm_dev *dev, filemapping_search_task *task, void *res, uint32_t res_len, comm_async_cb_func cbFunc, void *cbArg)
// {
// }

int comm_submit_sync_update_metajournal_tail_request(struct comm_dev *dev, uint64_t originLpa, uint32_t writeBlockNum)
{
    if (g_comm_test_update_metajournal_tail_hook != NULL) {
        return g_comm_test_update_metajournal_tail_hook(dev, originLpa, writeBlockNum);
    }

    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;
    if (0 == writeBlockNum) return 0;
    if (dev->metaJournalEndLpa <= dev->metaJournalStartLpa) return EINVAL;

    int res = rtfsMutexLock(&dev->metaJournalMutex);
    if (0 != res) return res;

    int result = 0;

    // 这里要求 originLpa 必须等于当前 tail。也就是说，调用者提交的这批 journal 必须紧跟着当前尾部写入。
    // 先检查 originLpa 是否落在 journal 合法范围内。
    if (originLpa < dev->metaJournalStartLpa || originLpa >= dev->metaJournalEndLpa)
    {
        result = EINVAL;
        goto out;
    }

    // 再检查它是否等于当前 tail。
    if (originLpa != dev->metaJournalTailLpa)
    {
        result = EINVAL;
        goto out;
    }

    // journal 区间长度，按环形缓冲区处理。
    uint64_t journalSize = dev->metaJournalEndLpa - dev->metaJournalStartLpa;
    if (0 == journalSize)
    {
        result = EINVAL;
        goto out;
    }

    // 计算 tail 前移后的新位置。
    uint64_t offset = originLpa - dev->metaJournalStartLpa;
    uint64_t step = (uint64_t)writeBlockNum % journalSize;
    uint64_t newOffset = offset + step;

    // 环形回绕。
    if (newOffset >= journalSize) newOffset -= journalSize;

    dev->metaJournalTailLpa = dev->metaJournalStartLpa + newOffset;

    /*
     * 当前简化实现没有独立的设备侧 apply-journal 线程。
     * 因此一旦尾指针推进，就视为这批 journal 已经被设备“消费”，
     * 需要同步推进头指针，否则 journal processor 会一直认为
     * 仍有未完成事务，卸载阶段会卡在后台线程退出上。
     */
    dev->metaJournalHeadLpa = dev->metaJournalTailLpa;

out:
    res = rtfsMutexUnlock(&dev->metaJournalMutex);
    if (0 == result && 0 != res) result = res;


    return result;
}

int comm_submit_async_update_metajournal_tail_request(struct comm_dev *dev, uint64_t originLpa, uint32_t writeBlockNum, comm_async_cb_func cbFunc, void *cbArg)
{
    // 简化版异步：先同步完成状态更新，再立刻触发回调。
    int res = comm_submit_sync_update_metajournal_tail_request(dev, originLpa, writeBlockNum);

    if (NULL != cbFunc) cbFunc((0 == res) ? COMM_CMD_SUCCESS : COMM_CMD_CQE_ERROR, cbArg);


    return res;
}

int comm_submit_sync_get_metajournal_head_request(struct comm_dev *dev, uint64_t *headLpa)
{
    if (g_comm_test_get_metajournal_head_hook != NULL) {
        return g_comm_test_get_metajournal_head_hook(dev, headLpa);
    }

    if (NULL == dev || NULL == headLpa) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 读取 head 时同样加锁，避免并发更新导致状态不一致。
    int res = rtfsMutexLock(&dev->metaJournalMutex);
    if (0 != res) return res;

    *headLpa = dev->metaJournalHeadLpa;

    res = rtfsMutexUnlock(&dev->metaJournalMutex);
    if (0 != res) return res;


    return 0;
}

int comm_submit_async_get_metajournal_head_request(struct comm_dev *dev, uint64_t *headLpa, comm_async_cb_func cbFunc, void *cbArg)
{
    // 简化版异步：先读 head，再立即回调。
    int res = comm_submit_sync_get_metajournal_head_request(dev, headLpa);

    if (NULL != cbFunc) cbFunc((0 == res) ? COMM_CMD_SUCCESS : COMM_CMD_CQE_ERROR, cbArg);


    return res;
}

static int comm_recovery_read_lpa(
    struct comm_dev *dev,
    uint32_t lpa,
    void *buffer
)
{
    if (dev == NULL || buffer == NULL) {
        return EINVAL;
    }

    return comm_submit_sync_rw_request(
        dev,
        buffer,
        LPA_TO_LBA(lpa),
        LBA_PER_LPA,
        COMM_IO_READ
    );
}

static int comm_recovery_write_lpa(
    struct comm_dev *dev,
    uint32_t lpa,
    const void *buffer
)
{
    if (dev == NULL || buffer == NULL) {
        return EINVAL;
    }

    return comm_submit_sync_rw_request(
        dev,
        (void *)buffer,
        LPA_TO_LBA(lpa),
        LBA_PER_LPA,
        COMM_IO_WRITE
    );
}

static uint64_t comm_recovery_next_journal_lpa(
    uint64_t cur_lpa,
    uint64_t journal_start_lpa,
    uint64_t journal_end_lpa
)
{
    ++cur_lpa;
    if (cur_lpa >= journal_end_lpa) {
        cur_lpa = journal_start_lpa;
    }
    return cur_lpa;
}

static void comm_recovery_tx_state_init(
    comm_recovery_tx_state *state,
    const struct RtfsSuperBlock *super_block
)
{
    memset(state, 0, sizeof(*state));
    memcpy(&state->super_block, super_block, sizeof(state->super_block));
}

static void comm_recovery_tx_state_destroy(comm_recovery_tx_state *state)
{
    comm_recovery_block *block = NULL;
    comm_recovery_block *next = NULL;

    if (state == NULL) {
        return;
    }

    block = state->blocks;
    while (block != NULL) {
        next = block->next;
        free(block);
        block = next;
    }
    state->blocks = NULL;
    comm_recovery_lpa_vector_destroy(&state->reclaim_data_lpas);
    comm_recovery_lpa_vector_destroy(&state->reclaim_node_lpas);
}

static void comm_recovery_lpa_vector_destroy(
    comm_recovery_lpa_vector *vec
)
{
    if (vec == NULL) {
        return;
    }

    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

static int comm_recovery_lpa_vector_append_unique(
    comm_recovery_lpa_vector *vec,
    uint32_t lpa
)
{
    uint32_t *new_items;
    size_t new_capacity;
    size_t i;

    if (vec == NULL || lpa == INVALID_LPA) {
        return 0;
    }

    for (i = 0; i < vec->count; ++i) {
        if (vec->items[i] == lpa) {
            return 0;
        }
    }

    if (vec->count == vec->capacity) {
        new_capacity = vec->capacity == 0 ? 4u : vec->capacity * 2u;
        new_items = (uint32_t *)realloc(
            vec->items,
            new_capacity * sizeof(*new_items)
        );
        if (new_items == NULL) {
            return ENOMEM;
        }
        vec->items = new_items;
        vec->capacity = new_capacity;
    }

    vec->items[vec->count++] = lpa;
    return 0;
}

static int comm_recovery_tx_get_block(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    uint32_t lpa,
    comm_recovery_block **out_block
)
{
    comm_recovery_block *block;
    int ret;

    if (dev == NULL || state == NULL || out_block == NULL) {
        return EINVAL;
    }

    block = state->blocks;
    while (block != NULL) {
        if (block->lpa == lpa) {
            *out_block = block;
            return 0;
        }
        block = block->next;
    }

    block = (comm_recovery_block *)calloc(1, sizeof(*block));
    if (block == NULL) {
        return ENOMEM;
    }

    block->lpa = lpa;
    ret = comm_recovery_read_lpa(dev, lpa, block->data);
    if (ret != 0) {
        free(block);
        return ret;
    }

    block->next = state->blocks;
    state->blocks = block;
    *out_block = block;
    return 0;
}

static int comm_recovery_apply_super_entries(
    comm_recovery_tx_state *state,
    const SuperBlockJournalEntry *entries,
    size_t count
)
{
    size_t i;

    if (state == NULL || (count > 0 && entries == NULL)) {
        return EINVAL;
    }

    for (i = 0; i < count; ++i) {
        uint32_t off = entries[i].Off;

        if ((size_t)off + sizeof(entries[i].newVal) > sizeof(state->super_block)) {
            return EIO;
        }

        memcpy(
            ((unsigned char *)&state->super_block) + off,
            &entries[i].newVal,
            sizeof(entries[i].newVal)
        );
    }

    return 0;
}

static int comm_recovery_collect_inode_data_reclaim(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    uint32_t old_inode_lpa,
    uint32_t new_inode_lpa
)
{
    struct RtfsNode old_node;
    struct RtfsNode new_node;
    bool old_inline;
    bool new_inline;
    uint32_t main_start_lpa;
    uint64_t block_count;
    size_t i;
    int ret;

    if (dev == NULL || state == NULL || old_inode_lpa == INVALID_LPA) {
        return EINVAL;
    }

    ret = comm_recovery_read_lpa(dev, old_inode_lpa, &old_node);
    if (ret != 0) {
        return ret;
    }

    old_inline = (old_node.i.i_inline & (RTFS_INLINE_DATA | RTFS_INLINE_DENTRY)) != 0;
    if (old_inline) {
        return 0;
    }

    main_start_lpa = state->super_block.main_blkaddr;
    block_count = state->super_block.block_count;

    memset(&new_node, 0, sizeof(new_node));
    if (new_inode_lpa != INVALID_LPA) {
        ret = comm_recovery_read_lpa(dev, new_inode_lpa, &new_node);
        if (ret != 0) {
            return ret;
        }
    }

    new_inline = new_inode_lpa != INVALID_LPA &&
        (new_node.i.i_inline & (RTFS_INLINE_DATA | RTFS_INLINE_DENTRY)) != 0;

    for (i = 0; i < DEF_ADDRS_PER_INODE; ++i) {
        uint32_t old_data_lpa = old_node.i.i_addr[i];
        uint32_t new_data_lpa = INVALID_LPA;

        if (new_inode_lpa != INVALID_LPA && !new_inline) {
            new_data_lpa = new_node.i.i_addr[i];
        }

        if (old_data_lpa != INVALID_LPA &&
            old_data_lpa != new_data_lpa &&
            old_data_lpa >= main_start_lpa &&
            (uint64_t)old_data_lpa < block_count) {
            ret = comm_recovery_lpa_vector_append_unique(
                &state->reclaim_data_lpas,
                old_data_lpa
            );
            if (ret != 0) {
                return ret;
            }
        }
    }

    return 0;
}

static int comm_recovery_record_nat_reclaim(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    uint32_t nid,
    const struct RtfsNatEntry *old_entry,
    const struct RtfsNatEntry *new_entry
)
{
    int ret;

    if (dev == NULL || state == NULL || old_entry == NULL || new_entry == NULL) {
        return EINVAL;
    }

    if (old_entry->ino == INVALID_NID ||
        old_entry->block_addr == INVALID_LPA ||
        old_entry->block_addr < state->super_block.main_blkaddr ||
        (uint64_t)old_entry->block_addr >= state->super_block.block_count ||
        old_entry->block_addr == new_entry->block_addr) {
        return 0;
    }

    ret = comm_recovery_lpa_vector_append_unique(
        &state->reclaim_node_lpas,
        old_entry->block_addr
    );
    if (ret != 0) {
        return ret;
    }
    if (old_entry->ino != nid) {
        return 0;
    }

    return comm_recovery_collect_inode_data_reclaim(
        dev,
        state,
        old_entry->block_addr,
        new_entry->block_addr
    );
}

static int comm_recovery_apply_nat_entries(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    const NatJournalEntry *entries,
    size_t count
)
{
    uint64_t nat_block_count;
    size_t i;

    if (dev == NULL || state == NULL || (count > 0 && entries == NULL)) {
        return EINVAL;
    }

    nat_block_count =
        (uint64_t)state->super_block.segment_count_nat * BLOCK_PER_SEGMENT;

    for (i = 0; i < count; ++i) {
        uint64_t nat_lpa_idx;
        uint32_t lpa;
        uint32_t entry_idx;
        comm_recovery_block *block;
        struct RtfsNatBlock *nat_block;
        int ret;

        nat_lpa_idx = (uint64_t)entries[i].nid / NAT_ENTRY_PER_BLOCK;
        if (nat_lpa_idx >= nat_block_count) {
            return EIO;
        }

        lpa = state->super_block.nat_blkaddr + (uint32_t)nat_lpa_idx;
        entry_idx = entries[i].nid % NAT_ENTRY_PER_BLOCK;
        ret = comm_recovery_tx_get_block(dev, state, lpa, &block);
        if (ret != 0) {
            return ret;
        }

        nat_block = (struct RtfsNatBlock *)block->data;
        ret = comm_recovery_record_nat_reclaim(
            dev,
            state,
            entries[i].nid,
            &nat_block->entries[entry_idx],
            &entries[i].newValue
        );
        if (ret != 0) {
            return ret;
        }
        nat_block->entries[entry_idx] = entries[i].newValue;
        block->dirty = true;
    }

    return 0;
}

static int comm_recovery_apply_sit_entries(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    const SitJournalEntry *entries,
    size_t count
)
{
    uint64_t sit_block_count;
    size_t i;

    if (dev == NULL || state == NULL || (count > 0 && entries == NULL)) {
        return EINVAL;
    }

    sit_block_count =
        (uint64_t)state->super_block.segment_count_sit * BLOCK_PER_SEGMENT;

    for (i = 0; i < count; ++i) {
        uint64_t sit_lpa_idx;
        uint32_t lpa;
        uint32_t entry_idx;
        comm_recovery_block *block;
        struct RtfsSitBlock *sit_block;
        int ret;

        sit_lpa_idx = (uint64_t)entries[i].segID / SIT_ENTRY_PER_BLOCK;
        if (sit_lpa_idx >= sit_block_count) {
            return EIO;
        }

        lpa = state->super_block.sit_blkaddr + (uint32_t)sit_lpa_idx;
        entry_idx = entries[i].segID % SIT_ENTRY_PER_BLOCK;
        ret = comm_recovery_tx_get_block(dev, state, lpa, &block);
        if (ret != 0) {
            return ret;
        }

        sit_block = (struct RtfsSitBlock *)block->data;
        sit_block->entries[entry_idx] = entries[i].newValue;
        block->dirty = true;
    }

    return 0;
}

static int comm_recovery_commit_tx(
    struct comm_dev *dev,
    comm_recovery_tx_state *state,
    struct RtfsSuperBlock *out_super_block
)
{
    comm_recovery_block *block;
    int ret;

    if (dev == NULL || state == NULL || out_super_block == NULL) {
        return EINVAL;
    }

    for (block = state->blocks; block != NULL; block = block->next) {
        if (!block->dirty) {
            continue;
        }

        ret = comm_recovery_write_lpa(dev, block->lpa, block->data);
        if (ret != 0) {
            return ret;
        }
    }

    memcpy(out_super_block, &state->super_block, sizeof(*out_super_block));
    return 0;
}

static int comm_recovery_append_reclaim_record(
    struct comm_dev *dev,
    comm_recovery_tx_state *state
)
{
    comm_recovered_reclaim_record *record;
    comm_recovered_reclaim_record *tail;

    if (dev == NULL || state == NULL) {
        return EINVAL;
    }

    if (state->reclaim_data_lpas.count == 0 &&
        state->reclaim_node_lpas.count == 0) {
        return 0;
    }

    record = (comm_recovered_reclaim_record *)calloc(1, sizeof(*record));
    if (record == NULL) {
        return ENOMEM;
    }

    record->data_lpas = state->reclaim_data_lpas.items;
    record->data_count = state->reclaim_data_lpas.count;
    record->node_lpas = state->reclaim_node_lpas.items;
    record->node_count = state->reclaim_node_lpas.count;
    state->reclaim_data_lpas.items = NULL;
    state->reclaim_data_lpas.count = 0;
    state->reclaim_data_lpas.capacity = 0;
    state->reclaim_node_lpas.items = NULL;
    state->reclaim_node_lpas.count = 0;
    state->reclaim_node_lpas.capacity = 0;

    if (dev->recoveredReclaimHead == NULL) {
        dev->recoveredReclaimHead = record;
        return 0;
    }

    tail = dev->recoveredReclaimHead;
    while (tail->next != NULL) {
        tail = tail->next;
    }
    tail->next = record;
    return 0;
}

static int comm_recovery_replay_one_transaction(
    struct comm_dev *dev,
    const struct RtfsSuperBlock *base_super_block,
    uint64_t start_lpa,
    uint64_t journal_start_lpa,
    uint64_t journal_end_lpa,
    uint64_t *out_end_lpa,
    struct RtfsSuperBlock *out_super_block,
    bool *out_committed
)
{
    unsigned char *journal_block;
    uint64_t journal_size;
    uint64_t cur_lpa;
    uint64_t scanned_blocks;
    comm_recovery_tx_state tx_state;
    int ret = 0;

    if (dev == NULL || base_super_block == NULL || out_end_lpa == NULL ||
        out_super_block == NULL || out_committed == NULL) {
        return EINVAL;
    }

    *out_committed = false;
    journal_size = journal_end_lpa - journal_start_lpa;
    if (journal_size == 0) {
        return EINVAL;
    }

    journal_block = (unsigned char *)comm_alloc_dma_mem(BLOCK_BUFFER_SIZE);
    if (journal_block == NULL) {
        return ENOMEM;
    }

    comm_recovery_tx_state_init(&tx_state, base_super_block);
    cur_lpa = start_lpa;
    scanned_blocks = 0;

    while (scanned_blocks < journal_size) {
        size_t off = 0;

        ret = comm_recovery_read_lpa(dev, (uint32_t)cur_lpa, journal_block);
        if (ret != 0) {
            goto out;
        }

        while (off + sizeof(MetaJournalEntry) <= BLOCK_BUFFER_SIZE) {
            MetaJournalEntry header;
            size_t payload_len;
            const unsigned char *payload;

            memcpy(&header, journal_block + off, sizeof(header));
            if (header.len == 0) {
                goto out;
            }
            if (header.len < sizeof(MetaJournalEntry) ||
                off + header.len > BLOCK_BUFFER_SIZE) {
                goto out;
            }

            payload_len = (size_t)header.len - sizeof(MetaJournalEntry);
            payload = journal_block + off + sizeof(MetaJournalEntry);

            switch (header.type) {
                case JOURNAL_TYPE_SUPER_BLOCK:
                    if ((payload_len % sizeof(SuperBlockJournalEntry)) != 0) {
                        goto out;
                    }
                    ret = comm_recovery_apply_super_entries(
                        &tx_state,
                        (const SuperBlockJournalEntry *)payload,
                        payload_len / sizeof(SuperBlockJournalEntry)
                    );
                    if (ret != 0) {
                        goto out;
                    }
                    off += header.len;
                    break;

                case JOURNAL_TYPE_NATS:
                    if ((payload_len % sizeof(NatJournalEntry)) != 0) {
                        goto out;
                    }
                    ret = comm_recovery_apply_nat_entries(
                        dev,
                        &tx_state,
                        (const NatJournalEntry *)payload,
                        payload_len / sizeof(NatJournalEntry)
                    );
                    if (ret != 0) {
                        goto out;
                    }
                    off += header.len;
                    break;

                case JOURNAL_TYPE_SITS:
                    if ((payload_len % sizeof(SitJournalEntry)) != 0) {
                        goto out;
                    }
                    ret = comm_recovery_apply_sit_entries(
                        dev,
                        &tx_state,
                        (const SitJournalEntry *)payload,
                        payload_len / sizeof(SitJournalEntry)
                    );
                    if (ret != 0) {
                        goto out;
                    }
                    off += header.len;
                    break;

                case JOURNAL_TYPE_NOP:
                    off = BLOCK_BUFFER_SIZE;
                    break;

                case JOURNAL_TYPE_END:
                    if (header.len != sizeof(MetaJournalEntry)) {
                        goto out;
                    }
                    ret = comm_recovery_commit_tx(
                        dev,
                        &tx_state,
                        out_super_block
                    );
                    if (ret != 0) {
                        goto out;
                    }
                    ret = comm_recovery_append_reclaim_record(dev, &tx_state);
                    if (ret != 0) {
                        goto out;
                    }
                    *out_end_lpa = comm_recovery_next_journal_lpa(
                        cur_lpa,
                        journal_start_lpa,
                        journal_end_lpa
                    );
                    *out_committed = true;
                    goto out;

                default:
                    goto out;
            }
        }

        cur_lpa = comm_recovery_next_journal_lpa(
            cur_lpa,
            journal_start_lpa,
            journal_end_lpa
        );
        ++scanned_blocks;
    }

out:
    comm_recovery_tx_state_destroy(&tx_state);
    comm_free_dma_mem(journal_block);
    return ret;
}

int comm_submit_fs_module_init_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;
    if (0 == dev->blockSize) return EINVAL;
    if (0 == dev->blockCount) return EINVAL;

    if (dev->metaJournalStartLpa >= dev->metaJournalEndLpa) return EINVAL;
    if (dev->metaJournalEndLpa > dev->blockCount) return EINVAL;


    return 0;
}

int comm_submit_fs_db_init_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 初始化元数据数据库。
    // 简化版实现为重置 meta journal 状态。
    int res = rtfsMutexLock(&dev->metaJournalMutex);
    if (0 != res) return res;

    dev->metaJournalHeadLpa = dev->metaJournalStartLpa;
    dev->metaJournalTailLpa = dev->metaJournalStartLpa;

    res = rtfsMutexUnlock(&dev->metaJournalMutex);
    if (0 != res) return res;

    return 0;
}

int comm_submit_fs_recover_from_db_request(comm_dev *dev)
{
    unsigned char *super_block_buf;
    struct RtfsSuperBlock super_block;
    uint64_t journal_start_lpa;
    uint64_t journal_end_lpa;
    uint64_t journal_size;
    uint64_t scan_start_lpa;
    uint64_t scan_lpa;
    uint64_t committed_end_lpa;
    int ret;

    if (g_comm_test_fs_recover_hook != NULL) {
        return g_comm_test_fs_recover_hook(dev);
    }

    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    commDevClearRecoveredReclaimRecords(dev);

    super_block_buf = (unsigned char *)comm_alloc_dma_mem(BLOCK_BUFFER_SIZE);
    if (super_block_buf == NULL) {
        return ENOMEM;
    }

    ret = comm_recovery_read_lpa(dev, 0u, super_block_buf);
    if (ret != 0) {
        comm_free_dma_mem(super_block_buf);
        commDevClearRecoveredReclaimRecords(dev);
        return ret;
    }

    memcpy(&super_block, super_block_buf, sizeof(super_block));
    if (super_block.magic != RTFS_MAGIC_NUMBER) {
        comm_free_dma_mem(super_block_buf);
        commDevClearRecoveredReclaimRecords(dev);
        return EIO;
    }

    journal_start_lpa = super_block.meta_journal_blkaddr;
    journal_end_lpa = journal_start_lpa +
        (uint64_t)super_block.segment_count_meta_journal * BLOCK_PER_SEGMENT;
    journal_size = journal_end_lpa - journal_start_lpa;
    if (journal_size == 0) {
        comm_free_dma_mem(super_block_buf);
        commDevClearRecoveredReclaimRecords(dev);
        return EIO;
    }

    scan_start_lpa = journal_start_lpa +
        ((uint64_t)super_block.meta_journal_end_blkoff % journal_size);
    scan_lpa = scan_start_lpa;
    committed_end_lpa = scan_start_lpa;

    while (true) {
        uint64_t next_lpa = scan_lpa;
        struct RtfsSuperBlock replayed_super;
        bool committed = false;

        ret = comm_recovery_replay_one_transaction(
            dev,
            &super_block,
            scan_lpa,
            journal_start_lpa,
            journal_end_lpa,
            &next_lpa,
            &replayed_super,
            &committed
        );
        if (ret != 0) {
            comm_free_dma_mem(super_block_buf);
            commDevClearRecoveredReclaimRecords(dev);
            return ret;
        }
        if (!committed) {
            break;
        }

        super_block = replayed_super;
        committed_end_lpa = next_lpa;
        scan_lpa = next_lpa;
        if (scan_lpa == scan_start_lpa) {
            break;
        }
    }

    super_block.meta_journal_start_blkoff =
        (uint16_t)((committed_end_lpa - journal_start_lpa) % journal_size);
    super_block.meta_journal_end_blkoff =
        (uint16_t)((committed_end_lpa - journal_start_lpa) % journal_size);
    memcpy(super_block_buf, &super_block, sizeof(super_block));

    ret = comm_recovery_write_lpa(dev, 0u, super_block_buf);
    comm_free_dma_mem(super_block_buf);
    if (ret != 0) {
        commDevClearRecoveredReclaimRecords(dev);
        return ret;
    }

    dev->metaJournalStartLpa = journal_start_lpa;
    dev->metaJournalEndLpa = journal_end_lpa;
    dev->metaJournalHeadLpa = committed_end_lpa;
    dev->metaJournalTailLpa = committed_end_lpa;

    return 0;
}

int comm_submit_clear_metajournal_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 清空 meta journal：head/tail 回到起始位置。
    int res = rtfsMutexLock(&dev->metaJournalMutex);
    if (0 != res) return res;

    dev->metaJournalHeadLpa = dev->metaJournalStartLpa;
    dev->metaJournalTailLpa = dev->metaJournalStartLpa;

    res = rtfsMutexUnlock(&dev->metaJournalMutex);
    if (0 != res) return res;

    return 0;
}

int comm_submit_start_apply_journal_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 简化版暂无后台 apply journal 线程。
    // 保留接口供后续扩展。
    return 0;
}

int comm_submit_stop_apply_journal_request(comm_dev *dev)
{
    if (NULL == dev) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;

    // 简化版暂无后台 apply journal 线程。
    // 保留接口供后续扩展。
    return 0;
}

void commSetTestSyncRwHook(comm_test_sync_rw_hook hook)
{
    g_comm_test_sync_rw_hook = hook;
}

void commSetTestAsyncRwHook(comm_test_async_rw_hook hook)
{
    g_comm_test_async_rw_hook = hook;
}

void commSetTestGetMetaJournalHeadHook(comm_test_get_metajournal_head_hook hook)
{
    g_comm_test_get_metajournal_head_hook = hook;
}

void commSetTestUpdateMetaJournalTailHook(comm_test_update_metajournal_tail_hook hook)
{
    g_comm_test_update_metajournal_tail_hook = hook;
}

void commSetTestFsRecoverHook(comm_test_fs_recover_hook hook)
{
    g_comm_test_fs_recover_hook = hook;
}


void comm_sync_rw_done(rtems_blkdev_request *req, rtems_status_code status)
{
    comm_sync_rw_ctx *ctx = (comm_sync_rw_ctx *)req->done_arg;

    if (NULL != ctx)
    {
        ctx->status = status;
        if (atomic_load_explicit(&ctx->timed_out, memory_order_acquire))
        {
            rtems_semaphore_delete(ctx->sem);
            free(ctx);
        }
        else
        {
            rtems_semaphore_release(ctx->sem);
        }
    }

    // request 已完成，释放申请的内存。
    free(req);
}

void comm_async_rw_done(rtems_blkdev_request *req, rtems_status_code status)
{
    comm_async_ctx *ctx = (comm_async_ctx *)req->done_arg;

    if (NULL != ctx && NULL != ctx->user_cb)
    {
        // 完成后触发用户回调，传递命令结果和上下文参数。
        if (RTEMS_SUCCESSFUL == status)
        {
            ctx->user_cb(COMM_CMD_SUCCESS, ctx->user_arg);
        }
        else
        {
            ctx->user_cb(COMM_CMD_CQE_ERROR, ctx->user_arg);
        }

        free(ctx); // 回调后释放资源。
    }

    // request 已完成，释放申请的内存。
    free(req);
}

int comm_submit_rw_request_common(struct comm_dev *dev, void *buffer, uint64_t lba, uint32_t lbaCount, comm_io_direction dir, comm_sync_rw_ctx *syncCtx, comm_async_ctx **asyncCtx)
{
    if (NULL == dev || NULL == buffer) return EINVAL;
    if (NULL == dev->diskDevice) return ENODEV;
    if (NULL == dev->diskDevice->phys_dev) return ENODEV;
    if (NULL == dev->diskDevice->phys_dev->ioctl) return ENODEV;
    if (0 == dev->blockSize || 0 == dev->blockCount) return EINVAL;
    if (COMM_IO_READ != dir && COMM_IO_WRITE != dir) return EINVAL;
    if (0 == lbaCount) return 0;

    uint64_t endLba = lba + (uint64_t)lbaCount;
    if (endLba < lba) return EOVERFLOW; // 加法溢出。

    if (lba >= dev->blockCount || endLba > dev->blockCount) return EINVAL;

    uint64_t totalBytes64 = (uint64_t)lbaCount * (uint64_t)dev->blockSize;
    if (0 != dev->blockSize && totalBytes64 / (uint64_t)dev->blockSize != (uint64_t)lbaCount) return EOVERFLOW;

    if (totalBytes64 > UINT32_MAX) return EOVERFLOW;
    if ((uint64_t)dev->diskDevice->start > UINT64_MAX - lba) return EOVERFLOW;

    uint64_t physicalLba = lba + (uint64_t)dev->diskDevice->start;

    size_t reqSize = sizeof(rtems_blkdev_request) + sizeof(rtems_blkdev_sg_buffer);

    rtems_blkdev_request *req = (rtems_blkdev_request *)malloc(reqSize);
    if (NULL == req) return ENOMEM;
    memset(req, 0, reqSize);

    // 处理同步请求。
    if (NULL != syncCtx)
    {
        rtems_status_code sc = rtems_semaphore_create(rtems_build_name('C', 'R', 'W', '0'), 0, RTEMS_SIMPLE_BINARY_SEMAPHORE, 0, &syncCtx->sem);
        if (RTEMS_SUCCESSFUL != sc)
        {
            free(req);


            return EIO;
        }

        syncCtx->status = RTEMS_IO_ERROR;
        req->done = comm_sync_rw_done;
        req->done_arg = syncCtx;
    }
    // 处理异步请求。
    else if (NULL != asyncCtx)
    {
        req->done = comm_async_rw_done;
        req->done_arg = *asyncCtx;
    }
    else
    {
        free(req);


        return EINVAL;
    }

    req->io_task = rtems_task_self();
    req->bufnum = 1;
    req->req = (dir == COMM_IO_READ) ? RTEMS_BLKDEV_REQ_READ : RTEMS_BLKDEV_REQ_WRITE;

    req->bufs[0].block = (rtems_blkdev_bnum)physicalLba;
    req->bufs[0].length = (uint32_t)totalBytes64;
    req->bufs[0].buffer = buffer;
    req->bufs[0].user = NULL;

    // 在通过 diskDevice->start 转换逻辑磁盘 LBA 后，再提交给物理驱动器。否则，/dev/nvdX-Y 的 LBA 0 可能会变成整个磁盘的 LBA 0，并覆盖分区元数据。
    int res = dev->diskDevice->phys_dev->ioctl(
        dev->diskDevice->phys_dev,
        RTEMS_BLKIO_REQUEST,
        req
    );
    if (0 != res)
    {
        int saved_errno = errno;
        if (NULL != syncCtx)
        {
            rtems_semaphore_delete(syncCtx->sem);
        }
        free(req);

        return saved_errno != 0 ? saved_errno : EIO;
    }


    return 0;
}
