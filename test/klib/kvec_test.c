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
        TEST_ASSERT_EQUAL(kv_A(v, i), i * i);
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

    for (int i = 0; i < 10; ++i) TEST_ASSERT_EQUAL(kv_A(v, i), i * 2);


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

        RTFS_LOG(RTFS_LOG_INFO, "vector size: %d, capicity: %d", kv_size(v), kv_max(v));

        // kv_size() 返回的是 vec 的大小 size，kv_max() 返回的是容量 capicity。
        TEST_ASSERT_GREATER_OR_EQUAL(kv_max(v), kv_size(v));
    }


    kv_destroy(v);
}

// RTFS_TEST(KvecSparseAssignTest)
// {
//     kvec_t(int) v;
//     kv_init(v);

//     kv_a(int, v, 5) = 50;
//     kv_a(int, v, 20) = 200;

//     TEST_ASSERT_EQUAL(kv_size(v), 21);

//     TEST_ASSERT_EQUAL(kv_A(v, 5), 50);
//     TEST_ASSERT_EQUAL(kv_A(v, 20), 200);

//     kv_destroy(v);
// }


// RTFS_TEST(KvecCopyTest)
// {
//     kvec_t(int) v1, v2;

//     kv_init(v1);
//     kv_init(v2);

//     for (int i = 0; i < 10; ++i)
//     {
//         kv_push(int, v1, i);
//     }

//     kv_copy(int, v2, v1);

//     TEST_ASSERT_EQUAL(kv_size(v2), kv_size(v1));

//     for (int i = 0; i < 10; ++i)
//     {
//         TEST_ASSERT_EQUAL(kv_A(v2, i), kv_A(v1, i));
//     }

//     kv_destroy(v1);
//     kv_destroy(v2);
// }


// RTFS_TEST(KvecEmptyTest)
// {
//     kvec_t(int) v;
//     kv_init(v);

//     TEST_ASSERT_EQUAL(kv_size(v), 0);
//     TEST_ASSERT_EQUAL(kv_max(v), 0);

//     kv_destroy(v);
// }


// RTFS_TEST(KvecStressTest)
// {
//     kvec_t(int) v;
//     kv_init(v);

//     const int N = 100000;

//     for (int i = 0; i < N; ++i)
//     {
//         kv_push(int, v, i);
//     }

//     TEST_ASSERT_EQUAL(kv_size(v), N);

//     for (int i = 0; i < N; ++i)
//     {
//         TEST_ASSERT_EQUAL(kv_A(v, i), i);
//     }

//     kv_destroy(v);
// }
