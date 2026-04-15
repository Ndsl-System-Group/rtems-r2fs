#include "rtfs_test.h"

#include "cache/page_cache.h"
#include "fs/fs.h"


RTFS_TEST(PageEntryInitTest)
{
    PageEntry entry;
    pageEntryInit(&entry, 123);


    TEST_ASSERT_EQUAL_UINT32(123, pageEntryGetBlkoff(&entry));
    TEST_ASSERT_EQUAL_UINT32(INVALID_LPA, pageEntryGetLpa(&entry));
    TEST_ASSERT_EQUAL(PAGE_INVALID, pageEntryGetState(&entry));

    TEST_ASSERT_NOT_NULL(pageEntryGetLock(&entry));
    TEST_ASSERT_NOT_NULL(pageEntryGetBuffer(&entry));

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
    TEST_ASSERT_EQUAL_UINT32(999, pageEntryGetLpa(&entry));


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

RTFS_TEST(PageEntryDirtyMarkTest)
{
    PageCache cache;
    pageCacheInit(&cache, 10);


    PageEntryHandle handle = pageCacheGet(&cache, 2);

    TEST_ASSERT_FALSE(atomic_load(&handle.entry->isDirty));
    pageEntryHandleMakeDirty(&handle);
    TEST_ASSERT_TRUE(atomic_load(&handle.entry->isDirty));

    // 再次标记，不应重复插入 dirty。
    pageEntryHandleMakeDirty(&handle);
    TEST_ASSERT_TRUE(atomic_load(&handle.entry->isDirty));

    // XXX 调用者需自己保证 dirtyPages 被清理干净，才能进行生命周期的销毁。
    pageCacheClearDirtyPages(&cache);


    pageEntryHandleDestroy(&handle);
    pageCacheDestroy(&cache);
}

RTFS_TEST(PageCacheGetSameEntryTest)
{
    PageCache cache;
    pageCacheInit(&cache, 10);


    PageEntryHandle h1 = pageCacheGet(&cache, 100);
    PageEntryHandle h2 = pageCacheGet(&cache, 100);

    // 应该是同一个 entry。
    TEST_ASSERT_EQUAL_PTR(h1.entry, h2.entry);
    TEST_ASSERT_EQUAL(2, atomic_load(&h1.entry->refCount));

    pageEntryHandleDestroy(&h2);
    pageEntryHandleDestroy(&h1);


    pageCacheDestroy(&cache);
}

RTFS_TEST(PageCacheReplaceTest)
{
    PageCache cache;
    pageCacheInit(&cache, 2);

    PageEntryHandle h1 = pageCacheGet(&cache, 1);
    PageEntryHandle h2 = pageCacheGet(&cache, 2);

    PageEntry *p1 = h1.entry;
    PageEntry *p2 = h2.entry;

    // 释放引用，让其可被替换。
    pageEntryHandleDestroy(&h2);
    pageEntryHandleDestroy(&h1);

    // 触发替换。
    PageEntryHandle h3 = pageCacheGet(&cache, 3);

    TEST_ASSERT_NOT_NULL(h3.entry);
    TEST_ASSERT_TRUE(h3.entry == p1 || h3.entry == p2);
    TEST_ASSERT_EQUAL(3, h3.entry->blkoff);
    TEST_ASSERT_EQUAL(PAGE_INVALID, h3.entry->contentState);

    pageEntryHandleDestroy(&h3);


    pageCacheDestroy(&cache);
}

RTFS_TEST(PageCacheClearDirtyPagesTest)
{
    PageCache cache;
    pageCacheInit(&cache, 10);


    PageEntryHandle h1 = pageCacheGet(&cache, 1);
    PageEntryHandle h2 = pageCacheGet(&cache, 2);

    pageEntryHandleMakeDirty(&h1);
    pageEntryHandleMakeDirty(&h2);

    TEST_ASSERT_TRUE(atomic_load(&h1.entry->isDirty));
    TEST_ASSERT_TRUE(atomic_load(&h2.entry->isDirty));

    pageCacheClearDirtyPages(&cache);

    TEST_ASSERT_FALSE(atomic_load(&h1.entry->isDirty));
    TEST_ASSERT_FALSE(atomic_load(&h2.entry->isDirty));

    pageEntryHandleDestroy(&h2);
    pageEntryHandleDestroy(&h1);


    pageCacheDestroy(&cache);
}

RTFS_TEST(PageCacheTruncateTest)
{
    PageCache cache;
    pageCacheInit(&cache, 10);


    PageEntryHandle h1 = pageCacheGet(&cache, 1);
    PageEntryHandle h2 = pageCacheGet(&cache, 5);
    PageEntryHandle h3 = pageCacheGet(&cache, 10);

    pageEntryHandleMakeDirty(&h1);
    pageEntryHandleMakeDirty(&h2);
    pageEntryHandleMakeDirty(&h3);

    // 截断 > 5。
    pageCacheTruncate(&cache, 5);

    // h3 应该被 invalid + 非 dirty。
    TEST_ASSERT_FALSE(atomic_load(&h3.entry->isDirty));
    TEST_ASSERT_EQUAL(PAGE_INVALID, h3.entry->contentState);

    // h1、h2 不受影响。
    TEST_ASSERT_TRUE(atomic_load(&h1.entry->isDirty));
    TEST_ASSERT_TRUE(atomic_load(&h2.entry->isDirty));

    pageCacheClearDirtyPages(&cache);

    pageEntryHandleDestroy(&h3);
    pageEntryHandleDestroy(&h2);
    pageEntryHandleDestroy(&h1);


    pageCacheDestroy(&cache);
}
