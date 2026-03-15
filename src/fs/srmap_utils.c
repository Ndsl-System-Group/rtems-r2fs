#include "srmap_utils.h"

#include "fs/fs.h"
#include "utils/rtfs_log.h"


typedef struct
{
    uint32_t srmapBlkLpa; // SRMAP block 的逻辑地址。
    uint32_t idx;         // block 内索引。
} SrmapPos;


/**
 * @brief 返回 lpa 的反向映射在 srmap 中的 <lpa, idx>。
 */
static SrmapPos srmapUtilsGetSrmapPosOfLpa(SrmapUtils *this, uint32_t lpa);

/**
 * @brief 封装 SrmapCache 不命中时，从 lpa 读取 Srmap Block 到缓存的过程。
 */
static BlockBuffer *srmapUtilsGetSrmapBlk(SrmapUtils *this, uint32_t lpa);


void srmapUtilsInit(SrmapUtils *this, struct file_system_manager *fsManager)
{
    this->fsManager = fsManager;
    this->srmapCache = NULL;
    this->dirtyBlks = NULL;
    // TODO 该部分依赖 file_system_manager 的接口。
    // this->srmapStartLpa = fsManager->fileSystemManagerGetSuperCache()->srmap_blkaddr;
}

void srmapUtilsDestroy(SrmapUtils *this)
{
    srmapUtilsClearCache(this);
}

void srmapUtilsWriteSrmapOfData(SrmapUtils *this, uint32_t dataLpa, uint32_t ino, uint32_t blkoff)
{
    SrmapPos pos = srmapUtilsGetSrmapPosOfLpa(this, dataLpa);
    BlockBuffer *blk = srmapUtilsGetSrmapBlk(this, pos.srmapBlkLpa);
    struct RtfsSummaryBlock *srmapBlk = (struct RtfsSummaryBlock *)blockBufferGetPtr(blk);

    srmapBlk->entries[pos.idx].nid = ino;
    srmapBlk->entries[pos.idx].ofs_in_node = blkoff;

    RTFS_LOG(RTFS_LOG_INFO, "set srmap of data lpa %u: ino=%u, blkoff=%u", dataLpa, ino, blkoff);

    DirtyBlkEntry *dentry = NULL;
    HASH_FIND(hh, this->dirtyBlks, &pos.srmapBlkLpa, sizeof(uint32_t), dentry);
    if (!dentry)
    {
        dentry = (DirtyBlkEntry *)malloc(sizeof(DirtyBlkEntry));
        dentry->lpa = pos.srmapBlkLpa;
        HASH_ADD(hh, this->dirtyBlks, lpa, sizeof(uint32_t), dentry);
    }
}

void srmapUtilsWriteSrmapOfNode(SrmapUtils *this, uint32_t nodeLpa, uint32_t nid)
{
    SrmapPos pos = srmapUtilsGetSrmapPosOfLpa(this, nodeLpa);
    BlockBuffer *blk = srmapUtilsGetSrmapBlk(this, pos.srmapBlkLpa);
    struct RtfsSummaryBlock *srmapBlk = (struct RtfsSummaryBlock *)blockBufferGetPtr(blk);

    srmapBlk->entries[pos.idx].nid = nid;

    RTFS_LOG(RTFS_LOG_INFO, "set srmap of node lpa %u: nid=%u", nodeLpa, nid);

    DirtyBlkEntry *dentry = NULL;
    HASH_FIND(hh, this->dirtyBlks, &pos.srmapBlkLpa, sizeof(uint32_t), dentry);
    if (!dentry)
    {
        dentry = (DirtyBlkEntry *)malloc(sizeof(DirtyBlkEntry));
        dentry->lpa = pos.srmapBlkLpa;
        HASH_ADD(hh, this->dirtyBlks, lpa, sizeof(uint32_t), dentry);
    }
}

void srmapUtilsWriteDirtySrmapSync(SrmapUtils *this)
{
    // TODO
}

void srmapUtilsClearCache(SrmapUtils *this)
{
    SrmapCacheEntry *entry, *tmp;
    HASH_ITER(hh, this->srmapCache, entry, tmp)
    {
        HASH_DEL(this->srmapCache, entry);
        blockBufferDestroy(&entry->blk);
        free(entry);
    }

    DirtyBlkEntry *dentry, *dtmp;
    HASH_ITER(hh, this->dirtyBlks, dentry, dtmp)
    {
        HASH_DEL(this->dirtyBlks, dentry);
        free(dentry);
    }
}


SrmapPos srmapUtilsGetSrmapPosOfLpa(SrmapUtils *this, uint32_t lpa)
{
    SrmapPos pos;
    pos.srmapBlkLpa = this->srmapStartLpa + (lpa / ENTRIES_IN_SUM);
    pos.idx = lpa % ENTRIES_IN_SUM;

    RTFS_LOG(RTFS_LOG_DEBUG, "srmap pos of lpa %u: srmapBlkLpa=%u, idx=%u", lpa, pos.srmapBlkLpa, pos.idx);


    return pos;
}

BlockBuffer *srmapUtilsGetSrmapBlk(SrmapUtils *this, uint32_t lpa)
{
    SrmapCacheEntry *entry = NULL;
    HASH_FIND(hh, this->srmapCache, &lpa, sizeof(uint32_t), entry);
    if (entry) return &entry->blk;

    entry = (SrmapCacheEntry *)malloc(sizeof(SrmapCacheEntry));
    entry->lpa = lpa;
    blockBufferInit(&entry->blk);
    HASH_ADD(hh, this->srmapCache, lpa, sizeof(uint32_t), entry);


    return &entry->blk;
}
