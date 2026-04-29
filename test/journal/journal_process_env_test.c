#include "rtfs_test.h"

#include "journal/journal_process_env.h"
#include "utils/rtfs_log.h"
#include "uthash/utlist.h"


RTFS_TEST(JpeSingletonTest)
{
    JournalProcessEnv *env1 = journalProcessEnvGetInstance();
    JournalProcessEnv *env2 = journalProcessEnvGetInstance();

    RTFS_LOG(RTFS_LOG_DEBUG, "env1: %p", env1);
    RTFS_LOG(RTFS_LOG_DEBUG, "env2: %p", env2);

    TEST_ASSERT_NOT_NULL(env1);
    TEST_ASSERT_EQUAL_PTR(env1, env2);
}

RTFS_TEST(JpeTxIdTest)
{
    JournalProcessEnv *env = journalProcessEnvGetInstance();

    uint64_t id1 = journalProcessEnvAllocTxId(env);
    uint64_t id2 = journalProcessEnvAllocTxId(env);
    uint64_t id3 = journalProcessEnvAllocTxId(env);

    TEST_ASSERT_EQUAL_UINT64(0, id1);
    TEST_ASSERT_EQUAL_UINT64(1, id2);
    TEST_ASSERT_EQUAL_UINT64(2, id3);
}

RTFS_TEST(JpeCommitQueueTest)
{
    JournalProcessEnv *env = journalProcessEnvGetInstance();

    rtfsMutexInit(&env->mtx);
    rtfsCondInit(&env->cond);

    env->commitQueueHead = NULL;

    JournalContainer j1, j2;
    journalContainerInit(&j1);
    journalContainerInit(&j2);

    journalProcessEnvCommitJournal(env, &j1);
    journalProcessEnvCommitJournal(env, &j2);

    TEST_ASSERT_NOT_NULL(env->commitQueueHead);

    // 因为是尾插，因此链表结构是 j1 -> j2。
    TEST_ASSERT_EQUAL_PTR(&j1, env->commitQueueHead->journal);
    TEST_ASSERT_EQUAL_PTR(&j2, env->commitQueueHead->next->journal);

    // 为了不影响其他测试，手动清理 commit queue。
    // XXX 注意这里不能直接 journalProcessEnvDestory，因为是全局唯一单例，直接删肯定会出问题。
    {
        rtfsMutexLock(&env->mtx);

        JournalCommitNode *el = NULL, *tmp = NULL;
        DL_FOREACH_SAFE(env->commitQueueHead, el, tmp)
        {
            DL_DELETE(env->commitQueueHead, el);
            free(el);
        }

        env->commitQueueHead = NULL;

        rtfsMutexUnlock(&env->mtx);
    }

    rtfsCondDestroy(&env->cond);
    rtfsMutexDestroy(&env->mtx);
}
