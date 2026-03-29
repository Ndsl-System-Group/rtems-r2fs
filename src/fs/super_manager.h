#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct super_manager super_manager;
typedef struct file_system_manager file_system_manager;

void SuperManagerInit(super_manager *this, file_system_manager *fs_manager);

uint32_t SuperManagerAllocNid(super_manager *this, uint32_t ino, bool is_inode);
void SuperManagerFreeNid(super_manager *this, uint32_t nid);

uint32_t SuperManagerAllocNodeLpa(super_manager *this);
uint32_t SuperManagerAllocDataLpa(super_manager *this);
