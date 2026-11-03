#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef VTPC_PAGE_SIZE
#define VTPC_PAGE_SIZE 4096
#endif

#ifndef VTPC_CACHE_PAGES
#define VTPC_CACHE_PAGES 256
#endif

#ifndef VTPC_RA_INIT_PAGES
#define VTPC_RA_INIT_PAGES 4
#endif

#ifndef VTPC_RA_MAX_PAGES
#define VTPC_RA_MAX_PAGES 32
#endif

#ifndef VTPC_RA_ENABLED
#define VTPC_RA_ENABLED 1
#endif

#ifndef VTPC_USE_O_DIRECT
#define VTPC_USE_O_DIRECT 1
#endif

#ifndef VTPC_ALIGNMENT
#define VTPC_ALIGNMENT 4096
#endif

struct vtpc_page {
    bool use;
    bool ref;
    bool dirty;
    off_t block_index;
    int file_id;
    char* data;
    size_t valid;
    struct vtpc_page* prev;
    struct vtpc_page* next;
    struct vtpc_page* h_prev;
    struct vtpc_page* h_next;
    bool in_hash;
};

struct vtpc_file {
    bool allocated;
    int os_fd;
    int flags;
    off_t offset;
    off_t size;
    off_t ra_prev_block;
    size_t ra_size;
    off_t ra_start;
    off_t ra_marker;
    dev_t dev;
    ino_t ino;
    bool mtime_dirty;
    struct timespec mtime;
};

struct vtpc_file* vtpc_get_file(int os_fd);
int vtpc_files_reserve(int os_fd);
void vtpc_init_file(int os_fd);
int vtpc_init_cache(void);
struct vtpc_page* vtpc_get_page(struct vtpc_file* file, off_t block_index, bool need_disk);
int vtpc_fsync_pages_file(struct vtpc_file* f, bool reset);
void vtpc_reset_file(struct vtpc_file* f);
off_t vtpc_count_lseek(off_t offset, int whence, struct vtpc_file* f);
void vtpc_ra_on_access(struct vtpc_file* f, off_t block);
void vtpc_stat_fix(struct stat* st);
void vtpc_truncate_pages(struct vtpc_file* f, off_t length);
int vtpc_flush_all(void);
int vtpc_flush_dev(dev_t dev);

void vtpc_lock(void);
void vtpc_unlock(void);