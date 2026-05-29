#include "rtfs_test.h"

#include "journal/journal_type.h"
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

    journalWriterDestroy(&writer);
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

    journalWriterDestroy(&writer);
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

    journalWriterDestroy(&writer);
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

    journalWriterDestroy(&writer);
}

RTFS_TEST(JwCollectPendingJournal_WhenOneSuperEntryExists_ShouldAllocateWritableBlockAndSerializeEntries)
{
    JournalWriter writer;
    JournalContainer journal;
    SuperBlockJournalEntry entry = {
        .Off = 7,
        .newVal = 0x12345678u
    };
    MetaJournalEntry *header;
    SuperBlockJournalEntry *payload;
    MetaJournalEntry *end_entry;
    char *buffer;

    memset(&writer, 0, sizeof(writer));
    journalWriterInit(&writer, NULL, 0, 100);
    journalContainerInit(&journal);
    journalContainerAppendSuperBlockJournalEntry(&journal, &entry);

    journalWriterSetPendingJournal(&writer, &journal);

    TEST_ASSERT_EQUAL_UINT64(1u, journalWriterCollectPendingJournalToWriteBuffer(&writer));
    TEST_ASSERT_EQUAL_UINT64(1u, kv_size(writer.journalBuffer));
    TEST_ASSERT_NOT_NULL(kv_A(writer.journalBuffer, 0).buffer);

    buffer = blockBufferGetPtr(&kv_A(writer.journalBuffer, 0));
    header = (MetaJournalEntry *)buffer;
    payload = (SuperBlockJournalEntry *)(buffer + sizeof(MetaJournalEntry));
    end_entry = (MetaJournalEntry *)((char *)payload + sizeof(*payload));

    TEST_ASSERT_EQUAL_UINT16(
        (uint16_t)(sizeof(MetaJournalEntry) + sizeof(SuperBlockJournalEntry)),
        header->len
    );
    TEST_ASSERT_EQUAL_UINT8(JOURNAL_TYPE_SUPER_BLOCK, header->type);
    TEST_ASSERT_EQUAL_UINT32(entry.Off, payload->Off);
    TEST_ASSERT_EQUAL_UINT32(entry.newVal, payload->newVal);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)sizeof(MetaJournalEntry), end_entry->len);
    TEST_ASSERT_EQUAL_UINT8(JOURNAL_TYPE_END, end_entry->type);

    journalContainerDestroy(&journal);
    journalWriterDestroy(&writer);
}
