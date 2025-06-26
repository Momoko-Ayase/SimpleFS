# SimpleFS 综合设计与实现指南：一个类 EXT2 的 Linux 文件系统

## 第一部分：基础概念与磁盘结构

本部分旨在为 SimpleFS 项目奠定物理和逻辑基础。此处的设计决策是根本性的，将决定整个文件系统的能力与局限。

### 1.1 SimpleFS 架构概述

**核心理念**

SimpleFS 是一个基于块的文件系统，其设计严格遵循了经典的 EXT2 架构。选择 EXT2 作为教学模型并非偶然，其清晰的元数据与数据分离、静态的 inode 分配机制，以及缺乏日志等复杂特性，使其成为学习文件系统核心概念的理想范本。SimpleFS 的目标是在简化实现的同时，保留 EXT2 的精髓，从而构建一个功能完备且易于理解的存储系统。

**FUSE 的角色**

SimpleFS 的实现将通过 FUSE（Filesystem in Userspace，用户空间文件系统）框架与 Linux 内核进行交互。FUSE 扮演着一个关键的桥梁角色：它拦截来自内核 VFS（Virtual File System，虚拟文件系统）层的标准文件系统调用（如`open`, `read`, `write`），将这些调用转化为回调函数，并转发给在用户空间运行的`simplefs`守护进程。这使得我们能够使用 C++语言在用户态实现文件系统的全部逻辑，而无需修改或重新编译内核，极大地降低了开发和调试的复杂度。

一个典型的`read()`系统调用流程如下：

1. 用户程序发起`read()`请求。
2. 请求进入内核，由 VFS 层接收。
3. VFS 发现该文件位于一个 FUSE 挂载点上，于是将请求打包并发送给`/dev/fuse`设备。
4. FUSE 内核模块将请求转发给用户空间的`simplefs`进程。
5. `simplefs`进程中预先注册的`read`回调函数被触发。
6. 我们的 C++代码执行读取逻辑：解析路径、定位 inode、通过间接块寻址找到数据块、从底层设备读取数据。
7. 数据通过 FUSE 返回给内核，并最终送达用户程序。

**关键设计原则**

- **基于块的分配 (Block-based Allocation)**：整个磁盘分区被划分为固定大小（本项目中为 4KB）的逻辑块。块是文件系统进行存储空间分配和管理的基本单位。
- **访问局部性 (Locality of Reference)**：为了提升性能，磁盘被进一步划分为若干个“块组”（Block Group）。这种设计的核心思想是将一个文件的元数据（inode）和其数据块尽可能地存放在同一个块组内，从而减少磁盘磁头的寻道时间，提高 I/O 效率。
- **静态元数据 (Static Metadata)**：与现代一些动态分配元数据的文件系统不同，SimpleFS 继承了 EXT2 的特点，即文件系统中 inode 的总数在格式化时就被固定下来。这意味着文件系统能够存储的文件数量上限是预先确定的。

### 1.2 磁盘数据布局：分区的蓝图

本节将详细、直观地剖析文件系统的物理布局。

**整体结构**

一个被格式化为 SimpleFS 的分区，其宏观结构是由连续的块组构成的序列：`块组 0`, `块组 1`,..., `块组 N`。

**块组的内部结构**

每个块组内部都遵循着相同的布局，包含了一系列元数据结构和数据存储区域。一个典型的块组结构如下：

1. **超级块 (Super Block)**：仅在部分块组（如 0、1 以及 3、5、7 的幂次方为组号的块组）的起始位置存在，作为主超级块的备份。
2. **块组描述符表 (Group Descriptor Table)**：同上，也存在备份。
3. **块位图 (Block Bitmap)**：占用 1 个块，用于追踪该块组内数据块的分配状态。
4. **inode 位图 (Inode Bitmap)**：占用 1 个块，用于追踪该块组内 inode 的分配状态。
5. **inode 表 (Inode Table)**：占用 N 个连续的块，存储该块组内所有 inode 的实际数据结构。
6. **数据块 (Data Blocks)**：块组中剩余的所有块，用于存储文件内容、目录内容等实际数据。

**块组 0 的特殊角色**

在所有块组中，块组 0 具有独一无二的地位。它必须包含文件系统唯一的**主超级块**和完整的**块组描述符表**（GDT）。GDT 中包含了文件系统中每一个块组的描述符信息。后续块组中存储的超级块和 GDT 均为备份，其目的是提高文件系统的容错能力。如果块组 0 中包含主 GDT 的磁盘扇区发生物理损坏，整个文件系统将因无法定位任何块组的元数据而变得不可访问。备份机制的存在，使得在这种灾难性场景下，文件系统检查工具（如

