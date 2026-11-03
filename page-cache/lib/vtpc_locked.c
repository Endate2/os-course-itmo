#undef _FORTIFY_SOURCE
#define _GNU_SOURCE

#include "vtpc_locked.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "sys.h"
#include "util.h"

#define VTPC_FAKE_FLAGS (O_ACCMODE | O_APPEND | O_DIRECT)

int vtpc_open_locked(const char *path, int flags, mode_t mode) {
  int base = flags & ~(O_ACCMODE | O_APPEND);

  int os_flags = base | O_RDWR;
#if VTPC_USE_O_DIRECT
  os_flags |= O_DIRECT;
#endif
  int os_fd = vtpc_sys_open(path, os_flags, mode);
  if (os_fd == -1) {
    return -1;
  }
  if (vtpc_init_cache() == -1) {
    vtpc_sys_close(os_fd);
    return -1;
  }

  struct stat st;
  if (vtpc_sys_fstat(os_fd, &st) == -1) {
    int err = errno;
    vtpc_sys_close(os_fd);
    errno = err;
    return -1;
  }

  if (vtpc_files_reserve(os_fd) == -1) {
    vtpc_sys_close(os_fd);
    return -1;
  }

  vtpc_init_file(os_fd);

  struct vtpc_file* f = vtpc_get_file(os_fd);
  if (f == NULL) {
    return -1;
  }

  f->os_fd = os_fd;
  f->flags = flags;
  f->offset = 0;
  f->size = st.st_size;
  f->ra_prev_block = -1;
  f->ra_size = 0;
  f->ra_start = 0;
  f->ra_marker = -1;
  f->dev = st.st_dev;
  f->ino = st.st_ino;
  f->mtime_dirty = false;

  return os_fd;
}

ssize_t vtpc_read_locked(int fd, void* buf, size_t count) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if ((f->flags & O_ACCMODE) == O_WRONLY) {
    errno = EBADF;
    return -1;
  }

  if (count == 0 || f->offset >= f->size) {
    return 0;
  }

  off_t remaining = f->size - f->offset;
  if ((off_t)count > remaining) {
    count = (size_t)remaining;
  }

  char* out = buf;
  size_t steps = 0;
  while (steps < count) {
    off_t block = f -> offset / (off_t)VTPC_PAGE_SIZE;
    off_t block_offset = f -> offset % (off_t)VTPC_PAGE_SIZE;

    size_t n = VTPC_PAGE_SIZE - block_offset;
    if (n > count - steps) {
      n = count - steps;
    }

    vtpc_ra_on_access(f, block);
    struct vtpc_page* p = vtpc_get_page(f, block, true);

    if (p == NULL) {
      if (steps > 0) {
        break;
      }
      return -1;
    }

    memcpy(out + steps, p->data + block_offset, n);
    steps += n;
    f -> offset += (off_t)n;
  }

  return (ssize_t)steps;
}

int vtpc_close_locked(int fd) {
  struct vtpc_file *f = vtpc_get_file(fd);
  if (f == NULL) {
    return vtpc_sys_close(fd);
  }

  int err = 0;

  if (vtpc_fsync_pages_file(f, true) == -1) {
    err = errno;
  }

  vtpc_reset_file(f);

  if (vtpc_sys_close(fd) == -1 && !err) {
    err = errno;
  }

  if (err) {
    errno = err;
    return -1;
  }
  return 0;
}

