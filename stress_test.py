#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import subprocess
import time
import hashlib
import pwd
import grp
import threading
import random
import string
import shutil
import sys
from collections import defaultdict

# --- 配置部分 ---

# 项目根目录 (假设此脚本位于项目根目录下)
PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))
# 构建目录
BUILD_DIR = os.path.join(PROJECT_DIR, "build")
# SimpleFS 可执行文件路径
SIMPLEFS_EXEC = os.path.join(BUILD_DIR, "simplefs")
# mkfs.simplefs 可执行文件路径
MKFS_EXEC = os.path.join(BUILD_DIR, "mkfs.simplefs")

# 测试环境配置
TEST_DIR = os.path.join(PROJECT_DIR, "simplefs_test_environment") # 测试环境的主目录
MOUNT_POINT = os.path.join(TEST_DIR, "mountpoint") # 文件系统挂载点
DISK_IMAGE = os.path.join(TEST_DIR, "simplefs.img") # 磁盘镜像文件
DISK_SIZE_MB = 256  # 磁盘镜像大小 (MB)

# 大文件测试配置
LARGE_FILE_SIZE_MB = 64  # 大文件大小 (MB)
LARGE_FILE_PATH = os.path.join(MOUNT_POINT, "large_file.dat")

# 大量文件测试配置
MANY_FILES_COUNT = 1024  # 创建的文件数量
MANY_FILES_DIR = os.path.join(MOUNT_POINT, "many_files_test")

# 权限测试配置
TEST_USER_NAME = "testuser"
TEST_GROUP_NAME = "testgroup"

# --- 日志和颜色 ---

class Colors:
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'

def log_info(message):
    print(f"{Colors.OKBLUE}[INFO] {message}{Colors.ENDC}")

def log_success(message):
    print(f"{Colors.OKGREEN}[SUCCESS] {message}{Colors.ENDC}")

def log_warning(message):
    print(f"{Colors.WARNING}[WARNING] {message}{Colors.ENDC}")

def log_error(message):
    print(f"{Colors.FAIL}[ERROR] {message}{Colors.ENDC}")
    sys.exit(1) # 发生错误时直接退出

def log_header(message):
    print(f"\n{Colors.HEADER}{Colors.BOLD}===== {message} ====={Colors.ENDC}")

# --- 辅助函数 ---

def run_command(command, cwd=None, check=True, as_user=None):
    """执行一个 shell 命令"""
    log_info(f"执行命令: {' '.join(command)}")
    if as_user:
        command = ['sudo', '-u', as_user] + command
    try:
        process = subprocess.run(
            command,
            cwd=cwd,
            check=check,
            capture_output=True,
            text=True,
            timeout=600 # 10分钟超时
        )
        if process.stdout:
            print(process.stdout)
        if process.stderr:
            print(process.stderr)
        return process
    except subprocess.CalledProcessError as e:
        log_error(f"命令执行失败，返回码: {e.returncode}")
        log_error(f"Stdout: {e.stdout}")
        log_error(f"Stderr: {e.stderr}")
        raise
    except subprocess.TimeoutExpired as e:
        log_error(f"命令执行超时: {' '.join(command)}")
        raise

def get_file_hash(filepath):
    """计算文件的 SHA256 哈希值"""
    sha256 = hashlib.sha256()
    with open(filepath, 'rb') as f:
        for chunk in iter(lambda: f.read(4096), b''):
            sha256.update(chunk)
    return sha256.hexdigest()

def create_random_file(filepath, size_bytes):
    """创建一个指定大小的随机内容文件"""
    log_info(f"正在创建大小为 {size_bytes / 1024 / 1024:.2f} MB 的随机文件: {filepath}")
    with open(filepath, 'wb') as f:
        f.write(os.urandom(size_bytes))
    log_success(f"文件创建成功: {filepath}")

# --- 环境准备和清理 ---