`fsck`）有可能通过查找并使用备份 GDT 来恢复文件系统。对于本课程设计而言，实现主 GDT 即可满足要求，但理解其备份机制对于认识文件系统的健壮性设计至关重要。

### 1.3 超级块：文件系统的总纲

**功能**

超级块是 SimpleFS 中最为关键的元数据结构。当文件系统被挂载时，操作系统首先会读取超级块。超级块中记录了文件系统的全局信息，如总块数、总 inode 数、块大小等，相当于文件系统的“说明书”。它还包含一个特殊的“魔数”（Magic Number），用于标识该分区确实是一个 SimpleFS 文件系统，防止误挂载其他类型的文件系统。

**实现细节**

超级块位于块组 0 的一个固定偏移位置。参照 EXT2 的惯例，如果块大小为 4KB，超级块通常从分区的 1024 字节偏移处开始，但为了简化，我们可以让它占据第 1 个块（块号为 0 的块通常预留给引导扇区，所以超级块在块号为 1 的块中）。

**表 1：`SimpleFS_SuperBlock`结构字段**

下表详尽列出了超级块的各个字段。这个表格是实现格式化工具`mkfs.simplefs`和主守护进程`simplefs`的权威参考，因为它精确定义了两者之间关于磁盘布局的“契约”。

| 字段名                | C++类型    | 大小(字节) | 描述                                                                | 引用 |
| --------------------- | ---------- | ---------- | ------------------------------------------------------------------- | ---- |
| `s_magic`             | `uint16_t` | 2          | 标识 SimpleFS 的魔数（`0x5350`）        |      |
| `s_inodes_count`      | `uint32_t` | 4          | 文件系统中 inode 的总数，在格式化时确定                             |      |
| `s_blocks_count`      | `uint32_t` | 4          | 文件系统中块的总数（包括元数据和数据块）                            |      |
| `s_free_blocks_count` | `uint32_t` | 4          | 空闲块的数量，在分配/释放块时必须实时更新                           |      |
| `s_free_inodes_count` | `uint32_t` | 4          | 空闲 inode 的数量，在分配/释放 inode 时必须实时更新                 |      |
| `s_first_data_block`  | `uint32_t` | 4          | 第一个可用数据块的块号（若块大小为 1KB，则为 1；若为 4KB，则为 0）  |      |
| `s_log_block_size`    | `uint32_t` | 4          | 块大小的对数值（如 4KB 块对应值为 2，因为**1024**<<**2**=**4096**） |      |
| `s_blocks_per_group`  | `uint32_t` | 4          | 每个块组包含的块数                                                  |      |
| `s_inodes_per_group`  | `uint32_t` | 4          | 每个块组包含的 inode 数                                             |      |
| `s_first_ino`         | `uint32_t` | 4          | 第一个非保留 inode 的 inode 号（EXT2 中通常是 11）                  |      |
| `s_inode_size`        | `uint16_t` | 2          | 磁盘上 inode 结构的大小（本项目设计为 128 字节）                    |      |
| `s_root_inode`        | `uint32_t` | 4          | 根目录的 inode 号（通常为 2）                                       |      |

### 1.4 块组描述符：管理分段的目录

**功能**

块组描述符表（GDT）是一个由块组描述符构成的数组，文件系统中的每个块组都对应一个描述符。每个描述符明确指出了对应块组的位图（Block Bitmap、Inode Bitmap）和 inode 表（Inode Table）的起始块号，并记录了该组内空闲资源的数量。

任何需要分配新块或新 inode 的操作，都必须首先查阅 GDT。分配算法通常会遍历 GDT，寻找一个拥有足够空闲资源的块组（例如，`bg_free_blocks_count > 0`）。一旦选定一个块组，GDT 就提供了直接访问该组元数据（位图、inode 表）所需的一切信息，从而进行下一步的分配操作。

**表 2：`SimpleFS_GroupDesc`结构字段**

| 字段名                 | C++类型    | 大小(字节) | 描述                              | 引用 |
| ---------------------- | ---------- | ---------- | --------------------------------- | ---- |
| `bg_block_bitmap`      | `uint32_t` | 4          | 该块组的块位图所在的块号          |      |
| `bg_inode_bitmap`      | `uint32_t` | 4          | 该块组的 inode 位图所在的块号     |      |
| `bg_inode_table`       | `uint32_t` | 4          | 该块组的 inode 表起始块号         |      |
| `bg_free_blocks_count` | `uint16_t` | 2          | 该块组中的空闲块数量              |      |
| `bg_free_inodes_count` | `uint16_t` | 2          | 该块组中的空闲 inode 数量         |      |
| `bg_used_dirs_count`   | `uint16_t` | 2          | 该块组中被分配为目录的 inode 数量 |      |

## 第二部分：核心数据结构与元数据管理

本部分将从物理布局过渡到在内存中代表文件系统对象的 C++数据结构。

