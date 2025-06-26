#include "disk_io.h"
#include "simplefs.h"

#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

// 读取磁盘块
int read_block(DeviceFd fd, uint32_t block_num, void* buffer) {
    off_t offset = static_cast<off_t>(block_num) * SIMPLEFS_BLOCK_SIZE;
    ssize_t bytes_read = pread(fd, buffer, SIMPLEFS_BLOCK_SIZE, offset);

    if (bytes_read == -1) {
        perror("磁盘读取失败");
        return -1;
    }
    if (bytes_read < SIMPLEFS_BLOCK_SIZE) {
        return -1;
    }
    return 0;
}

// 写入磁盘块
int write_block(DeviceFd fd, uint32_t block_num, const void* buffer) {
    off_t offset = static_cast<off_t>(block_num) * SIMPLEFS_BLOCK_SIZE;
    ssize_t bytes_written = pwrite(fd, buffer, SIMPLEFS_BLOCK_SIZE, offset);

    if (bytes_written == -1) {
        perror("磁盘写入失败");
        return -1;
    }
    if (bytes_written < SIMPLEFS_BLOCK_SIZE) {
        return -1;
    }
    return 0;
}

// 批量写入零块
int write_zero_blocks(DeviceFd fd, uint32_t start_block_num, uint32_t count) {
    if (count == 0) return 0;
    
    std::vector<uint8_t> zero_buffer(SIMPLEFS_BLOCK_SIZE, 0);
    for (uint32_t i = 0; i < count; ++i) {
        if (write_block(fd, start_block_num + i, zero_buffer.data()) != 0) {
            return -1;
        }
    }
    return 0;
}
