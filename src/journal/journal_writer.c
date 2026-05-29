#include "journal_writer.h"

#include "journal/journal_type.h"
#include "klib/kbtree.h"
#include "klib/khash.h"
#include "utils/rtfs_exception.h"
#include "utils/io_utils.h"
#include "communication/comm_api.h"


struct SuperJournalOutputVector;
struct NatJournalOutputVector;
struct SitJournalOutputVector;


/**
 * @brief 计算能够写入的日志条目个数的通用算法。
 * @param bufferSize 目标缓存区大小（字节为单位）。
 * @param entrySize 日志条目大小（字节为单位）。
 * @param expectedWriteNum 期望写入的日志条目个数。
 * @return 返回实际能写入的日志条目个数。
 * @details 算法考虑日志项首部，且保证：若不是恰好把缓存写满，则在尾部留下 NOP 空间（除非输入的 bufferSize 本身无法写入 NOP）。留下 NOP 空间指至少留下一个日志项首部长度。
 */
static uint64_t genericCalculateWritableEntryNum(uint64_t bufferSize, uint64_t entrySize, uint64_t expectedWriteNum);

static void fillBufferWithNop(char *start, char *end);

static struct SuperJournalOutputVector *journalWriterSuperJournalOutputVecGenerate(JournalWriter *this);

static struct NatJournalOutputVector *journalWriterNatJournalOutputVecGenerate(JournalWriter *this);

static struct SitJournalOutputVector *journalWriterSitJournalOutputVecGenerate(JournalWriter *this);

static char *journalWriterGetIthBufferBlock(JournalWriter *this, size_t index);

static void journalWriterAppendEndEntry(JournalWriter *this);

static void journalWriterAsyncWriteCallback(comm_cmd_result res, void *arg);

static int journalWriterEnsureBufferCapacity(JournalWriter *this, size_t required_count);


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


/** super block 日志项输出实现。 */
KHASH_MAP_INIT_INT(khsjde, size_t)

typedef struct SuperJournalOutputVector
{
    SuperBlockJournalVector *journal;
    uint8_t journalType;

    // 使用 unordered_map 为 journal 去重并保留最新值。key 为特定日志条目的唯一标识。value 为日志条目在 journal 数组的下标。
    khash_t(khsjde) * map;
    khiter_t outputIt;

    size_t restOutputNum;
} SuperJournalOutputVector;

static void superJournalOutputVectorInit(SuperJournalOutputVector *this, SuperBlockJournalVector *superJournal);

static void superJournalOutputVectorDestroy(SuperJournalOutputVector *this);

static void superJournalOutputVectorGenerateOutputVector(SuperJournalOutputVector *this);

static void superJournalOutputVectorAdvanceToNextValid(SuperJournalOutputVector *this);

static void superJournalOutputVectorPrepareOutput(SuperJournalOutputVector *this);

static JournalOutputState superJournalOutputVectorOutputToBuffer(SuperJournalOutputVector *this, char **pStartAddr, char *endAddr);


/** NAT 日志项输出实现。 */
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


/**  SIT 日志项输出实现。 */
typedef struct SitJournalDedupEntry
{
    uint32_t segID; // key。
    size_t index;   // data。
} SitJournalDedupEntry;

#define KBTREE_SIT_JOURNAL_DEDUP_ENTRY_CMP(a, b) ((a).segID < (b).segID ? -1 : ((a).segID > (b).segID ? 1 : 0))

KBTREE_INIT(ktsjde, SitJournalDedupEntry, KBTREE_SIT_JOURNAL_DEDUP_ENTRY_CMP)

typedef struct SitJournalOutputVector
{
    SitJournalVector *journal;
    uint8_t journalType;

    kbtree_t(ktsjde) * map;
    kbitr_t outputIt;

    size_t restOutputNum;
} SitJournalOutputVector;

static void sitJournalOutputVectorInit(SitJournalOutputVector *this, SitJournalVector *sitJournal);

static void sitJournalOutputVectorDestroy(SitJournalOutputVector *this);

static void sitJournalOutputVectorGenerateOutputVector(SitJournalOutputVector *this);

static void sitJournalOutputVectorPrepareOutput(SitJournalOutputVector *this);

static JournalOutputState sitJournalOutputVectorOutputToBuffer(SitJournalOutputVector *this, char **pStartAddr, char *endAddr);


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