### 2.1 inode：文件元数据的核心

**概念**

inode（index node，索引节点）是 SimpleFS 的中心数据结构。文件系统上的每一个对象——无论是普通文件、目录还是符号链接——都由一个唯一的 inode 来描述。它包含了关于该对象的所有元数据，**唯一的例外是文件名**。文件名并不存储在 inode 中，而是存储在指向该 inode 的目录项里。

**固定大小**

为了便于管理，本项目将采用固定的 128 字节大小的 inode 结构。这个尺寸的选择使得一个 4KB 的块恰好可以容纳 32 个 inode（**4096/128**=**32**），从而简化了 inode 的定位和读写计算。

**表 3：`SimpleFS_Inode`结构字段**

这是整个设计中最重要的表格之一。几乎每一个 FUSE 操作（`getattr`, `read`, `chmod`等）的最终目的都是读取或修改一个 inode。此表是 C++中`struct Inode`定义的权威参考。

| 字段名          | C++类型    | 大小(字节) | 描述                                                     | 引用 |
| --------------- | ---------- | ---------- | -------------------------------------------------------- | ---- |
| `i_mode`        | `uint16_t` | 2          | 文件类型（如`S_IFREG`,`S_IFDIR`）和权限位（如 0755）     |      |
| `i_uid`         | `uint16_t` | 2          | 文件所有者的用户 ID                                      |      |
| `i_gid`         | `uint16_t` | 2          | 文件所有者的组 ID                                        |      |
| `i_size`        | `uint32_t` | 4          | 文件大小（以字节为单位）。若要支持 64 位大小，需额外字段 |      |
| `i_atime`       | `uint32_t` | 4          | 最后访问时间（Unix 时间戳）                              |      |
| `i_ctime`       | `uint32_t` | 4          | inode 状态变更时间（元数据修改时间）                     |      |
| `i_mtime`       | `uint32_t` | 4          | 文件内容修改时间                                         |      |
| `i_links_count` | `uint16_t` | 2          | 硬链接计数。当此计数为 0 时，文件才被真正删除            |      |
| `i_blocks`      | `uint32_t` | 4          | 文件占用的块数（通常以 512 字节扇区为单位）              |      |
| `i_block`       | `uint32_t` | 60         | 15 个块指针数组（12 个直接，3 个间接）                   |      |

### 2.2 数据块寻址：三级索引机制

**挑战**

如何用一个固定大小的 inode 结构来表示一个大小可能从几字节到几百 GB 不等的文件？

**解决方案：`i_block`数组的混合寻址模式**

`i_block`数组的设计是解决这个问题的关键。它采用了一种混合策略，兼顾了小文件的访问效率和大文件的存储能力。

- **直接块 (`i_block[0-11]`)**: 数组的前 12 个指针直接存储了文件数据块的块号。对于小文件（小于等于 $12\times 4\text{KB}=48\text{KB}$），这提供了最快的访问速度，因为只需一次磁盘读取（读取 inode）即可获得所有数据块的位置。
- **一级间接块 (`i_block`)**: 第 13 个指针指向一个“索引块”。这个索引块本身不存储文件数据，而是存储一个块号数组，数组中的每个元素才是真正的数据块块号。这引入了一层间接寻址。对于一个 4KB 的块和 4 字节的块号，一个一级间接块可以指向 1024 个数据块（$4096/4=1024$），额外提供了 4MB 的寻址空间。
- **二级间接块 (`i_block`)**: 第 14 个指针指向一个“二级索引块”。这个块里的每个条目指向一个一级索引块。这引入了两层间接寻址，寻址能力大大增强。
- **三级间接块 (`i_block`)**: 第 15 个指针指向一个“三级索引块”，其每个条目指向一个二级索引块。这提供了三层间接寻址，使得文件系统能够支持 TB 级别的超大文件。

**性能与容量的权衡曲线**

`i_block`数组采用 12/1/1/1 的指针分配比例，这并非一个随意的选择，而是一个经过精心权衡的工程决策。在典型的 Unix 类系统中，绝大多数文件都是非常小的。通过为直接块分配 12 个指针，SimpleFS 优化了最常见的场景，确保了小文件（最大 48KB）的访问效率。当文件大小增长时，系统才开始为间接寻址付出额外的磁盘 I/O 开销（读取索引块）。这是一种典型的“为使用的功能付费”的设计哲学，它在保证巨大寻址能力的同时，没有牺牲常见小文件操作的性能。

**表 4：各级索引下的文件大小限制**

下表将抽象的多级索引概念具体化和量化。它是验证实现正确性的有力工具。例如，当写入一个 50KB 的文件时，此表明确指出代码逻辑必须已经分配并使用了一级间接块。它为边界条件测试提供了清晰的目标。

