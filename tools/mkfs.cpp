#include "simplefs.h"
#include "disk_io.h"
#include "utils.h"    // 位图操作工具函数
// 如果位图工具在utils.h中，mkfs.cpp就不再直接需要metadata.h了

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring> // 字符串操作
#include <sys/stat.h> // stat, fstat
#include <fcntl.h>    // open
#include <unistd.h>   // close, ftruncate
#include <cmath>      // ceil
#include <numeric>    // std::fill
#include <algorithm>  // std::fill

#include <sys/ioctl.h>  // ioctl
#include <linux/fs.h>   // BLKGETSIZE64

// 默认参数(可通过选项覆盖)
const uint32_t DEFAULT_BLOCKS_PER_GROUP = SIMPLEFS_BLOCK_SIZE * 8; // 位图中每位对应一个块
const uint32_t DEFAULT_INODES_PER_GROUP = 1024; // 选定的默认值

// 静态位图辅助函数已移至metadata.cpp

void print_usage(const char* prog_name) {
    std::cerr << "用法: " << prog_name << " <设备文件> [块数量]" << std::endl;
    std::cerr << "  <设备文件>: 磁盘镜像文件或块设备路径" << std::endl;
    std::cerr << "  [块数量]: 可选，新镜像文件的总块数" << std::endl;
}

// 获取文件大小的函数
off_t get_file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("获取文件大小失败");
        return -1;
    }
    return st.st_size;
}


