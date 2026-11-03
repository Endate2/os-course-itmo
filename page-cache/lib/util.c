#define _GNU_SOURCE

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sys.h"

static bool g_cache_initialized = false;
static struct vtpc_page g_pages[VTPC_CACHE_PAGES];
static struct vtpc_page* g_buckets[VTPC_CACHE_PAGES];
static struct vtpc_file* g_files = NULL;
static char* buf_readahead = NULL;
static size_t g_files_head = 0;
static struct vtpc_page* head = NULL;
static struct vtpc_page* tail = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

size_t vtpc_hash(int file_id, off_t block_index) {
  return ((size_t)block_index + (size_t)file_id) % VTPC_CACHE_PAGES;
}

void vtpc_lock(void) {
  pthread_mutex_lock(&g_lock);
}

void vtpc_unlock(void) {
  pthread_mutex_unlock(&g_lock);
}

static void hash_insert(struct vtpc_page* p) {
  if (p->in_hash) {
    return;
  }

  size_t index = vtpc_hash(p->file_id, p->block_index);
  p->h_prev = NULL;
  p->h_next = g_buckets[index];

  if (g_buckets[index] != NULL) {
    g_buckets[index]->h_prev = p;
  }
  g_buckets[index] = p;
  p->in_hash = true;
}

static void hash_remove(struct vtpc_page* p) {
  if (!p->in_hash) {
    return;
  }

  size_t index = vtpc_hash(p->file_id, p->block_index);
  if (p->h_prev != NULL) {
    p->h_prev->h_next = p->h_next;
  }else {
    g_buckets[index] = p->h_next;
  }

  if (p->h_next != NULL) {
    p->h_next->h_prev = p->h_prev;
  }

  p->h_prev = NULL;
  p->h_next = NULL;
  p->in_hash = false;
}

int vtpc_files_reserve(int fd) {
  if (fd < 0) {
    errno = EBADF;
    return -1;
  }
  if ((size_t)fd < g_files_head) {
    return 0;
  }
  size_t n = g_files_head;
  while (n <= (size_t)fd) {
    if (n == 0) {
      n = 1024;
    }else {
      n = n * 2;
    }
  }
  struct vtpc_file* file = calloc(n, sizeof(struct vtpc_file));
  if (file == NULL) {
    return -1;
  }

  if (g_files != NULL) {
    memcpy(file, g_files, g_files_head * sizeof(struct vtpc_file));
    free(g_files);
  }

  g_files = file;
  g_files_head = n;
  return 0;
}

struct vtpc_file* vtpc_get_file(int os_fd) {
  if (os_fd < 0 || (size_t)os_fd >= g_files_head) {
    return NULL;
  }
  struct vtpc_file* f = &g_files[os_fd];
  if (!f->allocated) {
    return NULL;
  }
  return f;
}

void vtpc_init_file(int os_fd) {
  g_files[os_fd].allocated = true;
}

static void vtpc_reset_tail(struct vtpc_page* p) {
  p->next = NULL;
  p->prev = tail;
  if (tail != NULL) {
    tail->next = p;
  }
  if (head == NULL) {
    head = p;
  }
  tail = p;
}

static int vtpc_pwrite(int fd, char* data, size_t data_size, off_t offset) {
  size_t count = 0;
  while (count < data_size) {
    ssize_t written = vtpc_sys_pwrite(fd, data + count, data_size - count, offset + (off_t)count);
    if (written < 0 && errno == EINTR) {
      continue;
    }if (written < 0) {
      return -1;
    }if (written == 0) {
      return -1;
    }
    count += written;
  }
  return 0;
}

static ssize_t vtpc_pread(int fd, char* data, size_t data_size, off_t offset) {
  size_t count = 0;
  while (count < data_size) {
    ssize_t read_data = vtpc_sys_pread(fd, data + count, data_size - count, offset + (off_t)count);
    if (read_data < 0 && errno == EINTR) {
      continue;
    }
    if (read_data == -1) {
      return -1;
    }
    if (read_data == 0) {
      break;
    }
    count += (size_t)read_data;
    if (read_data > 0 && (size_t)read_data < data_size) {
      break;
    }
  }
  return count;
}