| 索引级别 | inode 指针      | 可寻址块数                                  | 新增大小 | 累积最大文件大小 |
| -------- | --------------- | ------------------------------------------- | -------- | ---------------- |
| 直接     | `i_block[0-11]` | 12                                          | 48 KB    | 48 KB            |
| 一级间接 | `i_block`       | 1024                                        | 4 MB     | 4.048 MB         |
| 二级间接 | `i_block`       | $1024^2=1,048,576$         | 4 GB     | $≈4.004$GB     |
| 三级间接 | `i_block`       | $1024^3=1,073,741,824$ | 4 TB     | $≈4.004$TB     |

导出到 Google 表格

### 2.3 目录与文件表示

- **文件 (File)**：在 SimpleFS 中，一个普通文件就是一个类型为`S_IFREG`的 inode，以及由该 inode 的`i_block`数组指向的一系列数据块的集合。
- **目录 (Directory)**：一个目录则是一个类型为`S_IFDIR`的 inode。与普通文件不同，它的数据块中存储的不是用户数据，而是一个结构化的、由`SimpleFS_DirEntry`条目组成的列表。
- **链接 (Link)**：
  - **硬链接 (Hard Link)**：硬链接并非一种文件类型，而是一个概念。它指的是多个目录项指向了同一个 inode 号。inode 中的`i_links_count`字段就是用来追踪一个文件拥有多少个硬链接的。
  - **符号链接 (Symbolic Link)**：符号链接（或软链接）是一种特殊类型的文件，其类型为`S_IFLNK`。它的数据块中存储的内容是它所指向的另一个文件的路径字符串。作为一种可能的扩展，可以实现“快速符号链接”（fast symlink）优化：如果目标路径很短（例如小于 60 字节），可以直接将其存储在 inode 的`i_block`数组区域，从而节省一个数据块的开销。

### 2.4 目录项：连接文件名与 inode

**功能**

目录项是构建文件系统层次化命名空间的“粘合剂”。它将一个人类可读的**文件名**与一个机器可读的**inode 号**绑定在一起。

**结构**

为了节省磁盘空间，`SimpleFS_DirEntry`被设计为一种可变长度的结构。其中`rec_len`（记录长度）字段是其设计的精髓，它使得一个目录项可以指向下一个目录项的起始位置，从而在数据块内部形成一个链表结构。

**`rec_len`在删除操作中的巧妙应用**

如何从目录中删除一个文件？一种直接但低效的方法是，在删除一个目录项后，将后续所有目录项向前移动以填补空缺。EXT2（以及 SimpleFS）采用了一种更为优雅的方式。假设一个目录块中有 A、B、C 三个连续的目录项，要删除 B。我们只需修改 A 的`rec_len`，使其值等于原本 A 和 B 的`rec_len`之和。这样，在遍历目录时，系统会从 A 直接跳到 C，B 虽然物理上还存在于磁盘上，但逻辑上已经被跳过，成为了不可达的“死亡空间”。这种方法极其高效，因为它只涉及对一个目录项的修改，避免了大量的数据移动。

**C++定义**

（完整定义见附录 A，此处为描述）

```C++
// 位于simplefs.h中
struct SimpleFS_DirEntry {
    uint32_t inode;       // 文件的inode号。若为0，表示该目录项未使用。
    uint16_t rec_len;     // 该目录项记录的总长度。
    uint8_t  name_len;    // 文件名的实际长度（字节）。
    uint8_t  file_type;   // 文件类型 (S_IFREG, S_IFDIR等)。
    char     name;   // 文件名（非空字符结尾，实际长度由name_len决定）。
};
```

## 第三部分：文件系统操作与实现逻辑

本部分是报告的算法核心，为主要的文件系统操作提供伪代码和分步实现逻辑。

### 3.1 路径解析与 inode 查找

**算法**

这是文件系统中最基础、最频繁的操作，几乎所有其他的 FUSE 回调函数都依赖于它。下面是一个`lookup_inode(path)`函数的详细逻辑：

1. **起点**：从根 inode 开始。根 inode 的号码记录在超级块中。
2. **路径分割**：以`/`为分隔符，将输入路径（如`/home/user/file.txt`）分割成组件（`home`, `user`, `file.txt`）。
3. **逐级迭代**：对每一个路径组件执行以下操作： a. **权限检查**：验证当前 inode 是否是一个目录（`S_ISDIR(i_mode)`），并且当前进程是否拥有该目录的**执行**权限。没有执行权限的目录是无法进入的。 b. **读取目录内容**：读取当前目录 inode 所指向的所有数据块。 c. **遍历目录项**：在数据块中，根据`rec_len`遍历`DirEntry`链表。 d. **匹配组件**：如果某个`DirEntry`的`name`字段与当前路径组件匹配，则获取其`inode`号，并将这个新 inode 作为下一次迭代的“当前 inode”。 e. **处理特殊项**：处理`.`（停留在当前 inode）和`..`（需要找到父目录 inode，见下文讨论）。 f. **处理符号链接**：如果在解析过程中遇到的 inode 是一个符号链接，并且它不是路径的最后一个组件，那么需要读取其目标路径，然后根据目标路径是绝对路径还是相对路径，来重新开始或继续解析过程。
4. **返回结果**：当所有路径组件都处理完毕后，返回最终定位到的 inode。如果中途任何一步失败（如找不到组件、权限不足），则返回相应的错误。

