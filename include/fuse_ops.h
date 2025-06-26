#pragma once

#include <fuse_lowlevel.h>
#include <fuse.h>
#include "simplefs_context.h"
#include "simplefs.h"

// 获取文件系统上下文
SimpleFS_Context* get_fs_context();

// FUSE操作实现
int simplefs_getattr(const char *path, struct stat *stbuf);
int simplefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t offset, struct fuse_file_info *fi);
int simplefs_mknod(const char *path, mode_t mode, dev_t rdev);
int simplefs_mkdir(const char *path, mode_t mode);
int simplefs_unlink(const char *path);
int simplefs_rmdir(const char *path);
int simplefs_read(const char *path, char *buf, size_t size, off_t offset,
                  struct fuse_file_info *fi);
int simplefs_write(const char *path, const char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi);
int simplefs_truncate(const char *path, off_t size);
int simplefs_chmod(const char *path, mode_t mode);
int simplefs_chown(const char *path, uid_t uid, gid_t gid);
int simplefs_utimens(const char *path, const struct timespec tv[2]);
int simplefs_access(const char *path, int mask);
int simplefs_statfs(const char *path, struct statvfs *stbuf);
int simplefs_symlink(const char *target, const char *linkpath);
int simplefs_readlink(const char *path, char *buf, size_t size);

// 初始化FUSE操作结构
void init_fuse_operations(struct fuse_operations *ops);
