#include "simplefs.h"
#include "disk_io.h"
#include "utils.h"
#include <iostream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cmath>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <设备文件>" << std::endl;
        return 1;
    }
    const char* path = argv[1];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { 
        perror("打开设备文件失败"); 
        return 1; 
    }

    std::vector<uint8_t> buf(SIMPLEFS_BLOCK_SIZE);
    if (read_block(fd, 1, buf.data()) != 0) { 
        std::cerr << "读取超级块失败" << std::endl; 
        close(fd); 
        return 1; 
    }
    SimpleFS_SuperBlock sb; 
    std::memcpy(&sb, buf.data(), sizeof(sb));
    if (sb.s_magic != SIMPLEFS_MAGIC) {
        std::cerr << "魔数不匹配，不是SimpleFS镜像" << std::endl; 
        close(fd); 
        return 1; 
    }

    uint32_t num_groups = static_cast<uint32_t>(std::ceil((double)sb.s_blocks_count / sb.s_blocks_per_group));
    uint32_t gdt_size = num_groups * sizeof(SimpleFS_GroupDesc);
    uint32_t gdt_blocks = static_cast<uint32_t>(std::ceil((double)gdt_size / SIMPLEFS_BLOCK_SIZE));
    std::vector<uint8_t> gdt_raw(gdt_blocks * SIMPLEFS_BLOCK_SIZE);
    for (uint32_t i=0;i<gdt_blocks;++i){
        if (read_block(fd, 2 + i, gdt_raw.data()+i*SIMPLEFS_BLOCK_SIZE)!=0){ 
            std::cerr<<"读取组描述符表失败"<<std::endl; 
            close(fd); 
            return 1; 
        }
    }
    std::vector<SimpleFS_GroupDesc> gdt(num_groups);
    std::memcpy(gdt.data(), gdt_raw.data(), gdt_size);

    uint64_t calc_free_blocks=0, calc_free_inodes=0;
    for(uint32_t grp=0; grp<num_groups; ++grp){
        const auto& gd=gdt[grp];
        std::vector<uint8_t> bb(SIMPLEFS_BLOCK_SIZE), ib(SIMPLEFS_BLOCK_SIZE);
        read_block(fd, gd.bg_block_bitmap, bb.data());
        read_block(fd, gd.bg_inode_bitmap, ib.data());
        uint32_t freeb=0, freei=0;
        for(uint32_t b=0;b<sb.s_blocks_per_group && (grp*sb.s_blocks_per_group+b)<sb.s_blocks_count;++b)
            if(!is_bitmap_bit_set(bb,b)) freeb++;
        for(uint32_t i=0;i<sb.s_inodes_per_group;++i)
            if(!is_bitmap_bit_set(ib,i)) freei++;
        if(freeb!=gd.bg_free_blocks_count)
            std::cout<<"组 "<<grp<<" 块计数不匹配: 位图="<<freeb<<" 描述符="<<gd.bg_free_blocks_count<<std::endl;
        if(freei!=gd.bg_free_inodes_count)
            std::cout<<"组 "<<grp<<" inode计数不匹配: 位图="<<freei<<" 描述符="<<gd.bg_free_inodes_count<<std::endl;
        calc_free_blocks+=freeb;
        calc_free_inodes+=freei;
    }
    if(calc_free_blocks!=sb.s_free_blocks_count)
        std::cout<<"超级块空闲块计数不匹配: "<<calc_free_blocks<<" vs "<<sb.s_free_blocks_count<<std::endl;
    if(calc_free_inodes!=sb.s_free_inodes_count)
        std::cout<<"超级块空闲inode计数不匹配: "<<calc_free_inodes<<" vs "<<sb.s_free_inodes_count<<std::endl;

    std::cout<<"fsck检查完成"<<std::endl;
    close(fd); 
    return 0;
}
