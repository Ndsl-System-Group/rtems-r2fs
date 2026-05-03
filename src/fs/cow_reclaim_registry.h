#ifndef _COW_RECLAIM_REGISTRY_H_
#define _COW_RECLAIM_REGISTRY_H_

#include <stddef.h>
#include <stdint.h>

struct file_system_manager;

void cowReclaimRegistryInit(struct file_system_manager *fs_manager);
void cowReclaimRegistryDestroy(void);

int cowReclaimRegistryRegister(
    uint64_t tx_id,
    const uint32_t *data_lpas,
    size_t data_count,
    const uint32_t *node_lpas,
    size_t node_count
);

void cowReclaimRegistryOnTxComplete(uint64_t tx_id);
int cowReclaimRegistryDrainCompleted(void);

#endif
