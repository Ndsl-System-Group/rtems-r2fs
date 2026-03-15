#include "rtfs_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>


const char *logLevelStr[] = {"DEBUG", "INFO", "WARING", "ERROR"};
#define ERRNO_MSG_LEN 128
#define PRINT_TO_STDERR(fmt, ...) \
    vfprintf(stderr, fmt, ##__VA_ARGS__)


void rtfsLogPrint(RtfsLogLevel log_level, const char *funcname, unsigned int lineno, const char *fmt, ...)
{
    fprintf(stderr, "[%s:%u, %s]: ", funcname, lineno, logLevelStr[log_level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void rtfsLogErrno(RtfsLogLevel log_level, const char *funcname, unsigned int lineno, int err, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    rtfsLogPrint(log_level, funcname, lineno, fmt, args);
    va_end(args);

    char errno_msg[ERRNO_MSG_LEN];
    if (0 == strerror_r(err, errno_msg, sizeof(errno_msg))) fprintf(stderr, "error: %s\n", errno_msg);
}
