#include "metadata.h"
#include "disk_io.h"
#include "simplefs.h"
#include <fuse.h>
#include <unistd.h>
#include "simplefs_context.h"
#include "utils.h"
#include <sys/stat.h>
#include <vector>
#include <cstdio>
#include <iostream>
#include <cstring>
#include <string>
#include <algorithm>
#include <cmath>

// 分配可用的inode
uint32_t alloc_inode(SimpleFS_Context& context, mode_t mode) {
    if (context.sb.s_free_inodes_count == 0) {
        errno = ENOSPC;
        return 0;
    }

    for (uint32_t group_idx = 0; group_idx < context.gdt.size(); ++group_idx) {
        SimpleFS_GroupDesc& gd = context.gdt[group_idx];
        if (gd.bg_free_inodes_count > 0) {
            std::vector<uint8_t> inode_bitmap_data(SIMPLEFS_BLOCK_SIZE);
            if (read_block(context.device_fd, gd.bg_inode_bitmap, inode_bitmap_data.data()) != 0) {
                continue;
            }

            for (uint32_t bit_idx = 0; bit_idx < context.sb.s_inodes_per_group; ++bit_idx) {
                if (!is_bitmap_bit_set(inode_bitmap_data, bit_idx)) {
                    uint32_t inode_num = (group_idx * context.sb.s_inodes_per_group) + bit_idx + 1;

                    if (inode_num == 0 || inode_num > context.sb.s_inodes_count) {
                        continue;
                    }

                    set_bitmap_bit(inode_bitmap_data, bit_idx);
                    if (write_block(context.device_fd, gd.bg_inode_bitmap, inode_bitmap_data.data()) != 0) {
                        errno = EIO;
                        return 0;
                    }

                    gd.bg_free_inodes_count--;
                    context.sb.s_free_inodes_count--;
                    if (S_ISDIR(mode)) {
                        gd.bg_used_dirs_count++;
                    }

                    return inode_num;
                }
            }
        }
    }
    errno = ENOSPC;
    return 0;
}

// 释放inode
void free_inode(SimpleFS_Context& context, uint32_t inode_num, mode_t mode_of_freed_inode) {
    if (inode_num == 0 || inode_num > context.sb.s_inodes_count) {
        return;
    }
    if (inode_num < context.sb.s_first_ino && inode_num != 0) {
        return;
    }

    uint32_t group_idx = (inode_num - 1) / context.sb.s_inodes_per_group;
    if (group_idx >= context.gdt.size()) {
        return;
    }
    SimpleFS_GroupDesc& gd = context.gdt[group_idx];
    uint32_t bit_idx = (inode_num - 1) % context.sb.s_inodes_per_group;

    std::vector<uint8_t> inode_bitmap_data(SIMPLEFS_BLOCK_SIZE);
    if (read_block(context.device_fd, gd.bg_inode_bitmap, inode_bitmap_data.data()) != 0) {
        return;
    }

    clear_bitmap_bit(inode_bitmap_data, bit_idx);
    if (write_block(context.device_fd, gd.bg_inode_bitmap, inode_bitmap_data.data()) != 0) {
        return;
    }

    gd.bg_free_inodes_count++;
    context.sb.s_free_inodes_count++;

    if (S_ISDIR(mode_of_freed_inode)) {
       if (gd.bg_used_dirs_count > 0) gd.bg_used_dirs_count--;
    }
}

#include <numeric>

// 分配数据块
uint32_t alloc_block(SimpleFS_Context& context, uint32_t preferred_group_for_inode) {
    // 简化的一致性检查
    if (context.sb.s_free_blocks_count == 0) {
        errno = ENOSPC;
        return 0;
    }

    uint32_t target_group_idx = static_cast<uint32_t>(-1);
    uint32_t num_groups = context.gdt.size();

    // 优先尝试首选组
    if (preferred_group_for_inode < num_groups) {
        if (context.gdt[preferred_group_for_inode].bg_free_blocks_count > 0) {
            target_group_idx = preferred_group_for_inode;
        }
    }

    // 如果首选组不可用，搜索所有组
    if (target_group_idx == static_cast<uint32_t>(-1)) {
        for (uint32_t i = 0; i < num_groups; ++i) {
            if (context.gdt[i].bg_free_blocks_count > 0) {
                target_group_idx = i;
                break;
            }
        }
    }

    if (target_group_idx == static_cast<uint32_t>(-1)) {
        errno = ENOSPC;
        return 0;
    }

    SimpleFS_GroupDesc& gd = context.gdt[target_group_idx];
    std::vector<uint8_t> block_bitmap_data(SIMPLEFS_BLOCK_SIZE);

    if (read_block(context.device_fd, gd.bg_block_bitmap, block_bitmap_data.data()) != 0) {
        errno = EIO;
        return 0;
    }

    for (uint32_t bit_idx = 0; bit_idx < context.sb.s_blocks_per_group; ++bit_idx) {
        if (!is_bitmap_bit_set(block_bitmap_data, bit_idx)) {
            uint32_t block_num = (target_group_idx * context.sb.s_blocks_per_group) + bit_idx;

            if (block_num == 0 && target_group_idx == 0 && bit_idx == 0) {
                 continue;
            }
            if (block_num >= context.sb.s_blocks_count) {
                 continue;
            }

            set_bitmap_bit(block_bitmap_data, bit_idx);
            if (write_block(context.device_fd, gd.bg_block_bitmap, block_bitmap_data.data()) != 0) {
                errno = EIO;
                return 0;
            }

            gd.bg_free_blocks_count--;
            context.sb.s_free_blocks_count--;

            return block_num;
        }
    }

    errno = ENOSPC;
    return 0;
}

