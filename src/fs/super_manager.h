#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct file_system_manager file_system_manager;

uint32_t SuperManagerAllocNid(file_system_manager *fs, uint32_t ino, bool is_inode);
uint32_t SuperManagerAllocNodeLpa(file_system_manager *fs);
uint32_t SuperManagerAllocDataLpa(file_system_manager *fs);
