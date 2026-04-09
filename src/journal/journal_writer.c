#include "journal_writer.h"

#include "journal/journal_type.h"
#include "klib/kbtree.h"
#include "klib/khash.h"


/**
 * @brief 计算能够写入的日志条目个数的通用算法。
 * @param bufferSize 目标缓存区大小（字节为单位）。
 * @param entrySize 日志条目大小（字节为单位）。
 * @param expectedWriteNum 期望写入的日志条目个数。
 * @return 返回实际能写入的日志条目个数。
 * @details 算法考虑日志项首部，且保证：若不是恰好把缓存写满，则在尾部留下 NOP 空间（除非输入的 bufferSize 本身无法写入 NOP）。留下 NOP 空间指至少留下一个日志项首部长度。
 */
static uint64_t genericCalculateWritableEntryNum(uint64_t bufferSize, uint64_t entrySize, uint64_t expectedWriteNum);


typedef enum JournalOutputState
{
    JOURNAL_OUTPUT_OK,
    JOURNAL_OUTPUT_NO_ENOUGH_BUFFER,
    JOURNAL_OUTPUT_REACH_END
} JournalOutputState;


// TODO 想办法为下面的接口做一套伪继承和模板的设计，实现代码复用。
// /**
//  * @brief 日志条目的合并与输出接口。
//  */
// typedef struct JournalOutputVector
// {
//     /**
//      * @brief 生成日志项输出向量。向量中的每个元素是一个日志条目。
//      * @details 此接口内完成：
//      * 1. 修改相同目标的日志条目合并，仅保留最后一个日志条目，反应该目标的最新值。
//      * 2. 将日志条目在输出向量中按一定顺序排列，SSD 处理时能一次性处理多个目标在同一 page 内的日志条目。
//      */
//     void (*generateOutputVector)(struct JournalOutputVector *this);

//     /**
//      * @brief 上层即将开始输出。outputToBuffer 对调用者是无状态的，此接口内将初始化 output 状态，从输出向量头部开始。
//      */
//     void (*prepareOutput)(struct JournalOutputVector *this);

//     /**
//      * @brief 将日志项输出到调用者提供的缓存区域 [*pStartAddr, endAddr)。尽可能多地在缓存区域中输出，除非已经输出完毕或缓存空间不足。
//      *
//      * @return 返回值如下：
//      * OK：成功完成了输出，此时 *pStartAddr 被置为输出区域的尾后地址
//      * NO_ENOUGH_BUFFER：提供的缓存空间不足以输出一个[首部 + 日志条目]，*pStartAddr 不变。
//      * REACH_END：已经输出完毕，*pStartAddr 不变。
//      *
//      * @details 多次调用此接口，则本次调用将继续上一次调用已输出的日志条目之后，进行输出。如果缓存区域没有全部写入，此接口在尾部至少留下一个 NOP 日志项的空间，除非输入的缓存区域不足以放下 NOP。（需由调用方，即 JournalWriter 保证，提供的缓存区足够放下 NOP）。
//      */
//     JournalOutputState (*outputToBuffer)(struct JournalOutputVector *this, char **pStartAddr, char *endAddr);
// } JournalOutputVector;


KHASH_MAP_INIT_INT(khsjov, size_t)

typedef struct SuperJournalOutputVector
{
    SuperBlockJournalVector *journal;
    uint8_t journalType;

    // 使用 unordered_map 为 journal 去重并保留最新值。key 为特定日志条目的唯一标识。value 为日志条目在 journal 数组的下标。
    khash_t(khsjov) * map;
    khiter_t outputIt;

    size_t restOutputNum;
} SuperJournalOutputVector;

static void superJournalOutputVectorInit(SuperJournalOutputVector *this, SuperBlockJournalVector *superJournal);

static void superJournalOutputVectorDestroy(SuperJournalOutputVector *this);

static void superJournalOutputVectorGenerateOutputVector(SuperJournalOutputVector *this);

static void superJournalOutputVectorPrepareOutput(SuperJournalOutputVector *this);

static JournalOutputState superJournalOutputVectorOutputToBuffer(SuperJournalOutputVector *this, char **pStartAddr, char *endAddr);


// 使用 map 为 journal 去重并保留最新值，还可以按 journal 某字段排序。key 为特定日志条目的唯一标识。value 为日志条目在 journal 数组的下标。
typedef struct NatJournalDedupEntry
{
    uint32_t nid; // key。
    size_t index; // data。
} NatJournalDedupEntry;

#define KBTREE_NAT_JOURNAL_DEDUP_ENTRY_CMP(a, b) ((a).nid < (b).nid ? -1 : ((a).nid > (b).nid ? 1 : 0))

KBTREE_INIT(ktnjde, NatJournalDedupEntry, KBTREE_NAT_JOURNAL_DEDUP_ENTRY_CMP)

typedef struct NatJournalOutputVector
{
    NatJournalVector *journal;
    uint8_t journalType;

    kbtree_t(ktnjde) * map;
    kbitr_t outputIt;

    size_t restOutputNum;
} NatJournalOutputVector;

static void natJournalOutputVectorInit(NatJournalOutputVector *this, NatJournalVector *natJournal);

static void natJournalOutputVectorDestroy(NatJournalOutputVector *this);

static void natJournalOutputVectorGenerateOutputVector(NatJournalOutputVector *this);

static void natJournalOutputVectorPrepareOutput(NatJournalOutputVector *this);