// 释放数据块
void free_block(SimpleFS_Context& context, uint32_t block_num) {
    if (block_num == 0 || block_num >= context.sb.s_blocks_count) {
        return;
    }

    uint32_t group_idx = block_num / context.sb.s_blocks_per_group;
    if (group_idx >= context.gdt.size()) {
        return;
    }
    SimpleFS_GroupDesc& gd = context.gdt[group_idx];
    uint32_t bit_idx = block_num % context.sb.s_blocks_per_group;

    std::vector<uint8_t> block_bitmap_data(SIMPLEFS_BLOCK_SIZE);
    if (read_block(context.device_fd, gd.bg_block_bitmap, block_bitmap_data.data()) != 0) {
        return;
    }

    clear_bitmap_bit(block_bitmap_data, bit_idx);
    if (write_block(context.device_fd, gd.bg_block_bitmap, block_bitmap_data.data()) != 0) {
        return;
    }

    gd.bg_free_blocks_count++;
    context.sb.s_free_blocks_count++;
}

// 将inode数据写入磁盘
int write_inode_to_disk(SimpleFS_Context& context, uint32_t inode_num, const SimpleFS_Inode* inode_data) {
    if (inode_num == 0 || inode_num > context.sb.s_inodes_count) {
        errno = EINVAL;
        return -EINVAL;
    }

    uint32_t group_idx = (inode_num - 1) / context.sb.s_inodes_per_group;
    if (group_idx >= context.gdt.size()) {
        errno = EIO;
        return -EIO;
    }
    const SimpleFS_GroupDesc& gd = context.gdt[group_idx];

    uint32_t inode_offset_in_group = (inode_num - 1) % context.sb.s_inodes_per_group;
    uint32_t inodes_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(SimpleFS_Inode);

    uint32_t block_num_in_table = inode_offset_in_group / inodes_per_block;
    uint32_t offset_within_block = (inode_offset_in_group % inodes_per_block) * sizeof(SimpleFS_Inode);

    uint32_t absolute_block_rw = gd.bg_inode_table + block_num_in_table;

    if (absolute_block_rw == 0 || absolute_block_rw >= context.sb.s_blocks_count) {
         errno = EIO;
        return -EIO;
    }

    std::vector<uint8_t> block_buffer(SIMPLEFS_BLOCK_SIZE);
    if (read_block(context.device_fd, absolute_block_rw, block_buffer.data()) != 0) {
        return -EIO;
    }

    std::memcpy(block_buffer.data() + offset_within_block, inode_data, sizeof(SimpleFS_Inode));

    if (write_block(context.device_fd, absolute_block_rw, block_buffer.data()) != 0) {
        return -EIO;
    }
    return 0;
}

// 从磁盘读取inode数据
int read_inode_from_disk(SimpleFS_Context& context, uint32_t inode_num, SimpleFS_Inode* inode_struct) {
    if (inode_num == 0 || inode_num > context.sb.s_inodes_count) {
        errno = EINVAL;
        return -EINVAL;
    }

    uint32_t group_idx = (inode_num - 1) / context.sb.s_inodes_per_group;
    if (group_idx >= context.gdt.size()) {
        errno = EIO;
        return -EIO;
    }
    const SimpleFS_GroupDesc& gd = context.gdt[group_idx];

    uint32_t inode_offset_in_group = (inode_num - 1) % context.sb.s_inodes_per_group;
    uint32_t inodes_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(SimpleFS_Inode);

    uint32_t block_num_in_table = inode_offset_in_group / inodes_per_block;
    uint32_t offset_within_block = (inode_offset_in_group % inodes_per_block) * sizeof(SimpleFS_Inode);

    uint32_t absolute_block_to_read = gd.bg_inode_table + block_num_in_table;

    if (absolute_block_to_read == 0 || absolute_block_to_read >= context.sb.s_blocks_count) {
        errno = EIO;
        return -EIO;
    }

    std::vector<uint8_t> block_buffer(SIMPLEFS_BLOCK_SIZE);

    if (read_block(context.device_fd, absolute_block_to_read, block_buffer.data()) != 0) {
        return -EIO;
    }

    std::memcpy(inode_struct, block_buffer.data() + offset_within_block, sizeof(SimpleFS_Inode));
    return 0;
}


