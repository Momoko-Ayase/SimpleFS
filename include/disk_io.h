#pragma once

#include "simplefs.h"
#include <cstdint>
#include <vector>

using DeviceFd = int;

// 读取单个块
int read_block(DeviceFd fd, uint32_t block_num, void* buffer);

// 写入单个块
int write_block(DeviceFd fd, uint32_t block_num, const void* buffer);

// 写入零块
int write_zero_blocks(DeviceFd fd, uint32_t start_block_num, uint32_t count);
