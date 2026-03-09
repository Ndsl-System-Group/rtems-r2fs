#include "fs/super_manager.h"
#include "fs/fs_manager.h"
#include "fs.h"
#include <assert.h>

typedef struct
{
    uint32_t *curSegId;
    uint32_t *curSegOff;
} LpaAllocContext;

static struct RtfsSuperBlock *superManagerGetSuper(file_system_manager *fs)
{
    return (struct RtfsSuperBlock *)FileSystemManagerGetSuperBlkMem(fs);
}

static LpaAllocContext superManagerGetLpaCtx(file_system_manager *fs, bool isNode)
{
    struct RtfsSuperBlock *sb = superManagerGetSuper(fs);
    LpaAllocContext ctx = {0};

    if (sb == NULL) {
        return ctx;
    }

    if (isNode) {
        ctx.curSegId = &sb->current_node_segment_id;
        ctx.curSegOff = &sb->current_node_segment_blkoff;
    } else {
        ctx.curSegId = &sb->current_data_segment_id;
        ctx.curSegOff = &sb->current_data_segment_blkoff;
    }

    return ctx;
}

static uint32_t superManagerAllocSegment(file_system_manager *fs)
{
    (void)fs;
    return INVALID_SEGID;
}

uint32_t SuperManagerAllocNid(file_system_manager *fs, uint32_t ino, bool is_inode)
{
    (void)ino;
    (void)is_inode;

    struct RtfsSuperBlock *sb = superManagerGetSuper(fs);
    if (sb == NULL) {
        return INVALID_NID;
    }

    uint32_t nid = sb->next_free_nid;
    if (nid == INVALID_NID) {
        return INVALID_NID;
    }

    // TODO: 补齐 NAT 空闲链表维护后，从 NAT 读取 next_free_nid。
    sb->next_free_nid = INVALID_NID;
    return nid;
}

static uint32_t superManagerAllocLpa(file_system_manager *fs, bool isNode)
{
    struct RtfsSuperBlock *sb = superManagerGetSuper(fs);
    LpaAllocContext ctx = superManagerGetLpaCtx(fs, isNode);

    if (sb == NULL || ctx.curSegId == NULL || ctx.curSegOff == NULL) {
        return INVALID_LPA;
    }

    if (*ctx.curSegOff >= BLOCK_PER_SEGMENT) {
        uint32_t newSegId = superManagerAllocSegment(fs);
        if (newSegId == INVALID_SEGID) {
            return INVALID_LPA;
        }
        *ctx.curSegId = newSegId;
        *ctx.curSegOff = 0;
    }

    assert(*ctx.curSegOff < BLOCK_PER_SEGMENT);
    uint32_t lpa = sb->segment0_blkaddr + (*ctx.curSegId) * BLOCK_PER_SEGMENT + (*ctx.curSegOff);
    (*ctx.curSegOff)++;

    // TODO: 补齐 SIT 与 journal 接口后，写入有效块与日志。
    return lpa;
}

uint32_t SuperManagerAllocNodeLpa(file_system_manager *fs)
{
    return superManagerAllocLpa(fs, true);
}

uint32_t SuperManagerAllocDataLpa(file_system_manager *fs)
{
    return superManagerAllocLpa(fs, false);
}