void journalWriterDestroy(JournalWriter *this)
{
    size_t i;

    if (this == NULL) {
        return;
    }

    for (i = 0; i < kv_size(this->journalBuffer); ++i) {
        blockBufferDestroy(&kv_A(this->journalBuffer, i));
    }
    kv_destroy(this->journalBuffer);
    kv_init(this->journalBuffer);

    this->curJournal = NULL;
    this->bufferTailIdx = 0;
    this->bufferTailOff = 0;
    this->dev = NULL;
    this->startLpa = 0;
    this->endLpa = 0;
}

void journalWriterSetPendingJournal(JournalWriter *this, JournalContainer *journal)
{
    this->curJournal = journal;
}

uint64_t journalWriterCollectPendingJournalToWriteBuffer(JournalWriter *this)
{
    this->bufferTailIdx = this->bufferTailOff = 0;

    SuperJournalOutputVector *superJournalVec = journalWriterSuperJournalOutputVecGenerate(this);
    NatJournalOutputVector *natJournalVec = journalWriterNatJournalOutputVecGenerate(this);
    SitJournalOutputVector *sitJournalVec = journalWriterSitJournalOutputVecGenerate(this);

    for (int i = 0; i <= 2; ++i)
    {
        switch (i)
        {
            case 0:
            {
                superJournalOutputVectorGenerateOutputVector(superJournalVec);
                superJournalOutputVectorPrepareOutput(superJournalVec);
                break;
            }
            case 1:
            {
                natJournalOutputVectorGenerateOutputVector(natJournalVec);
                natJournalOutputVectorPrepareOutput(natJournalVec);
                break;
            }
            case 2:
            {
                sitJournalOutputVectorGenerateOutputVector(sitJournalVec);
                sitJournalOutputVectorPrepareOutput(sitJournalVec);
                break;
            }
        }

        bool cpltWriteEntry = false;
        while (!cpltWriteEntry)
        {
            char *curBufStartAddr = journalWriterGetIthBufferBlock(this, this->bufferTailIdx);
            char *curBufEndAddr = curBufStartAddr + 4096;
            char *outputAddr = curBufStartAddr + this->bufferTailOff;

            JournalOutputState state;
            switch (i)
            {
                case 0:
                    state = superJournalOutputVectorOutputToBuffer(superJournalVec, &outputAddr, curBufEndAddr);
                    break;
                case 1:
                    state = natJournalOutputVectorOutputToBuffer(natJournalVec, &outputAddr, curBufEndAddr);
                    break;
                case 2:
                    state = sitJournalOutputVectorOutputToBuffer(sitJournalVec, &outputAddr, curBufEndAddr);
                    break;
            }

            switch (state)
            {
                    // 成功写入 buffer，可能因缓存块空间不足只写了一部分，或已经全部写完。更新当前块内偏移到写入的尾后地址即可。
                case JOURNAL_OUTPUT_OK:
                    this->bufferTailOff = outputAddr - curBufStartAddr;
                    break;
                    // 当前 buffer block 空间不足，无法再存放日志条目。使用 NOP 进行填充。
                case JOURNAL_OUTPUT_NO_ENOUGH_BUFFER:
                    fillBufferWithNop(outputAddr, curBufEndAddr);
                    this->bufferTailOff = 4096;
                    break;
                    // 当前日志项已经写完。
                case JOURNAL_OUTPUT_REACH_END:
                    cpltWriteEntry = true;
                    break;
            }

            // 如果当前缓存块已经写满，继续使用下一个缓存块。
            if (4096 == this->bufferTailOff)
            {
                this->bufferTailIdx++;
                this->bufferTailOff = 0;
            }
        }
    }

    journalWriterAppendEndEntry(this);


    return 1 + this->bufferTailIdx;
}

