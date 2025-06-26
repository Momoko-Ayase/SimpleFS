#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "simplefs.h"

// 位图操作
void set_bitmap_bit(std::vector<uint8_t>& bitmap_data, uint32_t bit_index);
void clear_bitmap_bit(std::vector<uint8_t>& bitmap_data, uint32_t bit_index);
bool is_bitmap_bit_set(const std::vector<uint8_t>& bitmap_data, uint32_t bit_index);

// 路径解析
void parse_path(const std::string& path, std::string& dirname, std::string& basename);

// 目录项长度计算
inline uint16_t calculate_dir_entry_len(uint8_t name_len) {
    uint16_t len = 8 + name_len;
    return (len + 3) & ~3U;
}

// 设备类型检查
bool is_block_device(int fd);

// 块组备份检查
bool is_backup_group(uint32_t group_index);

