#pragma once

#include "simplefs.h"
#include "simplefs_context.h"

#include <cstdint>
#include <vector>
#include <string>

// inode管理
uint32_t alloc_inode(SimpleFS_Context& context, mode_t mode);
void free_inode(SimpleFS_Context& context, uint32_t inode_num, mode_t mode_of_freed_inode);

// 块管理
uint32_t alloc_block(SimpleFS_Context& context, uint32_t preferred_group_for_inode = static_cast<uint32_t>(-1));
void free_block(SimpleFS_Context& context, uint32_t block_num);

// inode读写
int write_inode_to_disk(SimpleFS_Context& context, uint32_t inode_num, const SimpleFS_Inode* inode_data);
int read_inode_from_disk(SimpleFS_Context& context, uint32_t inode_num, SimpleFS_Inode* inode_struct);

// 目录操作
int add_dir_entry(SimpleFS_Context& context, SimpleFS_Inode* parent_inode, uint32_t parent_inode_num,
                  const std::string& entry_name, uint32_t child_inode_num, uint8_t file_type);
int remove_dir_entry(SimpleFS_Context& context, SimpleFS_Inode* parent_inode, uint32_t parent_inode_num, const std::string& entry_name_to_remove);

// 路径解析
void parse_path(const std::string& path, std::string& dirname, std::string& basename);

// 位图操作
void set_bitmap_bit(std::vector<uint8_t>& bitmap_data, uint32_t bit_index);
void clear_bitmap_bit(std::vector<uint8_t>& bitmap_data, uint32_t bit_index);
bool is_bitmap_bit_set(const std::vector<uint8_t>& bitmap_data, uint32_t bit_index);

// 元数据同步
void sync_fs_metadata(SimpleFS_Context& context);

// 权限检查
struct fuse_context;
int check_access(const struct fuse_context* caller_context, const SimpleFS_Inode* inode, int requested_perm);

// 块释放
void free_all_inode_blocks(SimpleFS_Context& context, SimpleFS_Inode* inode);

// 块映射
uint32_t get_or_alloc_dir_block(SimpleFS_Context& context, SimpleFS_Inode* dir_inode, uint32_t dir_inode_num, uint32_t logical_block_idx);
uint32_t map_logical_to_physical_block(SimpleFS_Context& context, const SimpleFS_Inode* inode, uint32_t logical_block_idx);
void release_logical_block_range(SimpleFS_Context& context, SimpleFS_Inode* inode, uint32_t start_lbn, uint32_t end_lbn);