int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return 1;
    }

    std::string device_path = argv[1];
    uint64_t total_blocks_on_device = 0;
    uint64_t device_size_bytes = 0;
    bool create_new_image = false;

    DeviceFd fd = open(device_path.c_str(), O_RDWR);

    if (fd == -1) {
        if (errno == ENOENT && argc == 3) { // 文件不存在，且提供了大小
            std::cout << "正在创建新镜像文件: " << device_path << std::endl;
            fd = open(device_path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
            if (fd == -1) {
                perror("创建镜像文件失败");
                return 1;
            }
            create_new_image = true;
            try {
                total_blocks_on_device = std::stoull(argv[2]);
                if (total_blocks_on_device == 0) {
                    std::cerr << "块数量必须为正数" << std::endl;
                    close(fd);
                    unlink(device_path.c_str()); // 清理已创建的文件
                    return 1;
                }
                device_size_bytes = total_blocks_on_device * SIMPLEFS_BLOCK_SIZE;
                if (ftruncate(fd, device_size_bytes) == -1) {
                    perror("设置镜像大小失败");
                    close(fd);
                    unlink(device_path.c_str()); // 清理
                    return 1;
                }
                std::cout << "已创建新镜像文件 " << device_path << " (大小: " 
                          << total_blocks_on_device << " 块, " << device_size_bytes << " 字节)" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "块数量参数无效: " << e.what() << std::endl;
                close(fd);
                unlink(device_path.c_str()); // 清理
                return 1;
            }
        } else if (errno == ENOENT) {
            std::cerr << "错误: 镜像文件 '" << device_path << "' 未找到，请提供块数量来创建" << std::endl;
            perror("打开设备/镜像失败");
            return 1;
        }
         else {
            perror("打开设备/镜像失败");
            return 1;
        }
    } else {
        bool is_blk_dev = is_block_device(fd);

        if (is_blk_dev) {
            std::cout << "检测到块设备" << std::endl;
            uint64_t size_from_ioctl = 0;
            if (ioctl(fd, BLKGETSIZE64, &size_from_ioctl) == 0) {
                 std::cout << "通过ioctl获取块设备大小: " << size_from_ioctl << " 字节" << std::endl;
                 if (size_from_ioctl % SIMPLEFS_BLOCK_SIZE != 0) {
                      std::cerr << "警告: 块设备大小不是文件系统块大小的整数倍" << std::endl;
                 }
                 device_size_bytes = size_from_ioctl;
                 total_blocks_on_device = device_size_bytes / SIMPLEFS_BLOCK_SIZE;
                 if (argc == 3) {
                     std::cout << "注意: 忽略[num_blocks]参数，已自动检测设备大小" << std::endl;
                 }
            } else {
                perror("ioctl获取设备大小失败");
                std::cerr << "无法自动检测块设备大小" << std::endl;
                if (argc != 3) {
                    std::cerr << "错误: 请手动提供[num_blocks]参数" << std::endl;
                    close(fd);
                    return 1;
                }
                 try {
                    total_blocks_on_device = std::stoull(argv[2]);
                    if (total_blocks_on_device == 0) {
                        std::cerr << "块数量必须为正数" << std::endl;
                        close(fd);
                        return 1;
                    }
                    device_size_bytes = total_blocks_on_device * SIMPLEFS_BLOCK_SIZE;
                } catch (const std::exception& e) {
                    std::cerr << "块数量参数无效: " << e.what() << std::endl;
                    close(fd);
                    return 1;
                }
            }
             std::cout << "格式化块设备 " << device_path << "，块数: " << total_blocks_on_device
                      << " (" << device_size_bytes << " 字节)" << std::endl;

        } else {
            // 这是常规文件，获取其大小
            off_t existing_size = get_file_size(fd);
            if (existing_size == -1) {
                close(fd);
                return 1;
            }
            if (existing_size == 0) {
                 if (argc == 3) { // 这是空文件但用户要用指定大小格式化
                    std::cout << "现有文件为空，按新镜像创建处理" << std::endl;
                    create_new_image = true;
                    try {
                        total_blocks_on_device = std::stoull(argv[2]);
                         if (total_blocks_on_device == 0) {
                            std::cerr << "块数量必须为正数" << std::endl;
                            close(fd);
                            return 1;
                        }
                        device_size_bytes = total_blocks_on_device * SIMPLEFS_BLOCK_SIZE;
                        if (ftruncate(fd, device_size_bytes) == -1) {
                            perror("设置镜像大小失败");
                            close(fd);
                            return 1;
                        }
                        std::cout << "设置镜像 " << device_path << " 大小为 " << total_blocks_on_device
                                  << " 块 (" << device_size_bytes << " 字节)" << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "块数量参数无效: " << e.what() << std::endl;
                        close(fd);
                        return 1;
                    }
                } else {
                    std::cerr << "错误: 镜像文件为空" << std::endl;
                    std::cerr << "如需格式化，请提供num_blocks参数" << std::endl;
                    close(fd);
                    return 1;
                }
            } else {
                 if (existing_size % SIMPLEFS_BLOCK_SIZE != 0) {
                    std::cerr << "警告: 设备/镜像大小 (" << existing_size
                              << " 字节) 不是块大小 (" << SIMPLEFS_BLOCK_SIZE << " 字节) 的整数倍" << std::endl;
                }
                device_size_bytes = existing_size;
                total_blocks_on_device = device_size_bytes / SIMPLEFS_BLOCK_SIZE;
                std::cout << "Opened existing image " << device_path << ". Total blocks: " << total_blocks_on_device
                          << " (" << device_size_bytes << " bytes)." << std::endl;
                if (argc == 3 && !create_new_image) {
                    std::cout << "Note: [num_blocks] argument is ignored when using an existing, non-empty image file." << std::endl;
                }
            }
        }
    }

    if (total_blocks_on_device < 64) {
        std::cerr << "错误: 设备/镜像太小，至少需要64个块" << std::endl;
        close(fd);
        if (create_new_image) unlink(device_path.c_str());
        return 1;
    }

    std::cout << "正在格式化 " << device_path << " 为 SimpleFS..." << std::endl;

    uint32_t inodes_per_block = SIMPLEFS_BLOCK_SIZE / SIMPLEFS_INODE_SIZE;
    uint32_t sb_blocks_per_group = DEFAULT_BLOCKS_PER_GROUP;
    uint32_t sb_inodes_per_group = DEFAULT_INODES_PER_GROUP;
    if (sb_inodes_per_group > SIMPLEFS_BLOCK_SIZE * 8) {
        sb_inodes_per_group = SIMPLEFS_BLOCK_SIZE * 8;
        std::cout << "警告: 请求的每组inode数超过inode位图最大容量，调整为 "
                  << sb_inodes_per_group << std::endl;
    }

    uint32_t num_block_groups = static_cast<uint32_t>(std::ceil(static_cast<double>(total_blocks_on_device) / sb_blocks_per_group));
    if (num_block_groups == 0) num_block_groups = 1;

    uint64_t total_inodes_fs = static_cast<uint64_t>(num_block_groups) * sb_inodes_per_group;
    if (total_inodes_fs > UINT32_MAX) {
        total_inodes_fs = UINT32_MAX;
    }

    std::cout << "文件系统布局:" << std::endl;
    std::cout << "  总块数: " << total_blocks_on_device << std::endl;
    std::cout << "  每组inode数: " << sb_inodes_per_group << std::endl;
    std::cout << "  块组数: " << num_block_groups << std::endl;
    std::cout << "  总inode数: " << total_inodes_fs << std::endl;

    SimpleFS_SuperBlock sb;
    std::memset(&sb, 0, sizeof(SimpleFS_SuperBlock));
    sb.s_magic = SIMPLEFS_MAGIC;
    sb.s_blocks_count = total_blocks_on_device;
    sb.s_inodes_count = static_cast<uint32_t>(total_inodes_fs);
    sb.s_log_block_size = static_cast<uint32_t>(std::log2(SIMPLEFS_BLOCK_SIZE)) - 10;
    sb.s_blocks_per_group = sb_blocks_per_group;
    sb.s_inodes_per_group = sb_inodes_per_group;
    sb.s_inode_size = SIMPLEFS_INODE_SIZE;
    sb.s_root_inode = SIMPLEFS_ROOT_INODE_NUM;
    sb.s_first_ino = 11;
    sb.s_state = 1;
    sb.s_errors = 1;
    sb.s_max_mnt_count = 20;
    sb.s_mnt_count = 0;
    sb.s_wtime = time(nullptr);
    sb.s_block_group_nr = 0;

    uint32_t gdt_size_bytes = num_block_groups * sizeof(SimpleFS_GroupDesc);
    uint32_t gdt_blocks = static_cast<uint32_t>(std::ceil(static_cast<double>(gdt_size_bytes) / SIMPLEFS_BLOCK_SIZE));
    std::cout << "  GDT size: " << gdt_size_bytes << " bytes, requiring " << gdt_blocks << " blocks." << std::endl;

    uint32_t superblock_location_block = 1;
    uint32_t gdt_start_block = superblock_location_block + 1;

    std::vector<SimpleFS_GroupDesc> gdt(num_block_groups);
    std::memset(gdt.data(), 0, gdt_size_bytes);

    uint32_t running_total_free_blocks = total_blocks_on_device;
    uint32_t running_total_free_inodes = sb.s_inodes_count;

    if (superblock_location_block < total_blocks_on_device) running_total_free_blocks--; else { std::cerr << "No space for SB!" << std::endl; return 1; }
    for(uint32_t i = 0; i < gdt_blocks; ++i) {
        if ((gdt_start_block + i) < total_blocks_on_device) running_total_free_blocks--; else { std::cerr << "No space for GDT!" << std::endl; return 1; }
    }

    uint32_t current_group_meta_start_block = gdt_start_block + gdt_blocks;

    for (uint32_t i = 0; i < num_block_groups; ++i) {
        SimpleFS_GroupDesc& current_gd = gdt[i];
        uint32_t group_abs_start_block_for_data = i * sb.s_blocks_per_group;
        uint32_t inode_table_size_blocks = static_cast<uint32_t>(std::ceil(static_cast<double>(sb.s_inodes_per_group) * SIMPLEFS_INODE_SIZE / SIMPLEFS_BLOCK_SIZE));

        bool backup_here = is_backup_group(i);

        if (i == 0) {
            current_gd.bg_block_bitmap = current_group_meta_start_block;
            current_gd.bg_inode_bitmap = current_gd.bg_block_bitmap + 1;
            current_gd.bg_inode_table  = current_gd.bg_inode_bitmap + 1;
            sb.s_first_data_block = current_gd.bg_inode_table + inode_table_size_blocks;
        } else if (backup_here) {
            current_gd.bg_block_bitmap = group_abs_start_block_for_data + 1 + gdt_blocks;
            current_gd.bg_inode_bitmap = current_gd.bg_block_bitmap + 1;
            current_gd.bg_inode_table  = current_gd.bg_inode_bitmap + 1;
        } else {
            current_gd.bg_block_bitmap = group_abs_start_block_for_data;
            current_gd.bg_inode_bitmap = current_gd.bg_block_bitmap + 1;
            current_gd.bg_inode_table  = current_gd.bg_inode_bitmap + 1;
        }

        uint32_t last_meta_block_for_group = current_gd.bg_inode_table + inode_table_size_blocks -1;
        if (last_meta_block_for_group >= total_blocks_on_device || current_gd.bg_block_bitmap >= total_blocks_on_device) {
             std::cerr << "错误: 组 " << i << " 的元数据超出设备限制" << std::endl; close(fd); if(create_new_image) unlink(device_path.c_str()); return 1;
        }

        running_total_free_blocks--;  // 块位图
        running_total_free_blocks--;  // inode位图
        running_total_free_blocks -= inode_table_size_blocks;  // inode表
        // 注意：主组（组0）的超级块和GDT已在前面减去，这里不重复减去
        if (backup_here && i != 0) {  // 只有备份组且不是组0才减去
            running_total_free_blocks--; // 备份超级块
            running_total_free_blocks -= gdt_blocks;  // 备份GDT
        }

        uint32_t blocks_in_this_group_range = (i == num_block_groups - 1) ?
                                     (total_blocks_on_device - group_abs_start_block_for_data) :
                                     sb.s_blocks_per_group;

        current_gd.bg_free_blocks_count = blocks_in_this_group_range;
        current_gd.bg_free_blocks_count -= 1;  // 块位图
        current_gd.bg_free_blocks_count -= 1;  // inode位图
        current_gd.bg_free_blocks_count -= inode_table_size_blocks;  // inode表
        if (i == 0) {
            current_gd.bg_free_blocks_count--; // 超级块
            current_gd.bg_free_blocks_count -= gdt_blocks;  // GDT
        } else if (backup_here) {
            current_gd.bg_free_blocks_count--; // 备份超级块
            current_gd.bg_free_blocks_count -= gdt_blocks;  // 备份GDT
        }

        // 主要SB/GDT的额外空闲块调整已在前面完成
        current_gd.bg_free_inodes_count = sb.s_inodes_per_group;
        current_gd.bg_used_dirs_count = 0;
    }

    sb.s_free_blocks_count = running_total_free_blocks;
    sb.s_free_inodes_count = running_total_free_inodes;

    if (gdt_start_block + gdt_blocks > gdt[0].bg_block_bitmap) {
        std::cerr << "严重错误: GDT与组0块位图重叠，设备太小或计算错误" << std::endl;
        close(fd); if(create_new_image) unlink(device_path.c_str()); return 1;
    }

    std::cout << "超级块(根目录前的最终估计值):" << std::endl;
    std::cout << "  空闲块数: " << sb.s_free_blocks_count << std::endl;
    std::cout << "  空闲inode数: " << sb.s_free_inodes_count << std::endl;
    std::cout << "  首个数据块(全局): " << sb.s_first_data_block << std::endl;

    std::vector<uint8_t> fs_block_buffer(SIMPLEFS_BLOCK_SIZE, 0);
    std::memcpy(fs_block_buffer.data(), &sb, sizeof(sb));
    if (write_block(fd, superblock_location_block, fs_block_buffer.data()) != 0) {
        std::cerr << "超级块写入失败" << std::endl; close(fd); if (create_new_image) unlink(device_path.c_str()); return 1;
    }
    std::cout << "超级块已写入块 " << superblock_location_block << std::endl;

    uint8_t* gdt_write_ptr = reinterpret_cast<uint8_t*>(gdt.data());
    for (uint32_t i = 0; i < gdt_blocks; ++i) {
        std::fill(fs_block_buffer.begin(), fs_block_buffer.end(), 0);
        uint32_t bytes_to_copy_this_block = (gdt_size_bytes - (i * SIMPLEFS_BLOCK_SIZE) < SIMPLEFS_BLOCK_SIZE) ?
                                            (gdt_size_bytes % SIMPLEFS_BLOCK_SIZE == 0 && gdt_size_bytes > 0 ? SIMPLEFS_BLOCK_SIZE : gdt_size_bytes % SIMPLEFS_BLOCK_SIZE)
                                            : SIMPLEFS_BLOCK_SIZE;
        if (gdt_size_bytes == 0 && i==0) bytes_to_copy_this_block = 0;

        std::memcpy(fs_block_buffer.data(), gdt_write_ptr + (i * SIMPLEFS_BLOCK_SIZE), bytes_to_copy_this_block);
        if (write_block(fd, gdt_start_block + i, fs_block_buffer.data()) != 0) {
            std::cerr << "GDT块 " << (gdt_start_block + i) << " 写入失败" << std::endl; close(fd); if (create_new_image) unlink(device_path.c_str()); return 1;
        }
    }
    std::cout << "GDT已写入块 " << gdt_start_block << " 到 " << (gdt_start_block + gdt_blocks - 1) << std::endl;

    std::vector<uint8_t> group_block_bitmap_buffer(SIMPLEFS_BLOCK_SIZE, 0);
    std::vector<uint8_t> group_inode_bitmap_buffer(SIMPLEFS_BLOCK_SIZE, 0);

    for (uint32_t i = 0; i < num_block_groups; ++i) {
        SimpleFS_GroupDesc& current_gd_ref = gdt[i];
        uint32_t group_abs_data_start_block = i * sb.s_blocks_per_group; // 这是该组位图中位0的块号
        uint32_t inode_table_size_blocks = static_cast<uint32_t>(std::ceil(static_cast<double>(sb.s_inodes_per_group) * SIMPLEFS_INODE_SIZE / SIMPLEFS_BLOCK_SIZE));

        std::cout << "Processing Group " << i << ":" << std::endl;
        std::cout << "  BB@" << current_gd_ref.bg_block_bitmap << ", IB@" << current_gd_ref.bg_inode_bitmap << ", IT@" << current_gd_ref.bg_inode_table << " (" << inode_table_size_blocks << " blocks)" << std::endl;

        std::fill(group_block_bitmap_buffer.begin(), group_block_bitmap_buffer.end(), 0);
        std::fill(group_inode_bitmap_buffer.begin(), group_inode_bitmap_buffer.end(), 0);

        // 在其块位图中将组自己的元数据块标记为已使用
        // 组位图中的位索引相对于该组管理的块起始位置
        set_bitmap_bit(group_block_bitmap_buffer, current_gd_ref.bg_block_bitmap - group_abs_data_start_block);
        set_bitmap_bit(group_block_bitmap_buffer, current_gd_ref.bg_inode_bitmap - group_abs_data_start_block);
        for (uint32_t j = 0; j < inode_table_size_blocks; ++j) {
            set_bitmap_bit(group_block_bitmap_buffer, (current_gd_ref.bg_inode_table + j) - group_abs_data_start_block);
        }

        bool backup_here = is_backup_group(i);
        if (backup_here) {
            uint32_t sb_blk = (i == 0) ? superblock_location_block : group_abs_data_start_block;
            set_bitmap_bit(group_block_bitmap_buffer, sb_blk - group_abs_data_start_block);
            for (uint32_t gdt_idx = 0; gdt_idx < gdt_blocks; ++gdt_idx) {
                uint32_t abs_gdt_block = (i == 0) ? (gdt_start_block + gdt_idx)
                                                 : (group_abs_data_start_block + 1 + gdt_idx);
                set_bitmap_bit(group_block_bitmap_buffer, abs_gdt_block - group_abs_data_start_block);
            }


        }
        if (i == 0) {
            if (!is_bitmap_bit_set(group_block_bitmap_buffer, 0)) {
                set_bitmap_bit(group_block_bitmap_buffer, 0);
                bool block0_was_sb = (superblock_location_block == 0);
                bool block0_was_gdt = false;
                for(uint32_t gdt_idx=0; gdt_idx < gdt_blocks; ++gdt_idx) if((gdt_start_block + gdt_idx) == 0) block0_was_gdt = true;
                bool block0_was_group0_bb = (current_gd_ref.bg_block_bitmap == 0);
                bool block0_was_group0_ib = (current_gd_ref.bg_inode_bitmap == 0);
                bool block0_was_group0_it = (current_gd_ref.bg_inode_table == 0);
                if (!block0_was_sb && !block0_was_gdt && !block0_was_group0_bb && !block0_was_group0_ib && !block0_was_group0_it) {
                    if (current_gd_ref.bg_free_blocks_count > 0) current_gd_ref.bg_free_blocks_count--;
                    if (sb.s_free_blocks_count > 0) sb.s_free_blocks_count--;
                }
            }
            set_bitmap_bit(group_inode_bitmap_buffer, 0);
            set_bitmap_bit(group_inode_bitmap_buffer, 1);
            if (current_gd_ref.bg_free_inodes_count >= 2) current_gd_ref.bg_free_inodes_count -= 2; else current_gd_ref.bg_free_inodes_count = 0;
            if (sb.s_free_inodes_count >= 2) sb.s_free_inodes_count -= 2; else sb.s_free_inodes_count = 0;
        }

        if (write_block(fd, current_gd_ref.bg_block_bitmap, group_block_bitmap_buffer.data()) != 0) { /* error */ return 1; }
        std::cout << "    Written Block Bitmap. Group free blocks: " << current_gd_ref.bg_free_blocks_count << std::endl;

        if (write_block(fd, current_gd_ref.bg_inode_bitmap, group_inode_bitmap_buffer.data()) != 0) { /* error */ return 1; }
        std::cout << "    Written Inode Bitmap. Group free inodes: " << current_gd_ref.bg_free_inodes_count << std::endl;

        if (write_zero_blocks(fd, current_gd_ref.bg_inode_table, inode_table_size_blocks) != 0) { /* error */ return 1; }
        std::cout << "    Zeroed Inode Table." << std::endl;
    }

    std::cout << "Re-writing Superblock and GDT (final pre-root dir)..." << std::endl;
    std::memcpy(fs_block_buffer.data(), &sb, sizeof(sb));
    if (write_block(fd, superblock_location_block, fs_block_buffer.data()) != 0) { /* error */ return 1;}

    gdt_write_ptr = reinterpret_cast<uint8_t*>(gdt.data());
    for (uint32_t i = 0; i < gdt_blocks; ++i) {
        std::fill(fs_block_buffer.begin(), fs_block_buffer.end(), 0);
        uint32_t bytes_to_copy_this_block = (gdt_size_bytes - (i * SIMPLEFS_BLOCK_SIZE) < SIMPLEFS_BLOCK_SIZE) ?
                                            (gdt_size_bytes % SIMPLEFS_BLOCK_SIZE == 0 && gdt_size_bytes > 0 ? SIMPLEFS_BLOCK_SIZE : gdt_size_bytes % SIMPLEFS_BLOCK_SIZE)
                                            : SIMPLEFS_BLOCK_SIZE;
        if (gdt_size_bytes == 0 && i==0) bytes_to_copy_this_block = 0;
        std::memcpy(fs_block_buffer.data(), gdt_write_ptr + (i * SIMPLEFS_BLOCK_SIZE), bytes_to_copy_this_block);
        if (write_block(fd, gdt_start_block + i, fs_block_buffer.data()) != 0) { /* error */ return 1; }
    }

    // 将超级块和GDT的备份副本写入指定组
    for (uint32_t grp = 1; grp < num_block_groups; ++grp) {
        if (!is_backup_group(grp)) continue;
        uint32_t grp_start = grp * sb.s_blocks_per_group;
        std::memcpy(fs_block_buffer.data(), &sb, sizeof(sb));
        if (write_block(fd, grp_start, fs_block_buffer.data()) != 0) { /* error */ return 1; }
        for (uint32_t i = 0; i < gdt_blocks; ++i) {
            std::fill(fs_block_buffer.begin(), fs_block_buffer.end(), 0);
            uint32_t bytes_to_copy_this_block = (gdt_size_bytes - (i * SIMPLEFS_BLOCK_SIZE) < SIMPLEFS_BLOCK_SIZE) ?
                                                (gdt_size_bytes % SIMPLEFS_BLOCK_SIZE == 0 && gdt_size_bytes > 0 ? SIMPLEFS_BLOCK_SIZE : gdt_size_bytes % SIMPLEFS_BLOCK_SIZE)
                                                : SIMPLEFS_BLOCK_SIZE;
            if (gdt_size_bytes == 0 && i==0) bytes_to_copy_this_block = 0;
            std::memcpy(fs_block_buffer.data(), gdt_write_ptr + (i * SIMPLEFS_BLOCK_SIZE), bytes_to_copy_this_block);
            if (write_block(fd, grp_start + 1 + i, fs_block_buffer.data()) != 0) { /* error */ return 1; }
        }
    }

    std::cout << "Creating root directory..." << std::endl;

    SimpleFS_GroupDesc& group0_gd = gdt[0];
    // 根inode（SIMPLEFS_ROOT_INODE_NUM，通常为2）和inode 1（位0）
    // 已在inode位图中标记，以及超级块/组描述符
    // 在组0的初始设置循环中已减少了空闲inode计数
    // 因此，此处不需要再次调用set_bitmap_bit或减少计数
    // 只需为组0增加used_dirs_count
    group0_gd.bg_used_dirs_count++;

    std::cout << "  Root inode " << SIMPLEFS_ROOT_INODE_NUM << " allocation accounted for (marked in bitmap and counts updated earlier)." << std::endl;
    std::cout << "  Incremented used_dirs_count for group 0." << std::endl;

    // 如果后续决策依赖于最新状态，重新读取inode位图是好习惯
    // 但此处不对根inode号本身做分配决策，只分配其数据块，故继续进行
    // 如有疑问，可重新读取：
    // if (read_block(fd, group0_gd.bg_inode_bitmap, group_inode_bitmap_buffer.data()) != 0) {
    //     std::cerr << "根目录数据分配前重新读取组0 inode位图失败" << std::endl; return 1;
    // }

    uint32_t root_dir_data_block_num = 0;
    if (read_block(fd, group0_gd.bg_block_bitmap, group_block_bitmap_buffer.data()) != 0) {
         std::cerr << "组0块位图读取失败，无法分配根目录数据块" << std::endl; return 1;
    }

    uint32_t group0_abs_start_block = 0 * sb.s_blocks_per_group;
    uint32_t search_start_offset_in_group0 = 0;
    if (sb.s_first_data_block >= group0_abs_start_block && sb.s_first_data_block < (group0_abs_start_block + sb.s_blocks_per_group) ) {
        search_start_offset_in_group0 = sb.s_first_data_block - group0_abs_start_block;
    } else if (sb.s_first_data_block < group0_abs_start_block) {
         search_start_offset_in_group0 = (gdt[0].bg_inode_table + static_cast<uint32_t>(std::ceil(static_cast<double>(sb.s_inodes_per_group) * SIMPLEFS_INODE_SIZE / SIMPLEFS_BLOCK_SIZE))) - group0_abs_start_block;
    }

    for (uint32_t block_offset_in_group = search_start_offset_in_group0;
         block_offset_in_group < sb.s_blocks_per_group; ++block_offset_in_group) {
        if (!is_bitmap_bit_set(group_block_bitmap_buffer, block_offset_in_group)) {
            root_dir_data_block_num = group0_abs_start_block + block_offset_in_group;
            set_bitmap_bit(group_block_bitmap_buffer, block_offset_in_group);
            group0_gd.bg_free_blocks_count--;
            sb.s_free_blocks_count--;
            break;
        }
    }

    if (root_dir_data_block_num == 0) {
        std::cerr << "错误: 组0中找不到根目录的空闲数据块" << std::endl; return 1;
    }
    if (write_block(fd, group0_gd.bg_block_bitmap, group_block_bitmap_buffer.data()) != 0) {
        std::cerr << "组0块位图更新失败" << std::endl; return 1;
    }
    std::cout << "  已为根目录分配数据块 " << root_dir_data_block_num << std::endl;

    std::vector<uint8_t> root_dir_data_buffer(SIMPLEFS_BLOCK_SIZE, 0);
    uint16_t current_offset = 0;

    SimpleFS_DirEntry dot_entry;
    dot_entry.inode = SIMPLEFS_ROOT_INODE_NUM;
    dot_entry.name_len = 1;
    dot_entry.file_type = S_IFDIR >> 12;
    std::strncpy(dot_entry.name, ".", 1);
    dot_entry.rec_len = calculate_dir_entry_len(dot_entry.name_len);
    std::memcpy(root_dir_data_buffer.data() + current_offset, &dot_entry, (size_t)8 + dot_entry.name_len);
    current_offset += dot_entry.rec_len;

    SimpleFS_DirEntry dotdot_entry;
    dotdot_entry.inode = SIMPLEFS_ROOT_INODE_NUM;
    dotdot_entry.name_len = 2;
    dotdot_entry.file_type = S_IFDIR >> 12;
    std::strncpy(dotdot_entry.name, "..", 2);
    dotdot_entry.rec_len = SIMPLEFS_BLOCK_SIZE - current_offset;
    std::memcpy(root_dir_data_buffer.data() + current_offset, &dotdot_entry, (size_t)8 + dotdot_entry.name_len);

    if (write_block(fd, root_dir_data_block_num, root_dir_data_buffer.data()) != 0) {
        std::cerr << "根目录数据块写入失败" << std::endl; return 1;
    }
    std::cout << "  已向根目录数据块写入'.'和'..'项" << std::endl;

    SimpleFS_Inode root_inode;
    std::memset(&root_inode, 0, sizeof(SimpleFS_Inode));
    root_inode.i_mode = S_IFDIR | 0777; // 从0755改为0777便于测试
    root_inode.i_uid = 0;
    root_inode.i_gid = 0;
    root_inode.i_size = SIMPLEFS_BLOCK_SIZE;
    root_inode.i_links_count = 2;
    root_inode.i_blocks = SIMPLEFS_BLOCK_SIZE / 512;
    root_inode.i_atime = root_inode.i_ctime = root_inode.i_mtime = time(nullptr);
    root_inode.i_block[0] = root_dir_data_block_num;

    // 基于1的inode编号的修正计算
    uint32_t root_inode_idx_in_group = SIMPLEFS_ROOT_INODE_NUM - 1; 
    uint32_t root_inode_block_in_table = group0_gd.bg_inode_table + (root_inode_idx_in_group / inodes_per_block);
    uint32_t root_inode_offset_in_block = (root_inode_idx_in_group % inodes_per_block) * SIMPLEFS_INODE_SIZE;

    std::vector<uint8_t> inode_table_block_buffer(SIMPLEFS_BLOCK_SIZE);
    if (read_block(fd, root_inode_block_in_table, inode_table_block_buffer.data()) != 0) {
        std::cerr << "根inode的inode表块读取失败" << std::endl; return 1;
    }
    std::memcpy(inode_table_block_buffer.data() + root_inode_offset_in_block, &root_inode, sizeof(SimpleFS_Inode));
    if (write_block(fd, root_inode_block_in_table, inode_table_block_buffer.data()) != 0) {
        std::cerr << "根inode写入inode表失败" << std::endl; return 1;
    }
    std::cout << "  Initialized and written root inode (inode " << SIMPLEFS_ROOT_INODE_NUM << ")." << std::endl;

    std::cout << "Finalizing Superblock and GDT..." << std::endl;
    std::memcpy(fs_block_buffer.data(), &sb, sizeof(sb));
    if (write_block(fd, superblock_location_block, fs_block_buffer.data()) != 0) { /* error */ return 1;}

    gdt_write_ptr = reinterpret_cast<uint8_t*>(gdt.data());
    for (uint32_t i = 0; i < gdt_blocks; ++i) {
        std::fill(fs_block_buffer.begin(), fs_block_buffer.end(), 0);
        uint32_t bytes_to_copy_this_block = (gdt_size_bytes - (i * SIMPLEFS_BLOCK_SIZE) < SIMPLEFS_BLOCK_SIZE) ?
                                            (gdt_size_bytes % SIMPLEFS_BLOCK_SIZE == 0 && gdt_size_bytes > 0 ? SIMPLEFS_BLOCK_SIZE : gdt_size_bytes % SIMPLEFS_BLOCK_SIZE)
                                            : SIMPLEFS_BLOCK_SIZE;
        if (gdt_size_bytes == 0 && i==0) bytes_to_copy_this_block = 0;
        std::memcpy(fs_block_buffer.data(), gdt_write_ptr + (i * SIMPLEFS_BLOCK_SIZE), bytes_to_copy_this_block);
        if (write_block(fd, gdt_start_block + i, fs_block_buffer.data()) != 0) { /* error */ return 1; }
    }
    std::cout << "超级块和GDT已完成" << std::endl;

    std::cout << "文件系统格式化成功" << std::endl;

    if (close(fd) == -1) {
        perror("关闭设备/镜像失败");
    }

    std::cout << "SimpleFS格式化工具完成" << std::endl;
    return 0;
}
