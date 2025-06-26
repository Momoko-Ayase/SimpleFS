#pragma once

#include <cstdint>
#include <sys/types.h>

#pragma pack(push, 1)

// 文件系统常量定义
constexpr uint16_t SIMPLEFS_MAGIC = 0x5350;
constexpr uint32_t SIMPLEFS_BLOCK_SIZE = 4096;
constexpr uint32_t SIMPLEFS_ROOT_INODE_NUM = 2;
constexpr uint32_t SIMPLEFS_INODE_SIZE = 128;
constexpr uint32_t SIMPLEFS_NUM_DIRECT_BLOCKS = 12;
constexpr uint32_t SIMPLEFS_NUM_INDIRECT_BLOCKS = 1;
constexpr uint32_t SIMPLEFS_NUM_D_INDIRECT_BLOCKS = 1;
constexpr uint32_t SIMPLEFS_NUM_T_INDIRECT_BLOCKS = 1;
constexpr uint32_t SIMPLEFS_INODE_BLOCK_PTRS = SIMPLEFS_NUM_DIRECT_BLOCKS + \
                                             SIMPLEFS_NUM_INDIRECT_BLOCKS + \
                                             SIMPLEFS_NUM_D_INDIRECT_BLOCKS + \
                                             SIMPLEFS_NUM_T_INDIRECT_BLOCKS;

constexpr uint32_t SIMPLEFS_MAX_FILENAME_LEN = 255;

// 文件类型常量
#ifndef S_IFMT
#define S_IFMT   0xF000 // 文件类型掩码
#define S_IFSOCK 0xC000 // 套接字
#define S_IFLNK  0xA000 // 符号链接
#define S_IFREG  0x8000 // 普通文件
#define S_IFBLK  0x6000 // 块设备
#define S_IFDIR  0x4000 // 目录
#define S_IFCHR  0x2000 // 字符设备
#define S_IFIFO  0x1000 // 管道
#endif


// 超级块结构
struct SimpleFS_SuperBlock {
    uint16_t s_magic;               // 魔数
    uint32_t s_inodes_count;        // 总inode数
    uint32_t s_blocks_count;        // 总块数
    uint32_t s_free_blocks_count;   // 空闲块数
    uint32_t s_free_inodes_count;   // 空闲inode数
    uint32_t s_first_data_block;    // 首个数据块
    uint32_t s_log_block_size;      // 块大小的对数值
    uint32_t s_blocks_per_group;    // 每组块数
    uint32_t s_inodes_per_group;    // 每组inode数
    uint32_t s_mtime;               // 最后挂载时间
    uint32_t s_wtime;               // 最后写入时间
    uint16_t s_mnt_count;           // 挂载次数
    uint16_t s_max_mnt_count;       // 最大挂载次数
    uint16_t s_state;               // 文件系统状态
    uint16_t s_errors;              // 错误处理方式
    uint32_t s_first_ino;           // 首个非保留inode号
    uint16_t s_inode_size;          // inode大小
    uint16_t s_block_group_nr;      // 块组号
    uint32_t s_root_inode;          // 根inode号
    uint8_t  s_padding[962];        // 填充到1024字节
};
static_assert(sizeof(SimpleFS_SuperBlock) == 1024, "超级块大小必须为1024字节");

// 块组描述符结构
struct SimpleFS_GroupDesc {
    uint32_t bg_block_bitmap;       // 块位图块号
    uint32_t bg_inode_bitmap;       // inode位图块号
    uint32_t bg_inode_table;        // inode表起始块号
    uint16_t bg_free_blocks_count;  // 空闲块数
    uint16_t bg_free_inodes_count;  // 空闲inode数
    uint16_t bg_used_dirs_count;    // 已使用目录数
    uint8_t  bg_padding[14];        // 填充到32字节
};
static_assert(sizeof(SimpleFS_GroupDesc) == 32, "块组描述符大小必须为32字节");

// inode结构
struct SimpleFS_Inode {
    uint16_t i_mode;                // 文件模式
    uint16_t i_uid;                 // 用户ID
    uint32_t i_size;                // 文件大小
    uint32_t i_atime;               // 访问时间
    uint32_t i_ctime;               // 创建时间
    uint32_t i_mtime;               // 修改时间
    uint32_t i_dtime;               // 删除时间
    uint16_t i_gid;                 // 组ID
    uint16_t i_links_count;         // 硬链接数
    uint32_t i_blocks;              // 块数
    uint32_t i_flags;               // 标志
    uint32_t i_block[SIMPLEFS_INODE_BLOCK_PTRS]; // 块指针数组
    uint8_t  i_padding[32];         // 填充到128字节
};
static_assert(sizeof(SimpleFS_Inode) == 128, "inode大小必须为128字节");

// 目录项结构
struct SimpleFS_DirEntry {
    uint32_t inode;                 // inode号
    uint16_t rec_len;               // 记录长度
    uint8_t  name_len;              // 文件名长度
    uint8_t  file_type;             // 文件类型
    char     name[SIMPLEFS_MAX_FILENAME_LEN + 1]; // 文件名
};

#pragma pack(pop)
