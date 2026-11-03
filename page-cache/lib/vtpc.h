#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sched.h>
#include <sys/pidfd.h>


int open(const char *path, int flags, ...);
int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int fsync(int fd);

ssize_t pread(int fd, void *buf, size_t count, off_t offset);

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);

ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);

ssize_t preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);

ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);

int fstat(int fd, struct stat *st);

int stat(const char* path, struct stat *st);

int lstat(const char* path, struct stat *st);

int fstatat(int dirfd, const char *path, struct stat *st, int flags);

int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *stx);

int ftruncate(int fd, off_t length);

int truncate(const char* path, off_t length);

int fallocate(int fd, int mode, off_t offset, off_t len);

int fcntl(int fd, int cmd, ...);

int openat(int dirfd, const char *path, int flags, ...);

int creat(const char* path, mode_t mode);

int fdatasync(int fd);

void sync(void);

int syncfs(int fd);

int sync_file_range(int fd, off64_t offset, off64_t nbytes, unsigned int flags);

int execve(const char* path, char* const argv[], char* const envp[]);

int execveat(int dirfd, const char* path, char* const argv[], char* const envp[], int flags);

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);

int close_range(unsigned int first, unsigned int last, int flags);

ssize_t copy_file_range(int fd_in, off64_t* off_in, int fd_out, off64_t* off_out, size_t len, unsigned int flags);

int dup(int oldfd);

int dup2(int oldfd, int newfd);

int dup3(int oldfd, int newfd, int flags);

int pidfd_getfd(int pidfd, int targetfd, unsigned int flags);

pid_t fork(void);

int clone(int (*fn)(void*), void* stack, int flags, void* arg, ...);