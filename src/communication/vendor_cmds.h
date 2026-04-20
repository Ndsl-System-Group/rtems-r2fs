#ifndef _VENDOR_CMDS_H_
#define _VENDOR_CMDS_H_

#include "fs/fs.h"


typedef struct migrate_task
{
    uint32_t migrate_lpa_cnt;
    struct RtfsSitEntry victim_seg_info;
    uint64_t migrate_dst_lpa;
    uint64_t migrate_src_lpa;
} __attribute__((packed)) migrate_task;

// 例：从根目录开始查询 /home/huawei/hisilicon/ssd，startIno 为 / 的 ino，path 为 home/huawei/hisilicon/ssd，depth 为 4。
typedef struct path_lookup_task
{
    u32 start_ino; // 起始目录的 ino。
    u32 pathlen;   // 路径字符串的长度，不含 0 结束符。
    u32 depth;     // 路径的级数。
    char path[0];  // 路径字符串，不含 0 结束符。
} path_lookup_task;

#define MAX_PATH_DEPTH ((4096 - 12) / sizeof(u32))

typedef struct path_lookup_result
{
    u64 dentry_blkidx; // 若目标文件存在，该项表示对应 dentry 在父目录文件的哪个 block 中(块偏移)。若目标文件不存在，但其父目录存在，并且 dentryBitPos 不为 INVALID_DENTRY_BITPOS，该字段表示若要创建此文件，新 dentry 所在的 block。

    u32 dentry_bitpos; // 若目标文件存在，该项表示对应 dentry 在 block 中的位置（slot 号）。若目标文件不存在，但其父目录存在，此字段表示若要创建此文件，新 dentry 在 block 中的偏移。

    u32 path_inos[MAX_PATH_DEPTH]; // 路径各级的 ino，INVALID_NID 表示该级文件不存在。

    char parent_dir_node_page[4096]; // 若目标文件存在，该项是索引对应 dentry 所在的 block 的 node page 的内容。
    char parent_dir_data_page[4096]; // 若目标文件存在，该项是存放对应 dentry 的 data block 的内容。
} path_lookup_result;

typedef struct filemapping_search_task
{
    u32 ino;
    u32 nid_to_start;
    u64 file_blk_offset;
    u8 return_all_Level;
    u8 align[3];
} filemapping_search_task;

#define VENDOR_SET_OPCODE 0xc5
#define VENDOR_GET_OPCODE 0xc2


#endif