static int vtpc_displace_page_on_disk(struct vtpc_page* p) {
  if (!p->use || !p->dirty) {
    return 0;
  }

  size_t data_size = VTPC_PAGE_SIZE;

  struct vtpc_file* file = vtpc_get_file(p -> file_id);

  if (file == NULL) {
    return -1;
  }

  if (vtpc_pwrite(file->os_fd, p->data, data_size, p->block_index * (off_t)VTPC_PAGE_SIZE) == -1) {
    return -1;
  }

  off_t end = p->block_index * (off_t)VTPC_PAGE_SIZE + (off_t)data_size;
  if (end > file->size) {
    if (vtpc_sys_ftruncate(file->os_fd, file->size) == -1) {
      return -1;
    }
  }

  p->dirty = false;
  return 0;
}

static void vtpc_throw_page(struct vtpc_page* p) {
  if (!p->use) {
    return;
  }

  if (p->prev != NULL) {
    p->prev->next = p->next;
  } else {
    head = p->next;
  }

  if (p->next != NULL) {
    p->next->prev = p->prev;
  } else {
    tail = p->prev;
  }

  p->prev = NULL;
  p->next = NULL;
}

static void vtpc_reset_page(struct vtpc_page* p) {
  hash_remove(p);
  p->use = false;
  p->ref = false;
  p->dirty = false;
  p->block_index = 0;
  p->file_id = -1;
  p->valid = 0;
  p->next = NULL;
  p->prev = NULL;
}

void vtpc_reset_file(struct vtpc_file* file) {
  file->allocated = false;
  file->os_fd = -1;
  file->flags = 0;
  file->offset = 0;
  file->size = 0;
  file->ra_prev_block = -1;
  file->ra_size = 0;
  file->ra_start = 0;
  file->ra_marker = -1;
}

static struct vtpc_page* vtpc_second_chance_algo(void) {
  while (head != NULL && head->ref) {
    struct vtpc_page* p = head;
    p->ref = false;
    vtpc_throw_page(p);
    vtpc_reset_tail(p);
  }
  return head;
}

static struct vtpc_page* vtpc_evict_page(void) {
  for (size_t i = 0; i < VTPC_CACHE_PAGES; i++) {
    struct vtpc_page* page = vtpc_second_chance_algo();

    if (page == NULL) {
      break;
    }

    if (vtpc_displace_page_on_disk(page) == 0) {
      vtpc_throw_page(page);
      vtpc_reset_page(page);
      return page;
    }

    vtpc_throw_page(page);
    vtpc_reset_tail(page);
  }

  return NULL;
}

static struct vtpc_page* vtpc_find_page(int file_id, off_t block_index) {
  size_t index = vtpc_hash(file_id, block_index);
  struct vtpc_page* p = g_buckets[index];
  while (p != NULL) {
    if (p->file_id == file_id && p->block_index == block_index && p->use) {
      return p;
    }
    p = p->h_next;
  }
  return NULL;
}

static struct vtpc_page* vtpc_get_free_page(void) {
  for (size_t i = 0; i < VTPC_CACHE_PAGES; i++) {
    struct vtpc_page* p = &g_pages[i];
    if (!p->use) {
      return p;
    }
  }
  return NULL;
}

static int vtpc_create_page(
  struct vtpc_file* file,
  struct vtpc_page* p,
  off_t block_index
) {
  p->use = true;
  p->ref = false;
  p->dirty = false;
  p->file_id = file->os_fd;
  p->block_index = block_index;
  p->valid = 0;
  p->prev = NULL;
  p->next = NULL;
  p->h_next = NULL;
  p->h_prev = NULL;
  p->in_hash = false;
  p->data = memset(p->data, 0, VTPC_PAGE_SIZE);

  off_t offset = block_index * (off_t)VTPC_PAGE_SIZE;
  if (offset >= file->size) {
    return 0;
  }

  ssize_t data_size = vtpc_pread(file->os_fd, p->data, VTPC_PAGE_SIZE, offset);
  if (data_size == -1) {
    return -1;
  }
  p->valid = (size_t)data_size;

  return 0;
}

static struct vtpc_page* vtpc_alloc_slot(void) {
  struct vtpc_page* p = vtpc_get_free_page();
  if (p == NULL) {
    p = vtpc_evict_page();
  }
  if (p == NULL) {
    return NULL;
  }
  return p;
}

static struct vtpc_page* vtpc_reserve_slot(void) {
  struct vtpc_page* p = vtpc_alloc_slot();
  if (p == NULL) {
    return NULL;
  }
  p->use = true;
  p->ref = false;
  p->dirty = false;
  p->file_id = -1;
  p->in_hash = false;
  p->h_next = NULL;
  p->h_prev = NULL;
  p->next = NULL;
  p->prev = NULL;
  return p;
}

