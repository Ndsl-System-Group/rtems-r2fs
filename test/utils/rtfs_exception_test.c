#include "rtfs_test.h"

#include "utils/rtfs_exception.h"


RTFS_TEST(RtfsExceptionTest)
{
    CEXCEPTION_T e = EXIT_FAILURE;

    // XXX 启用这两行程序跑到这里的时候会输出异常信息并且退出。
    // THROW_FATAL(e);
    // THROW_FATAL_MESSAGE(EXIT_FAILURE, "Throw an exception: %d", e);
}