def setup_environment():
    """准备测试环境"""
    log_header("准备测试环境")
    if os.geteuid() != 0:
        log_error("此脚本需要 root 权限来执行挂载和用户切换操作。请使用 sudo 运行。")

    # 清理旧环境
    if os.path.exists(MOUNT_POINT):
        run_command(['fusermount', '-u', MOUNT_POINT], check=False)
    shutil.rmtree(TEST_DIR, ignore_errors=True)

    # 创建新目录
    os.makedirs(MOUNT_POINT, exist_ok=True)
    log_success(f"测试目录 '{TEST_DIR}' 和挂载点 '{MOUNT_POINT}' 已创建。")

def compile_project():
    """编译 SimpleFS 项目"""
    log_header("编译项目")
    if not os.path.exists(BUILD_DIR):
        os.makedirs(BUILD_DIR)
    
    # 检查CMakeLists.txt是否存在
    if not os.path.exists(os.path.join(PROJECT_DIR, "CMakeLists.txt")):
        log_error("项目根目录下未找到 CMakeLists.txt，请确认脚本位置是否正确。")

    run_command(['cmake', '..'], cwd=BUILD_DIR)
    run_command(['make', '-j'], cwd=BUILD_DIR) # 使用多核编译
    
    if not os.path.exists(SIMPLEFS_EXEC) or not os.path.exists(MKFS_EXEC):
        log_error("编译失败，未找到可执行文件。")
    log_success("项目编译成功。")

def create_and_format_disk():
    """创建并格式化磁盘镜像"""
    log_header("创建和格式化磁盘镜像")
    # 创建一个稀疏文件作为磁盘镜像
    run_command(['truncate', '-s', f'{DISK_SIZE_MB}M', DISK_IMAGE])
    log_success(f"创建了 {DISK_SIZE_MB}MB 的磁盘镜像: {DISK_IMAGE}")

    # 格式化磁盘
    run_command([MKFS_EXEC, DISK_IMAGE])
    log_success("磁盘镜像格式化成功。")

def mount_fs():
    """挂载文件系统"""
    log_header("挂载文件系统")
    # 使用 -f 在前台运行，便于调试，但这里我们需要后台运行
    command = [SIMPLEFS_EXEC, DISK_IMAGE, MOUNT_POINT, '-o', 'allow_other']
    # 在后台启动 simplefs 进程
    fs_process = subprocess.Popen(command)
    time.sleep(2) # 等待 FUSE 挂载完成

    # 检查挂载是否成功
    if not os.path.ismount(MOUNT_POINT):
        fs_process.kill()
        log_error("文件系统挂载失败！")

    log_success(f"文件系统已成功挂载到 {MOUNT_POINT}")
    return fs_process

def unmount_fs(fs_process):
    """卸载文件系统并终止进程"""
    log_header("卸载文件系统")
    run_command(['fusermount', '-u', MOUNT_POINT])
    
    # 尝试正常终止进程
    fs_process.terminate()
    try:
        fs_process.wait(timeout=5)
        log_success("SimpleFS 进程已正常终止。")
    except subprocess.TimeoutExpired:
        log_warning("SimpleFS 进程无法正常终止，强制杀死。")
        fs_process.kill()
    
    if os.path.ismount(MOUNT_POINT):
        log_warning("卸载失败，挂载点仍然存在。")
    else:
        log_success("文件系统已成功卸载。")


def cleanup_environment():
    """清理测试环境"""
    log_header("清理测试环境")
    shutil.rmtree(TEST_DIR, ignore_errors=True)
    cleanup_test_user()
    log_success("测试环境已清理。")

