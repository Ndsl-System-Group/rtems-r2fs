#ifndef _TYPES_H_
#define _TYPES_H_

#include <sys/endian.h>
#include <stddef.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t u16;
typedef uint8_t u8;

typedef u32 __le32;
typedef u8 __u8;
typedef u64 __le64;
typedef u16 __le16;


#define FS_OK 0
#define FS_FAIL 0x7f

#define BITS_PER_BYTE_SHIFT 3


#endif
