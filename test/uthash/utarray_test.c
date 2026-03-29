#include <stdio.h>
#include <stdlib.h>

#include "rtfs_test.h"

#include "uthash/utarray.h"
#include "utils/rtfs_log.h"


static int intCmp(const void *a, const void *b)
{
    int _a = *(int *)a;
    int _b = *(int *)b;
    return (_a > _b) - (_a < _b);
}


RTFS_TEST(UtArrayInitTest)
{
    UT_array a;

    utarray_init(&a, &ut_int_icd);

    utarray_done(&a);
}

RTFS_TEST(UtArrayIntPushPopTest)
{
    UT_array a;
    utarray_init(&a, &ut_int_icd);


    for (int i = 0; i < 10; ++i) utarray_push_back(&a, &i);
    TEST_ASSERT_EQUAL(utarray_len(&a), 10);

    int *p = (int *)utarray_back(&a);
    TEST_ASSERT_EQUAL(*p, 9);

    utarray_pop_back(&a);
    TEST_ASSERT_EQUAL(utarray_len(&a), 9);

    p = (int *)utarray_back(&a);
    TEST_ASSERT_EQUAL(*p, 8);


    utarray_done(&a);
}

RTFS_TEST(UtArrayInsertEraseTest)
{
    UT_array a;
    utarray_init(&a, &ut_int_icd);


    for (int i = 0; i < 5; ++i) utarray_push_back(&a, &i);

    int v = 100;
    utarray_insert(&a, &v, 2);

    TEST_ASSERT_EQUAL(utarray_len(&a), 6);

    int *p = (int *)utarray_eltptr(&a, 2);
    TEST_ASSERT_EQUAL(*p, 100);

    utarray_erase(&a, 2, 2);

    TEST_ASSERT_EQUAL(utarray_len(&a), 4);

    p = (int *)utarray_eltptr(&a, 2);
    TEST_ASSERT_EQUAL(*p, 3);


    utarray_done(&a);
}

RTFS_TEST(UtArrayResizeTest)
{
    UT_array a;
    utarray_init(&a, &ut_int_icd);


    utarray_resize(&a, 10);
    TEST_ASSERT_EQUAL(utarray_len(&a), 10);

    int *p = (int *)utarray_eltptr(&a, 5);
    TEST_ASSERT_EQUAL(*p, 0);

    utarray_resize(&a, 4);
    TEST_ASSERT_EQUAL(utarray_len(&a), 4);


    utarray_done(&a);
}

RTFS_TEST(UtArraySortFindTest)
{
    UT_array a;
    utarray_init(&a, &ut_int_icd);


    int data[] = {5, 3, 9, 1, 7};

    for (int i = 0; i < 5; ++i) utarray_push_back(&a, &data[i]);

    utarray_sort(&a, intCmp);

    for (int i = 0; i < utarray_len(&a); ++i) RTFS_LOG(RTFS_LOG_INFO, "%d %d", i, *(int *)utarray_eltptr(&a, i));

    // 因为 utarray 不知道元素类型，不知道怎么比较数组元素，所以需要提供函数，返回的是找到的元素指针。
    int key = 7;
    int *p = (int *)utarray_find(&a, &key, intCmp);

    TEST_ASSERT_TRUE(p != NULL);
    TEST_ASSERT_EQUAL(*p, 7);


    utarray_done(&a);
}

RTFS_TEST(UtArrayStringTest)
{
    UT_array a;
    utarray_init(&a, &ut_str_icd);


    const char *s1 = "hello";
    const char *s2 = "world";

    utarray_push_back(&a, &s1);
    utarray_push_back(&a, &s2);

    TEST_ASSERT_EQUAL(utarray_len(&a), 2);

    char **p = (char **)utarray_front(&a);
    TEST_ASSERT_EQUAL_STRING(*p, "hello");

    p = (char **)utarray_back(&a);
    TEST_ASSERT_EQUAL_STRING(*p, "world");


    utarray_done(&a);
}

RTFS_TEST(UtArrayIterTest)
{
    UT_array a;
    utarray_init(&a, &ut_int_icd);

    for (int i = 0; i < 5; ++i) utarray_push_back(&a, &i);

    int sum = 0;
    int *p = NULL;

    while (NULL != (p = (int *)utarray_next(&a, p))) sum += *p;
    TEST_ASSERT_EQUAL(sum, 10);


    utarray_done(&a);
}
