#ifndef _INODE_H_
#define _INODE_H_

#include <sys/endian.h>


/* Return the file system structure given a path location.
 *
 * @param[in] _loc is a pointer to the path location.
 * @return file_system_manager*
 */
#define RTFS_GET_FSMANAGER_STRUCTURE(_loc) ((file_system_manager *)((_loc)->mt_entry->fs_info))

/**
 * Get the ino from the I/O pointer.
 *
 * @param[in] _iop is the I/O pointer.
 * @return ino
 */
#define RTFS_GET_IOP_INO(_iop) ((intptr_t)(_iop)->pathinfo.node_access)


// The inode number or ino.
typedef uint64_t rtfs_ino;


#endif
