#include "block_buffer.h"


// TODO 该部分依赖底层 IO 层的接口设计。
int blockBufferInit(BlockBuffer *this)
{
    return 0;
}

void blockBufferDestroy(BlockBuffer *this)
{
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
