#include "srmap_utils.h"

#include "fs/fs.h"
#include "fs/fs_manager.h"
#include "utils/rtfs_log.h"
#include "utils/io_utils.h"

#include <assert.h>


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
    this->srmapCache = kh_init(khsc);
    this->dirtyBlks = kh_init(khdb);
    this->srmapStartLpa = fileSystemManagerGetSuperBlkMem(fsManager)->srmap_blkaddr;
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

    khiter_t k = kh_get(khdb, this->dirtyBlks, pos.srmapBlkLpa);
    if (kh_end(this->dirtyBlks) == k)
    {
        int res;
        kh_put(khdb, this->dirtyBlks, pos.srmapBlkLpa, &res);
    }
}

void srmapUtilsWriteSrmapOfNode(SrmapUtils *this, uint32_t nodeLpa, uint32_t nid)
{
    SrmapPos pos = srmapUtilsGetSrmapPosOfLpa(this, nodeLpa);
    BlockBuffer *blk = srmapUtilsGetSrmapBlk(this, pos.srmapBlkLpa);
    struct RtfsSummaryBlock *srmapBlk = (struct RtfsSummaryBlock *)blockBufferGetPtr(blk);
    srmapBlk->entries[pos.idx].nid = nid;

    RTFS_LOG(RTFS_LOG_INFO, "set srmap of node lpa %u: nid=%u", nodeLpa, nid);

    khiter_t k = kh_get(khdb, this->dirtyBlks, pos.srmapBlkLpa);
    if (kh_end(this->dirtyBlks) == k)
    {
        int res;
        kh_put(khdb, this->dirtyBlks, pos.srmapBlkLpa, &res);
    }
}

void srmapUtilsWriteDirtySrmapSync(SrmapUtils *this)
{
    AsyncVecioSynchronizer syn;
    asyncVecioSynchronizerInit(&syn, kh_size(this->dirtyBlks));

    khiter_t k;
    for (k = kh_begin(this->dirtyBlks); k != kh_end(this->dirtyBlks); ++k)
    {
        if (!kh_exist(this->dirtyBlks, k)) continue;

        uint32_t lpa = kh_key(this->dirtyBlks, k);

        khiter_t ck = kh_get(khsc, this->srmapCache, lpa);
        assert(kh_end(this->srmapCache) != ck);

        BlockBuffer *blk = &kh_value(this->srmapCache, ck);

        blockBufferWriteToLpaAsync(blk, fileSystemManagerGetDevice(this->fsManager), lpa, asyncVecioSynchronizerGenericCallback, &syn);
    }

    (void)asyncVecioSynchronizerWaitCplt(&syn);
    asyncVecioSynchronizerDestroy(&syn);
}

void srmapUtilsClearCache(SrmapUtils *this)
{
    khiter_t k;
    for (k = kh_begin(this->srmapCache); k != kh_end(this->srmapCache); ++k)
    {
        if (!kh_exist(this->srmapCache, k)) continue;

        blockBufferDestroy(&kh_value(this->srmapCache, k));
    }

    kh_destroy(khsc, this->srmapCache);
    kh_destroy(khdb, this->dirtyBlks);

    this->srmapCache = NULL;
    this->dirtyBlks = NULL;
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
    comm_dev *dev;
    khiter_t k = kh_get(khsc, this->srmapCache, lpa);
    if (kh_end(this->srmapCache) != k) return &kh_value(this->srmapCache, k); // 找到直接返回 BlockBuffer。

    int res;
    k = kh_put(khsc, this->srmapCache, lpa, &res);
    BlockBuffer *blk = &kh_value(this->srmapCache, k);
    blockBufferInit(blk);
    dev = fileSystemManagerGetDevice(this->fsManager);
    if (dev != NULL) {
        blockBufferReadFromLpa(blk, dev, lpa);
    } else {
        memset(blockBufferGetPtr(blk), 0, BLOCK_BUFFER_SIZE);
    }


    return blk;
}