**`..`（父目录）问题**

inode 结构本身并不存储其父目录的指针。那么，当解析路径遇到`..`时，如何找到父目录的 inode 呢？标准解决方案是利用每个目录（根目录除外）的数据块中都包含一个名为`..`的目录项，该目录项记录了父目录的 inode 号。因此，处理`..`的逻辑就是：在当前目录的数据块中查找名为`..`的目录项，并获取其 inode 号。根目录是一个特例，它的`..`目录项指向它自己。

### 3.2 文件与目录的生命周期操作

对于`mknod`（创建文件）、`mkdir`（创建目录）、`unlink`（删除文件）、`rmdir`（删除目录）等操作，我们将提供详细的分步逻辑，以展示元数据更新的连锁反应。

**示例：`mknod(path, mode)`的实现逻辑**

1. 调用`lookup_inode(dirname(path))`来获取父目录的 inode。
2. 检查当前进程对父目录是否拥有**写**权限。
3. 调用`alloc_inode()`来获取一个新的、空闲的 inode 号。这个过程可能涉及扫描 GDT 以找到有空闲 inode 的块组，然后读取该组的 inode 位图，找到一个为 0 的位，将其置 1，并写回位图。
4. 将新分配的 inode 写入 inode 表：
   - 设置`i_mode = mode`（包含文件类型和权限）。
   - 从 FUSE 上下文中获取`uid`和`gid`，设置`i_uid`和`i_gid`。
   - 设置`i_links_count = 1`。
   - 设置`i_size = 0`。
   - 设置`i_atime`, `i_mtime`, `i_ctime`为当前时间。
5. 调用`add_dir_entry(parent_inode, basename(path), new_inode_num)`。这个过程需要在父目录的数据块中找到足够的空间来写入新的目录项。如果空间不足，可能需要为父目录分配一个新的数据块。
6. 原子地更新并写回所有受影响的元数据：父目录的 inode（如果其大小或链接数改变）、新文件的 inode、inode 位图、块位图（如果为父目录分配了新块）、GDT 中的空闲计数、以及超级块中的全局空闲计数。

### 3.3 数据 I/O 操作：`read`和`write`

本节将重点关注与多级索引的交互。

**辅助函数**

为了实现清晰的读写逻辑，建议实现以下两个核心辅助函数：

- `map_logical_to_physical_block(inode, logical_block_index)`：此函数是寻址的核心。它接收一个 inode 和一个逻辑块索引（如文件的第 0 块、第 1000 块），然后根据索引值，在 inode 的`i_block`数组中进行查找。它包含了处理直接、一级、二级和三级间接指针的完整`if/else if`逻辑，最终返回一个物理块号。
- `allocate_block_for_write(inode, logical_block_index)`：此函数确保在写入时，目标逻辑块已经有对应的物理块。如果物理块不存在，它会负责分配新块，并在必要时创建和链接中间的各级索引块。

**写入逻辑**

`write`操作的逻辑需要特别注意。对于只覆盖块中一部分数据的“部分写”，需要执行一个“读-改-写”周期：先将整个物理块读入内存缓冲区，修改需要更新的部分，然后再将整个缓冲区写回磁盘。对于覆盖整个块的“完整写”，则可以跳过读取步骤，直接将用户数据写入，这是一个重要的性能优化。

### 3.4 元数据与属性操作

- **`getattr`**: 这是一个相对直接的操作。它首先调用`lookup_inode`找到目标 inode，然后简单地将 inode 结构中的字段（`i_mode`, `i_size`, `i_uid`等）复制到 FUSE 提供的`stat`结构体中。
- **`truncate`**: 这是一个复杂的操作，需要处理两种情况：
  - **缩小文件**：需要释放文件尾部多余的数据块。这要求从后向前遍历`i_block`指针，释放数据块，并可能级联释放不再需要的各级索引块。这是一个需要非常小心处理以避免资源泄露的操作。
  - **扩大文件**：可以通过写入零字节的方式来实现，这实际上是在文件尾部创建了一个“空洞”（sparse file）。或者，也可以选择实际分配填满零的块。

## 第四部分：安全与系统集成

