#include "rtfs_test.h"

#include "journal/journal_processor.h"


static TransactionJournalRecord makeRecord(uint64_t txId, uint64_t startLpa, uint64_t endLpa)
{
    TransactionJournalRecord record;

    memset(&record, 0, sizeof(record));
    transactionJournalRecordInit(&record, txId, startLpa, endLpa);


    return record;
}


RTFS_TEST(TjrInitAndGetterTest)
{
    TransactionJournalRecord record = makeRecord(100, 10, 20);

    TEST_ASSERT_EQUAL_UINT64(100, transactionJournalRecordGetTxId(&record));
    TEST_ASSERT_EQUAL_UINT64(10, transactionJournalRecordGetStartLpa(&record));
    TEST_ASSERT_EQUAL_UINT64(20, transactionJournalRecordGetEndLpa(&record));
}

// 普通区间：[10, 20)，当前有效 FIFO：[5, 9)，日志区间已不在 FIFO 内 => 已应用。
RTFS_TEST(TjrAppliedNormalRangeBeforeWindowTest)
{
    TransactionJournalRecord record = makeRecord(1, 10, 20);

    TEST_ASSERT_TRUE(transactionJournalRecordIsApplied(&record, 5, 9));
}

// 普通区间：[10, 20)，当前有效 FIFO：[12, 25)，与日志区间重叠 => 未应用完。
RTFS_TEST(TjrNotAppliedNormalRangeOverlapTest)
{
    TransactionJournalRecord record = makeRecord(1, 10, 20);

    TEST_ASSERT_FALSE(transactionJournalRecordIsApplied(&record, 12, 25));
}

// 普通区间：[10, 20)，head 已到 end。
RTFS_TEST(TjrAppliedNormalRangeHeadReachEndTest)
{
    TransactionJournalRecord record = makeRecord(1, 10, 20);

    TEST_ASSERT_TRUE(transactionJournalRecordIsApplied(&record, 20, 30));
}

// 普通区间：[10, 20)，FIFO 回绕：head > tail，head >= end => 已应用。
RTFS_TEST(TjrAppliedNormalRangeWrapQueueTest)
{
    TransactionJournalRecord record = makeRecord(1, 10, 20);

    TEST_ASSERT_TRUE(transactionJournalRecordIsApplied(&record, 25, 5));
}

// 普通区间：[10, 20)。FIFO 回绕，但 head < end => 未应用。
RTFS_TEST(TjrNotAppliedNormalRangeWrapQueueTest)
{
    TransactionJournalRecord record = makeRecord(1, 10, 20);

    TEST_ASSERT_FALSE(transactionJournalRecordIsApplied(&record, 15, 5));
}

// 回绕区间：[30, 10)。当前 FIFO 非回绕，head >= end => 已应用。
RTFS_TEST(TjrAppliedWrapRangeHeadReachEndTest)
{
    TransactionJournalRecord record = makeRecord(1, 30, 10);

    TEST_ASSERT_TRUE(transactionJournalRecordIsApplied(&record, 10, 25));
}

// 回绕区间：[30, 10)。当前 FIFO 非回绕，但 head < end => 未应用。
RTFS_TEST(TjrNotAppliedWrapRangeHeadNotReachEndTest)
{
    TransactionJournalRecord record = makeRecord(1, 30, 10);

    TEST_ASSERT_FALSE(transactionJournalRecordIsApplied(&record, 9, 25));
}

// 回绕区间：[30, 10)，当前 FIFO 回绕 => 未应用。
RTFS_TEST(TjrNotAppliedWrapRangeQueueWrapTest)
{
    TransactionJournalRecord record = makeRecord(1, 30, 10);

    TEST_ASSERT_FALSE(transactionJournalRecordIsApplied(&record, 20, 5));
}

// 正常初始化：totalAvailLpa = end - start - 1，curAvailLpa = totalAvailLpa，head/tail = fifoPos。
RTFS_TEST(JpInitBasicTest)
{
    JournalProcessor jp;

    memset(&jp, 0, sizeof(JournalProcessor));

    journalProcessorInit(&jp, NULL, 100, 200, 150);

    TEST_ASSERT_EQUAL_UINT64(100, jp.startLpa);
    TEST_ASSERT_EQUAL_UINT64(200, jp.endLpa);

    TEST_ASSERT_EQUAL_UINT64(150, jp.headLpa);
    TEST_ASSERT_EQUAL_UINT64(150, jp.tailLpa);

    TEST_ASSERT_EQUAL_UINT64(99, jp.totalAvailLpa);
    TEST_ASSERT_EQUAL_UINT64(99, jp.curAvailLpa);

    TEST_ASSERT_NULL(jp.curJournal);

    TEST_ASSERT_FALSE(jp.isPollTimerEnabled);

    TEST_ASSERT_NOT_NULL(jp.journalPosDmaBuffer);

    journalProcessorDestroy(&jp);
}

// fifoPos 在起点。
RTFS_TEST(JpInitFifoAtStartTest)
{
    JournalProcessor jp;

    memset(&jp, 0, sizeof(JournalProcessor));

    journalProcessorInit(&jp, NULL, 10, 20, 10);

    TEST_ASSERT_EQUAL_UINT64(10, jp.headLpa);
    TEST_ASSERT_EQUAL_UINT64(10, jp.tailLpa);

    TEST_ASSERT_EQUAL_UINT64(9, jp.totalAvailLpa);
    TEST_ASSERT_EQUAL_UINT64(9, jp.curAvailLpa);

    journalProcessorDestroy(&jp);
}

// fifoPos 在末尾附近。
RTFS_TEST(JpInitFifoNearEndTest)
{
    JournalProcessor jp;

    memset(&jp, 0, sizeof(JournalProcessor));

    journalProcessorInit(&jp, NULL, 50, 80, 79);

    TEST_ASSERT_EQUAL_UINT64(79, jp.headLpa);
    TEST_ASSERT_EQUAL_UINT64(79, jp.tailLpa);

    TEST_ASSERT_EQUAL_UINT64(29, jp.totalAvailLpa);
    TEST_ASSERT_EQUAL_UINT64(29, jp.curAvailLpa);

    journalProcessorDestroy(&jp);
}

// destroy 可重复调用前提：重新 init 后 destroy 不崩溃。
RTFS_TEST(JpDestroyAfterInitTest)
{
    JournalProcessor jp;

    memset(&jp, 0, sizeof(JournalProcessor));

    journalProcessorInit(&jp, NULL, 0, 100, 0);

    journalProcessorDestroy(&jp);

    TEST_PASS();
}

// 多次创建销毁。
RTFS_TEST(JpMultiInitDestroyTest)
{
    JournalProcessor jp;

    for (int i = 0; i < 10; ++i)
    {
        memset(&jp, 0, sizeof(JournalProcessor));

        journalProcessorInit(&jp, NULL, 1000, 1100, 1000 + i);

        TEST_ASSERT_EQUAL_UINT64((uint64_t)(1000 + i), jp.headLpa);
        TEST_ASSERT_EQUAL_UINT64((uint64_t)(1000 + i), jp.tailLpa);

        journalProcessorDestroy(&jp);
    }
}

// TODO 目前不单独测试 journalProcessorProcessJournal 函数。该函数主要作为线程主循环调度入口，自身逻辑较少且依赖线程同步、定时器与 I/O 协作，当前优先测试其内部核心处理函数，后续再通过集成测试覆盖整体行为。
