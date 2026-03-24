#include "rtfs_test.h"

#include "journal/journal_container.h"
#include "utils/rtfs_log.h"


RTFS_TEST(JournalContainerInitTest)
{
    JournalContainer jc;
    journalContainerInit(&jc);


    TEST_ASSERT_TRUE(journalContainerIsEmpty(&jc));


    journalContainerDestroy(&jc);
}

RTFS_TEST(JournalContainerTxIdTest)
{
    JournalContainer jc;
    journalContainerInit(&jc);


    journalContainerSetTxId(&jc, 10086);
    TEST_ASSERT_EQUAL(journalContainerGetTxId(&jc), 10086);


    journalContainerDestroy(&jc);
}

RTFS_TEST(JournalContainerAppendSuperBlockTest)
{
    JournalContainer jc;
    journalContainerInit(&jc);


    SuperBlockJournalEntry sbje = {0};

    journalContainerAppendSuperBlockJournalEntry(&jc, &sbje);
    TEST_ASSERT_FALSE(journalContainerIsEmpty(&jc));

    SuperBlockJournalVector *arr = journalContainerGetSuperBlockJournal(&jc);
    TEST_ASSERT_EQUAL(kv_size(*arr), 1);

    SuperBlockJournalEntry *p = &kv_a(SuperBlockJournalEntry, *arr, 0);
    TEST_ASSERT_NOT_NULL(p);


    journalContainerDestroy(&jc);
}

RTFS_TEST(JournalContainerAppendNatTest)
{
    JournalContainer jc;
    journalContainerInit(&jc);


    NatJournalEntry nje = {0};

    journalContainerAppendNatJournalEntry(&jc, &nje);
    TEST_ASSERT_FALSE(journalContainerIsEmpty(&jc));

    NatJournalVector *arr = journalContainerGetNatJournal(&jc);
    TEST_ASSERT_EQUAL(kv_size(*arr), 1);

    NatJournalEntry *p = &kv_a(NatJournalEntry, *arr, 0);
    TEST_ASSERT_NOT_NULL(p);


    journalContainerDestroy(&jc);
}

RTFS_TEST(JournalContainerAppendSitTest)
{
    JournalContainer jc;
    journalContainerInit(&jc);


    SitJournalEntry sje = {0};

    journalContainerAppendSitJournalEntry(&jc, &sje);
    TEST_ASSERT_FALSE(journalContainerIsEmpty(&jc));

    SitJournalVector *arr = journalContainerGetSitJournal(&jc);
    TEST_ASSERT_EQUAL(kv_size(*arr), 1);

    SitJournalEntry *p = &kv_a(SitJournalEntry, *arr, 0);
    TEST_ASSERT_NOT_NULL(p);


    journalContainerDestroy(&jc);
}

RTFS_TEST(JournalContainerMultiAppendTest)
{
    JournalContainer jc;
    journalContainerInit(&jc);


    NatJournalEntry nje = {0};
    for (int i = 0; i < 10; ++i) journalContainerAppendNatJournalEntry(&jc, &nje);

    NatJournalVector *arr = journalContainerGetNatJournal(&jc);
    TEST_ASSERT_EQUAL(kv_size(*arr), 10);


    journalContainerDestroy(&jc);
}

RTFS_TEST(JournalContainerIsEmptyTest)
{
    JournalContainer jc;
    journalContainerInit(&jc);


    TEST_ASSERT_TRUE(journalContainerIsEmpty(&jc));

    SitJournalEntry sje = {0};
    journalContainerAppendSitJournalEntry(&jc, &sje);

    TEST_ASSERT_FALSE(journalContainerIsEmpty(&jc));


    journalContainerDestroy(&jc);
}