// 向目录中添加文件项
int add_dir_entry(SimpleFS_Context& context, SimpleFS_Inode* parent_inode, uint32_t parent_inode_num,
                  const std::string& entry_name, uint32_t child_inode_num, uint8_t file_type) {
    if (entry_name.length() > SIMPLEFS_MAX_FILENAME_LEN) {
        errno = ENAMETOOLONG;
        return -ENAMETOOLONG;
    }

    uint16_t needed_len_for_new_entry = calculate_dir_entry_len(entry_name.length());
    uint16_t min_rec_len_for_empty = calculate_dir_entry_len(0);

    std::vector<uint8_t> dir_block_data_buffer(SIMPLEFS_BLOCK_SIZE);
    uint32_t pointers_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t);
    uint32_t max_logical_blocks = SIMPLEFS_NUM_DIRECT_BLOCKS +
                                 pointers_per_block + 
                                 pointers_per_block * pointers_per_block + 
                                 pointers_per_block * pointers_per_block * pointers_per_block;
    
    bool entry_placed = false;

    for (uint32_t logical_block_idx = 0; logical_block_idx < max_logical_blocks; ++logical_block_idx) {
        uint32_t current_physical_block = get_or_alloc_dir_block(context, parent_inode, parent_inode_num, logical_block_idx);
        if (current_physical_block == 0) {
            if (errno == 0) errno = ENOSPC;
            return -errno;
        }

        if (read_block(context.device_fd, current_physical_block, dir_block_data_buffer.data()) != 0) {
            errno = EIO;
            return -EIO;
        }

        uint16_t current_offset = 0;
        while(current_offset < SIMPLEFS_BLOCK_SIZE) {
            SimpleFS_DirEntry* dir_entry = reinterpret_cast<SimpleFS_DirEntry*>(dir_block_data_buffer.data() + current_offset);

            if (dir_entry->rec_len == 0) { 
                 if (current_offset == 0 && SIMPLEFS_BLOCK_SIZE >= needed_len_for_new_entry) {
                    dir_entry->inode = child_inode_num;
                    dir_entry->name_len = static_cast<uint8_t>(entry_name.length());
                    dir_entry->file_type = file_type;
                    std::strncpy(dir_entry->name, entry_name.c_str(), entry_name.length());
                    dir_entry->rec_len = SIMPLEFS_BLOCK_SIZE;
                    entry_placed = true;
                    break;
                 }
                 goto next_block; 
            }
            
            uint16_t actual_len_of_current_entry = calculate_dir_entry_len(dir_entry->name_len);

            // 尝试使用空条目
            if (dir_entry->inode == 0 && dir_entry->rec_len >= needed_len_for_new_entry) {
                uint16_t original_empty_rec_len = dir_entry->rec_len;
                
                dir_entry->inode = child_inode_num;
                dir_entry->name_len = static_cast<uint8_t>(entry_name.length());
                dir_entry->file_type = file_type;
                std::strncpy(dir_entry->name, entry_name.c_str(), entry_name.length());
                dir_entry->rec_len = needed_len_for_new_entry;

                uint16_t space_left_after_new_entry = original_empty_rec_len - needed_len_for_new_entry;

                if (space_left_after_new_entry > 0) {
                    if (space_left_after_new_entry < min_rec_len_for_empty) {
                        dir_entry->rec_len += space_left_after_new_entry;
                    } else {
                        SimpleFS_DirEntry* remainder_entry = reinterpret_cast<SimpleFS_DirEntry*>(dir_block_data_buffer.data() + current_offset + needed_len_for_new_entry);
                        remainder_entry->inode = 0;
                        remainder_entry->name_len = 0; 
                        remainder_entry->file_type = 0;
                        remainder_entry->rec_len = space_left_after_new_entry;
                    }
                }
                entry_placed = true;
                break;
            }

            // 尝试使用现有活动条目的空白填充
            uint16_t original_rec_len_of_active_entry = dir_entry->rec_len; 
            if (dir_entry->inode != 0 && (original_rec_len_of_active_entry - actual_len_of_current_entry >= needed_len_for_new_entry)) {
                uint16_t padding_available = original_rec_len_of_active_entry - actual_len_of_current_entry;
                
                dir_entry->rec_len = actual_len_of_current_entry; 

                SimpleFS_DirEntry* new_entry_spot = reinterpret_cast<SimpleFS_DirEntry*>(dir_block_data_buffer.data() + current_offset + actual_len_of_current_entry);
                new_entry_spot->inode = child_inode_num;
                new_entry_spot->name_len = static_cast<uint8_t>(entry_name.length());
                new_entry_spot->file_type = file_type;
                std::strncpy(new_entry_spot->name, entry_name.c_str(), entry_name.length());
                new_entry_spot->rec_len = needed_len_for_new_entry;

                uint16_t space_left_for_final_empty = padding_available - needed_len_for_new_entry;

                if (space_left_for_final_empty > 0) {
                    if (space_left_for_final_empty < min_rec_len_for_empty) {
                        new_entry_spot->rec_len += space_left_for_final_empty;
                    } else {
                        SimpleFS_DirEntry* final_empty_entry = reinterpret_cast<SimpleFS_DirEntry*>( (uint8_t*)new_entry_spot + needed_len_for_new_entry );
                        final_empty_entry->inode = 0;
                        final_empty_entry->name_len = 0;
                        final_empty_entry->file_type = 0; 
                        final_empty_entry->rec_len = space_left_for_final_empty;
                    }
                }
                entry_placed = true;
                break;
            }
            
            // 如果这是块中的最后一个条目
            if (current_offset + dir_entry->rec_len >= SIMPLEFS_BLOCK_SIZE) {
                if (dir_entry->inode != 0 && (current_offset + actual_len_of_current_entry + needed_len_for_new_entry <= SIMPLEFS_BLOCK_SIZE) ) {
                     dir_entry->rec_len = actual_len_of_current_entry; 

                     SimpleFS_DirEntry* new_entry_location = reinterpret_cast<SimpleFS_DirEntry*>(dir_block_data_buffer.data() + current_offset + actual_len_of_current_entry);
                     new_entry_location->inode = child_inode_num;
                     new_entry_location->name_len = static_cast<uint8_t>(entry_name.length());
                     new_entry_location->file_type = file_type;
                     std::strncpy(new_entry_location->name, entry_name.c_str(), entry_name.length());
                     new_entry_location->rec_len = SIMPLEFS_BLOCK_SIZE - (current_offset + actual_len_of_current_entry);
                     entry_placed = true;
                }
                break; 
            }
            current_offset += dir_entry->rec_len;
        } 

        if (entry_placed) {
            if (write_block(context.device_fd, current_physical_block, dir_block_data_buffer.data()) != 0) {
                errno = EIO; return -EIO;
            }
            
            // 更新父目录大小
            uint32_t size_if_this_block_is_last = (logical_block_idx + 1) * SIMPLEFS_BLOCK_SIZE;
            if (parent_inode->i_size < size_if_this_block_is_last) {
                 parent_inode->i_size = size_if_this_block_is_last;
            }

            parent_inode->i_mtime = parent_inode->i_ctime = time(nullptr);
            if (write_inode_to_disk(context, parent_inode_num, parent_inode) != 0) {
                 return -EIO; 
            }
            return 0; 
        }
        next_block:; 
    } 

    errno = ENOSPC;
    return -ENOSPC;
}

