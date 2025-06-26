#include <fuse.h>
#include "fuse_ops.h" // init_fuse_operations和SimpleFS_Context
#include "disk_io.h"  // read_block等
#include "simplefs.h" // 结构体
#include "utils.h"    // is_block_device

#include <iostream>
#include <vector>
#include <string>
#include <cstring> // memset, strerror
#include <cerrno>  // errno
#include <fcntl.h> // open
#include <unistd.h> // close
#include <cmath>    // ceil

static SimpleFS_Context fs_context; // 全局文件系统上下文

int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cerr << "用法: " << argv[0] << " <设备文件> <挂载点> [FUSE选项...]" << std::endl;
        return 1;
    }

    // 设备镜像路径是argv[1]
    // 挂载点是argv[2]
    // FUSE选项从argv[2]开始，如果我们移位的话，或者手动处理
    // 保持简单：FUSE选项在挂载点之后传递

    std::string device_path = argv[1];
    fs_context.device_fd = open(device_path.c_str(), O_RDWR);
    if (fs_context.device_fd < 0) {
        perror("无法打开设备文件");
        return 1;
    }

    // 读取超级块
    std::vector<uint8_t> sb_buffer(SIMPLEFS_BLOCK_SIZE);
    if (read_block(fs_context.device_fd, 1, sb_buffer.data()) != 0) { // 超级块在块1
        std::cerr << "无法读取超级块" << std::endl;
        close(fs_context.device_fd);
        return 1;
    }
    std::memcpy(&fs_context.sb, sb_buffer.data(), sizeof(SimpleFS_SuperBlock));

    // 验证超级块魔数
    if (fs_context.sb.s_magic != SIMPLEFS_MAGIC) {
        std::cerr << "魔数不匹配，不是有效的SimpleFS文件系统" << std::endl;
        close(fs_context.device_fd);
        return 1;
    }

    std::cout << "SimpleFS已加载 - 块总数: " << fs_context.sb.s_blocks_count 
              << ", 空闲块: " << fs_context.sb.s_free_blocks_count << std::endl;


    // 读取组描述符表(GDT)
    uint32_t num_block_groups = static_cast<uint32_t>(std::ceil(static_cast<double>(fs_context.sb.s_blocks_count) / fs_context.sb.s_blocks_per_group));
    if (num_block_groups == 0 && fs_context.sb.s_blocks_count > 0) num_block_groups = 1;


    uint32_t gdt_size_bytes = num_block_groups * sizeof(SimpleFS_GroupDesc);
    uint32_t gdt_blocks_count = static_cast<uint32_t>(std::ceil(static_cast<double>(gdt_size_bytes) / SIMPLEFS_BLOCK_SIZE));

    fs_context.gdt.resize(num_block_groups);
    std::vector<uint8_t> gdt_buffer_raw(gdt_blocks_count * SIMPLEFS_BLOCK_SIZE);

    uint32_t gdt_start_block = 1 + 1; // 超级块在块1，GDT从块2开始

    for (uint32_t i = 0; i < gdt_blocks_count; ++i) {
        if (read_block(fs_context.device_fd, gdt_start_block + i, gdt_buffer_raw.data() + (i * SIMPLEFS_BLOCK_SIZE)) != 0) {
            std::cerr << "无法读取组描述符表" << std::endl;
            close(fs_context.device_fd);
            return 1;
        }
    }
    std::memcpy(fs_context.gdt.data(), gdt_buffer_raw.data(), gdt_size_bytes);

    // 准备FUSE参数
    bool is_blk_dev = is_block_device(fs_context.device_fd);
    bool allow_other_found = false;
    for (int i = 0; i < argc; ++i) {
        if (std::string(argv[i]) == "-o" && (i + 1 < argc) && std::string(argv[i+1]).find("allow_other") != std::string::npos) {
            allow_other_found = true;
            break;
        }
        if (std::string(argv[i]).find("-oallow_other") != std::string::npos) {
            allow_other_found = true;
            break;
        }
    }

    std::vector<char*> fuse_argv_vec;
    fuse_argv_vec.push_back(argv[0]); // 程序名
    // 添加原始参数，除了设备路径
    for (int i = 2; i < argc; ++i) {
        fuse_argv_vec.push_back(argv[i]);
    }

    if (!allow_other_found) {
        fuse_argv_vec.push_back(const_cast<char*>("-o"));
        fuse_argv_vec.push_back(const_cast<char*>("allow_other"));
    }

    struct fuse_operations simplefs_ops;
    init_fuse_operations(&simplefs_ops);

    // 传递上下文给FUSE
    int ret = fuse_main(fuse_argv_vec.size(), fuse_argv_vec.data(), &simplefs_ops, &fs_context);

    close(fs_context.device_fd);

    return ret;
}
