#include "fuse_ops.h"
#include "disk_io.h"
#include "metadata.h"
#include "utils.h"    // 路径解析和目录条目计算

#include <iostream>
#include <cstring>
#include <cerrno>
#include <sys/stat.h> // S_ISDIR, S_IFMT, mode_t, S_ISUID, S_ISGID
#include <sys/statvfs.h> // struct statvfs
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
#include <unistd.h> // uid_t, gid_t, getgroups
#include <time.h>   // time_t, time(), timespec
#include <vector>   // std::vector
#include <cstdio>   // perror
#include <dirent.h> // DT_REG, DT_DIR, DT_LNK

// fuse_common.h (由fuse.h包含) 定义FUSE_SYMLINK_MAX
// 如果由于某种原因不可用，定义一个回退值
#ifndef FUSE_SYMLINK_MAX
#define FUSE_SYMLINK_MAX 40 // 符号链接最大遍历深度
#endif


// 从FUSE获取文件系统上下文的辅助函数
SimpleFS_Context* get_fs_context() {
    return static_cast<SimpleFS_Context*>(fuse_get_context()->private_data);
}

// 前向声明
static uint32_t resolve_path_recursive(const char* path_cstr, int depth, bool follow_last_symlink);
static uint32_t path_to_inode_num(const char* path_cstr); // resolve_path_recursive的包装器

// static uint32_t map_logical_to_physical_block(SimpleFS_Context& context, const SimpleFS_Inode* inode, uint32_t logical_block_idx); // 已移至metadata.h/cpp
static uint32_t allocate_block_for_write(SimpleFS_Context& context, SimpleFS_Inode* inode, uint32_t inode_num, uint32_t logical_block_idx, bool* p_was_newly_allocated);


// 给定路径，返回inode号
// 这现在是resolve_path_recursive的包装器，用于应该跟随所有符号链接的外部调用
static uint32_t path_to_inode_num(const char* path_cstr) {
    return resolve_path_recursive(path_cstr, 0, true); // 默认：跟随最后的符号链接
}