// 从目录中删除文件项
int remove_dir_entry(SimpleFS_Context& context, SimpleFS_Inode* parent_inode, uint32_t parent_inode_num, const std::string& entry_name_to_remove) {
    if (entry_name_to_remove.empty() || entry_name_to_remove == "." || entry_name_to_remove == "..") {
        errno = EINVAL;
        return -EINVAL;
    }

    std::vector<uint8_t> dir_block_data_buffer(SIMPLEFS_BLOCK_SIZE);
    bool entry_found_and_removed = false;

    uint32_t num_data_blocks_in_dir = (parent_inode->i_size + SIMPLEFS_BLOCK_SIZE - 1) / SIMPLEFS_BLOCK_SIZE;
    if (parent_inode->i_size == 0) num_data_blocks_in_dir = 0;

    for (uint32_t logical_block_idx = 0; logical_block_idx < num_data_blocks_in_dir; ++logical_block_idx) {
        uint32_t current_physical_block = map_logical_to_physical_block(context, parent_inode, logical_block_idx);
        
        if (current_physical_block == 0) {
            continue;
        }

        if (read_block(context.device_fd, current_physical_block, dir_block_data_buffer.data()) != 0) {
            errno = EIO;
            return -EIO;
        }

        uint16_t current_offset = 0;
        SimpleFS_DirEntry* prev_entry = nullptr;

        // 计算此块中的有效数据范围
        uint32_t block_start_byte_offset = logical_block_idx * SIMPLEFS_BLOCK_SIZE;
        uint32_t max_offset_in_block = SIMPLEFS_BLOCK_SIZE;
        if (block_start_byte_offset + SIMPLEFS_BLOCK_SIZE > parent_inode->i_size) {
             max_offset_in_block = parent_inode->i_size % SIMPLEFS_BLOCK_SIZE;
             if (max_offset_in_block == 0 && parent_inode->i_size > 0) max_offset_in_block = SIMPLEFS_BLOCK_SIZE;
        }

        while (current_offset < max_offset_in_block) {
            SimpleFS_DirEntry* current_entry = reinterpret_cast<SimpleFS_DirEntry*>(dir_block_data_buffer.data() + current_offset);

            if (current_entry->rec_len == 0) {
                break; 
            }
             uint16_t min_possible_rec_len = calculate_dir_entry_len(0);
             if (current_entry->inode != 0 && current_entry->name_len > 0) {
                min_possible_rec_len = calculate_dir_entry_len(current_entry->name_len);
             }

            if (current_entry->rec_len < min_possible_rec_len || (current_offset + current_entry->rec_len > SIMPLEFS_BLOCK_SIZE) ) {
                 errno = EIO;
                 return -EIO;
            }

            if (current_entry->inode != 0) {
                if (entry_name_to_remove.length() == current_entry->name_len &&
                    strncmp(current_entry->name, entry_name_to_remove.c_str(), current_entry->name_len) == 0) {
                    
                    // 找到要删除的条目
                    if (prev_entry != nullptr) {
                        // 将此条目的长度合并到前一个条目
                        prev_entry->rec_len += current_entry->rec_len;
                    } else {
                        // 这是块中的第一个条目，标记为未使用
                        current_entry->inode = 0;
                    }
                    entry_found_and_removed = true;
                    break;
                }
            }

            prev_entry = current_entry;
            current_offset += current_entry->rec_len;
        }

        if (entry_found_and_removed) {
            if (write_block(context.device_fd, current_physical_block, dir_block_data_buffer.data()) != 0) {
                errno = EIO;
                return -EIO;
            }
            
            parent_inode->i_mtime = parent_inode->i_ctime = time(nullptr);
            if (write_inode_to_disk(context, parent_inode_num, parent_inode) != 0) {
                 return -EIO; 
            }
            return 0;
        }
    }

    errno = ENOENT;
    return -ENOENT;
}


