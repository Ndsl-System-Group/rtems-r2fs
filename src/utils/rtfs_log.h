#ifndef _RTFS_LOG_H_
#define _RTFS_LOG_H_


typedef enum RtfsLogLevel
{
    RTFS_LOG_DEBUG,
    RTFS_LOG_INFO,
    RTFS_LOG_WARNING,
    RTFS_LOG_ERROR
} RtfsLogLevel;

void rtfsLogPrint(RtfsLogLevel log_level, const char *funcname, unsigned int lineno, const char *fmt, ...);
void rtfsLogErrno(RtfsLogLevel log_level, const char *funcname, unsigned int lineno, int err, const char *fmt, ...);

#define RTFS_LOG(log_level, format, ...) \
    rtfsLogPrint(log_level, __func__, __LINE__, format, ##__VA_ARGS__)

#define RTFS_ERRNO_LOG(log_level, err, format, ...) \
    rtfsLogErrno(log_level, __func__, __LINE__, err, format, ##__VA_ARGS__)


#endif