### 4.1 实现 Linux 权限模型

**FUSE 上下文**

FUSE 框架提供了一个至关重要的函数`fuse_get_context()`。每次回调函数被调用时，都可以通过这个函数获取发起系统调用的进程的`uid`（用户 ID）、`gid`（组 ID）和`pid`（进程 ID）。

**权限检查算法**

一个名为`check_access(inode, requested_perm)`的权限检查函数是安全模型的核心。其逻辑如下：

1. 调用`fuse_get_context()`获取当前进程的`uid`和`gid`。
2. **特权用户检查**：如果`context.uid == 0`（即 root 用户），则立即授予所有权限，跳过后续检查。
3. **所有者检查**：如果`context.uid == inode.i_uid`，则根据`inode.i_mode`中的“用户”（user）权限位（r, w, x）来判断是否满足`requested_perm`。
4. **组检查**：如果上一步不满足，但`context.gid == inode.i_gid`，则根据`inode.i_mode`中的“组”（group）权限位来判断。
5. **其他用户检查**：如果以上都不满足，则根据`inode.i_mode`中的“其他”（other）权限位来判断。
6. 如果权限满足，则返回成功；否则，返回`-EACCES`（权限被拒绝）错误码。

**内核检查 vs. 用户空间检查**

FUSE 提供了一个名为`default_permissions`的挂载选项。如果启用此选项，FUSE 内核模块会根据我们通过`getattr`提供的 inode 属性（`i_mode`, `i_uid`, `i_gid`）自动执行上述标准的权限检查，从而免去我们在每个操作中手动检查的麻烦。然而，对于教学目的，手动实现`check_access`函数能提供对 Linux 权限模型更深刻的理解和更精细的控制。因此，本项目推荐手动实现权限检查。

### 4.2 通过 FUSE 与内核对接

本节是关于 FUSE C++ API 的实践指南。

**`fuse_operations`结构体**

这是 FUSE 的核心。我们需要声明并初始化一个`fuse_operations`类型的结构体变量，将其中的函数指针（如`.getattr`, `.readdir`, `.mknod`等）指向我们自己实现的 C++函数。

**`main`函数**

`main.cpp`中的`main`函数是整个守护进程的入口。一个典型的`main`函数模板会执行以下步骤：解析 FUSE 的命令行参数，打开底层的设备文件，初始化文件系统的全局上下文（如读取超级块），最后调用`fuse_main`启动 FUSE 的主事件循环，此后程序将阻塞并等待来自内核的回调请求。

**表 5：核心 FUSE 操作映射表**

此表是项目的实施路线图。它将用户在 shell 中执行的常见命令与我们需要在 C++中实现的 FUSE 回调函数清晰地对应起来，将复杂的实现任务分解为一系列可管理、可测试的单元。

| 用户操作/命令      | VFS 调用                         | FUSE 回调函数       | SimpleFS 实现逻辑摘要                                                                                          | 引用 |
| ------------------ | -------------------------------- | ------------------- | -------------------------------------------------------------------------------------------------------------- | ---- |
| `ls -l /dir`       | `stat()`,`opendir()`,`readdir()` | `getattr`,`readdir` | `getattr`: 查找 inode，填充`stat`结构。`readdir`: 查找目录 inode，遍历其数据块，为每个目录项调用 filler 函数。 |      |
| `touch /file`      | `creat()`或`open(O_CREAT)`       | `mknod`(或`create`) | 分配新 inode，在父目录中添加目录项。                                                                           |      |
| `mkdir /dir`       | `mkdir()`                        | `mkdir`             | 类似于`mknod`，但 inode 类型为目录，需更新链接计数，并创建`.`和`..`目录项。                                    |      |
| `rm /file`         | `unlink()`                       | `unlink`            | 移除目录项，将 inode 链接数减 1，若减为 0 则释放 inode 和所有数据块。                                          |      |
| `cat /file`        | `open()`,`read()`                | `open`,`read`       | `open`: 检查读权限。`read`: 将逻辑偏移映射到物理块，从磁盘读取数据并复制到用户缓冲区。                         |      |
| `echo "x" > /file` | `open()`,`write()`               | `open`,`write`      | `open`: 检查写权限。`write`: 映射逻辑偏移，必要时分配新块，将用户数据写入磁盘。                                |      |

## 第五部分：系统工具与项目实施指南

### 5.1 `mkfs.simplefs`格式化工具

这是与文件系统本身同等重要的一个工具。它是一个独立的命令行程序，负责将一个裸设备（或磁盘镜像文件）初始化为 SimpleFS 格式。

**实现逻辑**