// 递归路径解析函数
static uint32_t resolve_path_recursive(const char* path_cstr, int depth, bool follow_last_symlink) {
    if (depth > FUSE_SYMLINK_MAX) {
        errno = ELOOP;
        return 0;
    }

    SimpleFS_Context* context = get_fs_context();
    if (!context) {
        errno = EACCES;
        return 0;
    }

    std::string path(path_cstr);
    if (path.empty()) {
        errno = EINVAL;
        return 0;
    }

    uint32_t current_inode_num;
    std::vector<std::string> components;

    if (path == "/") {
        return context->sb.s_root_inode;
    }

    std::string path_to_tokenize = (path[0] == '/') ? path.substr(1) : path;

    size_t start = 0;
    size_t end = 0;
    while ((end = path_to_tokenize.find('/', start)) != std::string::npos) {
        if (end != start) {
            components.push_back(path_to_tokenize.substr(start, end - start));
        }
        start = end + 1;
    }
    if (start < path_to_tokenize.length()) {
        components.push_back(path_to_tokenize.substr(start));
    }

    if (components.empty() && path[0] == '/') { // 应由 path == "/" 处理
         return context->sb.s_root_inode;
    }
    if (components.empty()) { // 空路径或相对路径错误
        errno = EINVAL; // 无效相对路径
        return 0;
    }

    current_inode_num = context->sb.s_root_inode;

    std::string current_path_for_symlink_resolution = "/";

    for (size_t component_idx = 0; component_idx < components.size(); ++component_idx) {
        const auto& component = components[component_idx];
        bool is_last_component = (component_idx == components.size() - 1);

        if (component.empty() || component == ".") {
            continue;
        }

        SimpleFS_Inode current_dir_inode_data;
        if (read_inode_from_disk(*context, current_inode_num, &current_dir_inode_data) != 0) {
            return 0;
        }

        if (!S_ISDIR(current_dir_inode_data.i_mode)) {
            errno = ENOTDIR;
            return 0;
        }

        int access_res = check_access(fuse_get_context(), &current_dir_inode_data, X_OK);
        if (access_res != 0) {
           errno = -access_res;
           return 0;
        }

        bool found_component_in_dir = false;
        uint32_t next_inode_num_candidate = 0;
        // SimpleFS_Inode component_inode_data; // 解析组件(文件/目录/符号链接)的数据

        std::vector<uint8_t> dir_data_block_buffer(SIMPLEFS_BLOCK_SIZE);

        // 遍历直接、一级、二级、三级间接块查找目录条目

        // 搜索直接块
        for (int i = 0; i < SIMPLEFS_NUM_DIRECT_BLOCKS && !found_component_in_dir; ++i) {
            uint32_t dir_block_ptr = current_dir_inode_data.i_block[i];
            if (dir_block_ptr == 0) continue;
            if (read_block(context->device_fd, dir_block_ptr, dir_data_block_buffer.data()) != 0) { errno = EIO; return 0; }
            uint16_t entry_offset = 0;
            uint32_t block_start_offset_in_file = i * SIMPLEFS_BLOCK_SIZE;
            uint32_t effective_size_in_this_block = (current_dir_inode_data.i_size > block_start_offset_in_file) ?
                                                std::min((uint32_t)SIMPLEFS_BLOCK_SIZE, current_dir_inode_data.i_size - block_start_offset_in_file) : 0;
            if (effective_size_in_this_block == 0 && i > 0 && current_dir_inode_data.i_size <= block_start_offset_in_file) continue;

            while (entry_offset < effective_size_in_this_block) {
                SimpleFS_DirEntry* entry = reinterpret_cast<SimpleFS_DirEntry*>(dir_data_block_buffer.data() + entry_offset);
                if (entry->rec_len == 0 || (calculate_dir_entry_len(entry->name_len) > entry->rec_len) || (entry_offset + entry->rec_len > effective_size_in_this_block)) break;
                if (entry->inode != 0 && entry->name_len == component.length() && strncmp(entry->name, component.c_str(), entry->name_len) == 0) {
                    next_inode_num_candidate = entry->inode;
                    found_component_in_dir = true;
                    break;
                }
                entry_offset += entry->rec_len;
            }
        }
        // 搜索一级间接块
        if (!found_component_in_dir) {
            uint32_t single_indirect_block_ptr = current_dir_inode_data.i_block[SIMPLEFS_NUM_DIRECT_BLOCKS];
            if (single_indirect_block_ptr != 0) {
                std::vector<uint32_t> indirect_block_content(SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t));
                if (read_block(context->device_fd, single_indirect_block_ptr, indirect_block_content.data()) == 0) {
                    for (uint32_t k = 0; k < (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) && !found_component_in_dir; ++k) {
                        uint32_t data_block_ptr = indirect_block_content[k];
                        if (data_block_ptr == 0) continue;
                        if (read_block(context->device_fd, data_block_ptr, dir_data_block_buffer.data()) != 0) { errno = EIO; return 0; }
                        uint16_t entry_offset = 0;
                        uint32_t logical_block_idx = SIMPLEFS_NUM_DIRECT_BLOCKS + k;
                        uint32_t block_start_offset_in_file = logical_block_idx * SIMPLEFS_BLOCK_SIZE;
                        uint32_t effective_size_in_this_block = (current_dir_inode_data.i_size > block_start_offset_in_file) ?
                                                            std::min((uint32_t)SIMPLEFS_BLOCK_SIZE, current_dir_inode_data.i_size - block_start_offset_in_file) : 0;
                        if(effective_size_in_this_block == 0 && current_dir_inode_data.i_size <= block_start_offset_in_file) continue;
                        while (entry_offset < effective_size_in_this_block) {
                            SimpleFS_DirEntry* entry = reinterpret_cast<SimpleFS_DirEntry*>(dir_data_block_buffer.data() + entry_offset);
                            if (entry->rec_len == 0 || (calculate_dir_entry_len(entry->name_len) > entry->rec_len) || (entry_offset + entry->rec_len > effective_size_in_this_block) ) break;
                            if (entry->inode != 0 && entry->name_len == component.length() && strncmp(entry->name, component.c_str(), entry->name_len) == 0) {
                                next_inode_num_candidate = entry->inode; found_component_in_dir = true; break;
                            }
                            entry_offset += entry->rec_len;
                        }
                    }
                }
            }
        }
        // 搜索二级间接块
        if (!found_component_in_dir) {
            uint32_t dbl_indirect_block_ptr = current_dir_inode_data.i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 1];
            if (dbl_indirect_block_ptr != 0) {
                std::vector<uint32_t> dbl_indirect_content(SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t));
                if (read_block(context->device_fd, dbl_indirect_block_ptr, dbl_indirect_content.data()) == 0) {
                    for (uint32_t l1_idx = 0; l1_idx < (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) && !found_component_in_dir; ++l1_idx) {
                        uint32_t single_indirect_ptr = dbl_indirect_content[l1_idx];
                        if (single_indirect_ptr == 0) continue;
                        std::vector<uint32_t> single_indirect_content(SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t));
                        if (read_block(context->device_fd, single_indirect_ptr, single_indirect_content.data()) == 0) {
                            for (uint32_t l2_idx = 0; l2_idx < (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) && !found_component_in_dir; ++l2_idx) {
                                uint32_t data_block_ptr = single_indirect_content[l2_idx];
                                if (data_block_ptr == 0) continue;
                                if (read_block(context->device_fd, data_block_ptr, dir_data_block_buffer.data()) != 0) { errno = EIO; return 0; }
                                uint16_t entry_offset = 0;
                                uint32_t logical_block_idx = SIMPLEFS_NUM_DIRECT_BLOCKS + (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) + (l1_idx * (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t))) + l2_idx; // 取消注释
                                uint32_t block_start_offset_in_file = logical_block_idx * SIMPLEFS_BLOCK_SIZE;
                                uint32_t effective_size_in_this_block = (current_dir_inode_data.i_size > block_start_offset_in_file ) ?
                                                                    std::min((uint32_t)SIMPLEFS_BLOCK_SIZE, (uint32_t)(current_dir_inode_data.i_size - block_start_offset_in_file) ) : 0;
                                if(effective_size_in_this_block == 0 && current_dir_inode_data.i_size <= block_start_offset_in_file) continue;
                                while (entry_offset < effective_size_in_this_block) {
                                   SimpleFS_DirEntry* entry = reinterpret_cast<SimpleFS_DirEntry*>(dir_data_block_buffer.data() + entry_offset);
                                   if (entry->rec_len == 0 || (calculate_dir_entry_len(entry->name_len) > entry->rec_len) || (entry_offset + entry->rec_len > effective_size_in_this_block) ) break;
                                   if (entry->inode != 0 && entry->name_len == component.length() && strncmp(entry->name, component.c_str(), entry->name_len) == 0) {
                                       next_inode_num_candidate = entry->inode; found_component_in_dir = true; break;
                                   }
                                   entry_offset += entry->rec_len;
                                }
                            }
                        }
                    }
                }
            }
        }
        // 搜索三级间接块
        if (!found_component_in_dir) {
            uint32_t tpl_indirect_block_ptr = current_dir_inode_data.i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 2];
            if (tpl_indirect_block_ptr != 0) {
                std::vector<uint32_t> tpl_indirect_content(SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t));
                if (read_block(context->device_fd, tpl_indirect_block_ptr, tpl_indirect_content.data()) == 0) {
                    for (uint32_t l0_idx = 0; l0_idx < (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) && !found_component_in_dir; ++l0_idx) {
                        uint32_t dbl_indirect_ptr_from_tpl = tpl_indirect_content[l0_idx];
                        if (dbl_indirect_ptr_from_tpl == 0) continue;
                        std::vector<uint32_t> dbl_indirect_content_via_tpl(SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t));
                        if (read_block(context->device_fd, dbl_indirect_ptr_from_tpl, dbl_indirect_content_via_tpl.data()) == 0) {
                            for (uint32_t l1_idx = 0; l1_idx < (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) && !found_component_in_dir; ++l1_idx) {
                                uint32_t sgl_indirect_ptr_from_dbl = dbl_indirect_content_via_tpl[l1_idx];
                                if (sgl_indirect_ptr_from_dbl == 0) continue;
                                std::vector<uint32_t> sgl_indirect_content_via_dbl_tpl(SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t));
                                if (read_block(context->device_fd, sgl_indirect_ptr_from_dbl, sgl_indirect_content_via_dbl_tpl.data()) == 0) {
                                    for (uint32_t l2_idx = 0; l2_idx < (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) && !found_component_in_dir; ++l2_idx) {
                                        uint32_t data_block_ptr = sgl_indirect_content_via_dbl_tpl[l2_idx];
                                        if (data_block_ptr == 0) continue;
                                        if (read_block(context->device_fd, data_block_ptr, dir_data_block_buffer.data()) != 0) { errno = EIO; return 0; }
                                        uint16_t entry_offset = 0;
                                        uint32_t tpl_logical_block_idx = SIMPLEFS_NUM_DIRECT_BLOCKS + (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) + ((SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) * (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t))) + (l0_idx * (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t)) * (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t))) + (l1_idx * (SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t))) + l2_idx;
                                        uint32_t tpl_block_start_offset_in_file = tpl_logical_block_idx * SIMPLEFS_BLOCK_SIZE;
                                        uint32_t effective_size_in_this_block = (current_dir_inode_data.i_size > tpl_block_start_offset_in_file) ?
                                                                            std::min((uint32_t)SIMPLEFS_BLOCK_SIZE, current_dir_inode_data.i_size - tpl_block_start_offset_in_file) : 0;
                                        if(effective_size_in_this_block == 0 && current_dir_inode_data.i_size <= tpl_block_start_offset_in_file) continue;
                                        while (entry_offset < effective_size_in_this_block) {
                                            SimpleFS_DirEntry* entry = reinterpret_cast<SimpleFS_DirEntry*>(dir_data_block_buffer.data() + entry_offset);
                                            if (entry->rec_len == 0 || (calculate_dir_entry_len(entry->name_len) > entry->rec_len) || (entry_offset + entry->rec_len > effective_size_in_this_block) ) break;
                                            if (entry->inode != 0 && entry->name_len == component.length() && strncmp(entry->name, component.c_str(), entry->name_len) == 0) {
                                                next_inode_num_candidate = entry->inode; found_component_in_dir = true; break;
                                            }
                                            entry_offset += entry->rec_len;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!found_component_in_dir) {
            errno = ENOENT;
            return 0;
        }

        SimpleFS_Inode component_inode_data; // 解析组件的数据
        if (read_inode_from_disk(*context, next_inode_num_candidate, &component_inode_data) != 0) {
             std::cerr << "无法读取inode " << next_inode_num_candidate
                       << " 组件: \"" << component << "\"" << std::endl;
            return 0;
        }

        std::string component_path_part = component;
        if (component == "..") {
            if (current_path_for_symlink_resolution.length() > 1) { // 非根目录
                size_t last_slash = current_path_for_symlink_resolution.find_last_of('/');
                 if (last_slash == 0) current_path_for_symlink_resolution = "/"; // 父目录是根目录
                 else if (last_slash != std::string::npos) current_path_for_symlink_resolution.erase(last_slash);
            }
        } else if (component != ".") {
            if (current_path_for_symlink_resolution.length() > 1) current_path_for_symlink_resolution += "/";
            current_path_for_symlink_resolution += component;
        }

        if (S_ISLNK(component_inode_data.i_mode)) {
            if (is_last_component && !follow_last_symlink) {
                return next_inode_num_candidate;
            }

            char target_path_buf[SIMPLEFS_BLOCK_SIZE];
            std::memset(target_path_buf, 0, sizeof(target_path_buf));

            std::string target_path_str;
            if (component_inode_data.i_blocks == 0) {
                // 快速符号链接：数据存储在i_block数组中
                if (component_inode_data.i_size > sizeof(component_inode_data.i_block)) {
                    std::cerr << "快速符号链接大小超出存储空间" << std::endl;
                    errno = EIO; return 0;
                }
                std::memcpy(target_path_buf, component_inode_data.i_block, component_inode_data.i_size);
                target_path_buf[component_inode_data.i_size] = '\0';
                target_path_str = target_path_buf;
            } else {
                // 传统符号链接存储在块中
                if (component_inode_data.i_size == 0 || component_inode_data.i_block[0] == 0) {
                    std::cerr << "符号链接inode " << next_inode_num_candidate << " 大小为零或无数据块" << std::endl;
                    errno = EIO; return 0;
                }
                if (read_block(context->device_fd, component_inode_data.i_block[0], target_path_buf) != 0) {
                    std::cerr << "无法读取inode " << next_inode_num_candidate << " 的符号链接目标数据块" << std::endl;
                    errno = EIO; return 0;
                }
                target_path_buf[std::min((size_t)SIMPLEFS_BLOCK_SIZE -1, (size_t)component_inode_data.i_size)] = '\0';
                target_path_str = target_path_buf;
            }

            std::string next_resolve_path_str;
            if (target_path_str[0] == '/') {
                next_resolve_path_str = target_path_str;
            } else {
                std::string base_dir = current_path_for_symlink_resolution;
                // 从current_path_for_symlink_resolution中删除当前组件以获取链接的目录
                if (base_dir.length() > component_path_part.length()) {
                    size_t pos = base_dir.rfind(component_path_part);
                    if (pos != std::string::npos && pos > 0 && base_dir[pos-1] == '/') {
                         base_dir.erase(pos-1); // 移除组件和尾随斜杠
                    } else if (pos == 0 && base_dir == "/" + component_path_part) { // 如 /link
                        base_dir = "/";
                    }
                }
                if (base_dir.empty()) base_dir = "/"; // 逻辑错误时不应发生

                if (base_dir.length() > 1 && base_dir.back() == '/') {
                    next_resolve_path_str = base_dir + target_path_str;
                } else if (base_dir == "/") {
                     next_resolve_path_str = "/" + target_path_str;
                } else {
                    next_resolve_path_str = base_dir + "/" + target_path_str;
                }
            }

            std::string remaining_path_components;
            if (!is_last_component) {
                for (size_t k = component_idx + 1; k < components.size(); ++k) {
                    remaining_path_components += "/";
                    remaining_path_components += components[k];
                }
            }
            if (!remaining_path_components.empty()) {
                 if (next_resolve_path_str.back() == '/') next_resolve_path_str.pop_back();
                 next_resolve_path_str += remaining_path_components;
            }
            return resolve_path_recursive(next_resolve_path_str.c_str(), depth + 1, follow_last_symlink);
        }
        current_inode_num = next_inode_num_candidate;
        if (is_last_component) {
            return current_inode_num;
        }
    }
    return current_inode_num;
}

