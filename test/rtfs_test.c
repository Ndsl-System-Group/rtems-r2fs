#include "rtfs_test.h"
#include "integration/rtfs_integration_fixture.h"
#include "integration/rtfs_rtems_mount_fixture.h"
#include "rtfs_config.h"

#include <errno.h>
#include <stdlib.h>
#include <rtems/counter.h>
#include <string.h>
#include <sys/stat.h>


struct RtfsTestEntry rtfsTestArray[RTFS_MAX_TESTS] = {};

int rtfsTestCount = 0;

static uint64_t rtfsCounterFreq = 0;


void setUp(void) {}

void tearDown(void)
{
    rtfsIntegrationFixtureCleanupActive();
    rtfsRtemsMountFixtureCleanupActive();
}


// 获取当前硬件计数器值。
static uint64_t rtfsGetCounter();

// 计数器差值转换为微秒。
static uint64_t rtfsCounterToUs(uint64_t diff);

// 输出耗时，自动切换 us/ms/s。
static void rtfsPrintElapsedTime(uint64_t us);
static void rtfsCheckBaseFilesystemAfterTest(const char *test_name);
static bool rtfsShouldListTests(void);
static void rtfsListTests(void);
static const char *rtfsResolveTestGroup(const struct RtfsTestEntry *entry);
static const char *rtfsInferGroupFromFile(const char *source_file);
static const char *rtfsGetConfiguredGroupFilter(void);
static const char *rtfsGetConfiguredNameFilter(void);
static bool rtfsGroupMatches(const char *test_group, const char *group_filter);
static bool rtfsNameMatches(const char *test_name, const char *name_filter);
static bool rtfsFilterTokenEquals(const char *begin, const char *end, const char *value);
static bool rtfsShouldRunTest(const struct RtfsTestEntry *entry);


void rtfsRunAllTests(void)
{
    if (rtfsShouldListTests())
    {
        rtfsListTests();
        return;
    }

    UNITY_BEGIN();

    int failedCases = 0;
    rtfsCounterFreq = rtems_counter_frequency();
    uint64_t totalBegin = rtfsGetCounter();

    for (int i = 0; i < rtfsTestCount; ++i)
    {
        const struct RtfsTestEntry *entry = &rtfsTestArray[i];
        const char *group = rtfsResolveTestGroup(entry);

        if (!rtfsShouldRunTest(entry))
        {
            continue;
        }

        printf(
            RTFS_COLOR_GREEN "[ RUN      ] [%s] %s" RTFS_COLOR_RESET "\n",
            group,
            entry->name);

        failedCases = Unity.TestFailures;

        uint64_t begin = rtfsGetCounter();
        RUN_TEST(entry->func);
        uint64_t end = rtfsGetCounter();

        uint64_t costUs = rtfsCounterToUs(end - begin);

        if (Unity.TestFailures > failedCases)
        {
            printf(RTFS_COLOR_RED "[   FAIL   ] [%s] %s (", group, entry->name);
            rtfsPrintElapsedTime(costUs);
            printf(")" RTFS_COLOR_RESET "\n");
        }
        else
        {
            printf(RTFS_COLOR_GREEN "[       OK ] [%s] %s (", group, entry->name);
            rtfsPrintElapsedTime(costUs);
            printf(")" RTFS_COLOR_RESET "\n");
        }

        rtfsCheckBaseFilesystemAfterTest(entry->name);
    }

    uint64_t totalEnd = rtfsGetCounter();
    uint64_t totalUs = rtfsCounterToUs(totalEnd - totalBegin);

    printf(RTFS_COLOR_YELLOW "\n[ TOTAL TIME ] ");
    rtfsPrintElapsedTime(totalUs);
    printf(RTFS_COLOR_RESET "\n");


    UNITY_END();
}


uint64_t rtfsGetCounter()
{
    return rtems_counter_read();
}

uint64_t rtfsCounterToUs(uint64_t diff)
{
    return (diff * 1000000ULL) / rtfsCounterFreq;
}

void rtfsPrintElapsedTime(uint64_t us)
{
    if (us < 1000ULL)
    {
        printf("%llu us", (unsigned long long)us);
    }
    else if (us < 1000000ULL)
    {
        printf("%.3f ms", (double)us / 1000.0);
    }
    else
    {
        printf("%.3f s", (double)us / 1000000.0);
    }
}

static void rtfsCheckBaseFilesystemAfterTest(const char *test_name)
{
    struct stat st;

    errno = 0;
    if (stat("/dev", &st) != 0)
    {
        printf(
            RTFS_COLOR_RED "[ FS-CHECK FAIL ] after %s errno=%d" RTFS_COLOR_RESET "\n",
            test_name,
            errno);
        TEST_FAIL_MESSAGE("base filesystem check failed");
    }
}