1. **打开设备**：接收一个设备路径作为参数，并以读写模式打开它。
2. **计算几何参数**：根据设备大小和预设参数（如每组块数），计算出文件系统的总体布局：总块数、总组数、每组 inode 数等。
3. **构建元数据**：在内存中构建超级块和所有块组的描述符。
4. **写入元数据**：将内存中的超级块和 GDT 写入到设备上的正确偏移位置。
5. **初始化位图**：为每个块组生成并写入块位图和 inode 位图。在位图中，所有元数据（超级块、GDT、位图自身、inode 表）占用的块需要被标记为“已使用”（置 1）。
6. **初始化 inode 表**：将所有 inode 表区域清零。
7. **创建根目录**：这是格式化的关键一步。

    a. 分配 inode 号为 2 的 inode。  
    b. 分配一个数据块作为根目录的内容存储区。  
    c. 在该数据块中，写入两个目录项：`.`（指向 inode 2）和`..`（也指向 inode 2）。  
    d. 在 inode 表中，初始化 inode 2 的各个字段（模式为目录、链接数为 2 等）。  
    e. 更新所有相关的元数据：将 inode 2 和其数据块在位图中标记为已用，更新 GDT 和超级块中的空闲计数。

### 5.2 推荐的 C++源代码结构

为了有效管理项目的复杂性，建议采用清晰的模块化代码结构：

- `simplefs.h`: 定义核心的磁盘数据结构（`SuperBlock`, `Inode`等）。此头文件将被`mkfs`和 FUSE 守护进程共享。
- `disk_io.cpp/.h`: 封装底层的块读写操作，提供如`read_block(block_num, buffer)`和`write_block(block_num, buffer)`的接口。
- `metadata.cpp/.h`: 实现元数据管理功能，如`alloc_inode()`, `free_inode()`, `alloc_block()`, `free_block()`，主要负责位图操作。
- `filesystem.cpp/.h`: 实现文件系统的高级操作逻辑，如创建文件、读取数据等。它会调用`disk_io`和`metadata`模块的功能。
- `fuse_ops.cpp`: FUSE 回调函数的具体实现。这些函数作为`filesystem`模块的薄封装层，负责处理 FUSE 特有的数据结构转换和上下文获取。
- `main.cpp`: FUSE 守护进程的主入口点。
- `mkfs.cpp`: 格式化工具的独立源码。

### 5.3 全面的测试与验证策略

一个健壮的文件系统离不开严格的测试。

**单元测试**

在不启动 FUSE 的情况下，对各个模块的核心函数进行独立测试。例如，编写测试用例验证`metadata.cpp`中的位图分配和释放逻辑是否准确无误。

**功能测试（通过 Shell 脚本）**

编写一系列自动化的 shell 脚本，在挂载的文件系统上执行标准命令，并验证其行为和结果。

- **基本操作**：`touch`, `mkdir`, `ls -lai`, `rm`, `rmdir`。
- **数据完整性**：`echo "some data" > file; diff file <(echo "some data")`。
- **边界条件**：创建空文件；写满一个块；写一个恰好跨越直接块到间接块边界的文件（如 48KB 到 49KB）。
- **链接测试**：`ln`（硬链接）和`ln -s`（符号链接）的行为，包括检查链接数和解析符号链接。
- **权限测试**：使用`su`或`sudo -u`切换用户，尝试访问无权限的文件或目录，验证是否返回`EACCES`。

**压力测试**

- **大量文件**：在一个目录中创建数千个文件，测试目录操作的性能和可扩展性。
- **深度目录**：创建非常深的嵌套目录结构（如`a/b/c/.../z`）。
- **大文件**：创建一个 GB 级别的单个大文件，以确保所有三级间接寻址逻辑都能正确工作。
- **磁盘耗尽**：将分区写满，验证后续的写操作是否正确返回`ENOSPC`（设备上没有空间）错误。

**一致性检查**

由于没有日志系统，SimpleFS 在意外断电后可能会出现元数据不一致。可以设计一个简单的`fsck.simplefs`工具。该工具以只读方式扫描整个文件系统，检查诸如“位图中标记为已用的块，但没有任何 inode 指向它”（块泄露）或“inode 链接数为正，但没有任何目录项指向它”（孤儿 inode）等问题，并报告这些不一致性。这不仅是一个有用的工具，也加深了对日志系统重要性的理解。

## 附录 A：参考头文件 (`simplefs.h`)