// 获取文件属性
int simplefs_getattr(const char *path, struct stat *stbuf) {
    std::memset(stbuf, 0, sizeof(struct stat));
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t inode_num = resolve_path_recursive(path, 0, false);
    if (inode_num == 0) return -errno;
    SimpleFS_Inode inode;
    if (read_inode_from_disk(*context, inode_num, &inode) != 0) return -errno;
    stbuf->st_ino = inode_num;
    stbuf->st_mode = inode.i_mode;
    stbuf->st_nlink = inode.i_links_count;
    stbuf->st_uid = inode.i_uid;
    stbuf->st_gid = inode.i_gid;
    stbuf->st_size = inode.i_size;
    stbuf->st_blocks = inode.i_blocks;
    stbuf->st_atime = inode.i_atime;
    stbuf->st_mtime = inode.i_mtime;
    stbuf->st_ctime = inode.i_ctime;
    return 0;
}

// 读取目录内容
int simplefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t offset, struct fuse_file_info *fi) {
    (void) offset; (void) fi;
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t dir_inode_num = path_to_inode_num(path);
    if (dir_inode_num == 0) return -errno;
    SimpleFS_Inode dir_inode;
    if (read_inode_from_disk(*context, dir_inode_num, &dir_inode) != 0) return -errno;
    if (!S_ISDIR(dir_inode.i_mode)) return -ENOTDIR;

    std::vector<uint8_t> block_buffer(SIMPLEFS_BLOCK_SIZE);
    uint32_t total_bytes_iterated = 0;

    auto process_dir_block = [&](uint32_t data_block_num) {
        if (read_block(context->device_fd, data_block_num, block_buffer.data()) != 0) return -EIO;
        uint16_t entry_offset = 0;
        uint32_t current_block_bytes_processed = 0;

        while(total_bytes_iterated < dir_inode.i_size && current_block_bytes_processed < SIMPLEFS_BLOCK_SIZE) {
            SimpleFS_DirEntry* entry = reinterpret_cast<SimpleFS_DirEntry*>(block_buffer.data() + entry_offset);
            if (entry->rec_len == 0 || (calculate_dir_entry_len(entry->name_len) > entry->rec_len) || (entry_offset + entry->rec_len > SIMPLEFS_BLOCK_SIZE) ) {
                 break;
            }
            if (entry->inode != 0 && entry->name_len > 0) {
                std::string filename(entry->name, entry->name_len);
                struct stat st_entry; std::memset(&st_entry, 0, sizeof(struct stat));
                st_entry.st_ino = entry->inode;

                SimpleFS_Inode temp_inode_for_type;
                if(read_inode_from_disk(*context, entry->inode, &temp_inode_for_type) == 0) {
                    st_entry.st_mode = temp_inode_for_type.i_mode; // 获取完整模式
                } else {
                    st_entry.st_mode = (entry->file_type << 12); // 从目录条目获取类型
                }

                if (filler(buf, filename.c_str(), &st_entry, 0) != 0) return -ENOMEM;
            }
            entry_offset += entry->rec_len;
            current_block_bytes_processed += entry->rec_len;
            total_bytes_iterated += entry->rec_len;
            if(entry_offset >= SIMPLEFS_BLOCK_SIZE) break;
        }
        return 0;
    };

    // 遍历所有块级别
    uint32_t lbn = 0;
    while(total_bytes_iterated < dir_inode.i_size) {
        uint32_t physical_block = map_logical_to_physical_block(*context, &dir_inode, lbn);
        if (physical_block == 0) { // 目录i_size正确时不应发生
            if (total_bytes_iterated >= dir_inode.i_size) break; // 到达末尾
            lbn++; continue; // 目录中的稀疏块或错误
        }
        int res = process_dir_block(physical_block);
        if (res != 0) return res;
        if (total_bytes_iterated >= dir_inode.i_size) break; // 基于i_size确保循环终止
        lbn++;
        if (lbn > (dir_inode.i_size / SIMPLEFS_BLOCK_SIZE) + SIMPLEFS_INODE_BLOCK_PTRS*1024*1024) { // 极端情况安全退出
             std::cerr << "目录 " << dir_inode_num << " LBN过大，可能出现无限循环" << std::endl;
             return -EIO;
        }
    }
    return 0;
}

// FUSE操作函数
int simplefs_getattr(const char *path, struct stat *stbuf);
int simplefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
int simplefs_mknod(const char *path, mode_t mode, dev_t rdev);
int simplefs_mkdir(const char *path, mode_t mode);
int simplefs_unlink(const char *path);
int simplefs_rmdir(const char *path);
int simplefs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int simplefs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int simplefs_truncate(const char *path, off_t size);
int simplefs_chmod(const char *path, mode_t mode);
int simplefs_chown(const char *path, uid_t uid, gid_t gid);
int simplefs_utimens(const char *path, const struct timespec tv[2]);
int simplefs_statfs(const char *path, struct statvfs *stbuf);
int simplefs_access(const char *path, int mask);
int simplefs_symlink(const char *target, const char *linkpath);
int simplefs_readlink(const char *path, char *buf, size_t size);
int simplefs_link(const char *oldpath, const char *newpath);


// 初始化fuse_operations结构体
void init_fuse_operations(struct fuse_operations *ops) {
    std::memset(ops, 0, sizeof(struct fuse_operations));
    ops->getattr = simplefs_getattr;
    ops->readdir = simplefs_readdir;
    ops->mknod   = simplefs_mknod;
    ops->mkdir   = simplefs_mkdir;
    ops->unlink  = simplefs_unlink;
    ops->rmdir   = simplefs_rmdir;
    ops->read    = simplefs_read;
    ops->write   = simplefs_write;
    ops->truncate = simplefs_truncate;
    ops->chmod   = simplefs_chmod;
    ops->chown   = simplefs_chown;
    ops->utimens = simplefs_utimens;
    ops->statfs  = simplefs_statfs;
    ops->access = simplefs_access;
    ops->symlink = simplefs_symlink;
    ops->readlink = simplefs_readlink;
    ops->link = simplefs_link;
}

// 文件系统统计
int simplefs_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    std::memset(stbuf, 0, sizeof(struct statvfs));
    stbuf->f_bsize   = SIMPLEFS_BLOCK_SIZE;
    stbuf->f_frsize  = SIMPLEFS_BLOCK_SIZE;
    stbuf->f_blocks  = context->sb.s_blocks_count;
    stbuf->f_bfree   = context->sb.s_free_blocks_count;
    stbuf->f_bavail  = context->sb.s_free_blocks_count;
    stbuf->f_files   = context->sb.s_inodes_count;
    stbuf->f_ffree   = context->sb.s_free_inodes_count;
    stbuf->f_favail  = context->sb.s_free_inodes_count;
    stbuf->f_fsid    = 0;
    stbuf->f_flag    = 0;
    stbuf->f_namemax = SIMPLEFS_MAX_FILENAME_LEN;
    return 0;
}

// 检查文件访问权限
int simplefs_access(const char *path, int mask) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t inode_num = path_to_inode_num(path); // 跟随符号链接进行访问检查
    if (inode_num == 0) return -errno;
    SimpleFS_Inode inode_data;
    if (read_inode_from_disk(*context, inode_num, &inode_data) != 0) return -errno;
    return check_access(fuse_get_context(), &inode_data, mask);
}