void journalWriterWriteToSsd(JournalWriter *this, uint64_t curTail)
{
    uint64_t ioNum = 1 + this->bufferTailIdx;
    AsyncVecioSynchronizer syr;
    asyncVecioSynchronizerInit(&syr, ioNum);

    for (size_t i = 0; i <= this->bufferTailIdx; ++i, ++curTail)
    {
        if (curTail == this->endLpa) curTail = this->startLpa;

        // TODO Print Debug

        int res = comm_submit_async_rw_request(this->dev, blockBufferGetPtr(&kv_a(BlockBuffer, this->journalBuffer, i)), LPA_TO_LBA(curTail), LBA_PER_LPA, journalWriterAsyncWriteCallback, &syr, COMM_IO_WRITE);
        if (0 != res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "journal writer: submit async write failed.");
    }

    comm_cmd_result result = asyncVecioSynchronizerWaitCplt(&syr);
    if (COMM_CMD_SUCCESS != result) THROW_FATAL_MESSAGE(EXIT_FAILURE, "journal writer: error occurred in async write process.");

    asyncVecioSynchronizerDestroy(&syr);
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

void fillBufferWithNop(char *start, char *end)
{
    uint16_t length = end - start;
    if (length < sizeof(MetaJournalEntry)) THROW_FATAL_MESSAGE(EXIT_FAILURE, "not enough memory to fill nop entry.");

    MetaJournalEntry entry = {.len = length, .type = JOURNAL_TYPE_NOP};
    memcpy(start, &entry, sizeof(MetaJournalEntry));
}

SuperJournalOutputVector *journalWriterSuperJournalOutputVecGenerate(JournalWriter *this)
{
    SuperJournalOutputVector *res = (SuperJournalOutputVector *)malloc(sizeof(SuperJournalOutputVector));
    if (!res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "journal writer: error when allocating SuperJournalOutputVector");

    SuperBlockJournalVector *superJournal = journalContainerGetSuperBlockJournal(this->curJournal);

    if (NULL == superJournal)
    {
        free(res);
        res = NULL;
    }
    else
    {
        superJournalOutputVectorInit(res, superJournal);
    }


    return res;
}

NatJournalOutputVector *journalWriterNatJournalOutputVecGenerate(JournalWriter *this)
{
    NatJournalOutputVector *res = (NatJournalOutputVector *)malloc(sizeof(NatJournalOutputVector));
    if (!res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "journal writer: error when allocating NatJournalOutputVector");

    NatJournalVector *natJournal = journalContainerGetNatJournal(this->curJournal);

    if (NULL == natJournal)
    {
        free(res);
        res = NULL;
    }
    else
    {
        natJournalOutputVectorInit(res, natJournal);
    }


    return res;
}

SitJournalOutputVector *journalWriterSitJournalOutputVecGenerate(JournalWriter *this)
{
    SitJournalOutputVector *res = (SitJournalOutputVector *)malloc(sizeof(SitJournalOutputVector));
    if (!res) THROW_FATAL_MESSAGE(EXIT_FAILURE, "journal writer: error when allocating SitJournalOutputVector");

    SitJournalVector *sitJournal = journalContainerGetSitJournal(this->curJournal);

    if (NULL == sitJournal)
    {
        free(res);
        res = NULL;
    }
    else
    {
        sitJournalOutputVectorInit(res, sitJournal);
    }


    return res;
}

char *journalWriterGetIthBufferBlock(JournalWriter *this, size_t index)
{
    if (journalWriterEnsureBufferCapacity(this, 1 + index) != 0) {
        THROW_FATAL_MESSAGE(EXIT_FAILURE, "journal writer: allocate buffer block failed.");
    }


    return blockBufferGetPtr(&kv_a(BlockBuffer, this->journalBuffer, index));
}

static int journalWriterEnsureBufferCapacity(
    JournalWriter *this,
    size_t required_count
)
{
    size_t old_count;
    size_t i;

    if (this == NULL) {
        return -1;
    }

    old_count = kv_size(this->journalBuffer);
    if (required_count <= old_count) {
        return 0;
    }

    if (kv_resize(BlockBuffer, this->journalBuffer, required_count) != 0) {
        return -1;
    }

    for (i = old_count; i < required_count; ++i) {
        memset(&kv_A(this->journalBuffer, i), 0, sizeof(BlockBuffer));
        if (blockBufferInit(&kv_A(this->journalBuffer, i)) != 0) {
            size_t j;

            for (j = old_count; j < i; ++j) {
                blockBufferDestroy(&kv_A(this->journalBuffer, j));
                memset(&kv_A(this->journalBuffer, j), 0, sizeof(BlockBuffer));
            }
            kv_resize(BlockBuffer, this->journalBuffer, old_count);
            return -1;
        }
    }

    return 0;
}

void journalWriterAppendEndEntry(JournalWriter *this)
{
    char *p = journalWriterGetIthBufferBlock(this, this->bufferTailIdx);
    p += this->bufferTailOff;

    MetaJournalEntry entry = {.len = sizeof(MetaJournalEntry), .type = JOURNAL_TYPE_END};
    memcpy(p, &entry, sizeof(MetaJournalEntry));
}

void journalWriterAsyncWriteCallback(comm_cmd_result res, void *arg)
{
    asyncVecioSynchronizerGenericCallback(res, arg);
}


void superJournalOutputVectorInit(SuperJournalOutputVector *this, SuperBlockJournalVector *superJournal)
{
    this->journal = superJournal;
    this->journalType = JOURNAL_TYPE_SUPER_BLOCK;
    this->map = kh_init(khsjde);
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
    kh_clear(khsjde, this->map);

    for (size_t i = 0; i < kv_size(*this->journal); ++i)
    {
        SuperBlockJournalEntry *p = &kv_a(SuperBlockJournalEntry, *this->journal, i);

        int res = 0;
        khiter_t iter = kh_put(khsjde, this->map, p->Off, &res);
        kh_value(this->map, iter) = i;
    }
}

void superJournalOutputVectorPrepareOutput(SuperJournalOutputVector *this)
{
    this->outputIt = kh_begin(this->map);
    this->restOutputNum = kh_size(this->map);
    superJournalOutputVectorAdvanceToNextValid(this);
}

static void superJournalOutputVectorAdvanceToNextValid(SuperJournalOutputVector *this)
{
    while (this->outputIt != kh_end(this->map) &&
           !kh_exist(this->map, this->outputIt)) {
        ++this->outputIt;
    }
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

    for (size_t i = 0; i < outputNum; ++i, ++entry)
    {
        SuperBlockJournalEntry *p = &kv_a(SuperBlockJournalEntry, *this->journal, kh_value(this->map, this->outputIt));

        *entry = *p;
        ++this->outputIt;
        superJournalOutputVectorAdvanceToNextValid(this);
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


void sitJournalOutputVectorInit(SitJournalOutputVector *this, SitJournalVector *sitJournal)
{
    this->journal = sitJournal;
    this->journalType = JOURNAL_TYPE_SITS;
    this->map = kb_init(ktsjde, KB_DEFAULT_SIZE);
    this->restOutputNum = 0;
}

void sitJournalOutputVectorDestroy(SitJournalOutputVector *this)
{
    this->restOutputNum = 0;
    this->map = NULL;
    this->journalType = -1;
    this->journal = NULL;
}

void sitJournalOutputVectorGenerateOutputVector(SitJournalOutputVector *this)
{
    kb_destroy(ktsjde, this->map);
    this->map = kb_init(ktsjde, KB_DEFAULT_SIZE);

    for (size_t i = 0; i < kv_size(*this->journal); ++i)
    {
        SitJournalEntry *p = &kv_a(SitJournalEntry, *this->journal, i);
        SitJournalDedupEntry t = {.segID = p->segID, .index = i};

        kb_put(ktsjde, this->map, t);
    }
}

void sitJournalOutputVectorPrepareOutput(SitJournalOutputVector *this)
{
    kb_itr_first(ktsjde, this->map, &this->outputIt);
    this->restOutputNum = kb_size(this->map);
}

JournalOutputState sitJournalOutputVectorOutputToBuffer(SitJournalOutputVector *this, char **pStartAddr, char *endAddr)
{
    size_t journalEntrySize = sizeof(SitJournalEntry);
    if (0 == this->restOutputNum) return JOURNAL_OUTPUT_REACH_END;

    size_t outputNum = genericCalculateWritableEntryNum(endAddr - *pStartAddr, journalEntrySize, this->restOutputNum);
    if (0 == outputNum) return JOURNAL_OUTPUT_NO_ENOUGH_BUFFER;

    char *p = *pStartAddr;
    MetaJournalEntry header = {
        .len = sizeof(MetaJournalEntry) + outputNum * journalEntrySize,
        .type = this->journalType};

    memcpy(p, &header, sizeof(header));
    SitJournalEntry *entry = (SitJournalEntry *)(p + sizeof(MetaJournalEntry));

    for (size_t i = 0; i < outputNum; ++i, kb_itr_next(ktsjde, this->map, &this->outputIt), ++entry)
    {
        SitJournalDedupEntry *t = &kb_itr_key(SitJournalDedupEntry, &this->outputIt);
        SitJournalEntry *p = &kv_a(SitJournalEntry, *this->journal, t->index);

        *entry = *p;
    }

    this->restOutputNum -= outputNum;
    *pStartAddr = (char *)(entry);


    return JOURNAL_OUTPUT_OK;
}
