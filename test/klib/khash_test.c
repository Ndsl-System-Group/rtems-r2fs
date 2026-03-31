#include "rtfs_test.h"

#include "utils/rtfs_log.h"

#include "klib/khash.h"


// 这个宏定义的哈希表的键是 int 类型，第二个参数代表需要映射的 data 类型。
KHASH_MAP_INIT_INT(khte, int)


RTFS_TEST(KhashInsertTest)
{
    khash_t(khte) *h = kh_init(khte);

    int res;
    khiter_t iter;

    // 插入 0~9。
    for (int i = 0; i < 10; ++i)
    {
        iter = kh_put(khte, h, i, &res);
        kh_value(h, iter) = i * 10;

        TEST_ASSERT_EQUAL(res, 1);
    }

    TEST_ASSERT_EQUAL(kh_size(h), 10);

    // 查找。
    for (int i = 0; i < 10; ++i)
    {
        iter = kh_get(khte, h, i);

        TEST_ASSERT_NOT_EQUAL(iter, kh_end(h));
        TEST_ASSERT_EQUAL(kh_value(h, iter), i * 10);
    }


    kh_destroy(khte, h);
}

RTFS_TEST(KhashUpdateTest)
{
    khash_t(khte) *h = kh_init(khte);


    int res;
    khiter_t iter;

    // 插入。
    iter = kh_put(khte, h, 5, &res);
    TEST_ASSERT_EQUAL(res, 1);
    kh_value(h, iter) = 100;

    // 再插入同一个 key。因为已存在，所以 res 是 0。
    iter = kh_put(khte, h, 5, &res);
    TEST_ASSERT_EQUAL(res, 0);

    kh_value(h, iter) = 200;

    iter = kh_get(khte, h, 5);
    TEST_ASSERT_EQUAL(kh_value(h, iter), 200);


    kh_destroy(khte, h);
}

RTFS_TEST(KhashDeleteTest)
{
    khash_t(khte) *h = kh_init(khte);


    int res;
    khiter_t iter;

    // 插入。
    for (int i = 0; i < 10; ++i)
    {
        iter = kh_put(khte, h, i, &res);
        kh_value(h, iter) = i;
    }

    // 删除 1~4。
    for (int i = 1; i <= 4; ++i)
    {
        iter = kh_get(khte, h, i);
        TEST_ASSERT_NOT_EQUAL(iter, kh_end(h));

        kh_del(khte, h, iter);
    }

    TEST_ASSERT_EQUAL(kh_size(h), 6);

    // 验证删除。
    for (int i = 0; i < 10; ++i)
    {
        iter = kh_get(khte, h, i);

        if (i >= 1 && i <= 4)
        {
            TEST_ASSERT_TRUE(iter == kh_end(h));
        }
        else
        {
            RTFS_LOG(RTFS_LOG_INFO, "key = %d, value = %d", iter, kh_value(h, iter));

            TEST_ASSERT_TRUE(iter != kh_end(h));
        }
    }


    kh_destroy(khte, h);
}

RTFS_TEST(KhashReuseDeletedSlotTest)
{
    khash_t(khte) *h = kh_init(khte);


    int res;
    khiter_t iter;

    // 插入。
    for (int i = 0; i < 5; ++i)
    {
        iter = kh_put(khte, h, i, &res);
        kh_value(h, iter) = i;

        TEST_ASSERT_EQUAL(res, 1);
    }

    // 删除。
    iter = kh_get(khte, h, 2);
    kh_del(khte, h, iter);

    // 再插入同 key，这时候会返回 2。
    iter = kh_put(khte, h, 2, &res);
    TEST_ASSERT_EQUAL(res, 2);

    kh_value(h, iter) = 114514;

    iter = kh_get(khte, h, 2);
    TEST_ASSERT_EQUAL(kh_value(h, iter), 114514);


    kh_destroy(khte, h);
}

RTFS_TEST(KhashIterTest)
{
    khash_t(khte) *h = kh_init(khte);


    int res;
    khiter_t iter;

    int keys[] = {5, 1, 9, 3, 7};

    for (int i = 0; i < 5; ++i)
    {
        iter = kh_put(khte, h, i, &res);
        kh_value(h, iter) = keys[i];

        TEST_ASSERT_EQUAL(res, 1);
    }

    int count = 0;

    for (iter = kh_begin(h); iter != kh_end(h); ++iter)
    {
        if (!kh_exist(h, iter)) continue;

        RTFS_LOG(RTFS_LOG_INFO, "key = %d val = %d", kh_key(h, iter), kh_value(h, iter));

        ++count;
    }

    TEST_ASSERT_EQUAL(count, 5);


    kh_destroy(khte, h);
}