def setup_test_user():
    """创建用于权限测试的用户和组"""
    log_header("设置测试用户和组")
    try:
        grp.getgrnam(TEST_GROUP_NAME)
        log_info(f"组 '{TEST_GROUP_NAME}' 已存在。")
    except KeyError:
        run_command(['groupadd', TEST_GROUP_NAME])
        log_success(f"组 '{TEST_GROUP_NAME}' 已创建。")
    
    try:
        pwd.getpwnam(TEST_USER_NAME)
        log_info(f"用户 '{TEST_USER_NAME}' 已存在。")
    except KeyError:
        run_command(['useradd', '-m', '-g', TEST_GROUP_NAME, '-s', '/bin/bash', TEST_USER_NAME])
        log_success(f"用户 '{TEST_USER_NAME}' 已创建。")

def cleanup_test_user():
    """删除测试用户和组"""
    log_info("清理测试用户和组...")
    run_command(['userdel', '-r', TEST_USER_NAME], check=False)
    run_command(['groupdel', TEST_GROUP_NAME], check=False)
    log_success("测试用户和组已清理。")

# --- 测试用例 ---

def test_large_file_io():
    """
    测试大文件的读写和完整性。
    1. 在本地创建一个大文件。
    2. 计算本地文件的哈希值。
    3. 使用 dd 命令将文件复制到挂载点。
    4. 从挂载点将文件复制回来。
    5. 比较复制回来的文件和原始文件的哈希值。
    6. 直接读取挂载点上的文件并验证其哈希值。
    7. 删除文件。
    """
    log_header("开始大文件读写和完整性测试")
    local_source_file = os.path.join(TEST_DIR, "large_file_source.dat")
    local_dest_file = os.path.join(TEST_DIR, "large_file_dest.dat")
    file_size_bytes = LARGE_FILE_SIZE_MB * 1024 * 1024

    # 1. 创建本地源文件
    create_random_file(local_source_file, file_size_bytes)
    
    # 2. 计算源文件哈希
    source_hash = get_file_hash(local_source_file)
    log_info(f"源文件哈希: {source_hash}")

    # 3. 写入文件到 simplefs
    log_info("使用 dd 命令将大文件写入 simplefs...")
    run_command([
        'dd', f'if={local_source_file}', f'of={LARGE_FILE_PATH}', 'bs=4M', 'oflag=direct'
    ])
    log_success("大文件写入成功。")
    time.sleep(1) # 等待数据落盘

    # 4. 检查文件大小
    stat_info = os.stat(LARGE_FILE_PATH)
    if stat_info.st_size != file_size_bytes:
        log_error(f"文件大小不匹配！期望: {file_size_bytes}, 实际: {stat_info.st_size}")
    log_success("文件大小验证成功。")

    # 5. 直接验证挂载点上文件的哈希
    log_info("直接验证挂载点上文件的哈希...")
    fs_file_hash = get_file_hash(LARGE_FILE_PATH)
    log_info(f"SimpleFS 中的文件哈希: {fs_file_hash}")
    if fs_file_hash != source_hash:
        log_error("文件内容完整性检查失败！哈希值不匹配。")
    log_success("文件内容完整性（直接读取）验证成功！")

    # 6. 从 simplefs 读回文件
    log_info("使用 dd 命令从 simplefs 读回大文件...")
    run_command([
        'dd', f'if={LARGE_FILE_PATH}', f'of={local_dest_file}', 'bs=4M', 'iflag=direct'
    ])
    
    # 7. 验证读回文件的哈希
    dest_hash = get_file_hash(local_dest_file)
    log_info(f"读回文件哈希: {dest_hash}")
    if dest_hash != source_hash:
        log_error("文件内容完整性检查失败！读回的文件与源文件不匹配。")
    log_success("文件内容完整性（读回）验证成功！")
    
    # 8. 删除文件
    os.remove(LARGE_FILE_PATH)
    if os.path.exists(LARGE_FILE_PATH):
        log_error("删除大文件失败！")
    log_success("大文件删除成功。")

    # 清理本地临时文件
    os.remove(local_source_file)
    os.remove(local_dest_file)


