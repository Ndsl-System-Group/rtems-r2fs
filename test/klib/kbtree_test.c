#include "rtfs_test.h"

#include "utils/rtfs_log.h"

#include "klib/kbtree.h"


typedef struct KbTreeTestEntry
{
    int key;
    int data;
} KbTreeTestEntry;


#define KBTREE_TEST_ENTRY_CMP(a, b) ((a).key < (b).key ? -1 : ((a).key > (b).key ? 1 : 0))

KBTREE_INIT(ktte, KbTreeTestEntry, KBTREE_TEST_ENTRY_CMP)


RTFS_TEST(KbtreeInsertTest)
{
    kbtree_t(ktte) *tree = kb_init(ktte, KB_DEFAULT_SIZE);


    KbTreeTestEntry t = {0};

    for (int i = 0; i < 10; ++i)
    {
        t.key = i;
        t.data = i * 10;
        kb_put(ktte, tree, t);
    }

    for (int i = 9; i >= 0; --i)
    {
        t.key = i;
        KbTreeTestEntry *p = kb_getp(ktte, tree, &t);

        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL(p->data, i * 10);
    }


    kb_destroy(ktte, tree);
}

RTFS_TEST(KbtreeInsertDupTest)
{
    kbtree_t(ktte) *tree = kb_init(ktte, KB_DEFAULT_SIZE);


    KbTreeTestEntry t = {0};

    for (int i = 0; i < 10; ++i)
    {
        t.key = i;
        t.data = i * 10;
        kb_put(ktte, tree, t);
    }

    // 刻意插入一条 key 重复的数据。插入 key = 0 的 data 数据，因为 key 重复，该条数据插入会被废弃。
    t.key = 0;
    t.data = 100;
    kb_put(ktte, tree, t);

    for (int i = 9; i >= 0; --i)
    {
        t.key = i;
        KbTreeTestEntry *p = kb_getp(ktte, tree, &t);

        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL(p->data, i * 10);
    }


    kb_destroy(ktte, tree);
}

RTFS_TEST(KbtreeOrderedIterTest)
{
    kbtree_t(ktte) *tree = kb_init(ktte, KB_DEFAULT_SIZE);


    int keys[] = {5, 1, 9, 3, 7};
    KbTreeTestEntry t;

    for (int i = 0; i < 5; ++i)
    {
        t.key = keys[i];
        t.data = keys[i];
        kb_put(ktte, tree, t);
    }

    kbitr_t itr;
    int key = -INT_MAX;

    for (kb_itr_first(ktte, tree, &itr); kb_itr_valid(&itr); kb_itr_next(ktte, tree, &itr))
    {
        KbTreeTestEntry *p = &kb_itr_key(KbTreeTestEntry, &itr);

        RTFS_LOG(RTFS_LOG_INFO, "key = %d, data = %d", p->key, p->data);

        TEST_ASSERT_TRUE(p->key > key);
        key = p->key;
    }


    kb_destroy(ktte, tree);
}

RTFS_TEST(KbtreeIntervalTest)
{
    kbtree_t(ktte) *tree = kb_init(ktte, KB_DEFAULT_SIZE);


    KbTreeTestEntry t;

    // 插入 0~9。
    for (int i = 0; i < 10; ++i)
    {
        t.key = i;
        t.data = i;
        kb_put(ktte, tree, t);
    }

    // 查找 5 的区间。
    KbTreeTestEntry query = {.key = 5};
    KbTreeTestEntry *lower = NULL;
    KbTreeTestEntry *upper = NULL;

    // lower 找到的是最后一个 <= query 的元素。upper 找到的是第一个 >= query 的元素。如果存在等于 query 的 key，lower 会直接指向该元素，同理 upper。
    kb_interval(ktte, tree, query, &lower, &upper);

    // lower = 5, upper = 5。
    TEST_ASSERT_NOT_NULL(lower);
    TEST_ASSERT_NOT_NULL(upper);

    TEST_ASSERT_EQUAL(lower->key, 5);
    TEST_ASSERT_EQUAL(upper->key, 5);


    kb_destroy(ktte, tree);
}

RTFS_TEST(KbtreeIntervalTest2)
{
    kbtree_t(ktte) *tree = kb_init(ktte, KB_DEFAULT_SIZE);


    KbTreeTestEntry t;

    // 插入偶数。
    for (int i = 0; i < 10; i += 2)
    {
        t.key = i;
        t.data = i;
        kb_put(ktte, tree, t);
    }

    // 查找 5 的区间。
    KbTreeTestEntry query = {.key = 5};
    KbTreeTestEntry *lower = NULL;
    KbTreeTestEntry *upper = NULL;

    kb_interval(ktte, tree, query, &lower, &upper);

    // lower = 4, upper = 6。
    TEST_ASSERT_NOT_NULL(lower);
    TEST_ASSERT_NOT_NULL(upper);

    TEST_ASSERT_EQUAL(lower->key, 4);
    TEST_ASSERT_EQUAL(upper->key, 6);


    kb_destroy(ktte, tree);
}

RTFS_TEST(KbtreeEmptyTest)
{
    kbtree_t(ktte) *tree = kb_init(ktte, KB_DEFAULT_SIZE);


    KbTreeTestEntry t = {.key = 1};

    TEST_ASSERT_NULL(kb_getp(ktte, tree, &t));

    kbitr_t itr;
    kb_itr_first(ktte, tree, &itr);

    TEST_ASSERT_FALSE(kb_itr_valid(&itr));


    kb_destroy(ktte, tree);
}

RTFS_TEST(KbtreeRangeEraseTest)
{
    kbtree_t(ktte) *tree = kb_init(ktte, KB_DEFAULT_SIZE);


    KbTreeTestEntry t;

    // 插入 0~9。
    for (int i = 9; i >= 0; --i)
    {
        t.key = i;
        t.data = i;
        kb_put(ktte, tree, t);
    }

    // 删除 [3, 7) 范围内的数。
    int start = 3;
    int end = 7;

    KbTreeTestEntry query;
    KbTreeTestEntry *lower = NULL;
    KbTreeTestEntry *upper = NULL;

    // 找 >= start 的第一个。
    query.key = start;
    kb_interval(ktte, tree, query, &lower, &upper);

    KbTreeTestEntry *cur = upper;

    while (cur && cur->key < end)
    {
        int k = cur->key;

        // 删除当前。
        kb_del(ktte, tree, (KbTreeTestEntry){.key = k});

        // 查找后继（严格大于 k）。
        query.key = k;
        kb_interval(ktte, tree, query, &lower, &upper);

        cur = upper;
    }

    // 验证。
    for (int i = 0; i < 10; ++i)
    {
        t.key = i;
        KbTreeTestEntry *p = kb_getp(ktte, tree, &t);

        if (i >= start && i < end)
        {
            TEST_ASSERT_NULL(p);
        }
        else
        {
            TEST_ASSERT_NOT_NULL(p);

            RTFS_LOG(RTFS_LOG_INFO, "key = %d, data = %d", p->key, p->data);
        }
    }


    kb_destroy(ktte, tree);
}