RTFS_TEST(KhashEmptyTest)
{
    khash_t(khte) *h = kh_init(khte);


    khiter_t iter = kh_get(khte, h, 1);

    TEST_ASSERT_EQUAL(iter, kh_end(h));
    TEST_ASSERT_EQUAL(kh_size(h), 0);


    kh_destroy(khte, h);
}

RTFS_TEST(KhashClearTest)
{
    khash_t(khte) *h = kh_init(khte);


    int res;
    khiter_t iter;

    for (int i = 0; i < 10; ++i)
    {
        iter = kh_put(khte, h, i, &res);
        kh_value(h, iter) = i;
    }

    kh_clear(khte, h);

    TEST_ASSERT_EQUAL(kh_size(h), 0);

    // 再使用。
    iter = kh_put(khte, h, 42, &res);
    TEST_ASSERT_EQUAL(res, 1);

    iter = kh_get(khte, h, 42);
    TEST_ASSERT_NOT_EQUAL(iter, kh_end(h));


    kh_destroy(khte, h);
}

RTFS_TEST(KhashStressTest)
{
    khash_t(khte) *h = kh_init(khte);


    int res;
    khiter_t iter;

    const int N = 10000;

    // 插入。
    for (int i = 0; i < N; ++i)
    {
        iter = kh_put(khte, h, i, &res);
        kh_value(h, iter) = i;
    }

    TEST_ASSERT_EQUAL(kh_size(h), N);

    // 查找。
    for (int i = 0; i < N; ++i)
    {
        iter = kh_get(khte, h, i);
        TEST_ASSERT_NOT_EQUAL(iter, kh_end(h));
    }


    kh_destroy(khte, h);
}


KHASH_MAP_INIT_PTR(khptr, int)

RTFS_TEST(KhashPtrKeyTest)
{
    khash_t(khptr) *h = kh_init(khptr);
    TEST_ASSERT_NOT_NULL(h);

    khiter_t iter;
    int res;

    // 模拟一些指针 key。
    void *keys[5];
    for (int i = 0; i < 5; ++i)
    {
        keys[i] = malloc(1);

        iter = kh_put(khptr, h, keys[i], &res);
        kh_value(h, iter) = i * 10;

        TEST_ASSERT_EQUAL(res, 1);
    }

    TEST_ASSERT_EQUAL(kh_size(h), 5);

    // 查找。
    for (int i = 0; i < 5; ++i)
    {
        iter = kh_get(khptr, h, keys[i]);

        RTFS_LOG(RTFS_LOG_INFO, "key: %p, data: %d", kh_key(h, iter), kh_value(h, iter));

        TEST_ASSERT_NOT_EQUAL(iter, kh_end(h));
        TEST_ASSERT_EQUAL(kh_value(h, iter), i * 10);
    }

    // 更新。
    iter = kh_put(khptr, h, keys[2], &res);
    TEST_ASSERT_EQUAL(res, 0); // 已存在。
    kh_value(h, iter) = 999;

    iter = kh_get(khptr, h, keys[2]);
    TEST_ASSERT_EQUAL(kh_value(h, iter), 999);

    // 删除。
    iter = kh_get(khptr, h, keys[1]);
    kh_del(khptr, h, iter);

    iter = kh_get(khptr, h, keys[1]);
    TEST_ASSERT_EQUAL(iter, kh_end(h));
    TEST_ASSERT_EQUAL(kh_size(h), 4);

    // 遍历并验证。
    int count = 0;
    for (iter = kh_begin(h); iter != kh_end(h); ++iter)
    {
        if (!kh_exist(h, iter)) continue;

        RTFS_LOG(RTFS_LOG_INFO, "key: %p, data: %d", kh_key(h, iter), kh_value(h, iter));

        TEST_ASSERT_TRUE(kh_value(h, iter) >= 0);

        ++count;
    }
    TEST_ASSERT_EQUAL(count, 4);

    // 清理。
    for (int i = 0; i < 5; ++i) free(keys[i]);
    kh_destroy(khptr, h);
}
