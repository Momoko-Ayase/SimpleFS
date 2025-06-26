#pragma once

#include "simplefs.h"
#include "disk_io.h"
#include <vector>
#include <string>

// 文件系统全局上下文
struct SimpleFS_Context {
    DeviceFd device_fd;
    SimpleFS_SuperBlock sb;
    std::vector<SimpleFS_GroupDesc> gdt;
};
