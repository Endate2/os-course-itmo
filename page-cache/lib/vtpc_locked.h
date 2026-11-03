#pragma once

#include <sys/types.h>
#include <sys/uio.h>
#include <sys/stat.h>

int vtpc_open_locked(const char *path, int flags, mode_t mode);
int vtpc_close_locked(int fd);
ssize_t vtpc_read_locked(int fd, void *buf, size_t count);
ssize_t vtpc_write_locked(int fd, const void *buf, size_t count);
off_t vtpc_lseek_locked(int fd, off_t offset, int whence);
int vtpc_fsync_locked(int fd);
ssize_t vtpc_pread_locked(int fd, void *buf, size_t count, off_t offset);
ssize_t vtpc_pwrite_locked(int fd, const void *buf, size_t count, off_t offset);
ssize_t vtpc_readv_locked(int fd, const struct iovec *iov, int iovcnt);
ssize_t vtpc_writev_locked(int fd, const struct iovec *iov, int iovcnt);
ssize_t vtpc_preadv_locked(int fd, const struct iovec *iov, int iovcnt, off_t offset);
ssize_t vtpc_pwritev_locked(int fd, const struct iovec *iov, int iovcnt, off_t offset);
int vtpc_fstat_locked(int fd, struct stat *st);
int vtpc_stat_locked(const char* path, struct stat *st);
int vtpc_lstat_locked(const char* path, struct stat *st);
int vtpc_fstatat_locked(int dirfd, const char *path, struct stat *st, int flags);
int vtpc_ftruncate_locked(int fd, off_t length);
int vtpc_fcntl_locked(int fd, int cmd, void* arg);
int vtpc_fdatasync_locked(int fd);
void vtpc_sync_locked(void);
int vtpc_syncfs_locked(int fd);