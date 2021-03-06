#ifndef NULLSTORE_H
#define NULLSTORE_H

#include "util/common.h"

extern int nullstore_stat(char *out , uint32_t len);

// extern int nullstore_mkfs(const char* dev_list[], int mkfs_flag /* , cb_func_t , void* */);
// extern int nullstore_mount(const char* dev_list[], /* size = 3*/  int mount_flag /**/, cb_func_t , void*);
// extern int nullstore_unmount(cb_func_t , void*);

extern int nullstore_mkfs(const char* dev_list[], int mkfs_flag );
extern int nullstore_mount(const char* dev_list[], /* size = 3*/  int mount_flag /**/);
extern int nullstore_unmount();

extern const int nullstore_obj_async_op_context_size();
extern int nullstore_obj_async_op_call(void *request_msg_with_op_context, cb_func_t _cb);


#endif