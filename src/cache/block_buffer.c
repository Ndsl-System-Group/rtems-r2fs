#include "block_buffer.h"

#include <memory.h>


// TODO 该部分依赖底层 IO 层的接口设计。
int blockBufferInit(BlockBuffer *this)
{
    return 0;
}

void blockBufferDestroy(BlockBuffer *this)
{
}

void blockBufferCopy(BlockBuffer *this, const BlockBuffer *other)
{
    memcpy(this->buffer, other->buffer, BLOCK_BUFFER_SIZE);
}

char *blockBufferGetPtr(BlockBuffer *this)
{
    return this->buffer;
}

void blockBufferCopyContentFromBuf(BlockBuffer *this, const char *src)
{
}

int blockBufferReadFromLpa(BlockBuffer *this, struct comm_dev *dev, uint32_t lpa)
{
    return 0;
}

int blockBufferWriteToLpaSync(BlockBuffer *this, struct comm_dev *dev, uint32_t lpa)
{
    return 0;
}

// int blockBufferWriteToLpaAsync(BlockBuffer *this, struct comm_dev *dev, uint32_t lpa, comm_async_cb_func cbFunc, void *cbArg)
// {
// }
