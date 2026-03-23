#include "rtfs_test.h"

#include "utils/rtfs_log.h"

#include "klib/kbtree.h"


typedef struct KbTreeTestEntry
{
    int key;
    int value;
} KbTreeTestEntry;


#define KBTREE_TEST_ENTRY_CMP(a, b) ((a).key < (b).key ? -1 : ((a).key > (b).key))

KBTREE_INIT(ktte, KbTreeTestEntry, KBTREE_TEST_ENTRY_CMP)


RTFS_TEST(KbtreeInsertFindTest)
{
    kbtree_t(ktte) *tree = kb_init(ktte, KB_DEFAULT_SIZE);


    KbTreeTestEntry t = {0};

    for (int i = 0; i < 10; ++i)
    {
        t.key = i;
        t.value = i * 10;
        kb_put(ktte, tree, t);
    }

    for (int i = 9; i >= 0; --i)
    {
        t.key = i;
        KbTreeTestEntry *p = kb_getp(ktte, tree, &t);

        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL(p->value, i * 10);
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
        t.value = keys[i];
        kb_put(ktte, tree, t);
    }

    kbitr_t itr;
    kb_itr_first(ktte, tree, &itr);
    int key = -INT_MAX;

    for (; kb_itr_valid(&itr); kb_itr_next(ktte, tree, &itr))
    {
        KbTreeTestEntry *p = &kb_itr_key(KbTreeTestEntry, &itr);

        RTFS_LOG(RTFS_LOG_INFO, "key = %d, value = %d", p->key, p->value);

        TEST_ASSERT_TRUE(p->key > key);
        key = p->key;
    }


    kb_destroy(ktte, tree);
}

RTFS_TEST(KbtreeIntervalTest)
{
    kbtree_t(ktte) *tree = kb_init(ktte, KB_DEFAULT_SIZE);


    KbTreeTestEntry t;

    // 插入偶数。
    for (int i = 0; i < 10; i += 2)
    {
        t.key = i;
        t.value = i;
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
        t.value = i;
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

            RTFS_LOG(RTFS_LOG_INFO, "key = %d, value = %d", p->key, p->value);
        }
    }


    kb_destroy(ktte, tree);
}