static void vtpc_readahead(struct vtpc_file* file, off_t start, size_t need_read) {
  struct vtpc_page* slots[VTPC_RA_MAX_PAGES];

  if (buf_readahead == NULL || need_read == 0) {
    return;
  }

  if (need_read > VTPC_RA_MAX_PAGES) {
    need_read = VTPC_RA_MAX_PAGES;
  }

  size_t last = (file->size - 1) / VTPC_PAGE_SIZE;
  if (start > last) {
    return;
  }
  if ((size_t)start + need_read - 1 > last) {
    need_read = last - (size_t)start + 1;
  }

  size_t run = 0;
  while (run < need_read && vtpc_find_page(file->os_fd, start + (off_t)run) == NULL) {
    run++;
  }
  if (run == 0) {
    return;
  }

  for (size_t i = 0; i < run; i++) {
    struct vtpc_page* p = vtpc_reserve_slot();
    if (p == NULL) {
      run = i;
      break;
    }
    slots[i] = p;
  }

  if (run == 0) {
    return;
  }

  ssize_t n = vtpc_pread(file->os_fd, buf_readahead,run * (size_t)VTPC_PAGE_SIZE,start * (off_t)VTPC_PAGE_SIZE);

  if (n == -1) {
    for (size_t i = 0; i < run; i++) {
      vtpc_reset_page(slots[i]);
    }
    return;
  }

  size_t read_pages = ((size_t)n + VTPC_PAGE_SIZE - 1) / VTPC_PAGE_SIZE;
  for (size_t i = read_pages; i < run; i++) {
    vtpc_reset_page(slots[i]);
  }

  for (size_t i = 0; i < read_pages; i++) {
    struct vtpc_page* p = slots[i];

    size_t valid = (size_t)n - i * (size_t)VTPC_PAGE_SIZE;
    if (valid > VTPC_PAGE_SIZE) {
      valid = VTPC_PAGE_SIZE;
    }

    memcpy(p->data, buf_readahead + i * (size_t)VTPC_PAGE_SIZE, valid);
    if (valid < VTPC_PAGE_SIZE) {
      memset(p->data + valid, 0, VTPC_PAGE_SIZE - valid);
    }

    p->file_id = file->os_fd;
    p->valid = valid;
    p->use = true;
    p->ref = false;
    p->dirty = false;
    p->block_index = start + (off_t)i;

    hash_insert(p);
    vtpc_reset_tail(p);
  }

  file->ra_start = start;
  file->ra_size = read_pages;
  file->ra_marker = start + (off_t)(read_pages / 2);
}

void vtpc_ra_on_access(struct vtpc_file* f, off_t block) {
#if VTPC_RA_ENABLED
  bool seq = (f->ra_prev_block == -1 && block == 0) || block == f->ra_prev_block || block == f->ra_prev_block + 1;

  struct vtpc_page* p = vtpc_find_page(f->os_fd, block);
  if (p == NULL) {
    if (!seq) {
      f->ra_size = 0;
      f->ra_marker = -1;
    }else {
      size_t need_read;
      if (f->ra_size != 0) {
        need_read = f->ra_size * 2;
      }else {
        need_read = VTPC_RA_INIT_PAGES;
      }
      vtpc_readahead(f, block, need_read);
    }
  }else if (block == f ->ra_marker) {
    f->ra_marker = -1;
    size_t need_read = f->ra_size * 2;
    vtpc_readahead(f, f->ra_start + (off_t)f->ra_size, need_read);
  }
  f->ra_prev_block = block;
#else
  (void)f; (void)block;
#endif
}

struct vtpc_page* vtpc_get_page(
    struct vtpc_file* file,
    off_t block_index,
    bool need_disk
) {
  struct vtpc_page* p = vtpc_find_page(file->os_fd, block_index);
  if (p != NULL) {
    p->ref = true;
    return p;
  }

  p = vtpc_alloc_slot();
  if (p == NULL) {
    return NULL;
  }

  if (need_disk) {
    if (vtpc_create_page(file, p, block_index) == -1) {
      vtpc_reset_page(p);
      return NULL;
    }

  }else {
    p->use = true;
    p->ref = false;
    p->dirty = false;
    p->file_id = file->os_fd;
    p->block_index = block_index;
    p->valid = 0;
    p->prev = NULL;
    p->next = NULL;
    p->h_next = NULL;
    p->h_prev = NULL;
    p->in_hash = false;
    p->ref = false;
    memset(p->data, 0, VTPC_PAGE_SIZE);
  }
  hash_insert(p);
  vtpc_reset_tail(p);
  return p;
}