ssize_t vtpc_write_locked(int fd, const void* buf, size_t count) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if ((f->flags & O_ACCMODE) == O_RDONLY) {
    errno = EBADF;
    return -1;
  }

  if (count == 0) {
    return 0;
  }

  const char* in = buf;
  size_t steps = 0;

  if (f->flags & O_APPEND) {
    f->offset = f->size;
  }

  while (steps < count) {
    off_t block = f -> offset / (off_t)VTPC_PAGE_SIZE;
    off_t block_offset = f -> offset % (off_t)VTPC_PAGE_SIZE;

    size_t n = VTPC_PAGE_SIZE - block_offset;
    if (n > count - steps) {
      n = count - steps;
    }

    bool need_read_from_disk = !(block_offset == 0 && n == VTPC_PAGE_SIZE);

    struct vtpc_page* p = vtpc_get_page(f, block, need_read_from_disk);
    if (p == NULL) {
      if (steps > 0) {
        break;
      }
      return -1;
    }

    memcpy(p->data + block_offset, in + steps, n);

    p->dirty = true;
    if (p->valid < block_offset + n) {
      p->valid = block_offset + n;
    }

    steps += n;
    f -> offset += (off_t)n;

    if (f -> offset >= f->size) {
      f -> size =  f->offset;
    }
  }

  if (steps > 0) {
    clock_gettime(CLOCK_REALTIME, &f->mtime);
    f->mtime_dirty = true;
  }

  return (ssize_t)steps;
}

off_t vtpc_lseek_locked(int fd, off_t offset, int whence) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  off_t new_offset = vtpc_count_lseek(offset, whence, f);

  if (new_offset == -1) {
    errno = EINVAL;
    return -1;
  }

  f -> offset = new_offset;

  return new_offset;
}

int vtpc_fsync_locked(int fd) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if (vtpc_fsync_pages_file(f, false) == -1) {
    return -1;
  }

  if (vtpc_sys_fsync(f ->os_fd) == -1) {
    return -1;
  }

  return 0;
}

ssize_t vtpc_pread_locked(int fd, void* buf, size_t count, off_t offset) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }

  off_t saved = f->offset;
  f->offset = offset;
  ssize_t r = vtpc_read_locked(fd, buf, count);
  f->offset = saved;
  return r;
}

ssize_t vtpc_pwrite_locked(int fd, const void* buf, size_t count, off_t offset) {
  struct vtpc_file* f = vtpc_get_file(fd);

  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }

  off_t saved = f->offset;
  f->offset = offset;
  ssize_t r = vtpc_write_locked(fd, buf, count);
  f->offset = saved;
  return r;
}

ssize_t vtpc_readv_locked(int fd, const struct iovec* iov, int iovcnt) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if ((f->flags & O_ACCMODE) == O_WRONLY) {
    errno = EBADF;
    return -1;
  }

  if (iovcnt < 0 || iovcnt > IOV_MAX) {
    errno = EINVAL;
    return -1;
  }

  size_t sum = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len > (size_t)SSIZE_MAX - sum) {
      errno = EINVAL;
      return -1;
    }
    sum += iov[i].iov_len;
  }

  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) {
      continue;
    }
    ssize_t r = vtpc_read_locked(fd, iov[i].iov_base, iov[i].iov_len);
    if (r == -1) {
      if (total > 0) {
        return total;
      }
      return -1;
    }
    total += r;
    if ((size_t)r != iov[i].iov_len) {
      break;
    }
  }
  return total;
}

ssize_t vtpc_writev_locked(int fd, const struct iovec* iov, int iovcnt) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if ((f->flags & O_ACCMODE) == O_RDONLY) {
    errno = EBADF;
    return -1;
  }

  if (iovcnt < 0 || iovcnt > IOV_MAX) {
    errno = EINVAL;
    return -1;
  }

  size_t sum = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len > (size_t)SSIZE_MAX - sum) {
      errno = EINVAL;
      return -1;
    }
    sum += iov[i].iov_len;
  }

  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) {
      continue;
    }
    ssize_t r = vtpc_write_locked(fd, iov[i].iov_base, iov[i].iov_len);
    if (r == -1) {
      if (total > 0) {
        return total;
      }
      return -1;
    }
    total += r;
    if ((size_t)r != iov[i].iov_len) {
      break;
    }
  }
  return total;
}

