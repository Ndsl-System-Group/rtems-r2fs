#include "block_buffer.h"

#include "communication/memory.h"
#include "utils/io_utils.h"
#include "utils/rtfs_exception.h"

#include <memory.h>


int blockBufferInit(BlockBuffer *this)
{
    this->buffer = (char *)comm_alloc_dma_mem(BLOCK_BUFFER_SIZE);
    if (NULL == this->buffer) THROW_FATAL_MESSAGE(EXIT_FAILURE, "blockBufferInit: alloc block buffer failed.");
}

void blockBufferDestroy(BlockBuffer *this)
{
    comm_free_dma_mem(this->buffer);
}

void blockBufferCopy(BlockBuffer *this, const BlockBuffer *other)
{
    memcpy(this->buffer, other->buffer, BLOCK_BUFFER_SIZE);
}

char *blockBufferGetPtr(BlockBuffer *this)
{
    return this->buffer;
}

void blockBufferCopyContentFromBuf(BlockBuffer *this, const char *buf)
{
    memcpy(this->buffer, buf, BLOCK_BUFFER_SIZE);
}

void blockBufferReadFromLpa(BlockBuffer *this, struct comm_dev *dev, uint32_t lpa)
{
    int res = comm_submit_sync_rw_request(dev, this->buffer, LPA_TO_LBA(lpa), LBA_PER_LPA, COMM_IO_READ);
    if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "blockBufferReadFromLpa: read lpa failed.");
}

void blockBufferWriteToLpaSync(BlockBuffer *this, struct comm_dev *dev, uint32_t lpa)
{
    int res = comm_submit_sync_rw_request(dev, this->buffer, LPA_TO_LBA(lpa), LBA_PER_LPA, COMM_IO_WRITE);
    if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "blockBufferWriteToLpaSync: sync write lpa failed.");
}

void blockBufferWriteToLpaAsync(BlockBuffer *this, struct comm_dev *dev, uint32_t lpa, comm_async_cb_func cbFunc, void *cbArg)
{
    int res = comm_submit_async_rw_request(dev, this->buffer, LPA_TO_LBA(lpa), LBA_PER_LPA, cbFunc, cbArg, COMM_IO_WRITE);
    if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "blockBufferWriteToLpaAsync: async write lpa failed.");
}
