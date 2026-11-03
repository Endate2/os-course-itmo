#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/uio.h>

static int vtpc_sys_open(const char *path, int flags, mode_t mode) {
    return (int)syscall(SYS_open, path, flags, mode);
}

static int vtpc_sys_close(int fd) {
    return (int)syscall(SYS_close, fd);
}

static int vtpc_sys_fsync(int fd) {
    return (int)syscall(SYS_fsync, fd);
}

static ssize_t vtpc_sys_pread(int fd, void *buf, size_t count, off_t offset) {
    return (ssize_t)syscall(SYS_pread64, fd, buf, count, offset);
}

static ssize_t vtpc_sys_pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return (ssize_t)syscall(SYS_pwrite64, fd, buf, count, offset);
}

static int vtpc_sys_ftruncate(int fd, off_t length) {
    return (int)syscall(SYS_ftruncate, fd, length);
}

static int vtpc_sys_truncate(const char* path, off_t length) {
  return (int)syscall(SYS_truncate, path, length);
}

static int vtpc_sys_fstat(int fd, struct stat *st) {
    return (int)syscall(SYS_fstat, fd, st);
}

static int vtpc_sys_stat(const char* path, struct stat *st) {
  return (int)syscall(SYS_stat, path, st);
}

static int vtpc_sys_lstat(const char* path, struct stat *st) {
  return (int)syscall(SYS_stat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

static int vtpc_sys_fstatat(int dirfd, const char *path, struct stat *st, int flags) {
  return (int)syscall(SYS_newfstatat, dirfd, path, st, flags);
}

static int vtpc_sys_fcntl(int fd, int cmd, void* arg) {
  return (int)syscall(SYS_fcntl, fd, cmd, arg);
}

static int vtpc_sys_fdatasync(int fd) {
  return (int)syscall(SYS_fdatasync, fd);
}

static void vtpc_sys_sync(void) {
  syscall(SYS_sync);
}

static int vtpc_sys_syncfs(int fd) {
  return (int)syscall(SYS_syncfs, fd);
}