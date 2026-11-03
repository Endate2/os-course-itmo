#define _GNU_SOURCE

#include "vtpc.h"

#include <errno.h>
#include <stdarg.h>
#include <sched.h>
#include <sys/pidfd.h>

#include "util.h"
#include "vtpc_locked.h"

int open(const char* path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }
  vtpc_lock();
  int r = vtpc_open_locked(path, flags, mode);
  vtpc_unlock();
  return r;
}

ssize_t read(int fd, void *buf, size_t count) {
  vtpc_lock();
  ssize_t r = vtpc_read_locked(fd, buf, count);
  vtpc_unlock();
  return r;
}

int close(int fd) {
  vtpc_lock();
  int r = vtpc_close_locked(fd);
  vtpc_unlock();
  return r;
}

ssize_t write(int fd, const void *buf, size_t count) {
  vtpc_lock();
  ssize_t r = vtpc_write_locked(fd, buf, count);
  vtpc_unlock();
  return r;
}

off_t lseek(int fd, off_t offset, int whence) {
  vtpc_lock();
  off_t r = vtpc_lseek_locked(fd, offset, whence);
  vtpc_unlock();
  return r;
}

int fsync(int fd) {
  vtpc_lock();
  int r = vtpc_fsync_locked(fd);
  vtpc_unlock();
  return r;
}

/*Другие системные вызовы линукс*/

/*нужно сохранить текущее положение дескриптора и прочитать из кеша*/
ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
  vtpc_lock();
  ssize_t r = vtpc_pread_locked(fd, buf, count, offset);
  vtpc_unlock();
  return r;
}

/*та же ошибка*/
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
  vtpc_lock();
  ssize_t r = vtpc_pwrite_locked(fd, buf, count, offset);
  vtpc_unlock();
  return r;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
  vtpc_lock();
  ssize_t r = vtpc_readv_locked(fd, iov, iovcnt);
  vtpc_unlock();
  return r;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
  vtpc_lock();
  ssize_t r = vtpc_writev_locked(fd, iov, iovcnt);
  vtpc_unlock();
  return r;
}

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  vtpc_lock();
  ssize_t r = vtpc_preadv_locked(fd, iov, iovcnt, offset);
  vtpc_unlock();
  return r;
}

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  vtpc_lock();
  ssize_t r = vtpc_pwritev_locked(fd, iov, iovcnt, offset);
  vtpc_unlock();
  return r;
}

ssize_t preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  (void)fd; (void)iov; (void)iovcnt; (void)offset; (void)flags;
  errno = EOPNOTSUPP;
  return -1;
}

ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  (void)fd; (void)iov; (void)iovcnt; (void)offset; (void)flags;
  errno = EOPNOTSUPP;
  return -1;
}

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
  (void)out_fd; (void)in_fd; (void)offset; (void)count;
  errno = EOPNOTSUPP;
  return -1;
}

int fstat(int fd, struct stat* st) {
  vtpc_lock();
  int r = vtpc_fstat_locked(fd, st);
  vtpc_unlock();
  return r;
}

int stat(const char* path, struct stat* st) {
  vtpc_lock();
  int r = vtpc_stat_locked(path, st);
  vtpc_unlock();
  return r;
}

int lstat(const char* path, struct stat* st) {
  vtpc_lock();
  int r = vtpc_lstat_locked(path, st);
  vtpc_unlock();
  return r;
}

int fstatat(int dirfd, const char* path, struct stat* st, int flags) {
  vtpc_lock();
  int r = vtpc_fstatat_locked(dirfd, path, st, flags);
  vtpc_unlock();
  return r;
}

int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *stx) {
  (void)dirfd; (void)path; (void)flags; (void)mask; (void)stx;
  errno = EOPNOTSUPP;
  return -1;
}

int ftruncate(int fd, off_t length) {
  vtpc_lock();
  int r = vtpc_ftruncate_locked(fd, length);
  vtpc_unlock();
  return r;
}

int truncate(const char* path, off_t length) {
  (void)path; (void)length;
  errno = EOPNOTSUPP;
  return -1;
}

int fallocate(int fd, int mode, off_t offset, off_t len) {
  (void)fd; (void)mode; (void)offset; (void)len;
  errno = EOPNOTSUPP;
  return -1;
}

int fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  void* arg = va_arg(ap, void*);
  va_end(ap);
  vtpc_lock();
  int r = vtpc_fcntl_locked(fd, cmd, arg);
  vtpc_unlock();
  return r;
}

int openat(int dirfd, const char *path, int flags, ...) {
  (void)dirfd; (void)path; (void)flags;
  errno = EOPNOTSUPP;
  return -1;
}

int creat(const char* path, mode_t mode) {
  return open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

int fdatasync(int fd) {
  vtpc_lock();
  int r = vtpc_fdatasync_locked(fd);
  vtpc_unlock();
  return r;
}

void sync(void) {
  vtpc_lock();
  vtpc_sync_locked();
  vtpc_unlock();
}

int syncfs(int fd) {
  vtpc_lock();
  int r = vtpc_syncfs_locked(fd);
  vtpc_unlock();
  return r;
}

int sync_file_range(int fd, off64_t offset, off64_t nbytes, unsigned int flags) {
  (void)fd; (void)offset; (void)nbytes; (void)flags;
  errno = EOPNOTSUPP;
  return -1;
}

pid_t fork(void) {
  errno = EOPNOTSUPP;
  return -1;
}

int clone(int (*fn)(void*), void* stack, int flags, void* arg, ...) {
  (void)fn; (void)stack; (void)flags; (void)arg;
  errno = EOPNOTSUPP;
  return -1;
}

int execveat(int dirfd, const char* path, char* const argv[], char* const envp[], int flags) {
  (void)dirfd; (void)path; (void)argv; (void)envp; (void)flags;
  errno = EOPNOTSUPP;
  return -1;
}

int execve(const char* path, char* const argv[], char* const envp[]) {
  (void)path; (void)argv; (void)envp;
  errno = EOPNOTSUPP;
  return -1;
}

int pidfd_getfd(int pidfd, int targetfd, unsigned int flags) {
  (void)pidfd; (void)targetfd; (void)flags;
  errno = EOPNOTSUPP;
  return -1;
}

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
  (void)addr; (void)len; (void)prot; (void)flags;
  errno = EOPNOTSUPP;
  return NULL;
}

int close_range(unsigned int first, unsigned int last, int flags) {
  (void)first; (void)last; (void)flags;
  errno = EOPNOTSUPP;
  return -1;
}

ssize_t copy_file_range(int fd_in, off64_t* off_in, int fd_out, off64_t* off_out, size_t len, unsigned int flags) {
  (void)fd_in; (void)off_in; (void)fd_out; (void)off_out; (void)len;
  errno = EOPNOTSUPP;
  return -1;
}

int dup(int oldfd) {
  (void)oldfd;
  errno = EOPNOTSUPP;
  return -1;
}

int dup2(int oldfd, int newfd) {
  (void)oldfd; (void)newfd;
  errno = EOPNOTSUPP;
  return -1;
}

int dup3(int oldfd, int newfd, int flags) {
  (void)oldfd; (void)newfd; (void)flags;
  errno = EOPNOTSUPP;
  return -1;
}