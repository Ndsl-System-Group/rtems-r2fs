#include "cow_reclaim_registry.h"

#include "fs/fs_manager.h"
#include "fs/sit_utils.h"
#include "journal/journal_processor.h"
#include "uthash/utlist.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct CowReclaimRecord
{
    uint64_t tx_id;
    uint32_t *data_lpas;
    size_t data_count;
    uint32_t *node_lpas;
    size_t node_count;
    struct CowReclaimRecord *prev;
    struct CowReclaimRecord *next;
} CowReclaimRecord;

typedef struct CowReclaimRegistry
{
    struct file_system_manager *fs_manager;
    CowReclaimRecord *pending_head;
    CowReclaimRecord *completed_head;
} CowReclaimRegistry;

static CowReclaimRegistry g_cow_reclaim_registry;

static void cowReclaimRegistryTxCompleteHook(uint64_t tx_id, void *arg)
{
    (void)arg;
    cowReclaimRegistryOnTxComplete(tx_id);
}

static void cowReclaimRegistryFreeRecord(CowReclaimRecord *record)
{
    if (record == NULL) {
        return;
    }

    free(record->data_lpas);
    free(record->node_lpas);
    free(record);
}

static int cowReclaimRegistryCopyLpas(
    const uint32_t *src,
    size_t count,
    uint32_t **out
)
{
    uint32_t *copy = NULL;

    if (out == NULL) {
        return EINVAL;
    }

    *out = NULL;
    if (count == 0) {
        return 0;
    }

    if (src == NULL) {
        return EINVAL;
    }

    copy = (uint32_t *)malloc(count * sizeof(*copy));
    if (copy == NULL) {
        return ENOMEM;
    }

    memcpy(copy, src, count * sizeof(*copy));
    *out = copy;
    return 0;
}

void cowReclaimRegistryInit(struct file_system_manager *fs_manager)
{
    g_cow_reclaim_registry.fs_manager = fs_manager;
    g_cow_reclaim_registry.pending_head = NULL;
    g_cow_reclaim_registry.completed_head = NULL;

    journalProcessorSetDefaultTxCompleteHook(
        cowReclaimRegistryTxCompleteHook,
        &g_cow_reclaim_registry
    );
}

void cowReclaimRegistryDestroy(void)
{
    CowReclaimRecord *record = NULL;
    CowReclaimRecord *tmp = NULL;

    DL_FOREACH_SAFE(g_cow_reclaim_registry.pending_head, record, tmp) {
        DL_DELETE(g_cow_reclaim_registry.pending_head, record);
        cowReclaimRegistryFreeRecord(record);
    }

    DL_FOREACH_SAFE(g_cow_reclaim_registry.completed_head, record, tmp) {
        DL_DELETE(g_cow_reclaim_registry.completed_head, record);
        cowReclaimRegistryFreeRecord(record);
    }

    g_cow_reclaim_registry.fs_manager = NULL;
    journalProcessorSetDefaultTxCompleteHook(NULL, NULL);
}

int cowReclaimRegistryRegister(
    uint64_t tx_id,
    const uint32_t *data_lpas,
    size_t data_count,
    const uint32_t *node_lpas,
    size_t node_count
)
{
    CowReclaimRecord *record;
    int ret;

    if (g_cow_reclaim_registry.fs_manager == NULL) {
        return 0;
    }

    if (data_count == 0 && node_count == 0) {
        return 0;
    }

    record = (CowReclaimRecord *)calloc(1, sizeof(*record));
    if (record == NULL) {
        return ENOMEM;
    }

    record->tx_id = tx_id;
    record->data_count = data_count;
    record->node_count = node_count;

    ret = cowReclaimRegistryCopyLpas(data_lpas, data_count, &record->data_lpas);
    if (ret != 0) {
        cowReclaimRegistryFreeRecord(record);
        return ret;
    }

    ret = cowReclaimRegistryCopyLpas(node_lpas, node_count, &record->node_lpas);
    if (ret != 0) {
        cowReclaimRegistryFreeRecord(record);
        return ret;
    }

    DL_APPEND(g_cow_reclaim_registry.pending_head, record);
    return 0;
}

void cowReclaimRegistryOnTxComplete(uint64_t tx_id)
{
    CowReclaimRecord *record;

    DL_FOREACH(g_cow_reclaim_registry.pending_head, record) {
        if (record->tx_id == tx_id) {
            DL_DELETE(g_cow_reclaim_registry.pending_head, record);
            DL_APPEND(g_cow_reclaim_registry.completed_head, record);
            return;
        }
    }
}

int cowReclaimRegistryDrainCompleted(void)
{
    CowReclaimRecord *record;
    CowReclaimRecord *tmp;
    SitOperator sit_op;

    if (g_cow_reclaim_registry.fs_manager == NULL) {
        return 0;
    }

    sitOperatorInit(&sit_op, g_cow_reclaim_registry.fs_manager);

    DL_FOREACH_SAFE(g_cow_reclaim_registry.completed_head, record, tmp) {
        size_t i;

        for (i = 0; i < record->data_count; ++i) {
            sitInvalidateLpa(&sit_op, record->data_lpas[i]);
        }
        for (i = 0; i < record->node_count; ++i) {
            sitInvalidateLpa(&sit_op, record->node_lpas[i]);
        }

        DL_DELETE(g_cow_reclaim_registry.completed_head, record);
        cowReclaimRegistryFreeRecord(record);
    }

    return 0;
}
