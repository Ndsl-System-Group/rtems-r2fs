#include "rtfs_test.h"

#include "cexception/cexception.h"

#include "utils/rtfs_log.h"


RTFS_TEST(CeNoThrowTest)
{
    CEXCEPTION_T e;


    int reached = 0;

    Try
    {
        reached = 1;
    }
    Catch(e)
    {
        TEST_FAIL_MESSAGE("Should not catch exception");
    }

    TEST_ASSERT_EQUAL_INT(1, reached);
}

RTFS_TEST(CeThrowTest)
{
    CEXCEPTION_T e;


    int caught = 0;

    Try
    {
        Throw(100);

        TEST_FAIL_MESSAGE("Should not reach here");
    }
    Catch(e)
    {
        caught = 1;

        TEST_ASSERT_EQUAL_INT(100, e);
    }

    TEST_ASSERT_EQUAL_INT(1, caught);
}

RTFS_TEST(CeNestedThrowTest)
{
    CEXCEPTION_T e;


    int innerCaught = 0;
    int outerCaught = 0;

    Try
    {
        Try
        {
            Throw(200);
        }
        Catch(e)
        {
            innerCaught = 1;

            TEST_ASSERT_EQUAL_INT(200, e);

            // 继续往外抛。
            Throw(e);
        }
    }
    Catch(e)
    {
        outerCaught = 1;
        TEST_ASSERT_EQUAL_INT(200, e);
    }

    TEST_ASSERT_EQUAL_INT(1, innerCaught);
    TEST_ASSERT_EQUAL_INT(1, outerCaught);
}

RTFS_TEST(CeFlowContinueTest)
{
    CEXCEPTION_T e;


    int flag = 0;

    Try
    {
        Throw(300);
    }
    Catch(e)
    {
        TEST_ASSERT_EQUAL_INT(300, e);
    }

    // 应该还能继续执行。
    flag = 1;

    RTFS_LOG(RTFS_LOG_INFO, "flag: %d", flag);

    TEST_ASSERT_NOT_EQUAL_INT(0, flag);
}