def test_many_files_io():
    """
    测试大量小文件的创建、读写和删除。
    1. 创建一个目录。
    2. 在目录中创建大量小文件，并写入少量随机数据。
    3. 记录每个文件的路径和其内容的哈希。
    4. 随机抽查一些文件，验证其内容是否正确。
    5. 验证目录列表是否能正确显示所有文件。
    6. 依次删除所有文件。
    7. 删除目录。
    """
    log_header("开始大量小文件读写测试")
    os.makedirs(MANY_FILES_DIR, exist_ok=True)
    
    file_hashes = {}
    
    # 1. 创建大量文件
    log_info(f"正在创建 {MANY_FILES_COUNT} 个小文件...")
    for i in range(MANY_FILES_COUNT):
        filename = f"small_file_{i}.txt"
        filepath = os.path.join(MANY_FILES_DIR, filename)
        content = ''.join(random.choices(string.ascii_letters + string.digits, k=128)).encode('utf-8')
        
        with open(filepath, 'wb') as f:
            f.write(content)
        
        file_hashes[filepath] = hashlib.sha256(content).hexdigest()
    log_success(f"{MANY_FILES_COUNT} 个小文件创建完成。")
    
    # 2. 验证文件数量
    listed_files = os.listdir(MANY_FILES_DIR)
    if len(listed_files) != MANY_FILES_COUNT:
        log_error(f"文件数量不匹配！期望: {MANY_FILES_COUNT}, 实际: {len(listed_files)}")
    log_success("文件数量验证成功。")

    # 3. 随机抽样验证
    log_info("随机抽样 50 个文件进行内容验证...")
    sample_paths = random.sample(list(file_hashes.keys()), 50)
    for path in sample_paths:
        with open(path, 'rb') as f:
            content = f.read()
            read_hash = hashlib.sha256(content).hexdigest()
            if read_hash != file_hashes[path]:
                log_error(f"文件 {path} 内容验证失败！")
    log_success("随机抽样文件内容验证成功。")

    # 4. 删除所有文件
    log_info(f"正在删除 {MANY_FILES_COUNT} 个小文件...")
    for filepath in file_hashes.keys():
        os.remove(filepath)
    log_success("所有小文件删除成功。")

    # 5. 验证目录是否为空
    if len(os.listdir(MANY_FILES_DIR)) != 0:
        log_error("删除文件后，目录不为空！")
    log_success("目录已清空验证成功。")

    # 6. 删除目录
    os.rmdir(MANY_FILES_DIR)
    if os.path.exists(MANY_FILES_DIR):
        log_error("删除目录失败！")
    log_success("测试目录删除成功。")