static bool rtfsShouldListTests(void)
{
    const char *list_tests = getenv("RTFS_TEST_LIST");

    if (list_tests != NULL)
    {
        return list_tests[0] != '\0' && strcmp(list_tests, "0") != 0;
    }

#ifdef RTFS_CONFIG_LIST_TESTS
    return RTFS_CONFIG_LIST_TESTS != 0;
#else
    return false;
#endif
}

static void rtfsListTests(void)
{
    for (int i = 0; i < rtfsTestCount; ++i)
    {
        const struct RtfsTestEntry *entry = &rtfsTestArray[i];

        printf("[%s] %s\n", rtfsResolveTestGroup(entry), entry->name);
    }
}

static const char *rtfsResolveTestGroup(const struct RtfsTestEntry *entry)
{
    if (entry == NULL)
    {
        return "unknown";
    }

    if (entry->group != NULL && entry->group[0] != '\0')
    {
        return entry->group;
    }

    return rtfsInferGroupFromFile(entry->source_file);
}

static const char *rtfsInferGroupFromFile(const char *source_file)
{
    static const char test_prefix[] = "test/";
    static const char fallback_group[] = "default";
    static char inferred_group[64];
    const char *group_begin;
    const char *group_end;
    const char *match;
    size_t group_len;

    if (source_file == NULL || source_file[0] == '\0')
    {
        return fallback_group;
    }

    match = strstr(source_file, test_prefix);
    if (match == NULL)
    {
        return fallback_group;
    }

    group_begin = match + (sizeof(test_prefix) - 1);
    group_end = strchr(group_begin, '/');
    if (group_end == NULL || group_end == group_begin)
    {
        return fallback_group;
    }

    group_len = (size_t)(group_end - group_begin);
    if (group_len >= sizeof(inferred_group))
    {
        group_len = sizeof(inferred_group) - 1;
    }

    memcpy(inferred_group, group_begin, group_len);
    inferred_group[group_len] = '\0';

    return inferred_group;
}

static const char *rtfsGetConfiguredGroupFilter(void)
{
    const char *group_filter = getenv("RTFS_TEST_GROUP");

    if (group_filter != NULL && group_filter[0] != '\0')
    {
        return group_filter;
    }

#ifdef RTFS_CONFIG_TEST_GROUP
    return RTFS_CONFIG_TEST_GROUP;
#else
    return NULL;
#endif
}

static const char *rtfsGetConfiguredNameFilter(void)
{
    const char *name_filter = getenv("RTFS_TEST_FILTER");

    if (name_filter != NULL && name_filter[0] != '\0')
    {
        return name_filter;
    }

#ifdef RTFS_CONFIG_TEST_FILTER
    return RTFS_CONFIG_TEST_FILTER;
#else
    return NULL;
#endif
}

static bool rtfsGroupMatches(const char *test_group, const char *group_filter)
{
    const char *token_begin;
    const char *token_end;

    if (group_filter == NULL || group_filter[0] == '\0')
    {
        return true;
    }

    if (test_group == NULL || test_group[0] == '\0')
    {
        return false;
    }

    token_begin = group_filter;
    while (*token_begin != '\0')
    {
        while (*token_begin == ' ' || *token_begin == '\t' || *token_begin == ',')
        {
            ++token_begin;
        }

        if (*token_begin == '\0')
        {
            break;
        }

        token_end = token_begin;
        while (*token_end != '\0' && *token_end != ',')
        {
            ++token_end;
        }

        if (rtfsFilterTokenEquals(token_begin, token_end, test_group))
        {
            return true;
        }

        token_begin = token_end;
    }

    return false;
}

static bool rtfsNameMatches(const char *test_name, const char *name_filter)
{
    if (test_name == NULL)
    {
        return false;
    }

    if (name_filter == NULL || name_filter[0] == '\0')
    {
        return true;
    }

    return strstr(test_name, name_filter) != NULL;
}

static bool rtfsFilterTokenEquals(const char *begin, const char *end, const char *value)
{
    size_t token_len;

    if (begin == NULL || end == NULL || value == NULL)
    {
        return false;
    }

    while (begin < end && (*begin == ' ' || *begin == '\t'))
    {
        ++begin;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
    {
        --end;
    }

    token_len = (size_t)(end - begin);
    if (token_len == 0)
    {
        return false;
    }

    return strlen(value) == token_len && strncmp(begin, value, token_len) == 0;
}

static bool rtfsShouldRunTest(const struct RtfsTestEntry *entry)
{
    const char *group_filter;
    const char *name_filter;

    if (entry == NULL || entry->name == NULL)
    {
        return false;
    }

    group_filter = rtfsGetConfiguredGroupFilter();
    name_filter = rtfsGetConfiguredNameFilter();

    return rtfsGroupMatches(rtfsResolveTestGroup(entry), group_filter) && rtfsNameMatches(entry->name, name_filter);
}
