#include "rtfs_test.h"

#include "cache/node_block_cache.h"


// TODO 因为 BlockBuffer 还没完成，因此涉及到 BlockBuffer 的操作全是空操作，会出现空指针的内存泄露问题导致崩溃，暂时无法测试。
RTFS_TEST(NbcTest)
{
    TEST_PASS();
}
