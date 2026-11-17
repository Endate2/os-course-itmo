#ifndef OPS_H
#define OPS_H
#include <fuse_lowlevel.h>

typedef enum { WRITE, READ } type_of_work;

extern const struct fuse_lowlevel_ops tgfs_ops;

#endif