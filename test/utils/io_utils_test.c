#include "rtfs_test.h"

#include "utils/io_utils.h"


typedef struct IoUtilsTestWorkerArg
{
    AsyncVecioSynchronizer *sync;
    comm_cmd_result res;
    int delayMs;
} IoUtilsTestWorkerArg;

// 工作线程：延迟后完成一次 I/O。
static void *workerComplete(void *arg)
{
    IoUtilsTestWorkerArg *p = (IoUtilsTestWorkerArg *)arg;

    usleep(p->delayMs * 1000);

    asyncVecioSynchronizerCpltOnce(p->sync, p->res);


    return NULL;
}


// ioNum = 0，应立即完成。
RTFS_TEST(AvsZeroIoTest)
{
    AsyncVecioSynchronizer sync;
    asyncVecioSynchronizerInit(&sync, 0);


    comm_cmd_result res = asyncVecioSynchronizerWaitCplt(&sync);

    TEST_ASSERT_EQUAL(COMM_CMD_SUCCESS, res);


    asyncVecioSynchronizerDestroy(&sync);
}

// 单次完成。
RTFS_TEST(AvsSingleIoTest)
{
    AsyncVecioSynchronizer sync;
    asyncVecioSynchronizerInit(&sync, 1);


    asyncVecioSynchronizerCpltOnce(&sync, COMM_CMD_SUCCESS);

    comm_cmd_result res = asyncVecioSynchronizerWaitCplt(&sync);

    TEST_ASSERT_EQUAL(COMM_CMD_SUCCESS, res);


    asyncVecioSynchronizerDestroy(&sync);
}

// 多次完成，全部成功。
RTFS_TEST(AvsMultiIoTest)
{
    int ioNum = 3;

    AsyncVecioSynchronizer sync;
    asyncVecioSynchronizerInit(&sync, ioNum);


    for (int i = 0; i < ioNum; ++i) asyncVecioSynchronizerCpltOnce(&sync, COMM_CMD_SUCCESS);

    comm_cmd_result res = asyncVecioSynchronizerWaitCplt(&sync);

    TEST_ASSERT_EQUAL(COMM_CMD_SUCCESS, res);


    asyncVecioSynchronizerDestroy(&sync);
}

// 多次完成，出现错误，第一个非 SUCCESS 应被记录。
RTFS_TEST(AvsMultiIoWithErrorTest)
{
    AsyncVecioSynchronizer sync;
    asyncVecioSynchronizerInit(&sync, 3);


    asyncVecioSynchronizerCpltOnce(&sync, COMM_CMD_SUCCESS);
    asyncVecioSynchronizerCpltOnce(&sync, COMM_CMD_CQE_ERROR);
    asyncVecioSynchronizerCpltOnce(&sync, COMM_CMD_SUCCESS);

    comm_cmd_result res = asyncVecioSynchronizerWaitCplt(&sync);

    TEST_ASSERT_EQUAL(COMM_CMD_CQE_ERROR, res);


    asyncVecioSynchronizerDestroy(&sync);
}

// 多次完成，两个错误时应保留第一个错误。
RTFS_TEST(AvsKeepFirstErrorTest)
{
    AsyncVecioSynchronizer sync;
    asyncVecioSynchronizerInit(&sync, 3);


    asyncVecioSynchronizerCpltOnce(&sync, COMM_CMD_TID_QUERY_ERROR);
    asyncVecioSynchronizerCpltOnce(&sync, COMM_CMD_CQE_ERROR);
    asyncVecioSynchronizerCpltOnce(&sync, COMM_CMD_SUCCESS);

    comm_cmd_result res = asyncVecioSynchronizerWaitCplt(&sync);

    TEST_ASSERT_EQUAL(COMM_CMD_TID_QUERY_ERROR, res);


    asyncVecioSynchronizerDestroy(&sync);
}

// 回调函数测试。
RTFS_TEST(AvsGenericCallbackTest)
{
    AsyncVecioSynchronizer sync;
    asyncVecioSynchronizerInit(&sync, 1);


    asyncVecioSynchronizerGenericCallback(COMM_CMD_SUCCESS, &sync);

    comm_cmd_result res = asyncVecioSynchronizerWaitCplt(&sync);

    TEST_ASSERT_EQUAL(COMM_CMD_SUCCESS, res);


    asyncVecioSynchronizerDestroy(&sync);
}

// 多线程异步完成测试。
RTFS_TEST(AvsThreadedCompleteTest)
{
    AsyncVecioSynchronizer sync;
    pthread_t t1, t2, t3;

    IoUtilsTestWorkerArg a1 = {&sync, COMM_CMD_SUCCESS, 30};
    IoUtilsTestWorkerArg a2 = {&sync, COMM_CMD_SUCCESS, 10};
    IoUtilsTestWorkerArg a3 = {&sync, COMM_CMD_SUCCESS, 20};

    asyncVecioSynchronizerInit(&sync, 3);


    pthread_create(&t1, NULL, workerComplete, &a1);
    pthread_create(&t2, NULL, workerComplete, &a2);
    pthread_create(&t3, NULL, workerComplete, &a3);

    comm_cmd_result res = asyncVecioSynchronizerWaitCplt(&sync);

    TEST_ASSERT_EQUAL(COMM_CMD_SUCCESS, res);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);


    asyncVecioSynchronizerDestroy(&sync);
}

// 多线程异步完成测试。
RTFS_TEST(AvsThreadedWithErrorTest)
{
    AsyncVecioSynchronizer sync;

    pthread_t t1, t2, t3;

    IoUtilsTestWorkerArg a1 = {&sync, COMM_CMD_SUCCESS, 30};
    IoUtilsTestWorkerArg a2 = {&sync, COMM_CMD_CQE_ERROR, 10};
    IoUtilsTestWorkerArg a3 = {&sync, COMM_CMD_SUCCESS, 20};

    asyncVecioSynchronizerInit(&sync, 3);


    pthread_create(&t1, NULL, workerComplete, &a1);
    pthread_create(&t2, NULL, workerComplete, &a2);
    pthread_create(&t3, NULL, workerComplete, &a3);

    comm_cmd_result res = asyncVecioSynchronizerWaitCplt(&sync);

    TEST_ASSERT_EQUAL(COMM_CMD_CQE_ERROR, res);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);


    asyncVecioSynchronizerDestroy(&sync);
}
