#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct super_manager super_manager;
typedef struct file_system_manager file_system_manager;

void superManagerInit(super_manager *this, file_system_manager *fs_manager);

uint32_t superManagerAllocNid(super_manager *this, uint32_t ino, bool is_inode);
void superManagerFreeNid(super_manager *this, uint32_t nid);

uint32_t superManagerAllocNodeLpa(super_manager *this);
uint32_t superManagerAllocDataLpa(super_manager *this);
