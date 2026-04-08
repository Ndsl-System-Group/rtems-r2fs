#include "journal_writer.h"
#include "journal_type.h"


typedef enum JournalOutputState
{
    JOURNAL_OUTPUT_OK,
    JOURNAL_OUTPUT_NO_ENOUGH_BUFFER,
    JOURNAL_OUTPUT_REACH_END
} JournalOutputState;


/**
 * @brief 计算能够写入的日志条目个数的通用算法。
 * @param bufferSize 目标缓存区大小（字节为单位）。
 * @param entrySize 日志条目大小（字节为单位）。
 * @param expectedWriteNum 期望写入的日志条目个数。
 * @return 返回实际能写入的日志条目个数。
 * @details 算法考虑日志项首部，且保证：若不是恰好把缓存写满，则在尾部留下 NOP 空间（除非输入的 bufferSize 本身无法写入 NOP）。留下 NOP 空间指至少留下一个日志项首部长度。
 */
static uint64_t genericCalculateWritableEntryNum(uint64_t bufferSize, uint64_t entrySize, uint64_t expectedWriteNum);


void journalWriterInit(JournalWriter *this, struct comm_dev *dev, uint64_t journalAreaStartLpa, uint64_t journalAreaEndLpa)
{
    this->startLpa = journalAreaStartLpa;
    this->endLpa = journalAreaEndLpa;
    this->curJournal = NULL;
    this->bufferTailIdx = 0;
    this->bufferTailOff = 0;
    this->dev = dev;

    kv_init(this->journalBuffer);
}

void journalWriterSetPendingJournal(JournalWriter *this, JournalContainer *journal)
{
    this->curJournal = journal;
}

// TODO
uint64_t journalWriterCollectPendingJournalToWriteBuffer(JournalWriter *this)
{
}

void journalWriterWriteToSsd(JournalWriter *this, uint64_t curTail)
{
}


uint64_t genericCalculateWritableEntryNum(uint64_t bufferSize, uint64_t entrySize, uint64_t expectedWriteNum)
{
    uint64_t res = 0;
    const uint64_t entryHeaderLen = sizeof(struct MetaJournalEntry);
    uint64_t expectedWriteLen = entryHeaderLen + expectedWriteNum * entrySize;

    // 若期待写的长度小于 buffer 长度。
    if (expectedWriteLen < bufferSize)
    {
        // 如果全部写完，还能至少剩下 header 的空间（即 NOP 的空间），则可以全部写完。
        if (bufferSize - expectedWriteLen >= entryHeaderLen)
        {
            res = expectedWriteNum;
        }
        // 如果全部写完无法留出 NOP 了，就计算最多能写多少个。
        // buffer 长度减去 NOP 和该日志项首部（2 * entryHeaderLen），就是能够存放日志条目的最大空间。
        else if (bufferSize > 2 * entryHeaderLen)
        {
            res = (bufferSize - 2 * entryHeaderLen) / entrySize;
        }
        else
        {
            res = 0;
        }
    }
    // 若 buffer 长度正好等于期待写的长度，则正好全部写完。
    else if (expectedWriteLen == bufferSize)
    {
        res = expectedWriteNum;
    }
    // buffer 长度小于期待写长度。
    else
    {
        // buffer 已经不够放一个首部了，不能再写。
        // 这种情况理论上不应该出现，因为需始终遵守留出 NOP 空间（最小即是首部）的协议。
        if (bufferSize < entryHeaderLen)
        {
            res = 0;
        }
        // 计算最大能写的日志条目个数 (bufferSize - entryHeaderLen) / entrySize。
        // 递归计算，以考虑 NOP 留空处理。递归时只可能进入前两个分支。
        else
        {
            res = genericCalculateWritableEntryNum(bufferSize, entrySize, (bufferSize - entryHeaderLen) / entrySize);
        }
    }


    return res;
}