// 同步文件系统元数据到磁盘
void sync_fs_metadata(SimpleFS_Context& context) {
    // 写入超级块
    std::vector<uint8_t> sb_block_buffer(SIMPLEFS_BLOCK_SIZE, 0);
    std::memcpy(sb_block_buffer.data(), &(context.sb), sizeof(SimpleFS_SuperBlock));
    if (write_block(context.device_fd, 1, sb_block_buffer.data()) != 0) {
        return;
    }

    // 写入组描述符表(GDT)
    if (context.gdt.empty()) {
        return;
    }

    uint32_t gdt_size_bytes = context.gdt.size() * sizeof(SimpleFS_GroupDesc);
    uint32_t gdt_blocks_count = static_cast<uint32_t>(std::ceil(static_cast<double>(gdt_size_bytes) / SIMPLEFS_BLOCK_SIZE));
    uint32_t gdt_start_block = 1 + 1; // 超级块在块1，GDT从块2开始

    std::vector<uint8_t> gdt_one_block_buffer(SIMPLEFS_BLOCK_SIZE, 0);
    const uint8_t* gdt_data_ptr = reinterpret_cast<const uint8_t*>(context.gdt.data());

    for (uint32_t i = 0; i < gdt_blocks_count; ++i) {
        std::fill(gdt_one_block_buffer.begin(), gdt_one_block_buffer.end(), 0);

        uint32_t bytes_to_copy_this_block = SIMPLEFS_BLOCK_SIZE;
        uint32_t offset_in_gdt_data = i * SIMPLEFS_BLOCK_SIZE;

        if (offset_in_gdt_data >= gdt_size_bytes) break;

        if (offset_in_gdt_data + bytes_to_copy_this_block > gdt_size_bytes) {
            bytes_to_copy_this_block = gdt_size_bytes - offset_in_gdt_data;
        }

        if (bytes_to_copy_this_block > 0) {
             std::memcpy(gdt_one_block_buffer.data(), gdt_data_ptr + offset_in_gdt_data, bytes_to_copy_this_block);
        }

        write_block(context.device_fd, gdt_start_block + i, gdt_one_block_buffer.data());
    }

    // 写入备份副本
    uint32_t num_groups = context.gdt.size();
    for (uint32_t grp = 1; grp < num_groups; ++grp) {
        if (!is_backup_group(grp)) continue;
        uint32_t grp_start = grp * context.sb.s_blocks_per_group;
        write_block(context.device_fd, grp_start, sb_block_buffer.data());
        
        for (uint32_t i = 0; i < gdt_blocks_count; ++i) {
            std::fill(gdt_one_block_buffer.begin(), gdt_one_block_buffer.end(), 0);
            uint32_t bytes_to_copy_this_block = SIMPLEFS_BLOCK_SIZE;
            uint32_t offset_in_gdt_data = i * SIMPLEFS_BLOCK_SIZE;
            if (offset_in_gdt_data >= gdt_size_bytes) break;
            if (offset_in_gdt_data + bytes_to_copy_this_block > gdt_size_bytes) {
                bytes_to_copy_this_block = gdt_size_bytes - offset_in_gdt_data;
            }
            if (bytes_to_copy_this_block > 0) {
                std::memcpy(gdt_one_block_buffer.data(), gdt_data_ptr + offset_in_gdt_data, bytes_to_copy_this_block);
            }
            write_block(context.device_fd, grp_start + 1 + i, gdt_one_block_buffer.data());
        }
    }
}

// 检查文件访问权限
int check_access(const struct fuse_context* caller_context, const SimpleFS_Inode* inode, int requested_perm) {
    if (!caller_context || !inode) {
        return -EINVAL; 
    }

    // root用户拥有所有权限
    if (caller_context->uid == 0) {
        return 0;
    }

    uint16_t mode = inode->i_mode; 
    uint16_t perms_to_check = 0;

    if (caller_context->uid == inode->i_uid) { // 文件所有者
        perms_to_check = (mode & S_IRWXU) >> 6;
    } else { // 检查组权限
        bool in_group = false;
        if (caller_context->gid == inode->i_gid) {
            in_group = true;
        } else { // 检查附加组
            int num_supp_groups = getgroups(0, nullptr);
            if (num_supp_groups > 0) {
                std::vector<gid_t> supp_group_list(num_supp_groups);
                if (getgroups(num_supp_groups, supp_group_list.data()) >= 0) {
                    for (gid_t supp_gid : supp_group_list) {
                        if (supp_gid == inode->i_gid) {
                            in_group = true;
                            break;
                        }
                    }
                }
            }
        }

        if (in_group) {
            perms_to_check = (mode & S_IRWXG) >> 3;
        } else { // 其他用户
            perms_to_check = (mode & S_IRWXO);
        }
    }

    if ((requested_perm & R_OK) && !(perms_to_check & 0x4)) { 
        errno = EACCES;
        return -EACCES;
    }
    if ((requested_perm & W_OK) && !(perms_to_check & 0x2)) { 
        errno = EACCES;
        return -EACCES;
    }
    if ((requested_perm & X_OK) && !(perms_to_check & 0x1)) { 
        errno = EACCES;
        return -EACCES;
    }
    
    return 0;
}

// 递归释放块树结构
static void free_block_tree_recursive(SimpleFS_Context& context, uint32_t block_num, int level) {
    if (block_num == 0) {
        return;
    }

    if (level == 0) { // 数据块
        free_block(context, block_num);
        return;
    }

    // 间接块，需要读取并释放其子块
    std::vector<uint32_t> indirect_block_content(SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t));
    if (read_block(context.device_fd, block_num, indirect_block_content.data()) != 0) {
        return;
    }

    // 递归释放子块
    for (uint32_t child_block_num : indirect_block_content) {
        if (child_block_num != 0) {
            free_block_tree_recursive(context, child_block_num, level - 1);
        }
    }

    // 释放间接块本身
    free_block(context, block_num);
}

