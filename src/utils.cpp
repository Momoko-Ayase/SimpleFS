#include "utils.h"
#include <iostream>
#include <cstring>

void set_bitmap_bit(std::vector<uint8_t>& bitmap_data, uint32_t bit_index) {
    uint32_t byte_index = bit_index / 8;
    uint8_t bit_offset = bit_index % 8;
    if (bit_index < bitmap_data.size() * 8) {
        bitmap_data[byte_index] |= (1 << bit_offset);
    }
}

void clear_bitmap_bit(std::vector<uint8_t>& bitmap_data, uint32_t bit_index) {
    uint32_t byte_index = bit_index / 8;
    uint8_t bit_offset = bit_index % 8;
    if (bit_index < bitmap_data.size() * 8) {
        bitmap_data[byte_index] &= ~(1 << bit_offset);
    }
}

bool is_bitmap_bit_set(const std::vector<uint8_t>& bitmap_data, uint32_t bit_index) {
    uint32_t byte_index = bit_index / 8;
    uint8_t bit_offset = bit_index % 8;
    if (bit_index < bitmap_data.size() * 8) {
        return (bitmap_data[byte_index] & (1 << bit_offset)) != 0;
    }
    return true;
}

void parse_path(const std::string& path, std::string& dirname, std::string& basename) {
    if (path.empty()) {
        dirname = ".";
        basename = "";
        return;
    }

    // 规范化路径
    std::string p_normalized;
    p_normalized.reserve(path.length());
    if (path[0] == '/') {
        p_normalized += '/';
    }
    for (size_t i = 0; i < path.length(); ++i) {
        if (path[i] == '/') {
            if (!p_normalized.empty() && p_normalized.back() != '/') {
                p_normalized += '/';
            }
        } else {
            p_normalized += path[i];
        }
    }

    // 移除尾部斜杠，除非是根路径
    if (p_normalized.length() > 1 && p_normalized.back() == '/') {
        p_normalized.pop_back();
    }

    if (p_normalized.empty() && path[0] == '/') {
        p_normalized = "/";
    }

    if (p_normalized == "/") {
        dirname = "/";
        basename = "/";
        return;
    }

    size_t last_slash = p_normalized.find_last_of('/');
    if (last_slash == std::string::npos) {
        dirname = ".";
        basename = p_normalized;
    } else {
        basename = p_normalized.substr(last_slash + 1);
        if (last_slash == 0) {
            dirname = "/";
        } else {
            dirname = p_normalized.substr(0, last_slash);
        }
    }
}

#include <sys/stat.h>

bool is_block_device(int fd) {
    struct stat st;
    if (fstat(fd, &st) == -1) {
        return false;
    }
    return S_ISBLK(st.st_mode);
}

bool is_backup_group(uint32_t group_index) {
    if (group_index == 0 || group_index == 1) return true;
    uint32_t n = group_index;
    while (n % 3 == 0) n /= 3;
    while (n % 5 == 0) n /= 5;
    while (n % 7 == 0) n /= 7;
    return n == 1;
}

