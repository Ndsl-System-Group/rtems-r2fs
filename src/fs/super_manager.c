#include "fs/super_manager.h"

#include <assert.h>
#include <errno.h>

#include "fs.h"
#include "fs/fs_manager.h"
#include "fs/nat_utils.h"
#include "cache/sit_nat_cache.h"
#include "uthash/utarray.h"
#include "sit_utils.h"
#include "utils/rtfs_log.h"

typedef struct LpaAllocContext
{
    uint32_t *cur_seg_id_;
    uint32_t *cur_seg_off_;
    UT_array *uncommit_segs;
} LpaAllocContext;

void LpaAllocContextInit(LpaAllocContext *this, uint32_t *cur_seg_id, uint32_t *cur_seg_off)
{
    this->cur_seg_id_ = cur_seg_id;
    this->cur_seg_off_ = cur_seg_off;
    UT_icd int_icd = {sizeof(int), NULL, NULL, NULL};
    utarray_new(this->uncommit_segs, &int_icd);
}

typedef struct super_manager
{
    file_system_manager *fs_manager_;
    RtfsSuperBlock *super_block_;
    UT_array *uncommit_node_segs, *uncommit_data_segs;
}super_manager;

void SuperManagerInit(super_manager *this, file_system_manager *fs_manager){
    this->fs_manager_ = fs_manager;
    this->super_block_ = FileSystemManagerGetSuperBlkMem(fs_manager);
    UT_icd int_icd = {sizeof(int), NULL, NULL, NULL};
    utarray_new(this->uncommit_node_segs, &int_icd);
    utarray_new(this->uncommit_data_segs, &int_icd);
}

/******************************* Nid ******************************************** */

uint32_t SuperManagerAllocNid(super_manager *this, uint32_t ino, bool is_inode)
{
    uint32_t nid = this->super_block_->next_free_nid;
    if (nid == INVALID_NID) {
        return INVALID_NID;
    }

    NatLpaMapping nat_lpa_mapping;
    natLpaMappingInit(&nat_lpa_mapping, this->fs_manager_, this->super_block_->nat_blkaddr, this->super_block_->segment_count_nat);
    NatNidPos nat_nid_pos = natGetNidPos(&nat_lpa_mapping, nid);

    SitNatCache* nat_cache = FileSystemManagerGetNatCache(this->fs_manager_);
    if (nat_cache == NULL){
        return INVALID_NID;
    }
    SitNatCacheEntryHandle nat_cache_handle = sitNatCacheGet(nat_cache, nat_nid_pos.lpa);
    SitNatCacheEntry* nat_cache_entry = nat_cache_handle.entry;
    struct RtfsNatBlock* nat_block = (struct RtfsNatBlock*)nat_cache_entry->cache.buffer; 
    struct RtfsNatEntry* nat_entry = &nat_block->entries[nat_nid_pos.idx];

    if (nat_entry->block_addr == INVALID_LPA) {
        return INVALID_NID;
    }

    uint32_t next_nid = nat_entry->block_addr;
    this->super_block_->next_free_nid = next_nid;

    if(is_inode){
        nat_entry->ino = nid;
    }else{
        nat_entry->ino = ino;
    }
    nat_entry->block_addr = INVALID_LPA;
    
    // TODO: 添加超级块日志
    return nid;
}

void SuperManagerFreeNid(super_manager *this, uint32_t nid)
{
    NatLpaMapping nat_lpa_mapping;
    natLpaMappingInit(&nat_lpa_mapping, this->fs_manager_, this->super_block_->nat_blkaddr, this->super_block_->segment_count_nat);
    NatNidPos nat_nid_pos = natGetNidPos(&nat_lpa_mapping, nid);

    SitNatCache* nat_cache = FileSystemManagerGetNatCache(this->fs_manager_);
    if (nat_cache == NULL){
        return;
    }
    SitNatCacheEntryHandle nat_cache_handle = sitNatCacheGet(nat_cache, nat_nid_pos.lpa);
    SitNatCacheEntry* nat_cache_entry = nat_cache_handle.entry;
    struct RtfsNatBlock* nat_block = (struct RtfsNatBlock*)nat_cache_entry->cache.buffer;
    struct RtfsNatEntry* nat_entry = &nat_block->entries[nat_nid_pos.idx];

    nat_entry->ino = INVALID_NID;
    nat_entry->block_addr = this->super_block_->next_free_nid;
    this->super_block_->next_free_nid = nid;

    // TODO: 添加超级块日志
}

