#include "integration/rtfs_integration_fixture.h"
#include "rtfs_test.h"

#include <errno.h>
#include <stdio.h>

#define RTFS_ITEST_SPACE_DIR "/space"

RTFS_TEST(IntegrationError_WhenSpaceIsExhausted_ShouldReturnEnospcAndKeepExistingEntriesVisible)
{
    RTFS_TEST_ANNOUNCE("IT-SPACE-01");
    RtfsIntegrationFixture fixture;
    struct stat st;
    char path[RTFS_ITEST_MAX_PATH_LEN];
    int ret = 0;
    unsigned created = 0;

    TEST_ASSERT_EQUAL(
        0,
        rtfsIntegrationFixtureFormatAndMount(
            &fixture,
            8U * BLOCK_PER_SEGMENT));

    TEST_ASSERT_EQUAL(0, rtfsIntegrationMkdir(&fixture, RTFS_ITEST_SPACE_DIR, 0755));

    for (created = 0; created < 512u; ++created)
    {
        TEST_ASSERT_TRUE(
            snprintf(path, sizeof(path), "%s/f%03u", RTFS_ITEST_SPACE_DIR, created) > 0);
        ret = rtfsIntegrationCreateFile(&fixture, path, 0644);
        if (ret != 0)
        {
            break;
        }
    }

    TEST_ASSERT_EQUAL(ENOSPC, ret);
    TEST_ASSERT_TRUE(created > 0u);

    TEST_ASSERT_TRUE(
        snprintf(path, sizeof(path), "%s/f%03u", RTFS_ITEST_SPACE_DIR, created - 1u) > 0);
    TEST_ASSERT_EQUAL(0, rtfsIntegrationStatPath(&fixture, path, &st));
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));

    TEST_ASSERT_TRUE(
        snprintf(path, sizeof(path), "%s/fail-after-full", RTFS_ITEST_SPACE_DIR) > 0);
    TEST_ASSERT_EQUAL(ENOSPC, rtfsIntegrationCreateFile(&fixture, path, 0644));
    TEST_ASSERT_EQUAL(
        ENOENT,
        rtfsIntegrationStatPath(&fixture, path, &st));

    rtfsIntegrationFixtureDestroy(&fixture);
}