def test_permission_system():
    """
    测试文件系统的权限控制。
    1. 创建一个测试文件，所有者为 root。
    2. 切换到 testuser，尝试读取该文件（应该失败）。
    3. 切换到 testuser，尝试写入该文件（应该失败）。
    4. 使用 root 修改文件权限为 0644，允许其他人读取。
    5. 切换到 testuser，尝试读取该文件（应该成功）。
    6. 切换到 testuser，尝试写入该文件（应该失败）。
    7. 使用 root 修改文件权限为 0666，允许其他人写入。
    8. 切换到 testuser，尝试写入该文件（应该成功）。
    9. 使用 chown 修改文件所有者为 testuser。
    10. 切换到 testuser，修改文件权限为 0600。
    11. 切换回 root，尝试读取文件（应该成功，因为 root 有超级权限）。
    """
    log_header("开始权限系统测试")
    setup_test_user()
    test_file = os.path.join(MOUNT_POINT, "permission_test.txt")
    test_content = "permission test content"

    # 1. root 创建文件
    with open(test_file, 'w') as f:
        f.write(test_content)
    run_command(['chown', f'root:root', test_file])
    run_command(['chmod', '0600', test_file])
    log_success("root 创建了文件 permission_test.txt (权限 0600)")

    # 2. testuser 尝试读取 (失败)
    log_info("测试: testuser 尝试读取 root 的 0600 文件 (应失败)")
    try:
        run_command(['cat', test_file], as_user=TEST_USER_NAME, check=True)
        log_error("权限测试失败：testuser 不应能读取此文件！")
    except subprocess.CalledProcessError:
        log_success("测试通过：testuser 无法读取文件。")

    # 3. testuser 尝试写入 (失败)
    log_info("测试: testuser 尝试写入 root 的 0600 文件 (应失败)")
    try:
        run_command(['sh', '-c', f'echo "more" >> {test_file}'], as_user=TEST_USER_NAME, check=True)
        log_error("权限测试失败：testuser 不应能写入此文件！")
    except subprocess.CalledProcessError:
        log_success("测试通过：testuser 无法写入文件。")

    # 4. root 修改权限为 0644
    log_info("root 修改权限为 0644 (允许 others 读取)")
    run_command(['chmod', '0644', test_file])

    # 5. testuser 尝试读取 (成功)
    log_info("测试: testuser 尝试读取 root 的 0644 文件 (应成功)")
    run_command(['cat', test_file], as_user=TEST_USER_NAME)
    log_success("测试通过：testuser 可以读取文件。")

    # 6. testuser 尝试写入 (失败)
    log_info("测试: testuser 尝试写入 root 的 0644 文件 (应失败)")
    try:
        run_command(['sh', '-c', f'echo "more" >> {test_file}'], as_user=TEST_USER_NAME, check=True)
        log_error("权限测试失败：testuser 不应能写入此文件！")
    except subprocess.CalledProcessError:
        log_success("测试通过：testuser 无法写入文件。")

    # 7. root 修改所有者为 testuser
    log_info("root 使用 chown 将文件所有者改为 testuser")
    run_command(['chown', f'{TEST_USER_NAME}:{TEST_GROUP_NAME}', test_file])
    
    # 8. testuser 尝试写入 (成功)
    log_info("测试: testuser 作为所有者尝试写入文件 (应成功)")
    run_command(['sh', '-c', f'echo " more content" >> {test_file}'], as_user=TEST_USER_NAME)
    log_success("测试通过：testuser 作为所有者可以写入文件。")

    # 9. testuser 修改权限为 0600
    log_info("testuser 修改权限为 0600")
    run_command(['chmod', '0600', test_file], as_user=TEST_USER_NAME)
    
    # 10. root 尝试读取 (成功)
    log_info("测试: root 尝试读取 testuser 的 0600 文件 (应成功)")
    run_command(['cat', test_file]) # root 默认有权限
    log_success("测试通过：root 可以读取任何文件。")

    # 清理
    os.remove(test_file)
    log_success("权限测试文件已清理。")