/******************************* segment ******************************************** */

LpaAllocContext SuperManagerGetLpaCtx(super_manager *this, bool is_node){
    LpaAllocContext lpa_ctx;
    if(is_node){
        LpaAllocContextInit(&lpa_ctx, &this->super_block_->current_node_segment_id, &this->super_block_->current_node_segment_blkoff);
    }else{
        LpaAllocContextInit(&lpa_ctx, &this->super_block_->current_data_segment_id, &this->super_block_->current_data_segment_blkoff);
    }
    return lpa_ctx;
}

uint32_t SuperManagerAllocSegment(super_manager *this)
{
    if(this->super_block_->free_segment_count <= 0){
        RTFS_ERRNO_LOG(RTFS_LOG_ERROR, errno, "no free segment");
        return INVALID_SEGID;
    }

    uint32_t seg_id = this->super_block_->first_free_segment_id;
    SitOperator sit_operator;
    sitOperatorInit(&sit_operator,
                    this->fs_manager_,
                    this->super_block_->segment0_blkaddr,
                    this->super_block_->segment_count,
                    this->super_block_->sit_blkaddr,
                    this->super_block_->segment_count_sit);
    SitPos sit_pos = sitGetSegIdPosInSit(&sit_operator, seg_id);
    SitNatCache* sit_cache = FileSystemManagerGetSitCache(this->fs_manager_);
    SitNatCacheEntryHandle sit_cache_handle = sitNatCacheGet(sit_cache, sit_pos.sitLpa);
    SitNatCacheEntry* sit_cache_entry = sit_cache_handle.entry;
    struct RtfsSitBlock* sit_block = (struct RtfsSitBlock*)sit_cache_entry->cache.buffer;
    struct RtfsSitEntry* sit_entry = &sit_block->entries[sit_pos.idx];
    
    uint32_t next_segid = GET_NEXT_SEG(sit_entry);
    this->super_block_->first_free_segment_id = next_segid;
    this->super_block_->free_segment_count--;

    // TODO: 添加超级块日志

    return seg_id;
}

uint32_t SuperManagerAllocLpaInner(super_manager *this, LpaAllocContext *ctx)
{
    // TODO：开启日志容器
    if(*ctx->cur_seg_off_ >= BLOCK_PER_SEGMENT){
        utarray_push_back(ctx->uncommit_segs, ctx->cur_seg_id_);
        uint32_t new_seg_id = SuperManagerAllocSegment(this);
        *ctx->cur_seg_id_ = new_seg_id;
        *ctx->cur_seg_off_ = 0;
        // TODO: 添加超级块日志
    }

    SitOperator sit_operator;
    sitOperatorInit(&sit_operator,
                    this->fs_manager_,
                    this->super_block_->segment0_blkaddr,
                    this->super_block_->segment_count,
                    this->super_block_->sit_blkaddr,
                    this->super_block_->segment_count_sit);
    uint32_t lpa = sitGetFirstLpaOfSegId(&sit_operator, *ctx->cur_seg_id_);

    // TODO: 添加超级块日志

    sitValidateLpa(&sit_operator, lpa); // ! sit_operator 不是栈对象吗？
    return lpa;
}

uint32_t SuperManagerAllocNodeLpa(super_manager *this)
{
    LpaAllocContext ctx;
    LpaAllocContextInit(&ctx, &this->super_block_->current_node_segment_id, &this->super_block_->current_node_segment_blkoff);
    return SuperManagerAllocLpaInner(this, &ctx);
}

uint32_t SuperManagerAllocDataLpa(super_manager *this)
{
    LpaAllocContext ctx;
    LpaAllocContextInit(&ctx, &this->super_block_->current_data_segment_id, &this->super_block_->current_data_segment_blkoff);
    return SuperManagerAllocLpaInner(this, &ctx);
}
