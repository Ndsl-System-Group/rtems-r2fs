#ifndef _JOURNAL_WRITER_H_
#define _JOURNAL_WRITER_H_

#include "journal/journal_container.h"

#include "cache/block_buffer.h"

#include "klib/kvec.h"


struct comm_dev;


/**
 * @brief Journal 日志写入器。负责将事务日志中的各类日志项整理为可落盘格式，写入内部 4 KB 缓冲区，并按顺序同步写入 SSD Journal 区域。
 *
 * @details 主要职责：
 * 1. 接收当前待处理事务日志；
 * 2. 将日志项去重、整理并编码到写缓存；
 * 3. 维护写缓存块列表及当前写入位置；
 * 4. 按给定 Journal FIFO 尾指针将缓存内容写入 SSD。
 */
typedef struct JournalWriter
{
    /**
     * @brief 当前待处理的事务日志对象。
     */
    JournalContainer *curJournal;

    /**
     * @brief SSD 上日志区域的起止位置。[startLpa, endLpa)。
     */
    uint64_t startLpa, endLpa;

    /**
     * @brief 4 KB 缓存块的列表，按需增长。
     */
    kvec_t(BlockBuffer) journalBuffer;

    /**
     * @brief 当前缓存块列表中，使用到的最后一个缓存块下标和块中偏移。
     */
    size_t bufferTailIdx, bufferTailOff;

    /**
     * @brief 底层通讯设备对象。
     */
    struct comm_dev *dev;
} JournalWriter;


/**
 * @brief 初始化 JournalWriter 对象。
 */
void journalWriterInit(JournalWriter *this, struct comm_dev *dev, uint64_t journalAreaStartLpa, uint64_t journalAreaEndLpa);

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
