#include "rtfs_test.h"

#include "cache/page_cache.h"
#include "fs/fs.h"


RTFS_TEST(PageEntryInitTest)
{
    PageEntry entry;
    pageEntryInit(&entry, 123);


    TEST_ASSERT_EQUAL_UINT32(123, pageEntryGetBlkoff(&entry));
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, pageEntryGetLpaRef(&entry));
    TEST_ASSERT_EQUAL(PAGE_INVALID, pageEntryGetState(&entry));

    TEST_ASSERT_EQUAL(0, atomic_load(&entry.refCount));
    TEST_ASSERT_FALSE(atomic_load(&entry.isDirty));


    pageEntryDestroy(&entry);
}

RTFS_TEST(PageEntryStateTransitionTest)
{
    PageEntry entry;
    pageEntryInit(&entry, 1);


    pageEntrySetState(&entry, PAGE_READY);
    TEST_ASSERT_EQUAL(PAGE_READY, pageEntryGetState(&entry));

    pageEntrySetState(&entry, PAGE_INVALID);
    TEST_ASSERT_EQUAL(PAGE_INVALID, pageEntryGetState(&entry));


    pageEntryDestroy(&entry);
}

RTFS_TEST(PageEntryLpaTest)
{
    PageEntry entry;
    pageEntryInit(&entry, 10);


    pageEntrySetLpa(&entry, 999);
    TEST_ASSERT_EQUAL_UINT32(999, pageEntryGetLpaRef(&entry));


    pageEntryDestroy(&entry);
}

RTFS_TEST(PageEntryHandleRefCountTest)
{
    PageCache cache;
    pageCacheInit(&cache, 10);


    PageEntryHandle h1 = pageCacheGet(&cache, 1);

    TEST_ASSERT_EQUAL(1, atomic_load(&h1.entry->refCount));

    PageEntryHandle h2;
    pageEntryHandleCopy(&h2, &h1);

    TEST_ASSERT_EQUAL(2, atomic_load(&h1.entry->refCount));

    pageEntryHandleDestroy(&h1);
    TEST_ASSERT_EQUAL(1, atomic_load(&h2.entry->refCount));

    pageEntryHandleDestroy(&h2);


    pageCacheDestroy(&cache);
}
