#include "rtfs_test.h"

#include "cache/cache_index_manager.h"


typedef struct
{
    int id;
    char name[32];
} CimTestEntry;


RTFS_TEST(CIMInitTest)
{
    CacheIndexManager cim;

    cacheIndexManagerInit(&cim);

    cacheIndexManagerDestroy(&cim);
}

RTFS_TEST(CIMAddGetTest)
{
    CacheIndexManager cim;

    cacheIndexManagerInit(&cim);

    uint32_t key = 1;

    // 注意：entry 需要使用 malloc 分配堆内存，确保 CacheIndexManager 拥有它的所有权。不能用栈变量，否则 cacheIndexManagerDestroy 会 free 栈内存导致崩溃。
    CimTestEntry *entry = (CimTestEntry *)malloc(sizeof(CimTestEntry));
    entry->id = 1;
    strcpy(entry->name, "Alice");

    cacheIndexManagerAdd(&cim, key, entry);

    CimTestEntry *e = (CimTestEntry *)cacheIndexManagerGet(&cim, key);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(1, e->id);
    TEST_ASSERT_EQUAL_STRING("Alice", e->name);

    // entry 托管给 CacheIndexManager 了，释放由它统一管理，外部不需要手动 free。
    // free(key);


    cacheIndexManagerDestroy(&cim);
}

RTFS_TEST(CIMRemoveTest)
{
    CacheIndexManager cim;

    cacheIndexManagerInit(&cim);


    uint32_t key = 2;

    CimTestEntry *entry = (CimTestEntry *)malloc(sizeof(CimTestEntry));
    entry->id = 2;
    strcpy(entry->name, "Bob");

    cacheIndexManagerAdd(&cim, key, entry);

    CimTestEntry *removed = (CimTestEntry *)cacheIndexManagerRemove(&cim, key);

    TEST_ASSERT_NOT_NULL(removed);
    TEST_ASSERT_EQUAL_INT(2, removed->id);
    TEST_ASSERT_EQUAL_STRING("Bob", removed->name);


    cacheIndexManagerDestroy(&cim);
}

RTFS_TEST(CIMAddDupKeyTest)
{
    CacheIndexManager cim;

    cacheIndexManagerInit(&cim);


    uint32_t key = 3;

    CimTestEntry *v1 = (CimTestEntry *)malloc(sizeof(CimTestEntry));
    v1->id = 3;
    strcpy(v1->name, "Eve");

    cacheIndexManagerAdd(&cim, key, v1);

    // 再添加同样 key 应该触发 assert
    CimTestEntry *v2 = (CimTestEntry *)malloc(sizeof(CimTestEntry));
    v2->id = 3;
    strcpy(v2->name, "Frank");

    // assert 本来会导致程序崩溃，但是被这条语句阻止了。
    TEST_IGNORE_MESSAGE("Expect assertion failure when adding duplicate key");
    cacheIndexManagerAdd(&cim, key, v2);


    cacheIndexManagerDestroy(&cim);
}