```C++
#pragma once

#include <cstdint>
#include <ctime>

// 使用#pragma pack(1)确保结构体在内存中紧密排列，与磁盘布局一致
#pragma pack(1)

// 定义一些常量
constexpr uint16_t SIMPLEFS_MAGIC = 0x5350;
constexpr uint32_t BLOCK_SIZE = 4096;
constexpr uint32_t ROOT_INODE_NUM = 2;
constexpr uint32_t INODE_SIZE = 128;
constexpr uint32_t NUM_DIRECT_BLOCKS = 12;
constexpr uint32_t NUM_INDIRECT_BLOCKS = 1;
constexpr uint32_t NUM_D_INDIRECT_BLOCKS = 1;
constexpr uint32_t NUM_T_INDIRECT_BLOCKS = 1;
constexpr uint32_t INODE_BLOCK_PTRS = NUM_DIRECT_BLOCKS + NUM_INDIRECT_BLOCKS + NUM_D_INDIRECT_BLOCKS + NUM_T_INDIRECT_BLOCKS; // 15
constexpr uint32_t MAX_FILENAME_LEN = 255;

// 文件类型常量 (与Linux stat.h中的S_IFxxx对应)
constexpr uint16_t S_IFMT   = 0xF000; // bit mask for the file type bit fields
constexpr uint16_t S_IFSOCK = 0xC000; // socket
constexpr uint16_t S_IFLNK  = 0xA000; // symbolic link
constexpr uint16_t S_IFREG  = 0x8000; // regular file
constexpr uint16_t S_IFBLK  = 0x6000; // block device
constexpr uint16_t S_IFDIR  = 0x4000; // directory
constexpr uint16_t S_IFCHR  = 0x2000; // character device
constexpr uint16_t S_IFIFO  = 0x1000; // FIFO

// 超级块结构
struct SimpleFS_SuperBlock {
    uint16_t s_magic;               // 魔数
    uint32_t s_inodes_count;        // inode总数
    uint32_t s_blocks_count;        // 块总数
    uint32_t s_free_blocks_count;   // 空闲块数
    uint32_t s_free_inodes_count;   // 空闲inode数
    uint32_t s_first_data_block;    // 第一个数据块的块号
    uint32_t s_log_block_size;      // log2(块大小) - 10, e.g., 4KB -> log2(4096) - 10 = 2
    uint32_t s_blocks_per_group;    // 每组的块数
    uint32_t s_inodes_per_group;    // 每组的inode数
    uint32_t s_mtime;               // 最后挂载时间
    uint32_t s_wtime;               // 最后写入时间
    uint16_t s_mnt_count;           // 挂载次数
    uint16_t s_max_mnt_count;       // 最大挂载次数
    uint16_t s_state;               // 文件系统状态
    uint16_t s_errors;              // 错误处理方式
    uint32_t s_first_ino;           // 第一个非保留inode号
    uint16_t s_inode_size;          // inode结构大小
    uint16_t s_block_group_nr;      // 本超级块所在的块组号
    uint32_t s_root_inode;          // 根目录inode号
    uint8_t  s_padding;        // 填充至1024字节
};
static_assert(sizeof(SimpleFS_SuperBlock) == 1024, "SuperBlock size must be 1024 bytes");

// 块组描述符结构
struct SimpleFS_GroupDesc {
    uint32_t bg_block_bitmap;        // 块位图的块号
    uint32_t bg_inode_bitmap;        // inode位图的块号
    uint32_t bg_inode_table;         // inode表的起始块号
    uint16_t bg_free_blocks_count;   // 本组空闲块数
    uint16_t bg_free_inodes_count;   // 本组空闲inode数
    uint16_t bg_used_dirs_count;     // 本组目录数
    uint8_t  bg_padding;         // 填充至32字节
};
static_assert(sizeof(SimpleFS_GroupDesc) == 32, "GroupDesc size must be 32 bytes");

// Inode结构
struct SimpleFS_Inode {
    uint16_t i_mode;                // 文件类型和权限
    uint16_t i_uid;                 // 所有者UID
    uint32_t i_size;                // 文件大小（字节）
    uint32_t i_atime;               // 访问时间
    uint32_t i_ctime;               // 创建/状态改变时间
    uint32_t i_mtime;               // 修改时间
    uint32_t i_dtime;               // 删除时间
    uint16_t i_gid;                 // 所有者GID
    uint16_t i_links_count;         // 硬链接数
    uint32_t i_blocks;              // 文件占用的块数（512字节单位）
    uint32_t i_flags;               // 文件标志
    uint32_t i_block[INODE_BLOCK_PTRS]; // 块指针数组 (15 * 4 = 60 bytes)
    uint8_t  i_padding[962];
};
static_assert(sizeof(SimpleFS_Inode) == 128, "Inode size must be 128 bytes");

// 目录项结构
struct SimpleFS_DirEntry {
    uint32_t inode;                 // inode号 (0表示未使用)
    uint16_t rec_len;               // 本记录的长度
    uint8_t  name_len;              // 文件名长度
    uint8_t  file_type;             // 文件类型
    char     name[MAX_FILENAME_LEN + 1]; // 文件名
};
// 注意：DirEntry是变长的，sizeof不能直接反映其在磁盘上的真实大小

#pragma pack()
```