// 确保为给定逻辑块索引分配物理块的辅助函数
static uint32_t allocate_block_for_write(SimpleFS_Context& context, SimpleFS_Inode* inode, uint32_t inode_num ,
                                         uint32_t logical_block_idx, bool* p_was_newly_allocated) {
    if(p_was_newly_allocated) *p_was_newly_allocated = false;
    if (!inode) { errno = EIO; return 0; }
    uint32_t preferred_group = (inode_num -1) / context.sb.s_inodes_per_group;
    uint32_t pointers_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(uint32_t);
    std::vector<uint32_t> indirect_block_buffer(pointers_per_block);

    if (logical_block_idx < SIMPLEFS_NUM_DIRECT_BLOCKS) {
        if (inode->i_block[logical_block_idx] == 0) {
            uint32_t new_physical_block = alloc_block(context, preferred_group);
            if (new_physical_block == 0) { return 0; }
            inode->i_block[logical_block_idx] = new_physical_block;
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            if(p_was_newly_allocated) *p_was_newly_allocated = true;
        }
        return inode->i_block[logical_block_idx];
    }

    uint32_t single_indirect_start_idx = SIMPLEFS_NUM_DIRECT_BLOCKS;
    uint32_t single_indirect_end_idx = single_indirect_start_idx + pointers_per_block;
    if (logical_block_idx < single_indirect_end_idx) {
        uint32_t* p_single_indirect_block_num = &inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS];
        if (*p_single_indirect_block_num == 0) {
            uint32_t new_l1_block = alloc_block(context, preferred_group);
            if (new_l1_block == 0) { errno = ENOSPC; return 0; }
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_buffer.begin(), indirect_block_buffer.end(), 0);
            if (write_block(context.device_fd, new_l1_block, indirect_block_buffer.data()) != 0) {
                free_block(context, new_l1_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_single_indirect_block_num = new_l1_block;
        }
        if (read_block(context.device_fd, *p_single_indirect_block_num, indirect_block_buffer.data()) != 0) {
             errno = EIO; return 0;
        }
        uint32_t idx_in_indirect = logical_block_idx - single_indirect_start_idx;
        if (indirect_block_buffer[idx_in_indirect] == 0) {
            uint32_t new_data_block = alloc_block(context, preferred_group);
            if (new_data_block == 0) { errno = ENOSPC; return 0; }
            indirect_block_buffer[idx_in_indirect] = new_data_block;
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            if(p_was_newly_allocated) *p_was_newly_allocated = true;
            if (write_block(context.device_fd, *p_single_indirect_block_num, indirect_block_buffer.data()) != 0) {
                free_block(context, new_data_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                indirect_block_buffer[idx_in_indirect] = 0;
                errno = EIO; return 0;
            }
        }
        return indirect_block_buffer[idx_in_indirect];
    }

    uint32_t double_indirect_start_idx = single_indirect_end_idx;
    uint32_t double_indirect_max_logical_blocks = pointers_per_block * pointers_per_block;
    uint32_t double_indirect_end_idx = double_indirect_start_idx + double_indirect_max_logical_blocks;
    if (logical_block_idx < double_indirect_end_idx) {
        uint32_t* p_dbl_indirect_block_num = &inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 1];
        if (*p_dbl_indirect_block_num == 0) {
            uint32_t new_l2_block = alloc_block(context, preferred_group);
            if (new_l2_block == 0) { errno = ENOSPC; return 0; }
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_buffer.begin(), indirect_block_buffer.end(), 0);
            if (write_block(context.device_fd, new_l2_block, indirect_block_buffer.data()) != 0) {
                free_block(context, new_l2_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
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
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_buffer.begin(), indirect_block_buffer.end(), 0);
            if (write_block(context.device_fd, new_l1_block, indirect_block_buffer.data()) != 0) {
                free_block(context, new_l1_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_l1_block_num_from_l2 = new_l1_block;
            if (write_block(context.device_fd, *p_dbl_indirect_block_num, l2_buffer.data()) != 0) {
                free_block(context, new_l1_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
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
            l1_buffer[idx_in_l1_block] = new_data_block;
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            if(p_was_newly_allocated) *p_was_newly_allocated = true;
            if (write_block(context.device_fd, *p_l1_block_num_from_l2, l1_buffer.data()) != 0) {
                free_block(context, new_data_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                l1_buffer[idx_in_l1_block] = 0;
                errno = EIO; return 0;
            }
        }
        return l1_buffer[idx_in_l1_block];
    }

    uint32_t triple_indirect_start_idx = double_indirect_end_idx;
    uint64_t triple_indirect_max_logical_blocks = (uint64_t)pointers_per_block * pointers_per_block * pointers_per_block;
    uint64_t triple_indirect_end_idx = triple_indirect_start_idx + triple_indirect_max_logical_blocks;
    if (logical_block_idx < triple_indirect_end_idx) {
        uint32_t* p_tpl_indirect_block_num = &inode->i_block[SIMPLEFS_NUM_DIRECT_BLOCKS + 2];
        if (*p_tpl_indirect_block_num == 0) {
            uint32_t new_l3_block = alloc_block(context, preferred_group);
            if (new_l3_block == 0) { errno = ENOSPC; return 0; }
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_buffer.begin(), indirect_block_buffer.end(), 0);
            if (write_block(context.device_fd, new_l3_block, indirect_block_buffer.data()) != 0) {
                free_block(context, new_l3_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
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
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_buffer.begin(), indirect_block_buffer.end(), 0);
            if (write_block(context.device_fd, new_l2_block, indirect_block_buffer.data()) != 0) {
                free_block(context, new_l2_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_l2_block_num_from_l3 = new_l2_block;
            if (write_block(context.device_fd, *p_tpl_indirect_block_num, l3_buffer.data()) != 0) {
                free_block(context, new_l2_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
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
            if (new_l1_block == 0) { errno = ENOSPC; return 0; }
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            std::fill(indirect_block_buffer.begin(), indirect_block_buffer.end(), 0);
            if (write_block(context.device_fd, new_l1_block, indirect_block_buffer.data()) != 0) {
                free_block(context, new_l1_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                errno = EIO; return 0;
            }
            *p_l1_block_num_from_l2 = new_l1_block;
            if (write_block(context.device_fd, *p_l2_block_num_from_l3, l2_buffer.data()) != 0) {
                free_block(context, new_l1_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
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
            l1_buffer[idx_in_l1_final] = new_data_block;
            inode->i_blocks += (SIMPLEFS_BLOCK_SIZE / 512);
            if(p_was_newly_allocated) *p_was_newly_allocated = true;
            if (write_block(context.device_fd, *p_l1_block_num_from_l2, l1_buffer.data()) != 0) {
                free_block(context, new_data_block);
                inode->i_blocks -= (SIMPLEFS_BLOCK_SIZE / 512);
                l1_buffer[idx_in_l1_final] = 0;
                errno = EIO; return 0;
            }
        }
        return l1_buffer[idx_in_l1_final];
    }

    std::cerr << "写入时分配块失败: 逻辑块 " << logical_block_idx
              << " is beyond implemented allocation support (direct + single + double + triple indirect)." << std::endl;
    errno = EFBIG;
    return 0;
}

// 创建文件节点
int simplefs_mknod(const char *path, mode_t mode, dev_t rdev) {
    (void)rdev;
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    if (!S_ISREG(mode) && !S_ISFIFO(mode)) { // 也允许FIFO
        // 本项目只计划支持S_IFREG，符号链接是分开的
        // 如果严格只要S_IFREG:
        if(!S_ISREG(mode)) {
             std::cerr << "mknod: Unsupported file type in mode: 0x" << std::hex << mode << std::dec << ". Only regular files allowed." << std::endl;
             return -EPERM;
        }
    }

    std::string path_str(path);
    std::string dirname_str, basename_str;
    parse_path(path_str, dirname_str, basename_str);

    if (basename_str.empty() || basename_str == "." || basename_str == ".." || basename_str == "/")
        return -EINVAL;
    if (basename_str.length() > SIMPLEFS_MAX_FILENAME_LEN) return -ENAMETOOLONG;

    errno = 0;
    uint32_t parent_inode_num = path_to_inode_num(dirname_str.c_str());
    if (parent_inode_num == 0) return -errno;
    SimpleFS_Inode parent_inode_data;
    if (read_inode_from_disk(*context, parent_inode_num, &parent_inode_data) != 0) return -errno;
    if (!S_ISDIR(parent_inode_data.i_mode)) return -ENOTDIR;

    int access_res = check_access(fuse_get_context(), &parent_inode_data, W_OK | X_OK);
    if (access_res != 0) return access_res;

    errno = 0;
    uint32_t existing_inode_check = resolve_path_recursive(path_str.c_str(), 0, false); // 检查不跟随最后链接
    if (existing_inode_check != 0) return -EEXIST;
    if (errno != 0 && errno != ENOENT) return -errno;

    uint32_t new_inode_num = alloc_inode(*context, mode);
    if (new_inode_num == 0) return -errno;

    SimpleFS_Inode new_inode;
    std::memset(&new_inode, 0, sizeof(SimpleFS_Inode));
    new_inode.i_mode = mode; // 包含S_IFREG和权限设置
    new_inode.i_uid = fuse_get_context()->uid;
    new_inode.i_gid = fuse_get_context()->gid;
    new_inode.i_links_count = 1;
    new_inode.i_size = 0;
    new_inode.i_atime = new_inode.i_mtime = new_inode.i_ctime = time(nullptr);
    new_inode.i_blocks = 0;

    if (write_inode_to_disk(*context, new_inode_num, &new_inode) != 0) {
        free_inode(*context, new_inode_num, new_inode.i_mode);
        return -errno;
    }

    uint8_t dentry_file_type = 0;
    if (S_ISREG(mode)) dentry_file_type = DT_REG; // DT_REG是8
    else if (S_ISDIR(mode)) dentry_file_type = DT_DIR; // DT_DIR是4
    else if (S_ISLNK(mode)) dentry_file_type = DT_LNK; // DT_LNK是10
    // else if (S_ISFIFO(mode)) dentry_file_type = DT_FIFO; // DT_FIFO是1，需要确保simplefs.h中有这些定义
    // 现在，如果DT_xxx常量未定义/使用不一致，使用S_IFMT >> 12技巧
    // DirEntry file_type通常从S_IFMT位派生
    dentry_file_type = (mode & S_IFMT) >> 12;


    int add_entry_res = add_dir_entry(*context, &parent_inode_data, parent_inode_num, basename_str, new_inode_num, dentry_file_type);
    if (add_entry_res != 0) {
        // 回滚：释放inode（新文件还未分配块）
        new_inode.i_dtime = time(nullptr);
        new_inode.i_links_count = 0;
        write_inode_to_disk(*context, new_inode_num, &new_inode);
        free_inode(*context, new_inode_num, new_inode.i_mode);
        return add_entry_res;
    }
    sync_fs_metadata(*context);
    return 0;
}

// 创建目录
int simplefs_mkdir(const char *path, mode_t mode) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    std::string path_str(path);
    std::string dirname_str, basename_str;
    parse_path(path_str, dirname_str, basename_str);

    if (basename_str.empty() || basename_str == "." || basename_str == ".." || basename_str == "/")
        return -EINVAL;
    if (basename_str.length() > SIMPLEFS_MAX_FILENAME_LEN) return -ENAMETOOLONG;

    errno = 0;
    uint32_t parent_inode_num = path_to_inode_num(dirname_str.c_str());
    if (parent_inode_num == 0) return -errno;
    SimpleFS_Inode parent_inode_data;
    if (read_inode_from_disk(*context, parent_inode_num, &parent_inode_data) != 0) return -errno;
    if (!S_ISDIR(parent_inode_data.i_mode)) return -ENOTDIR;

    int access_res = check_access(fuse_get_context(), &parent_inode_data, W_OK | X_OK);
    if (access_res != 0) return access_res;

    errno = 0;
    uint32_t existing_inode_check = resolve_path_recursive(path_str.c_str(), 0, false);
    if (existing_inode_check != 0) return -EEXIST;
    if (errno != 0 && errno != ENOENT) return -errno;

    uint32_t new_dir_inode_num = alloc_inode(*context, S_IFDIR | (mode & 07777));
    if (new_dir_inode_num == 0) return -errno;

    uint32_t new_dir_data_block = alloc_block(*context, (new_dir_inode_num -1) / context->sb.s_inodes_per_group);
    if (new_dir_data_block == 0) {
        free_inode(*context, new_dir_inode_num, S_IFDIR | (mode & 07777));
        return -errno;
    }

    SimpleFS_Inode new_dir_inode;
    std::memset(&new_dir_inode, 0, sizeof(SimpleFS_Inode));
    new_dir_inode.i_mode = S_IFDIR | (mode & 07777); // 应用模式权限
    new_dir_inode.i_uid = fuse_get_context()->uid;
    new_dir_inode.i_gid = fuse_get_context()->gid;
    new_dir_inode.i_links_count = 2; // 用于'.'和父目录中的条目
    new_dir_inode.i_size = 0; // 将由add_dir_entry为"."和".."设置
    new_dir_inode.i_blocks = 0; // 计算"."和".."数据块时设置
    new_dir_inode.i_atime = new_dir_inode.i_mtime = new_dir_inode.i_ctime = time(nullptr);
    // new_dir_inode.i_block[0] = new_dir_data_block; // 块准备后设置

    // 准备包含"."和".."的目录块
    std::vector<uint8_t> dir_block_buffer(SIMPLEFS_BLOCK_SIZE, 0);
    uint16_t current_offset = 0;
    uint8_t dot_file_type = (S_IFDIR & S_IFMT) >> 12;

    SimpleFS_DirEntry dot_entry;
    dot_entry.inode = new_dir_inode_num;
    dot_entry.name_len = 1;
    dot_entry.file_type = dot_file_type;
    std::strncpy(dot_entry.name, ".", 1);
    dot_entry.rec_len = calculate_dir_entry_len(dot_entry.name_len);
    std::memcpy(dir_block_buffer.data() + current_offset, &dot_entry, (size_t)calculate_dir_entry_len(0) + dot_entry.name_len);
    current_offset += dot_entry.rec_len;

    SimpleFS_DirEntry dotdot_entry;
    dotdot_entry.inode = parent_inode_num;
    dotdot_entry.name_len = 2;
    dotdot_entry.file_type = dot_file_type;
    std::strncpy(dotdot_entry.name, "..", 2);
    dotdot_entry.rec_len = SIMPLEFS_BLOCK_SIZE - current_offset; // 使".."填充剩余空间
    std::memcpy(dir_block_buffer.data() + current_offset, &dotdot_entry, (size_t)calculate_dir_entry_len(0) + dotdot_entry.name_len);
    current_offset += dotdot_entry.rec_len;

    if (write_block(context->device_fd, new_dir_data_block, dir_block_buffer.data()) != 0) {
        free_block(*context, new_dir_data_block);
        free_inode(*context, new_dir_inode_num, new_dir_inode.i_mode);
        return -EIO;
    }

    new_dir_inode.i_block[0] = new_dir_data_block;
    new_dir_inode.i_blocks = SIMPLEFS_BLOCK_SIZE / 512;
    new_dir_inode.i_size = current_offset; // "."和".."使用的实际大小

    if (write_inode_to_disk(*context, new_dir_inode_num, &new_dir_inode) != 0) {
        // 回滚数据块
        std::fill(dir_block_buffer.begin(), dir_block_buffer.end(), 0);
        write_block(context->device_fd, new_dir_data_block, dir_block_buffer.data()); // 尝试清理
        free_block(*context, new_dir_data_block);
        // 回滚inode
        new_dir_inode.i_dtime = time(nullptr); new_dir_inode.i_links_count = 0;
        write_inode_to_disk(*context, new_dir_inode_num, &new_dir_inode);
        free_inode(*context, new_dir_inode_num, new_dir_inode.i_mode);
        return -errno;
    }

    uint8_t dentry_file_type_for_parent = (S_IFDIR & S_IFMT) >> 12;
    int add_entry_res_parent = add_dir_entry(*context, &parent_inode_data, parent_inode_num, basename_str, new_dir_inode_num, dentry_file_type_for_parent);

    if (add_entry_res_parent != 0) {
        // 完全回滚
        std::fill(dir_block_buffer.begin(), dir_block_buffer.end(), 0);
        write_block(context->device_fd, new_dir_data_block, dir_block_buffer.data());
        free_block(*context, new_dir_data_block);
        new_dir_inode.i_dtime = time(nullptr); new_dir_inode.i_links_count = 0;
        write_inode_to_disk(*context, new_dir_inode_num, &new_dir_inode);
        free_inode(*context, new_dir_inode_num, S_IFDIR | (mode & 07777)); // 使用原始模式计算目录数
        return add_entry_res_parent;
    }

    // 如果add_dir_entry为父目录分配新块，父目录的链接数会被增加
    // 但是，mkdir传统上增加父目录的链接数，因为新的".."条目指向它
    // 我们的add_dir_entry不处理parent_inode.i_links_count
    // 对于mkdir，父目录的i_links_count应该增加1
    parent_inode_data.i_links_count++;
    parent_inode_data.i_mtime = parent_inode_data.i_ctime = time(nullptr);
    if(write_inode_to_disk(*context, parent_inode_num, &parent_inode_data) != 0) {
        // 完全回滚有些复杂，现在记录并继续
        std::cerr << "mkdir: 父目录链接数更新失败，文件系统可能不一致" << std::endl;
    }

    sync_fs_metadata(*context);
    return 0;
}

// 删除文件
int simplefs_unlink(const char *path) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;

    std::string path_str(path);
    std::string dirname_str, basename_str;
    parse_path(path_str, dirname_str, basename_str);

    if (basename_str.empty() || basename_str == "." || basename_str == ".." || basename_str == "/") {
        return -EINVAL;
    }

    errno = 0;
    uint32_t parent_inode_num = path_to_inode_num(dirname_str.c_str());
    if (parent_inode_num == 0) {
        return -errno;
    }
    SimpleFS_Inode parent_inode_data;
    if (read_inode_from_disk(*context, parent_inode_num, &parent_inode_data) != 0) {
        return -errno;
    }
    if (!S_ISDIR(parent_inode_data.i_mode)) {
        return -ENOTDIR;
    }

    // 权限检查：父目录的W_OK和X_OK
    int access_res = check_access(fuse_get_context(), &parent_inode_data, W_OK | X_OK);
    if (access_res != 0) {
        return access_res;
    }

    errno = 0;
    // unlink操作的是文件/符号链接本身，不是其目标
    uint32_t target_inode_num = resolve_path_recursive(path_str.c_str(), 0, false);
    if (target_inode_num == 0) {
        return -errno;
    }

    SimpleFS_Inode target_inode_data;
    if (read_inode_from_disk(*context, target_inode_num, &target_inode_data) != 0) {
        return -errno;
    }

    // 粘滞位(S_ISVTX)检查删除权限
    if ((parent_inode_data.i_mode & S_ISVTX)) {
        if (!S_ISDIR(target_inode_data.i_mode)) {
            struct fuse_context *caller_ctx = fuse_get_context();
            if (caller_ctx->uid != 0 &&
                caller_ctx->uid != parent_inode_data.i_uid &&
                caller_ctx->uid != target_inode_data.i_uid) {
                std::cout << "父目录 " << parent_inode_num
                          << " 对项目 " << target_inode_num << " 的粘滞位权限拒绝" << std::endl;
                return -EACCES;
            }
        }
    }

    // 现在检查目标是否为目录（unlink无法移除）
    if (S_ISDIR(target_inode_data.i_mode)) {
        return -EISDIR;
    }

    int remove_res = remove_dir_entry(*context, &parent_inode_data, parent_inode_num, basename_str);
    if (remove_res != 0) {
        return remove_res;
    }

    target_inode_data.i_links_count--;
    target_inode_data.i_ctime = time(nullptr);

    if (target_inode_data.i_links_count == 0) {
        
        // 重要：仅在非快速符号链接时释放块
        // 快速符号链接i_blocks==0且数据存储在i_block数组中
        if (!(S_ISLNK(target_inode_data.i_mode) && target_inode_data.i_blocks == 0)) {
            free_all_inode_blocks(*context, &target_inode_data);
        }

        target_inode_data.i_size = 0;
        target_inode_data.i_dtime = time(nullptr);

        if (write_inode_to_disk(*context, target_inode_num, &target_inode_data) != 0) {
            std::cerr << "unlink: 释放前目标inode " << target_inode_num << " 写入失败，继续释放inode" << std::endl;
        }
        free_inode(*context, target_inode_num, target_inode_data.i_mode);
    } else {
        if (write_inode_to_disk(*context, target_inode_num, &target_inode_data) != 0) {
            std::cerr << "unlink: 目标inode " << target_inode_num << " 链接数更新失败" << std::endl;
            return -EIO;
        }
    }
    sync_fs_metadata(*context);
    return 0;
}

// 删除目录
int simplefs_rmdir(const char *path) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    std::string path_str(path);
    std::string dirname_str, basename_str;
    parse_path(path_str, dirname_str, basename_str);

    if (basename_str.empty() || basename_str == "." || basename_str == ".." || basename_str == "/")
        return -EINVAL;

    errno = 0;
    uint32_t parent_inode_num = path_to_inode_num(dirname_str.c_str());
    if (parent_inode_num == 0) return -errno;
    SimpleFS_Inode parent_inode_data;
    if (read_inode_from_disk(*context, parent_inode_num, &parent_inode_data) != 0) return -errno;
    if (!S_ISDIR(parent_inode_data.i_mode)) return -ENOTDIR;

    int access_res = check_access(fuse_get_context(), &parent_inode_data, W_OK | X_OK);
    if (access_res != 0) return access_res;

    errno = 0;
    uint32_t target_inode_num = resolve_path_recursive(path_str.c_str(), 0, false);
    if (target_inode_num == 0) return -errno;
    SimpleFS_Inode target_inode_data;
    if (read_inode_from_disk(*context, target_inode_num, &target_inode_data) != 0) return -errno;

    if ((parent_inode_data.i_mode & S_ISVTX)) {
        struct fuse_context *caller_ctx = fuse_get_context();
        if (caller_ctx->uid != 0 &&
            caller_ctx->uid != parent_inode_data.i_uid &&
            caller_ctx->uid != target_inode_data.i_uid) {
            return -EACCES;
        }
    }

    if (!S_ISDIR(target_inode_data.i_mode)) return -ENOTDIR;

    bool is_empty = true;
    if (target_inode_data.i_size > 0) { // 检查目录是否为空（只包含"."和".."）
        std::vector<uint8_t> block_buffer(SIMPLEFS_BLOCK_SIZE);
        uint32_t total_dir_bytes_iterated = 0;
        int non_dot_entries = 0;

        uint32_t dir_lbn = 0;
        while(total_dir_bytes_iterated < target_inode_data.i_size && non_dot_entries == 0) {
            uint32_t physical_block = map_logical_to_physical_block(*context, &target_inode_data, dir_lbn);
            if (physical_block == 0) { // 目录通常应该是连续的
                if (total_dir_bytes_iterated >= target_inode_data.i_size) break;
                dir_lbn++; continue;
            }
            if (read_block(context->device_fd, physical_block, block_buffer.data()) != 0) return -EIO;

            uint16_t entry_offset = 0;
            uint32_t current_block_dir_bytes_processed = 0;
            while(total_dir_bytes_iterated < target_inode_data.i_size && current_block_dir_bytes_processed < SIMPLEFS_BLOCK_SIZE) {
                SimpleFS_DirEntry* entry = reinterpret_cast<SimpleFS_DirEntry*>(block_buffer.data() + entry_offset);
                if (entry->rec_len == 0 || (calculate_dir_entry_len(entry->name_len) > entry->rec_len) || (entry_offset + entry->rec_len > SIMPLEFS_BLOCK_SIZE)) break;
                if (entry->inode != 0 && entry->name_len > 0) {
                    std::string entry_s_name(entry->name, entry->name_len);
                    if (entry_s_name != "." && entry_s_name != "..") {
                        non_dot_entries++;
                        break;
                    }
                }
                entry_offset += entry->rec_len;
                current_block_dir_bytes_processed += entry->rec_len;
                total_dir_bytes_iterated += entry->rec_len;
                if(entry_offset >= SIMPLEFS_BLOCK_SIZE) break;
            }
            if (non_dot_entries > 0) break;
            dir_lbn++;
             if (dir_lbn > (target_inode_data.i_size / SIMPLEFS_BLOCK_SIZE) + SIMPLEFS_INODE_BLOCK_PTRS*1024*1024) {
                 std::cerr << "rmdir: Excessive LBN for dir " << target_inode_num << std::endl; return -EIO;
             }
        }
        if (non_dot_entries > 0) is_empty = false;
    }
    if (!is_empty) return -ENOTEMPTY;

    int remove_res = remove_dir_entry(*context, &parent_inode_data, parent_inode_num, basename_str);
    if (remove_res != 0) return remove_res;

    // 减少父目录链接数，因为被删除目录的".."不再指向它
    parent_inode_data.i_links_count--;
    parent_inode_data.i_mtime = parent_inode_data.i_ctime = time(nullptr);
    if (write_inode_to_disk(*context, parent_inode_num, &parent_inode_data) != 0) { /* Log error, but proceed */ }

    // 释放被删除目录的块（应该只有一个包含"."和".."的块）
    free_all_inode_blocks(*context, &target_inode_data);

    target_inode_data.i_links_count = 0; // 父目录链接消失，"."消失
    target_inode_data.i_dtime = time(nullptr);
    target_inode_data.i_size = 0;
    // i_blocks由free_all_inode_blocks处理

    mode_t mode_of_target = target_inode_data.i_mode;
    if(write_inode_to_disk(*context, target_inode_num, &target_inode_data) !=0) {
        std::cerr << "rmdir: 释放前目标inode " << target_inode_num << " 写入失败" << std::endl;
    }
    free_inode(*context, target_inode_num, mode_of_target); // mode_of_target用于bg_used_dirs_count
    sync_fs_metadata(*context);
    return 0;
}

// 从打开的文件读取数据
int simplefs_read(const char *path, char *buf, size_t size, off_t offset,
                  struct fuse_file_info *fi) {
    (void)fi;
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t inode_num = path_to_inode_num(path);
    if (inode_num == 0) return -errno;
    SimpleFS_Inode inode_data;
    if (read_inode_from_disk(*context, inode_num, &inode_data) != 0) return -errno;
    if (S_ISDIR(inode_data.i_mode)) return -EISDIR;
    int access_res = check_access(fuse_get_context(), &inode_data, R_OK);
    if (access_res != 0) return access_res;

    if (offset >= (off_t)inode_data.i_size) return 0;
    if (offset + size > inode_data.i_size) {
        size = inode_data.i_size - offset;
    }

    size_t total_bytes_read = 0;
    std::vector<uint8_t> block_buffer(SIMPLEFS_BLOCK_SIZE);
    while (total_bytes_read < size) {
        uint32_t current_offset_in_file = offset + total_bytes_read;
        uint32_t logical_block_idx = current_offset_in_file / SIMPLEFS_BLOCK_SIZE;
        uint32_t offset_in_block = current_offset_in_file % SIMPLEFS_BLOCK_SIZE;
        uint32_t physical_block_num = map_logical_to_physical_block(*context, &inode_data, logical_block_idx);
        if (physical_block_num == 0) {
            size_t bytes_to_zero_in_this_block = SIMPLEFS_BLOCK_SIZE - offset_in_block;
            if (bytes_to_zero_in_this_block > (size - total_bytes_read)) {
                bytes_to_zero_in_this_block = size - total_bytes_read;
            }
            std::memset(buf + total_bytes_read, 0, bytes_to_zero_in_this_block);
            total_bytes_read += bytes_to_zero_in_this_block;
            if (errno != 0 && errno != ENOENT) {
                 if (total_bytes_read > 0) break;
                 return -errno;
            }
            errno = 0;
            continue;
        }
        if (read_block(context->device_fd, physical_block_num, block_buffer.data()) != 0) {
            if (total_bytes_read > 0) break;
            return -EIO;
        }
        size_t bytes_to_read_from_this_block = SIMPLEFS_BLOCK_SIZE - offset_in_block;
        if (bytes_to_read_from_this_block > (size - total_bytes_read)) {
            bytes_to_read_from_this_block = size - total_bytes_read;
        }
        std::memcpy(buf + total_bytes_read, block_buffer.data() + offset_in_block, bytes_to_read_from_this_block);
        total_bytes_read += bytes_to_read_from_this_block;
    }
    inode_data.i_atime = time(nullptr);
    if (write_inode_to_disk(*context, inode_num, &inode_data) != 0) {
        std::cerr << "read: inode " << inode_num << " atime更新失败" << std::endl;
    }
    return total_bytes_read;
}

// 向打开的文件写入数据
int simplefs_write(const char *path, const char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi) {
    (void)fi;
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t inode_num = path_to_inode_num(path);
    if (inode_num == 0) return -errno;
    SimpleFS_Inode inode_data;
    if (read_inode_from_disk(*context, inode_num, &inode_data) != 0) return -errno;
    if (S_ISDIR(inode_data.i_mode)) return -EISDIR;
    int access_res = check_access(fuse_get_context(), &inode_data, W_OK);
    if (access_res != 0) return access_res;

    size_t total_bytes_written = 0;
    std::vector<uint8_t> block_rw_buffer(SIMPLEFS_BLOCK_SIZE);
    while (total_bytes_written < size) {
        uint32_t current_offset_in_file = offset + total_bytes_written;
        uint32_t logical_block_idx = current_offset_in_file / SIMPLEFS_BLOCK_SIZE;
        uint32_t offset_in_block = current_offset_in_file % SIMPLEFS_BLOCK_SIZE;
        bool block_was_newly_allocated = false;
        uint32_t physical_block_num = allocate_block_for_write(*context, &inode_data, inode_num, logical_block_idx, &block_was_newly_allocated);
        if (physical_block_num == 0) {
            if (total_bytes_written > 0) break;
            return -errno;
        }
        size_t bytes_to_write_in_this_block = SIMPLEFS_BLOCK_SIZE - offset_in_block;
        if (bytes_to_write_in_this_block > (size - total_bytes_written)) {
            bytes_to_write_in_this_block = size - total_bytes_written;
        }
        bool is_partial_block_overwrite = (offset_in_block != 0 || bytes_to_write_in_this_block < SIMPLEFS_BLOCK_SIZE);
        if (is_partial_block_overwrite && !block_was_newly_allocated) {
            if (read_block(context->device_fd, physical_block_num, block_rw_buffer.data()) != 0) {
                 if (total_bytes_written > 0) break;
                 return -EIO;
            }
        } else {
            if (is_partial_block_overwrite && block_was_newly_allocated)
                 std::fill(block_rw_buffer.begin(), block_rw_buffer.end(), 0);
        }
        std::memcpy(block_rw_buffer.data() + offset_in_block, buf + total_bytes_written, bytes_to_write_in_this_block);
        if (write_block(context->device_fd, physical_block_num, block_rw_buffer.data()) != 0) {
            if (total_bytes_written > 0) break;
            return -EIO;
        }
        total_bytes_written += bytes_to_write_in_this_block;
    }
    if ((offset + total_bytes_written) > inode_data.i_size) {
        inode_data.i_size = offset + total_bytes_written;
    }
    inode_data.i_mtime = inode_data.i_ctime = time(nullptr);
    if (write_inode_to_disk(*context, inode_num, &inode_data) != 0) {
        if (total_bytes_written == 0 && size > 0) return -EIO;
    }
    sync_fs_metadata(*context);
    return total_bytes_written;
}

// 更改文件大小
int simplefs_truncate(const char *path, off_t size) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t inode_num = path_to_inode_num(path); // 跟随符号链接
    if (inode_num == 0) return -errno;
    SimpleFS_Inode inode_data;
    if (read_inode_from_disk(*context, inode_num, &inode_data) != 0) return -errno;
    if (S_ISDIR(inode_data.i_mode)) return -EISDIR;
    int access_res = check_access(fuse_get_context(), &inode_data, W_OK);
    if (access_res != 0) return access_res;

    if (inode_data.i_size == (uint32_t)size) {
        inode_data.i_ctime = time(nullptr);
        if (write_inode_to_disk(*context, inode_num, &inode_data) != 0) return -errno;
        return 0;
    }
    uint32_t old_size = inode_data.i_size;
    inode_data.i_size = size;
    if (size == 0) {
        free_all_inode_blocks(*context, &inode_data);
    } else if (size < old_size) {
        uint32_t old_num_fs_blocks = (old_size + SIMPLEFS_BLOCK_SIZE - 1) / SIMPLEFS_BLOCK_SIZE;
        if (inode_data.i_size == (uint32_t)size) {
        return 0;
    }
    uint32_t old_size = inode_data.i_size;
    inode_data.i_size = size;
        uint32_t new_num_fs_blocks = (size + SIMPLEFS_BLOCK_SIZE - 1) / SIMPLEFS_BLOCK_SIZE;
        if (size == 0) new_num_fs_blocks = 0;
        if (new_num_fs_blocks < old_num_fs_blocks) {
            release_logical_block_range(*context, &inode_data, new_num_fs_blocks, old_num_fs_blocks);
        }
    }
    inode_data.i_mtime = time(nullptr);
    inode_data.i_ctime = time(nullptr);
    if (write_inode_to_disk(*context, inode_num, &inode_data) != 0) return -errno;
    if (size < old_size || size == 0) {
        sync_fs_metadata(*context);
    }
    return 0;
}

// 更改文件的权限位
int simplefs_chmod(const char *path, mode_t mode) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t inode_num = path_to_inode_num(path); // 跟随符号链接
    if (inode_num == 0) return -errno;
    SimpleFS_Inode inode_data;
    if (read_inode_from_disk(*context, inode_num, &inode_data) != 0) return -errno;
    struct fuse_context *caller_context = fuse_get_context();
    if (caller_context->uid != 0 && caller_context->uid != inode_data.i_uid) {
         return -EPERM;
    }
    inode_data.i_mode = (inode_data.i_mode & S_IFMT) | (mode & 07777);
    inode_data.i_ctime = time(nullptr);
    if (write_inode_to_disk(*context, inode_num, &inode_data) != 0) return -errno;
    return 0;
}

// 更改文件的所有者和组
int simplefs_chown(const char *path, uid_t uid, gid_t gid) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t inode_num = resolve_path_recursive(path, 0, false); // 如果是链接则操作链接本身
    if (inode_num == 0) return -errno;
    SimpleFS_Inode inode_data;
    if (read_inode_from_disk(*context, inode_num, &inode_data) != 0) return -errno;
    struct fuse_context *caller_context = fuse_get_context();

    if (caller_context->uid != 0) {
        bool uid_changing = (uid != (uid_t)-1 && uid != inode_data.i_uid);
        bool gid_changing = (gid != (gid_t)-1 && gid != inode_data.i_gid);
        if (uid_changing) return -EPERM;
        if (gid_changing) {
            if (caller_context->uid != inode_data.i_uid) return -EPERM;
            bool new_gid_is_valid_for_user = false;
            if (gid == caller_context->gid) {
                new_gid_is_valid_for_user = true;
            } else {
                int num_supp_groups = getgroups(0, nullptr);
                if (num_supp_groups < 0) {
                    perror("获取组数失败"); return -EIO;
                }
                if (num_supp_groups > 0) {
                    std::vector<gid_t> supp_group_list(num_supp_groups);
                    if (getgroups(num_supp_groups, supp_group_list.data()) < 0) {
                        perror("获取组列表失败"); return -EIO;
                    }
                    for (gid_t supp_gid : supp_group_list) {
                        if (gid == supp_gid) {
                            new_gid_is_valid_for_user = true; break;
                        }
                    }
                }
            }
            if (!new_gid_is_valid_for_user) {
                return -EPERM;
            }
        }
    }
    bool changed = false;
    if (uid != (uid_t)-1 && inode_data.i_uid != uid) {
        inode_data.i_uid = uid; changed = true;
    }
    if (gid != (gid_t)-1 && inode_data.i_gid != gid) {
        inode_data.i_gid = gid; changed = true;
    }
    if (changed) {
        if (caller_context->uid != 0) {
            inode_data.i_mode &= ~(S_ISUID | S_ISGID);
        }
        inode_data.i_ctime = time(nullptr);
        if (write_inode_to_disk(*context, inode_num, &inode_data) != 0) return -errno;
    }
    return 0;
}

// 创建符号链接
int simplefs_symlink(const char *target, const char *linkpath) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    std::string linkpath_str(linkpath);
    std::string target_str(target);
    std::string dirname_str, basename_str;
    if (target_str.empty()) return -EINVAL;
    parse_path(linkpath_str, dirname_str, basename_str);
    if (basename_str.empty() || basename_str == "." || basename_str == ".." || basename_str == "/") return -EINVAL;
    if (basename_str.length() > SIMPLEFS_MAX_FILENAME_LEN) return -ENAMETOOLONG;

    errno = 0;
    uint32_t parent_inode_num = path_to_inode_num(dirname_str.c_str());
    if (parent_inode_num == 0) return -errno;
    SimpleFS_Inode parent_inode_data;
    if (read_inode_from_disk(*context, parent_inode_num, &parent_inode_data) != 0) return -errno;
    if (!S_ISDIR(parent_inode_data.i_mode)) return -ENOTDIR;

    int access_res = check_access(fuse_get_context(), &parent_inode_data, W_OK | X_OK);
    if (access_res != 0) return access_res;

    errno = 0;
    uint32_t existing_inode_check = resolve_path_recursive(linkpath_str.c_str(), 0, false); // 检查linkpath本身是否存在
    if (existing_inode_check != 0) return -EEXIST;
    if (errno != 0 && errno != ENOENT) return -errno;

    uint32_t symlink_inode_num = alloc_inode(*context, S_IFLNK | 0777);
    if (symlink_inode_num == 0) return -errno;

    SimpleFS_Inode symlink_inode;
    std::memset(&symlink_inode, 0, sizeof(SimpleFS_Inode));
    symlink_inode.i_mode = S_IFLNK | 0777;
    symlink_inode.i_uid = fuse_get_context()->uid;
    symlink_inode.i_gid = fuse_get_context()->gid;
    symlink_inode.i_links_count = 1;
    symlink_inode.i_size = target_str.length();
    symlink_inode.i_atime = symlink_inode.i_mtime = symlink_inode.i_ctime = time(nullptr);

    // 快速符号链接优化
    if (target_str.length() < sizeof(symlink_inode.i_block)) {
        // 路径足够短，直接存储在inode的块指针中
        std::memcpy(symlink_inode.i_block, target_str.c_str(), target_str.length());
        symlink_inode.i_blocks = 0; // 快速符号链接标识
        std::cout << "[符号链接] 创建快速符号链接: " << target_str << std::endl;
    } else {
        // 路径太长，使用数据块（慢速符号链接）
        if (target_str.length() >= SIMPLEFS_BLOCK_SIZE) {
            free_inode(*context, symlink_inode_num, symlink_inode.i_mode); return -ENAMETOOLONG;
        }
        uint32_t data_block_num = alloc_block(*context, (symlink_inode_num - 1) / context->sb.s_inodes_per_group);
        if (data_block_num == 0) {
            free_inode(*context, symlink_inode_num, symlink_inode.i_mode); return -errno;
        }
        std::vector<char> block_buffer_vec(SIMPLEFS_BLOCK_SIZE, 0);
        std::memcpy(block_buffer_vec.data(), target_str.c_str(), target_str.length());
        if (write_block(context->device_fd, data_block_num, block_buffer_vec.data()) != 0) {
            free_block(*context, data_block_num);
            free_inode(*context, symlink_inode_num, symlink_inode.i_mode); return -EIO;
        }
        symlink_inode.i_block[0] = data_block_num;
        symlink_inode.i_blocks = SIMPLEFS_BLOCK_SIZE / 512;
    }

    if (write_inode_to_disk(*context, symlink_inode_num, &symlink_inode) != 0) {
        if (symlink_inode.i_blocks > 0 && symlink_inode.i_block[0] != 0) {
             free_block(*context, symlink_inode.i_block[0]);
        }
        free_inode(*context, symlink_inode_num, symlink_inode.i_mode); return -errno;
    }

    uint8_t dentry_file_type = (S_IFLNK & S_IFMT) >> 12;
    int add_entry_res = add_dir_entry(*context, &parent_inode_data, parent_inode_num, basename_str, symlink_inode_num, dentry_file_type);
    if (add_entry_res != 0) {
        if (symlink_inode.i_blocks > 0 && symlink_inode.i_block[0] != 0) {
             free_block(*context, symlink_inode.i_block[0]);
        }
        symlink_inode.i_dtime = time(nullptr); symlink_inode.i_links_count = 0;
        write_inode_to_disk(*context, symlink_inode_num, &symlink_inode);
        free_inode(*context, symlink_inode_num, symlink_inode.i_mode);
        return add_entry_res;
    }
    sync_fs_metadata(*context);
    return 0;
}

// 读取符号链接的目标
int simplefs_readlink(const char *path, char *buf, size_t size) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t inode_num = resolve_path_recursive(path, 0, false);
    if (inode_num == 0) return -errno;
    SimpleFS_Inode inode_data;
    if (read_inode_from_disk(*context, inode_num, &inode_data) != 0) return -errno;
    if (!S_ISLNK(inode_data.i_mode)) return -EINVAL;
    if (inode_data.i_size == 0) return 0;

    size_t actual_bytes_copied = 0;

    // 检查是快速符号链接(i_blocks==0)还是慢速符号链接
    if (inode_data.i_blocks == 0) {
        // 快速符号链接：目标路径存储在inode的i_block数组中
        size_t symlink_target_len = inode_data.i_size;
        actual_bytes_copied = std::min(size - 1, symlink_target_len); // 为null终止符留空间
        if (actual_bytes_copied > 0) {
            std::memcpy(buf, inode_data.i_block, actual_bytes_copied);
        }
    } else {
        // 慢速符号链接：目标路径在数据块中
        if (inode_data.i_block[0] == 0) return -EIO;
        size_t symlink_target_len = inode_data.i_size;
        std::vector<char> block_buffer_vec(SIMPLEFS_BLOCK_SIZE);
        if (read_block(context->device_fd, inode_data.i_block[0], block_buffer_vec.data()) != 0) return -EIO;
        actual_bytes_copied = std::min(size - 1, symlink_target_len); // 为null终止符留空间
        if (actual_bytes_copied > 0) {
            std::memcpy(buf, block_buffer_vec.data(), actual_bytes_copied);
        }
    }
    
    buf[actual_bytes_copied] = '\0'; // 总是null终止缓冲区

    inode_data.i_atime = time(nullptr);
    if (write_inode_to_disk(*context, inode_num, &inode_data) != 0) {
        std::cerr << "readlink: inode " << inode_num << " atime更新失败" << std::endl;
    }
    return 0; // 成功时readlink应返回0，并填充缓冲区
}

// 创建硬链接
int simplefs_link(const char *oldpath, const char *newpath) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;

    std::string oldpath_str(oldpath);
    std::string newpath_str(newpath);

    // 1. 解析oldpath到其inode
    errno = 0;
    uint32_t target_inode_num = path_to_inode_num(oldpath_str.c_str());
    if (target_inode_num == 0) return -errno;

    SimpleFS_Inode target_inode_data;
    if (read_inode_from_disk(*context, target_inode_num, &target_inode_data) != 0) return -errno;

    // 硬链接到目录是不允许的
    if (S_ISDIR(target_inode_data.i_mode)) return -EPERM;

    // 2. 解析newpath的父目录
    std::string new_dirname_str, new_basename_str;
    parse_path(newpath_str, new_dirname_str, new_basename_str);

    if (new_basename_str.empty() || new_basename_str == "." || new_basename_str == ".." || new_basename_str == "/") return -EINVAL;
    if (new_basename_str.length() > SIMPLEFS_MAX_FILENAME_LEN) return -ENAMETOOLONG;

    errno = 0;
    uint32_t parent_inode_num = path_to_inode_num(new_dirname_str.c_str());
    if (parent_inode_num == 0) return -errno;

    SimpleFS_Inode parent_inode_data;
    if (read_inode_from_disk(*context, parent_inode_num, &parent_inode_data) != 0) return -errno;

    if (!S_ISDIR(parent_inode_data.i_mode)) return -ENOTDIR;

    // 3. 检查父目录权限
    int access_res = check_access(fuse_get_context(), &parent_inode_data, W_OK | X_OK);
    if (access_res != 0) return access_res;

    // 4. 检查newpath是否已存在
    errno = 0;
    uint32_t existing_inode_check = resolve_path_recursive(newpath_str.c_str(), 0, false);
    if (existing_inode_check != 0) return -EEXIST;
    if (errno != 0 && errno != ENOENT) return -errno;

    // 5. 为新硬链接添加目录条目
    uint8_t dentry_file_type = (target_inode_data.i_mode & S_IFMT) >> 12;
    int add_entry_res = add_dir_entry(*context, &parent_inode_data, parent_inode_num, new_basename_str, target_inode_num, dentry_file_type);
    if (add_entry_res != 0) return add_entry_res;

    // 6. 增加目标inode的链接数
    target_inode_data.i_links_count++;
    target_inode_data.i_ctime = time(nullptr);
    if (write_inode_to_disk(*context, target_inode_num, &target_inode_data) != 0) {
        // 尝试回滚目录条目添加
        remove_dir_entry(*context, &parent_inode_data, parent_inode_num, new_basename_str);
        return -errno;
    }

    sync_fs_metadata(*context);
    return 0;
}

// 以纳秒精度更改文件的访问和修改时间
int simplefs_utimens(const char *path, const struct timespec tv[2]) {
    SimpleFS_Context* context = get_fs_context();
    if (!context) return -EACCES;
    errno = 0;
    uint32_t inode_num = path_to_inode_num(path); // 跟随符号链接进行utimens
    if (inode_num == 0) return -errno;
    SimpleFS_Inode inode_data;
    if (read_inode_from_disk(*context, inode_num, &inode_data) != 0) return -errno;

    struct fuse_context *caller_ctx = fuse_get_context();
    bool specific_times_given = true;
    if (tv == nullptr) {
        specific_times_given = false;
    } else {
        #ifndef UTIME_NOW
        #define UTIME_NOW   ((1l << 30) - 1l)
        #endif
        #ifndef UTIME_OMIT
        #define UTIME_OMIT  ((1l << 30) - 2l)
        #endif
        if ((tv[0].tv_nsec == UTIME_NOW || tv[0].tv_nsec == UTIME_OMIT) &&
            (tv[1].tv_nsec == UTIME_NOW || tv[1].tv_nsec == UTIME_OMIT)) {
            if (tv[0].tv_nsec == UTIME_OMIT && tv[1].tv_nsec == UTIME_OMIT) {
                 // 无变化
            } else {
                 specific_times_given = false;
            }
        }
    }
    if (specific_times_given) {
        if (caller_ctx->uid != 0 && caller_ctx->uid != inode_data.i_uid) return -EPERM;
    } else {
        int access_res = check_access(caller_ctx, &inode_data, W_OK);
        if (access_res != 0) return access_res;
    }
    time_t current_time = time(nullptr);
    if (tv == nullptr) {
        inode_data.i_atime = current_time;
        inode_data.i_mtime = current_time;
    } else {
        #ifndef UTIME_NOW // 如果全局包含中不可用则在本地重新定义
        #define UTIME_NOW   ((1l << 30) - 1l)
        #endif
        #ifndef UTIME_OMIT
        #define UTIME_OMIT  ((1l << 30) - 2l)
        #endif
        if (tv[0].tv_nsec != UTIME_OMIT) {
            inode_data.i_atime = (tv[0].tv_nsec == UTIME_NOW) ? current_time : tv[0].tv_sec;
        }
        if (tv[1].tv_nsec != UTIME_OMIT) {
            inode_data.i_mtime = (tv[1].tv_nsec == UTIME_NOW) ? current_time : tv[1].tv_sec;
        }
    }
    inode_data.i_ctime = current_time;
    if (write_inode_to_disk(*context, inode_num, &inode_data) != 0) return -errno;
    return 0;
}
