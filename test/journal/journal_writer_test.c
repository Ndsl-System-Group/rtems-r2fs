#include "rtfs_test.h"

#include "journal/journal_writer.h"


RTFS_TEST(JwInitBasicTest)
{
    JournalWriter writer;

    memset(&writer, 0, sizeof(JournalWriter));

    journalWriterInit(&writer, NULL, 100, 200);

    TEST_ASSERT_NULL(writer.curJournal);

    TEST_ASSERT_EQUAL_UINT64(100, writer.startLpa);
    TEST_ASSERT_EQUAL_UINT64(200, writer.endLpa);

    TEST_ASSERT_EQUAL_UINT64(0, writer.bufferTailIdx);
    TEST_ASSERT_EQUAL_UINT64(0, writer.bufferTailOff);

    TEST_ASSERT_NULL(writer.dev);

    TEST_ASSERT_EQUAL_UINT64(0, kv_size(writer.journalBuffer));
}

RTFS_TEST(JwInitWithDevTest)
{
    JournalWriter writer;
    struct comm_dev *dev = (struct comm_dev *)0x1234;

    memset(&writer, 0, sizeof(JournalWriter));

    journalWriterInit(&writer, dev, 10, 20);

    TEST_ASSERT_EQUAL_PTR(dev, writer.dev);
    TEST_ASSERT_EQUAL_UINT64(10, writer.startLpa);
    TEST_ASSERT_EQUAL_UINT64(20, writer.endLpa);
}

RTFS_TEST(JwSetPendingJournalTest)
{
    JournalWriter writer;
    JournalContainer *journal = (JournalContainer *)0x5678;

    memset(&writer, 0, sizeof(JournalWriter));

    journalWriterInit(&writer, NULL, 0, 100);

    TEST_ASSERT_NULL(writer.curJournal);

    journalWriterSetPendingJournal(&writer, journal);

    TEST_ASSERT_EQUAL_PTR(journal, writer.curJournal);
}

RTFS_TEST(JwSetPendingJournalOverwriteTest)
{
    JournalWriter writer;

    JournalContainer *journal1 = (JournalContainer *)0x1111;
    JournalContainer *journal2 = (JournalContainer *)0x2222;

    memset(&writer, 0, sizeof(JournalWriter));

    journalWriterInit(&writer, NULL, 0, 100);

    journalWriterSetPendingJournal(&writer, journal1);
    TEST_ASSERT_EQUAL_PTR(journal1, writer.curJournal);

    journalWriterSetPendingJournal(&writer, journal2);
    TEST_ASSERT_EQUAL_PTR(journal2, writer.curJournal);
}

// TODO journalWriterCollectPendingJournalToWriteBuffer 和 journalWriterWriteToSsd 依赖全局模块，目前不太好测试。
