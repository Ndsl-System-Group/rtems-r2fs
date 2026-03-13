#ifndef _JOURNAL_WRITER_H_
#define _JOURNAL_WRITER_H_

#include "journal_container.h"

#include "uthash/utarray.h"


struct comm_dev;


typedef struct JournalWriter
{
    JournalContainer *curJournal;

    uint64_t startLpa, endLpa; // SSD 上日志区域的起止位置。[startLpa, endLpa)

    UT_array journalBuffer;              // 4 KB 缓存块的列表，按需增长。
    size_t bufferTailIdx, bufferTailOff; // 当前缓存块列表中，使用到的最后一个缓存块下标和块中偏移。

    struct comm_dev *dev;
} JournalWriter;


void journalWriterInit(JournalWriter *this, struct comm_dev *device, uint64_t journalAreaStartLpa, uint64_t journalAreaEndLpa);

/**
 * @brief 设置将要处理的日志。
 */
void journalWriterSetPendingJournal(JournalWriter *this, JournalContainer *journal);

/**
 * @brief 将日志中的日志项收集到写缓存，返回写缓存的块个数。
 */
uint64_t journalWriterCollectPendingJournalToWriteBuffer(JournalWriter *this);

/**
 * @brief 将写缓存写入 SSD（同步，全部写完后返回）。
 * @param cur_tail 当前日志区域 FIFO 的队列尾地址，调用者需保证 SSD 有足够的空间保存本次的日志。
 * @details 异常：io_error 通信层发生错误或者其它系统异常。
 */
void journalWriterWriteToSsd(JournalWriter *this, uint64_t curTail);


#endif
