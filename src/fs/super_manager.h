#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct super_manager super_manager;
typedef struct file_system_manager file_system_manager;

super_manager *superManagerCreate(file_system_manager *fs_manager);
void superManagerDestroy(super_manager *this);

uint32_t superManagerAllocNid(super_manager *this, uint32_t ino, bool is_inode);
void superManagerFreeNid(super_manager *this, uint32_t nid);

uint32_t superManagerAllocNodeLpa(super_manager *this);
uint32_t superManagerAllocDataLpa(super_manager *this);
uint32_t superManagerAllocDataLpaRange(
    super_manager *this,
    uint32_t requested_count,
    uint32_t *allocated_count
);