def test_links():
    """
    测试硬链接和符号链接。
    1. 创建一个源文件。
    2. 创建一个硬链接到该文件。
    3. 验证硬链接和源文件有相同的 inode。
    4. 验证修改源文件后，硬链接内容也改变。
    5. 删除源文件，验证硬链接依然可以访问。
    6. 删除硬链接。
    7. 创建一个新的源文件。
    8. 创建一个符号链接到该文件。
    9. 验证读取符号链接会返回源文件的内容。
    10. 删除源文件，验证访问符号链接会失败（悬空链接）。
    11. 重新创建源文件，验证符号链接恢复正常。
    12. 删除符号链接和源文件。
    """
    log_header("开始链接功能测试")
    
    # --- 硬链接测试 ---
    log_info("--- 硬链接测试 ---")
    source_file = os.path.join(MOUNT_POINT, "hardlink_source.txt")
    hard_link = os.path.join(MOUNT_POINT, "hardlink_link.txt")
    
    # 1. 创建源文件
    with open(source_file, "w") as f:
        f.write("initial content")
    log_success(f"创建源文件: {source_file}")

    # 2. 创建硬链接
    os.link(source_file, hard_link)
    log_success(f"创建硬链接: {hard_link}")

    # 3. 验证 inode
    source_stat = os.stat(source_file)
    link_stat = os.stat(hard_link)
    if source_stat.st_ino != link_stat.st_ino:
        log_error(f"硬链接 Inode 不匹配! 源: {source_stat.st_ino}, 链接: {link_stat.st_ino}")
    log_success("Inode 验证成功。")
    if source_stat.st_nlink != 2:
        log_error(f"链接数不为 2，实际为: {source_stat.st_nlink}")
    log_success("链接数验证成功 (nlink=2)。")

    # 4. 验证内容同步
    with open(source_file, "a") as f:
        f.write(" appended")
    with open(hard_link, "r") as f:
        content = f.read()
    if content != "initial content appended":
        log_error("硬链接内容未同步！")
    log_success("内容同步验证成功。")
    
    # 5. 删除源文件后访问硬链接
    os.remove(source_file)
    log_info("源文件已删除。")
    with open(hard_link, "r") as f:
        content = f.read()
    if content != "initial content appended":
        log_error("删除源文件后，无法通过硬链接访问正确内容！")
    link_stat_after_rm = os.stat(hard_link)
    if link_stat_after_rm.st_nlink != 1:
        log_error(f"删除源文件后，链接数不为 1，实际为: {link_stat_after_rm.st_nlink}")
    log_success("删除源文件后，硬链接依然可用，且 nlink=1。")

    # 6. 删除硬链接
    os.remove(hard_link)
    log_success("硬链接已删除。")

    # --- 符号链接测试 ---
    log_info("--- 符号链接测试 ---")
    source_file_sym = os.path.join(MOUNT_POINT, "symlink_source.txt")
    sym_link = os.path.join(MOUNT_POINT, "symlink_link.txt")

    # 7. 创建源文件
    with open(source_file_sym, "w") as f:
        f.write("symlink test")
    log_success(f"创建源文件: {source_file_sym}")
    
    # 8. 创建符号链接
    os.symlink(source_file_sym, sym_link)
    log_success(f"创建符号链接: {sym_link}")
    if not os.path.islink(sym_link):
        log_error("创建的不是符号链接！")
    log_success("符号链接类型验证成功。")

    # 9. 验证内容
    with open(sym_link, "r") as f:
        content = f.read()
    if content != "symlink test":
        log_error("通过符号链接读取的内容不正确！")
    log_success("通过符号链接读取内容验证成功。")

    # 10. 删除源文件后访问 (悬空链接)
    os.remove(source_file_sym)
    log_info("源文件已删除。")
    try:
        with open(sym_link, "r") as f:
            f.read()
        log_error("访问悬空链接时没有报错！")
    except FileNotFoundError:
        log_success("访问悬空链接时正确地抛出 FileNotFoundError。")

    # 11. 重新创建源文件
    with open(source_file_sym, "w") as f:
        f.write("symlink reborn")
    log_info("源文件已重新创建。")
    with open(sym_link, "r") as f:
        content = f.read()
    if content != "symlink reborn":
        log_error("重新创建源文件后，符号链接内容不正确！")
    log_success("重新创建源文件后，符号链接恢复正常。")
    
    # 12. 清理
    os.remove(sym_link)
    os.remove(source_file_sym)
    log_success("符号链接测试清理完毕。")

# --- 主函数 ---

def main():
    """主执行函数"""
    fs_process = None
    try:
        setup_environment()
        compile_project()
        create_and_format_disk()
        fs_process = mount_fs()
        
        # --- 按顺序执行所有测试 ---
        test_large_file_io()
        test_many_files_io()
        test_permission_system()
        test_links()
        
        log_header("所有测试已成功完成！")

    except Exception as e:
        log_error(f"测试过程中发生未捕获的异常: {e}")
        import traceback
        traceback.print_exc()

    finally:
        if fs_process:
            unmount_fs(fs_process)
        cleanup_environment()
        log_info("脚本执行结束。")

if __name__ == '__main__':
    main()

