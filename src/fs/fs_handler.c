#include "fs_handler.h"

// ****************************** Handler API ******************************

void r2fsLock(const rtems_filesystem_mount_table_entry_t *mt_entry){

}

void r2fsUnlock(const rtems_filesystem_mount_table_entry_t *mt_entry){

}

// ****************** Handler EvalPath API *******************

static bool r2fsFsIsDirectory(rtems_filesystem_eval_path_context_t *ctx, void *arg){

}

static rtems_filesystem_eval_path_generic_status r2fsFsEvalToken(
    rtems_filesystem_eval_path_context_t *ctx, void *arg, const char *token, size_t tokenlen) {

}

static const rtems_filesystem_eval_path_generic_config r2fsFsEvalConfig = {
    .is_directory = r2fsFsIsDirectory, .eval_token = r2fsFsEvalToken};

static void r2fsEvalPath(rtems_filesystem_eval_path_context_t *ctx) {
    rtems_filesystem_eval_path_generic(ctx, NULL, &r2fsFsEvalConfig);
}

// ****************** Handler Other API *******************

int r2fsMknod(const rtems_filesystem_location_info_t *parentloc, 
            const char *name, size_t namelen, mode_t mode, dev_t dev){

}

int r2fsRmnod(const rtems_filesystem_location_info_t *parentloc,
            const rtems_filesystem_location_info_t *loc){

}

int r2fsFchmod(const rtems_filesystem_location_info_t *loc, mode_t mode){

}

int r2fsChown(const rtems_filesystem_location_info_t *loc, uid_t owner, gid_t group){

}

int r2fsCloneNode(rtems_filesystem_location_info_t *loc){

}

void r2fsFreeNode(const rtems_filesystem_location_info_t *loc){

}

int r2fsMount(rtems_filesystem_mount_table_entry_t *mt_entry){

}

int r2fsUnmount(rtems_filesystem_mount_table_entry_t *mt_entry){

}

void r2fsUnmountMe(rtems_filesystem_mount_table_entry_t *temp_mt_entry){

}

int r2fsSymlink(const rtems_filesystem_location_info_t *parentloc, 
                const char *name, size_t namelen, const char *target){

}

int r2fsUtimens(const rtems_filesystem_location_info_t *loc, struct timespec times[2]){

}

int r2fsSymlink(const rtems_filesystem_location_info_t *parentloc,
                const char *name, size_t namelen, const char *target){

}

ssize_t r2fsReadlink(const rtems_filesystem_location_info_t *loc, char *buf, size_t bufsize){

}

int r2fsRename(const rtems_filesystem_location_info_t *oldparentloc,
                const rtems_filesystem_location_info_t *oldloc,
                const rtems_filesystem_location_info_t *newparentloc,
                const char *name, size_t namelen){

}

int r2fsStatvfs(const rtems_filesystem_location_info_t *loc, struct statvfs *buf){

}

// ****************************** Register API ******************************

const rtems_filesystem_operations_table r2fsFsHandler = {
    .lock_h = r2fsLock,
    .unlock_h = r2fsUnlock,
    .eval_path_h = r2fsEvalPath,
    .are_nodes_equal_h = rtems_filesystem_default_are_nodes_equal,
    .mknod_h = r2fsMknod,
    .rmnod_h = r2fsRmnod,
    .fchmod_h = r2fsFchmod,
    .chown_h = r2fsChown,
    .clonenod_h = r2fsCloneNode,
    .freenod_h = r2fsFreeNode,
    .mount_h = r2fsMount,
    .unmount_h = r2fsUnmount,
    .fsunmount_me_h = r2fsUnmountMe,
    .utimens_h = r2fsUtimens,
    .symlink_h = r2fsSymlink,
    .readlink_h = r2fsReadlink,
    .rename_h = r2fsRename,
    .statvfs_h = r2fsStatvfs};