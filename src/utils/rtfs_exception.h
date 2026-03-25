#ifndef _RTFS_EXCEPTION_H_
#define _RTFS_EXCEPTION_H_

#include "rtfs_log.h"

#include "cexception/cexception.h"

#include <stdlib.h>


#define THREAD_INTERRUPTED_ID 1001


#define THROW_FATAL(e)                                      \
    do                                                      \
    {                                                       \
        RTFS_LOG(RTFS_LOG_ERROR, "fatal exception: %d", e); \
        abort();                                            \
    } while (0)

#define THROW_FATAL_MESSAGE(e, format, ...)                                      \
    do                                                                           \
    {                                                                            \
        rtfsLogPrint(RTFS_LOG_ERROR, __func__, __LINE__, format, ##__VA_ARGS__); \
        RTFS_LOG(RTFS_LOG_ERROR, "fatal exception: %d", e);                      \
        abort();                                                                 \
    } while (0)


#endif