static JournalOutputState natJournalOutputVectorOutputToBuffer(NatJournalOutputVector *this, char **pStartAddr, char *endAddr);


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


void superJournalOutputVectorInit(SuperJournalOutputVector *this, SuperBlockJournalVector *superJournal)
{
    this->journal = superJournal;
    this->journalType = JOURNAL_TYPE_SUPER_BLOCK;
    this->map = kh_init(khsjov);
    this->restOutputNum = 0;
}

void superJournalOutputVectorDestroy(SuperJournalOutputVector *this)
{
    this->restOutputNum = 0;
    this->map = NULL;
    this->journalType = -1;
    this->journal = NULL;
}

void superJournalOutputVectorGenerateOutputVector(SuperJournalOutputVector *this)
{
    kh_clear(khsjov, this->map);

    for (size_t i = 0; i < kv_size(*this->journal); ++i)
    {
        SuperBlockJournalEntry *p = &kv_a(SuperBlockJournalEntry, *this->journal, i);

        int res = 0;
        khiter_t iter = kh_put(khsjov, this->map, p->Off, &res);
        kh_value(this->map, iter) = i;
    }
}

void superJournalOutputVectorPrepareOutput(SuperJournalOutputVector *this)
{
    this->outputIt = kh_begin(this->map);
    this->restOutputNum = kh_size(this->map);
}

JournalOutputState superJournalOutputVectorOutputToBuffer(SuperJournalOutputVector *this, char **pStartAddr, char *endAddr)
{
    size_t journalEntrySize = sizeof(SuperBlockJournalEntry);
    if (0 == this->restOutputNum) return JOURNAL_OUTPUT_REACH_END;

    size_t outputNum = genericCalculateWritableEntryNum(endAddr - *pStartAddr, journalEntrySize, this->restOutputNum);
    if (0 == outputNum) return JOURNAL_OUTPUT_NO_ENOUGH_BUFFER;

    char *p = *pStartAddr;
    MetaJournalEntry header = {
        .len = sizeof(MetaJournalEntry) + outputNum * journalEntrySize,
        .type = this->journalType};

    memcpy(p, &header, sizeof(header));
    SuperBlockJournalEntry *entry = (SuperBlockJournalEntry *)(p + sizeof(MetaJournalEntry));

    for (size_t i = 0; i < outputNum; ++i, ++this->outputIt, ++entry)
    {
        SuperBlockJournalEntry *p = &kv_a(SuperBlockJournalEntry, *this->journal, kh_value(this->map, this->outputIt));

        *entry = *p;
    }

    this->restOutputNum -= outputNum;
    *pStartAddr = (char *)(entry);


    return JOURNAL_OUTPUT_OK;
}


void natJournalOutputVectorInit(NatJournalOutputVector *this, NatJournalVector *natJournal)
{
    this->journal = natJournal;
    this->journalType = JOURNAL_TYPE_NATS;
    this->map = kb_init(ktnjde, KB_DEFAULT_SIZE);
    this->restOutputNum = 0;
}

void natJournalOutputVectorDestroy(NatJournalOutputVector *this)
{
    this->restOutputNum = 0;
    this->map = NULL;
    this->journalType = -1;
    this->journal = NULL;
}

void natJournalOutputVectorGenerateOutputVector(NatJournalOutputVector *this)
{
    kb_destroy(ktnjde, this->map);
    this->map = kb_init(ktnjde, KB_DEFAULT_SIZE);

    for (size_t i = 0; i < kv_size(*this->journal); ++i)
    {
        NatJournalEntry *p = &kv_a(NatJournalEntry, *this->journal, i);
        NatJournalDedupEntry t = {.nid = p->nid, .index = i};

        kb_put(ktnjde, this->map, t);
    }
}

void natJournalOutputVectorPrepareOutput(NatJournalOutputVector *this)
{
    kb_itr_first(ktnjde, this->map, &this->outputIt);
    this->restOutputNum = kb_size(this->map);
}

JournalOutputState natJournalOutputVectorOutputToBuffer(NatJournalOutputVector *this, char **pStartAddr, char *endAddr)
{
    size_t journalEntrySize = sizeof(NatJournalEntry);
    if (0 == this->restOutputNum) return JOURNAL_OUTPUT_REACH_END;

    size_t outputNum = genericCalculateWritableEntryNum(endAddr - *pStartAddr, journalEntrySize, this->restOutputNum);
    if (0 == outputNum) return JOURNAL_OUTPUT_NO_ENOUGH_BUFFER;

    char *p = *pStartAddr;
    MetaJournalEntry header = {
        .len = sizeof(MetaJournalEntry) + outputNum * journalEntrySize,
        .type = this->journalType};

    memcpy(p, &header, sizeof(header));
    NatJournalEntry *entry = (NatJournalEntry *)(p + sizeof(MetaJournalEntry));

    for (size_t i = 0; i < outputNum; ++i, kb_itr_next(ktnjde, this->map, &this->outputIt), ++entry)
    {
        NatJournalDedupEntry *t = &kb_itr_key(NatJournalDedupEntry, &this->outputIt);
        NatJournalEntry *p = &kv_a(NatJournalEntry, *this->journal, t->index);

        *entry = *p;
    }

    this->restOutputNum -= outputNum;
    *pStartAddr = (char *)(entry);


    return JOURNAL_OUTPUT_OK;
}
