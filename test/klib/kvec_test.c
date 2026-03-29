#include "rtfs_test.h"

#include "utils/rtfs_log.h"

#include "klib/kvec.h"


RTFS_TEST(KvecPushTest)
{
    kvec_t(int) v;
    kv_init(v);


    for (int i = 0; i < 10; ++i)
    {
        kv_push(int, v, i *i);
    }

    TEST_ASSERT_EQUAL(kv_size(v), 10);

    for (int i = 0; i < 10; ++i)
    {
        TEST_ASSERT_EQUAL(kv_a(int, v, i), i * i);
    }


    kv_destroy(v);
}

RTFS_TEST(KvecPushpTest)
{
    kvec_t(int) v;
    kv_init(v);


    for (int i = 0; i < 10; ++i)
    {
        // kv_pushp 的作用是：在 vector 尾部开一个新位置，并返回这个位置的指针。
        int *p = (kv_pushp(int, v));
        *p = i * 2;
    }

    TEST_ASSERT_EQUAL(kv_size(v), 10);

    for (int i = 0; i < 10; ++i) TEST_ASSERT_EQUAL(kv_a(int, v, i), i * 2);


    kv_destroy(v);
}

RTFS_TEST(KvecPopTest)
{
    kvec_t(int) v;
    kv_init(v);

    for (int i = 0; i < 5; ++i) kv_push(int, v, i);

    for (int i = 4; i >= 2; --i)
    {
        int val = kv_pop(v);
        TEST_ASSERT_EQUAL(val, i);
    }

    TEST_ASSERT_EQUAL(kv_size(v), 2);

    kv_destroy(v);
}

RTFS_TEST(KvecSizeCapTest)
{
    kvec_t(int) v;
    kv_init(v);


    // TODO
    for (int i = 0; i < 10; ++i)
    {
        kv_push(int, v, i *i);

        RTFS_LOG(RTFS_LOG_INFO, "vector size: %d, capicity: %d", kv_size(v), kv_cap(v));

        // kv_size() 返回的是 vec 的大小 size，kv_cap() 返回的是容量 capicity。
        // TEST_ASSERT_GREATER_OR_EQUAL(a, b) 检测的是 b 大于等于 a，跟常规认知反过来了。
        TEST_ASSERT_GREATER_OR_EQUAL(kv_size(v), kv_cap(v));
    }


    kv_destroy(v);
}

RTFS_TEST(KvecSizeCapTest2)
{
    kvec_t(int) v;
    kv_init(v);


    kv_resize(int, v, 14);

    RTFS_LOG(RTFS_LOG_INFO, "vector size: %d, capicity: %d", kv_size(v), kv_cap(v)); // 14, 16
    TEST_ASSERT_EQUAL(kv_size(v), 14);
    TEST_ASSERT_EQUAL(kv_cap(v), 16);

    kv_a(int, v, 5) = 50;
    kv_a(int, v, 20) = 200;

    TEST_ASSERT_EQUAL(kv_a(int, v, 5), 50);
    TEST_ASSERT_EQUAL(kv_a(int, v, 20), 200);

    RTFS_LOG(RTFS_LOG_INFO, "vector size: %d, capicity: %d", kv_size(v), kv_cap(v)); // 21, 32
    TEST_ASSERT_EQUAL(kv_size(v), 21);
    TEST_ASSERT_EQUAL(kv_cap(v), 32);


    kv_destroy(v);
}

RTFS_TEST(KvecCopyTest)
{
    kvec_t(int) v1, v2;

    kv_init(v1);
    kv_init(v2);


    for (int i = 0; i < 10; ++i) kv_push(int, v1, i);

    kv_copy(int, v2, v1);

    TEST_ASSERT_EQUAL(kv_size(v2), kv_size(v1));
    TEST_ASSERT_EQUAL(kv_cap(v2), kv_cap(v1));

    for (int i = 0; i < 10; ++i) TEST_ASSERT_EQUAL(kv_a(int, v2, i), kv_a(int, v1, i));


    kv_destroy(v2);
    kv_destroy(v1);
}

RTFS_TEST(KvecEmptyTest)
{
    kvec_t(int) v;
    kv_init(v);


    TEST_ASSERT_EQUAL(kv_size(v), 0);
    TEST_ASSERT_EQUAL(kv_cap(v), 0);


    kv_destroy(v);
}

RTFS_TEST(KvecStressTest)
{
    kvec_t(int) v;
    kv_init(v);


    const int N = 100000;

    for (int i = 0; i < N; ++i) kv_push(int, v, i);

    TEST_ASSERT_EQUAL(kv_size(v), N);

    for (int i = 0; i < N; ++i) TEST_ASSERT_EQUAL(kv_a(int, v, i), i);


    kv_destroy(v);
}