int vtpc_init_cache(void) {
  if (g_cache_initialized) {
    return 0;
  }

  for (size_t i = 0; i < VTPC_CACHE_PAGES; i++) {
    struct vtpc_page* p = &g_pages[i];
    p->use = false;
    p->ref = false;
    p->dirty = false;
    p->file_id = -1;
    p->block_index = 0;
    p->valid = 0;
    p->prev = NULL;
    p->next = NULL;
    p->data = NULL;
    p->h_next = NULL;
    p->h_prev = NULL;
    p->in_hash = false;
    p->ref = false;

    int pm = posix_memalign((void**)&p->data, VTPC_ALIGNMENT, VTPC_PAGE_SIZE);
    if (pm != 0) {
      for (size_t j = 0; j <= i; j++) {
        free(g_pages[j].data);
        g_pages[j].data = NULL;
      }
      return -1;
    }
    memset(p->data, 0, VTPC_PAGE_SIZE);
  }

  if (VTPC_RA_ENABLED) {
    if (posix_memalign((void**)&buf_readahead, VTPC_ALIGNMENT, (size_t)VTPC_PAGE_SIZE * VTPC_RA_MAX_PAGES) != 0) {
      buf_readahead = NULL;
    }
  }
  g_cache_initialized = true;
  return 0;
}

int vtpc_fsync_pages_file(struct vtpc_file* f, bool reset) {
  int result = 0;
  for (size_t i = 0; i < VTPC_CACHE_PAGES; i++) {
    struct vtpc_page* p = &g_pages[i];
    if (!p->use || p->file_id != f->os_fd) {
      continue;
    }
    if (p->dirty && vtpc_displace_page_on_disk(p) == -1) {
      result = -1;
    }
    if (reset) {
      vtpc_throw_page(p);
      vtpc_reset_page(p);
    }
  }
  f->mtime_dirty = false;
  return result;
}

off_t vtpc_count_lseek(off_t offset, int whence, struct vtpc_file* f) {
  switch (whence) {
    case SEEK_SET:
      return offset;
    case SEEK_CUR:
      return offset + f -> offset;
    case SEEK_END:
      return f->size + offset;
    default:
      return -1;
  }
}

void vtpc_stat_fix(struct stat* st) {
  size_t i = 0;
  while (i < g_files_head) {
    struct vtpc_file* f = &g_files[i];
    i++;

    if (!f->allocated || f->dev != st->st_dev || f->ino != st->st_ino) {
      continue;
    }

    st->st_size = f->size;

    blkcnt_t need = (blkcnt_t)((f->size + 511) / 512);
    if (st->st_blocks < need) {
      st->st_blocks = need;
    }
    if (f->mtime_dirty) {
      st->st_mtim = f->mtime;
    }
    return;
  }
}

void vtpc_truncate_pages(struct vtpc_file* f, off_t length) {
  off_t dead_block = (length + VTPC_PAGE_SIZE - 1) / VTPC_PAGE_SIZE;
  off_t new_last_block = length / VTPC_PAGE_SIZE;
  size_t new_last_block_offset = length % VTPC_PAGE_SIZE;

  for (size_t i = 0; i < VTPC_CACHE_PAGES; i++) {
    struct vtpc_page* p = &g_pages[i];
    if (!p->use || p->file_id != f->os_fd) {
      continue;
    }
    if (p->block_index >= dead_block) {
      vtpc_throw_page(p);
      vtpc_reset_page(p);
    }else if (p->block_index == new_last_block) {
      memset(p->data + new_last_block_offset, 0, VTPC_PAGE_SIZE - new_last_block_offset);
      if (p->valid > new_last_block_offset) {
        p->valid = new_last_block_offset;
      }
    }
  }
}

int vtpc_flush_all(void) {
  int result = 0;
  for (size_t i = 0; i < g_files_head; i++) {
    struct vtpc_file* f = &g_files[i];
    if (!f->allocated) {
      continue;
    }
    if (vtpc_fsync_pages_file(f, false) == -1) {
      result = -1;
    }
  }
  return result;
}


int vtpc_flush_dev(dev_t dev) {
  int result = 0;
  for (size_t i = 0; i < g_files_head; i++) {
    struct vtpc_file* f = &g_files[i];
    if (!f->allocated || f->dev != dev) {
      continue;
    }
    if (vtpc_fsync_pages_file(f, false) == -1) {
      result = -1;
    }
  }
  return result;
}