ssize_t vtpc_preadv_locked(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }

  off_t saved = f->offset;
  f->offset = offset;
  ssize_t r = vtpc_readv_locked(fd, iov, iovcnt);
  f->offset = saved;
  return r;
}

ssize_t vtpc_pwritev_locked(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }

  off_t saved = f->offset;
  f->offset = offset;
  ssize_t r = vtpc_writev_locked(fd, iov, iovcnt);
  f->offset = saved;
  return r;
}

int vtpc_fstat_locked(int fd, struct stat *st) {
  if (vtpc_sys_fstat(fd, st) == -1) {
    return -1;
  }
  vtpc_stat_fix(st);
  return 0;
}

int vtpc_stat_locked(const char* path, struct stat *st) {
  if (vtpc_sys_stat(path, st) == -1) {
    return -1;
  }
  vtpc_stat_fix(st);
  return 0;
}

int vtpc_lstat_locked(const char* path, struct stat *st) {
  if (vtpc_sys_lstat(path, st) == -1) {
    return -1;
  }
  vtpc_stat_fix(st);
  return 0;
}

int vtpc_fstatat_locked(int dirfd, const char* path, struct stat* st, int flags) {
  if (vtpc_sys_fstatat(dirfd, path, st, flags) == -1) {
    return -1;
  }
  vtpc_stat_fix(st);
  return 0;
}

int vtpc_ftruncate_locked(int fd, off_t length) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if (length < 0) {
    errno = EINVAL;
    return -1;
  }

  if ((f->flags & O_ACCMODE) == O_RDONLY) {
    errno = EINVAL;
    return -1;
  }

  if (vtpc_sys_ftruncate(f->os_fd, length) == -1) {
    return -1;
  }

  if (length < f->size) {
    vtpc_truncate_pages(f, length);
  }

  f->size = length;

  f->ra_prev_block = -1;
  f->ra_size = 0;
  f->ra_marker = -1;
  f->ra_start = 0;

  clock_gettime(CLOCK_REALTIME, &f->mtime);
  f->mtime_dirty = true;
  return 0;
}

int vtpc_fcntl_locked(int fd, int cmd, void* arg) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  switch (cmd) {
    case F_GETFL: {
      int k = vtpc_sys_fcntl(fd, cmd, arg);
      if (k == -1) {
        return -1;
      }
      return (k & ~VTPC_FAKE_FLAGS) | (f->flags & VTPC_FAKE_FLAGS);
    }
    case F_SETFL: {
      int k = (int)arg;
      int kf = k & ~O_ACCMODE;
#if VTPC_USE_O_DIRECT
      kf |= O_DIRECT;
#endif
      if (vtpc_sys_fcntl(fd, cmd, arg) == -1) {
        return -1;
      }
      f->flags = (f->flags & ~(O_APPEND | O_DIRECT)) | (k & (O_APPEND | O_DIRECT));
      return 0;
    }
    case F_DUPFD:
      errno = EOPNOTSUPP;
      return -1;
    default:
      return vtpc_sys_fcntl(fd, cmd, arg);
  }
}

int vtpc_fdatasync_locked(int fd) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  if (vtpc_fsync_pages_file(f, false) == -1) {
    return -1;
  }

  if (vtpc_sys_fdatasync(f ->os_fd) == -1) {
    return -1;
  }

  return 0;
}

void vtpc_sync_locked(void) {
  int saved = errno;
  vtpc_flush_all();
  vtpc_sys_sync();
  errno = saved;
}

int vtpc_syncfs_locked(int fd) {
  struct vtpc_file* f = vtpc_get_file(fd);
  if (f == NULL) {
    errno = EBADF;
    return -1;
  }

  int error = 0;
  if (vtpc_flush_dev(f->dev) == -1) {
    error = errno;
  }

  if (vtpc_sys_syncfs(f->os_fd) == -1 && !error) {
    errno = error;
  }

  if (error) {
    errno = error;
    return -1;
  }
  return 0;
}