// 释放inode关联的所有数据块
void free_all_inode_blocks(SimpleFS_Context& context, SimpleFS_Inode* inode) {
    if (!inode) {
        return;
    }

    // 释放直接块
    for (int i = 0; i < SIMPLEFS_NUM_DIRECT_BLOCKS; ++i) {
        if (inode->i_block[i] != 0) {
            free_block(context, inode->i_block[i]);
        }
    }

    // 释放间接块结构
    free_block_tree_recursive(context, inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS], 1);     // 一级间接
    free_block_tree_recursive(context, inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 1], 2); // 二级间接
    free_block_tree_recursive(context, inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 2], 3); // 三级间接

    // 重置inode块指针
    std::memset(inode->i_block, 0, sizeof(uint32_t) * SIMPLEFS_INODE_BLOCK_PTRS);
    inode->i_blocks = 0;
}

// 获取或分配目录数据块
uint32_t get_or_alloc_dir_block(SimpleFS_Context& context, SimpleFS_Inode* dir_inode, uint32_t dir_inode_num, uint32_t logical_block_idx) {
    if (!dir_inode) { errno = EIO; return 0; }

    uint32_t preferred_group = (dir_inode_num - 1) / context.sb.s_inodes_per_group;
    uint32_t pointers_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t);
    std::vector<uint32_t> indirect_block_content(pointers_per_block);
    std::vector<uint8_t> zero_block_buffer(SIMPLEFS_BLOCK_SIZE, 0);

    // 直接块
    if (logical_block_idx < SIMPLEFS_NUM_DIRECT_BLOCKS) {
        if (dir_inode->i_block[logical_block_idx] == 0) {
            uint32_t new_data_block = alloc_block(context, preferred_group);
            if (new_data_block == 0) { return 0; }
            if (write_block(context.device_fd, new_data_block, zero_block_buffer.data()) != 0) {
                errno = EIO;
                return 0;
            }
            dir_inode->i_block[logical_block_idx] = new_data_block;
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
        }
        return dir_inode->i_block[logical_block_idx];
    }

    // 一级间接块
    uint32_t single_indirect_start_idx = SIMPLEFS_NUM_DIRECT_BLOCKS;
    uint32_t single_indirect_end_idx = single_indirect_start_idx + pointers_per_block;
    if (logical_block_idx < single_indirect_end_idx) {
        uint32_t* p_single_indirect_block_num = &dir_inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS];
        if (*p_single_indirect_block_num == 0) {
            uint32_t new_l1_block = alloc_block(context, preferred_group);
            if (new_l1_block == 0) { errno = ENOSPC; return 0; }
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_content.begin(), indirect_block_content.end(), 0);
            if (write_block(context.device_fd, new_l1_block, indirect_block_content.data()) != 0) {
                free_block(context, new_l1_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_single_indirect_block_num = new_l1_block;
        }
        if (read_block(context.device_fd, *p_single_indirect_block_num, indirect_block_content.data()) != 0) {
             errno = EIO; return 0;
        }

        uint32_t idx_in_indirect = logical_block_idx - single_indirect_start_idx;
        if (indirect_block_content[idx_in_indirect] == 0) {
            uint32_t new_data_block = alloc_block(context, preferred_group);
            if (new_data_block == 0) { errno = ENOSPC; return 0; }
            if (write_block(context.device_fd, new_data_block, zero_block_buffer.data()) != 0) {
                free_block(context, new_data_block);
                errno = EIO; return 0;
            }
            
            indirect_block_content[idx_in_indirect] = new_data_block;
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            if (write_block(context.device_fd, *p_single_indirect_block_num, indirect_block_content.data()) != 0) {
                free_block(context, new_data_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                indirect_block_content[idx_in_indirect] = 0;
                errno = EIO; return 0;
            }
        }
        return indirect_block_content[idx_in_indirect];
    }

    // 二级间接块
    uint32_t double_indirect_start_idx = single_indirect_end_idx;
    uint32_t double_indirect_max_logical_blocks = pointers_per_block * pointers_per_block;
    uint32_t double_indirect_end_idx = double_indirect_start_idx + double_indirect_max_logical_blocks;
    if (logical_block_idx < double_indirect_end_idx) {
        uint32_t* p_dbl_indirect_block_num = &dir_inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 1];
        if (*p_dbl_indirect_block_num == 0) {
            uint32_t new_l2_block = alloc_block(context, preferred_group);
            if (new_l2_block == 0) { errno = ENOSPC; return 0; }
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_content.begin(), indirect_block_content.end(), 0);
            if (write_block(context.device_fd, new_l2_block, indirect_block_content.data()) != 0) {
                free_block(context, new_l2_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_dbl_indirect_block_num = new_l2_block;
        }

        std::vector<uint32_t> l2_buffer(pointers_per_block);
        if (read_block(context.device_fd, *p_dbl_indirect_block_num, l2_buffer.data()) != 0) { errno = EIO; return 0; }

        uint32_t logical_offset_in_dbl_range = logical_block_idx - double_indirect_start_idx;
        uint32_t idx_in_l2_block = logical_offset_in_dbl_range / pointers_per_block;

        uint32_t* p_l1_block_num_from_l2 = &l2_buffer[idx_in_l2_block];
        if (*p_l1_block_num_from_l2 == 0) {
            uint32_t new_l1_block = alloc_block(context, preferred_group);
            if (new_l1_block == 0) { errno = ENOSPC; return 0;}
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_content.begin(), indirect_block_content.end(), 0);
            if (write_block(context.device_fd, new_l1_block, indirect_block_content.data()) != 0) {
                free_block(context, new_l1_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_l1_block_num_from_l2 = new_l1_block;
            if (write_block(context.device_fd, *p_dbl_indirect_block_num, l2_buffer.data()) != 0) {
                free_block(context, new_l1_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                *p_l1_block_num_from_l2 = 0;
                errno = EIO; return 0;
            }
        }
        
        std::vector<uint32_t> l1_buffer(pointers_per_block);
        if (read_block(context.device_fd, *p_l1_block_num_from_l2, l1_buffer.data()) != 0) { errno = EIO; return 0;}

        uint32_t idx_in_l1_block = logical_offset_in_dbl_range % pointers_per_block;
        if (l1_buffer[idx_in_l1_block] == 0) {
            uint32_t new_data_block = alloc_block(context, preferred_group);
            if (new_data_block == 0) { errno = ENOSPC; return 0; }
            if (write_block(context.device_fd, new_data_block, zero_block_buffer.data()) != 0) {
                free_block(context, new_data_block);
                errno = EIO; return 0;
            }
            
            l1_buffer[idx_in_l1_block] = new_data_block;
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            if (write_block(context.device_fd, *p_l1_block_num_from_l2, l1_buffer.data()) != 0) {
                free_block(context, new_data_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                l1_buffer[idx_in_l1_block] = 0;
                errno = EIO; return 0;
            }
        }
        return l1_buffer[idx_in_l1_block];
    }

    // 三级间接块
    uint32_t triple_indirect_start_idx = double_indirect_end_idx;
    uint64_t triple_indirect_max_logical_blocks = (uint64_t)pointers_per_block * pointers_per_block * pointers_per_block;
    uint64_t triple_indirect_end_idx = triple_indirect_start_idx + triple_indirect_max_logical_blocks;

    if (logical_block_idx < triple_indirect_end_idx) {
        uint32_t* p_tpl_indirect_block_num = &dir_inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 2];
        if (*p_tpl_indirect_block_num == 0) {
            uint32_t new_l3_block = alloc_block(context, preferred_group);
            if (new_l3_block == 0) { errno = ENOSPC; return 0; }
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_content.begin(), indirect_block_content.end(), 0);
            if (write_block(context.device_fd, new_l3_block, indirect_block_content.data()) != 0) {
                free_block(context, new_l3_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_tpl_indirect_block_num = new_l3_block;
        }

        std::vector<uint32_t> l3_buffer(pointers_per_block);
        if (read_block(context.device_fd, *p_tpl_indirect_block_num, l3_buffer.data()) != 0) { errno = EIO; return 0; }

        uint32_t logical_offset_in_tpl_range = logical_block_idx - triple_indirect_start_idx;
        uint32_t idx_in_l3_block = logical_offset_in_tpl_range / (pointers_per_block * pointers_per_block);

        uint32_t* p_l2_block_num_from_l3 = &l3_buffer[idx_in_l3_block];
        if (*p_l2_block_num_from_l3 == 0) {
            uint32_t new_l2_block = alloc_block(context, preferred_group);
            if (new_l2_block == 0) { errno = ENOSPC; return 0; }
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_content.begin(), indirect_block_content.end(), 0);
            if (write_block(context.device_fd, new_l2_block, indirect_block_content.data()) != 0) {
                free_block(context, new_l2_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_l2_block_num_from_l3 = new_l2_block;
            if (write_block(context.device_fd, *p_tpl_indirect_block_num, l3_buffer.data()) != 0) {
                free_block(context, new_l2_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                *p_l2_block_num_from_l3 = 0;
                errno = EIO; return 0;
            }
        }

        std::vector<uint32_t> l2_buffer(pointers_per_block);
        if (read_block(context.device_fd, *p_l2_block_num_from_l3, l2_buffer.data()) != 0) { errno = EIO; return 0; }

        uint32_t logical_offset_in_l2_from_tpl = logical_offset_in_tpl_range % (pointers_per_block * pointers_per_block);
        uint32_t idx_in_l2_from_tpl = logical_offset_in_l2_from_tpl / pointers_per_block;

        uint32_t* p_l1_block_num_from_l2 = &l2_buffer[idx_in_l2_from_tpl];
        if (*p_l1_block_num_from_l2 == 0) {
            uint32_t new_l1_block = alloc_block(context, preferred_group);
            if (new_l1_block == 0) { errno = ENOSPC; return 0;}
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_content.begin(), indirect_block_content.end(), 0);
            if (write_block(context.device_fd, new_l1_block, indirect_block_content.data()) != 0) {
                free_block(context, new_l1_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_l1_block_num_from_l2 = new_l1_block;
            if (write_block(context.device_fd, *p_l2_block_num_from_l3, l2_buffer.data()) != 0) {
                free_block(context, new_l1_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                *p_l1_block_num_from_l2 = 0;
                errno = EIO; return 0;
            }
        }

        std::vector<uint32_t> l1_buffer(pointers_per_block);
        if (read_block(context.device_fd, *p_l1_block_num_from_l2, l1_buffer.data()) != 0) { errno = EIO; return 0; }

        uint32_t idx_in_l1_final = logical_offset_in_l2_from_tpl % pointers_per_block;
        if (l1_buffer[idx_in_l1_final] == 0) {
            uint32_t new_data_block = alloc_block(context, preferred_group);
            if (new_data_block == 0) { errno = ENOSPC; return 0; }
            if (write_block(context.device_fd, new_data_block, zero_block_buffer.data()) != 0) {
                free_block(context, new_data_block);
                errno = EIO; return 0;
            }
            
            l1_buffer[idx_in_l1_final] = new_data_block;
            dir_inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            if (write_block(context.device_fd, *p_l1_block_num_from_l2, l1_buffer.data()) != 0) {
                free_block(context, new_data_block);
                dir_inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                l1_buffer[idx_in_l1_final] = 0;
                errno = EIO; return 0;
            }
        }
        return l1_buffer[idx_in_l1_final];
    }

    errno = EFBIG;
    return 0;
}



// 映射逻辑块到物理块（不分配）
uint32_t map_logical_to_physical_block(SimpleFS_Context& context, const SimpleFS_Inode* inode, uint32_t logical_block_idx) {
    if (!inode) { errno = EINVAL; return 0; }

    if (logical_block_idx < SIMPLEFS_NUM_DIRECT_BLOCKS) {
        return inode->i_block[logical_block_idx];
    }

    uint32_t pointers_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t);
    std::vector<uint32_t> indirect_block_buffer(pointers_per_block);

    uint32_t single_indirect_start_idx = SIMPLEFS_NUM_DIRECT_BLOCKS;
    uint32_t single_indirect_end_idx = single_indirect_start_idx + pointers_per_block;
    if (logical_block_idx < single_indirect_end_idx) {
        uint32_t single_indirect_block_num = inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS];
        if (single_indirect_block_num == 0) {errno = 0; return 0;}

        if (read_block(context.device_fd, single_indirect_block_num, indirect_block_buffer.data()) != 0) {
            errno = EIO;
            return 0;
        }
        uint32_t idx_in_indirect = logical_block_idx - single_indirect_start_idx;
        return indirect_block_buffer[idx_in_indirect];
    }

    uint32_t double_indirect_start_idx = single_indirect_end_idx;
    uint32_t double_indirect_max_entries = pointers_per_block * pointers_per_block;
    uint32_t double_indirect_end_idx = double_indirect_start_idx + double_indirect_max_entries;
    if (logical_block_idx < double_indirect_end_idx) {
        uint32_t dbl_indirect_block_num = inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 1];
        if (dbl_indirect_block_num == 0) {errno=0; return 0;}

        if (read_block(context.device_fd, dbl_indirect_block_num, indirect_block_buffer.data()) != 0) {
             errno = EIO; return 0;
        }
        uint32_t logical_offset_in_dbl = logical_block_idx - double_indirect_start_idx;
        uint32_t idx_in_dbl_indirect_block = logical_offset_in_dbl / pointers_per_block;
        uint32_t single_indirect_block_to_read = indirect_block_buffer[idx_in_dbl_indirect_block];
        if (single_indirect_block_to_read == 0) {errno=0; return 0;}

        if (read_block(context.device_fd, single_indirect_block_to_read, indirect_block_buffer.data()) != 0) {
            errno = EIO; return 0;
        }
        uint32_t idx_in_single_indirect_block = logical_offset_in_dbl % pointers_per_block;
        return indirect_block_buffer[idx_in_single_indirect_block];
    }

    uint32_t triple_indirect_start_idx = double_indirect_end_idx;
    uint64_t triple_indirect_max_logical_blocks = (uint64_t)pointers_per_block * pointers_per_block * pointers_per_block;
    uint64_t triple_indirect_end_idx = triple_indirect_start_idx + triple_indirect_max_logical_blocks;

    if (logical_block_idx < triple_indirect_end_idx) {
        uint32_t tpl_indirect_block_num = inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 2];
        if (tpl_indirect_block_num == 0) { errno = 0; return 0; }

        if (read_block(context.device_fd, tpl_indirect_block_num, indirect_block_buffer.data()) != 0) {
            errno = EIO; return 0;
        }

        uint32_t logical_offset_in_tpl = logical_block_idx - triple_indirect_start_idx;
        uint32_t idx_in_tpl_indirect_block = logical_offset_in_tpl / (pointers_per_block * pointers_per_block);
        uint32_t dbl_indirect_block_to_read = indirect_block_buffer[idx_in_tpl_indirect_block];
        if (dbl_indirect_block_to_read == 0) { errno = 0; return 0; }

        if (read_block(context.device_fd, dbl_indirect_block_to_read, indirect_block_buffer.data()) != 0) {
            errno = EIO; return 0;
        }

        uint32_t logical_offset_in_dbl_from_tpl = logical_offset_in_tpl % (pointers_per_block * pointers_per_block);
        uint32_t idx_in_dbl_from_tpl = logical_offset_in_dbl_from_tpl / pointers_per_block;
        uint32_t sgl_indirect_block_to_read = indirect_block_buffer[idx_in_dbl_from_tpl];
        if (sgl_indirect_block_to_read == 0) { errno = 0; return 0; }

        if (read_block(context.device_fd, sgl_indirect_block_to_read, indirect_block_buffer.data()) != 0) {
            errno = EIO; return 0;
        }
        uint32_t idx_in_sgl_final = logical_offset_in_dbl_from_tpl % pointers_per_block;
        return indirect_block_buffer[idx_in_sgl_final];
    }

    errno = EFBIG;
    return 0;
}

// 释放逻辑块范围
void release_logical_block_range(SimpleFS_Context& context, SimpleFS_Inode* inode, uint32_t start_lbn, uint32_t end_lbn) {
    if (start_lbn >= end_lbn || !inode) {
        return;
    }

    for (uint32_t lbn_to_free = start_lbn; lbn_to_free < end_lbn; ++lbn_to_free) {
        uint32_t physical_block = map_logical_to_physical_block(context, inode, lbn_to_free);
        if (physical_block != 0) {
            free_block(context, physical_block);
        }
    }

    // 简单重新计算块计数
    inode->i_blocks = (inode->i_size + 511) / 512;
}
