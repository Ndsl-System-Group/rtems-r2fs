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
