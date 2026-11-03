/*
 * bench.c — программа-нагрузчик для проверки блочного кэша vtpc.
 *
 * Использует ТОЛЬКО open/read/write/lseek/fsync/close, поэтому библиотека,
 * подгруженная через LD_PRELOAD, перехватывает весь файловый ввод-вывод.
 *
 * Собирать БЕЗ _FORTIFY_SOURCE (иначе read() превращается в __read_chk и
 * мимо LD_PRELOAD) и без _FILE_OFFSET_BITS=64 (иначе open -> open64).
 *
 * Использование:
 *   bench gen   FILE --size 64M          создать файл с псевдослучайными данными
 *   bench drop  FILE                     выкинуть файл из кэша страниц ОС
 *   bench seq   FILE [--bs N] [--ops N]  последовательное чтение
 *   bench hot   FILE [--bs N] [--ops N] [--hot SIZE]
 *                                        случайное чтение внутри "горячей" области
 *   bench rand  FILE [--bs N] [--ops N]  случайное чтение по всему файлу
 *   bench write FILE [--bs N] [--total SIZE] [--rand] [--no-fsync]
 *                                        запись мелкими кусками (по умолчанию
 *                                        последовательно, --rand — вразброс)
 * Общие опции:
 *   --direct      открыть с O_DIRECT (режим "без какого-либо кэша"; без LD_PRELOAD!)
 *   --seed N      зерно генератора случайных смещений
 *
 * Результат — одна строка key=value: wall/user/sys в секундах, inblk/outblk
 * (блоки по 512 Б, реально прочитанные/записанные с диска), ctxsw (добровольные
 * переключения контекста ≈ число блокирующих I/O), vtpc=on|off (перехватила ли
 * библиотека файл), cksum (контрольная сумма прочитанных данных).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ALIGN 4096

static int g_direct = 0;
static off_t g_pos = 0;      /* позиция файла, как мы её знаем (чтобы не звать lseek зря) */
static off_t g_size = 0;     /* логический размер файла при записи в режиме --direct */
static char *g_bounce = NULL;
static size_t g_bounce_cap = 0;

static void die(const char *what) {
  fprintf(stderr, "bench: %s: %s\n", what, strerror(errno));
  exit(1);
}

static void usage(void) {
  fprintf(stderr,
          "usage: bench gen|drop|seq|hot|rand|write FILE [--bs N] [--ops N] [--hot SIZE]\n"
          "             [--size SIZE] [--total SIZE] [--rand] [--no-fsync] [--direct] [--seed N]\n");
  exit(2);
}

static size_t parse_size(const char *s) {
  char *end;
  unsigned long long v = strtoull(s, &end, 10);
  switch (toupper((unsigned char)*end)) {
    case 'K': v <<= 10; break;
    case 'M': v <<= 20; break;
    case 'G': v <<= 30; break;
    case 0: break;
    default: fprintf(stderr, "bench: bad size '%s'\n", s); exit(2);
  }
  return (size_t)v;
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) {
  uint64_t x = g_rng;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  return g_rng = x;
}

static uint64_t cks(uint64_t h, const unsigned char *p, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x100000001B3ULL;
  }
  for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ULL;
  return h;
}

static void *xalloc(size_t n) {
  void *p;
  if (posix_memalign(&p, ALIGN, n < ALIGN ? ALIGN : n) != 0) die("posix_memalign");
  memset(p, 0, n);
  return p;
}

static void ensure_bounce(size_t n) {
  if (n <= g_bounce_cap) return;
  free(g_bounce);
  g_bounce = xalloc(n);
  g_bounce_cap = n;
}

/* ---------- низкоуровневый слой: только read/write/lseek ---------- */

static int seek_to(int fd, off_t off) {
  if (g_pos == off) return 0;
  if (lseek(fd, off, SEEK_SET) == (off_t)-1) return -1;
  g_pos = off;
  return 0;
}

static ssize_t read_full(int fd, void *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, (char *)buf + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) break;
    got += (size_t)r;
    if (g_direct && got < n) break; /* O_DIRECT: короткое чтение = конец файла */
  }
  return (ssize_t)got;
}

static ssize_t write_full(int fd, const void *buf, size_t n) {
  size_t done = 0;
  while (done < n) {
    ssize_t w = write(fd, (const char *)buf + done, n - done);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (w == 0) { errno = EIO; return -1; }
    done += (size_t)w;
  }
  return (ssize_t)done;
}

static int aligned_req(const void *buf, off_t off, size_t n) {
  return ((uintptr_t)buf % ALIGN) == 0 && off % ALIGN == 0 && n % ALIGN == 0;
}

/* Прочитать n байт со смещения off. В режиме --direct невыровненные запросы
 * расширяются до границ 4 КиБ и идут через промежуточный буфер — так ведёт
 * себя программа, у которой нет вообще никакого кэша. */
static ssize_t io_read(int fd, void *buf, size_t n, off_t off) {
  if (!g_direct || aligned_req(buf, off, n)) {
    if (seek_to(fd, off) == -1) return -1;
    ssize_t r = read_full(fd, buf, n);
    if (r > 0) g_pos += r;
    return r;
  }
  off_t a = off & ~(off_t)(ALIGN - 1);
  size_t skip = (size_t)(off - a);
  size_t span = (skip + n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
  ensure_bounce(span);
  if (seek_to(fd, a) == -1) return -1;
  ssize_t r = read_full(fd, g_bounce, span);
  if (r < 0) return -1;
  g_pos = a + r;
  if ((size_t)r <= skip) return 0;
  size_t avail = (size_t)r - skip;
  if (avail > n) avail = n;
  memcpy(buf, g_bounce + skip, avail);
  return (ssize_t)avail;
}

/* Записать n байт по смещению off. В режиме --direct невыровненная запись
 * превращается в read-modify-write целых блоков по 4 КиБ. */
static ssize_t io_write(int fd, const void *buf, size_t n, off_t off) {
  if (!g_direct || aligned_req(buf, off, n)) {
    if (seek_to(fd, off) == -1) return -1;
    ssize_t w = write_full(fd, buf, n);
    if (w > 0) g_pos += w;
    if (off + (off_t)n > g_size) g_size = off + (off_t)n;
    return w;
  }
  off_t a = off & ~(off_t)(ALIGN - 1);
  size_t skip = (size_t)(off - a);
  size_t span = (skip + n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
  ensure_bounce(span);
  memset(g_bounce, 0, span);
  if (a < g_size) {
    if (seek_to(fd, a) == -1) return -1;
    ssize_t r = read_full(fd, g_bounce, span);
    if (r < 0) return -1;
    g_pos = a + r;
  }
  memcpy(g_bounce + skip, buf, n);
  if (seek_to(fd, a) == -1) return -1;
  if (write_full(fd, g_bounce, span) < 0) return -1;
  g_pos = a + (off_t)span;
  if (off + (off_t)n > g_size) g_size = off + (off_t)n;
  return (ssize_t)n;
}

/* ---------- замеры ---------- */

struct meas {
  struct timespec t;
  struct rusage ru;
};

static void mark(struct meas *m) {
  clock_gettime(CLOCK_MONOTONIC, &m->t);
  getrusage(RUSAGE_SELF, &m->ru);
}

static double tvsec(struct timeval a) { return (double)a.tv_sec + (double)a.tv_usec / 1e6; }

/* Сколько дескрипторов процесса указывают на этот файл. Если библиотека
 * перехватила open, их два: пользовательский и её собственный (O_DIRECT). */
static int count_fds(const char *path) {
  char real[PATH_MAX], link[PATH_MAX], name[300];
  if (realpath(path, real) == NULL) return -1;
  DIR *d = opendir("/proc/self/fd");
  if (d == NULL) return -1;
  int cnt = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    snprintf(name, sizeof name, "/proc/self/fd/%s", e->d_name);
    ssize_t l = readlink(name, link, sizeof link - 1);
    if (l < 0) continue;
    link[l] = 0;
    if (strcmp(link, real) == 0) cnt++;
  }
  closedir(d);
  return cnt;
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
  if (argc < 3) usage();
  const char *wl = argv[1];
  const char *path = argv[2];

  size_t bs = 4096, ops = 0, hot = 512 * 1024, size = 64u << 20, total = 4u << 20;
  int rand_wr = 0, do_fsync = 1;

  for (int i = 3; i < argc; i++) {
    const char *a = argv[i];
    const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (!strcmp(a, "--direct")) g_direct = 1;
    else if (!strcmp(a, "--rand")) rand_wr = 1;
    else if (!strcmp(a, "--no-fsync")) do_fsync = 0;
    else if (v == NULL) usage();
    else if (!strcmp(a, "--bs")) { bs = parse_size(v); i++; }
    else if (!strcmp(a, "--ops")) { ops = parse_size(v); i++; }
    else if (!strcmp(a, "--hot")) { hot = parse_size(v); i++; }
    else if (!strcmp(a, "--size")) { size = parse_size(v); i++; }
    else if (!strcmp(a, "--total")) { total = parse_size(v); i++; }
    else if (!strcmp(a, "--seed")) { g_rng = strtoull(v, NULL, 0) | 1; i++; }
    else usage();
  }
  if (bs == 0) usage();

  /* служебные режимы — без замеров */
  if (!strcmp(wl, "gen")) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open");
    size_t chunk = 1u << 20;
    uint64_t *b = xalloc(chunk);
    for (size_t done = 0; done < size; done += chunk) {
      for (size_t k = 0; k < chunk / 8; k++) b[k] = rnd();
      size_t n = size - done < chunk ? size - done : chunk;
      if (write_full(fd, b, n) < 0) die("write");
    }
    if (fsync(fd) < 0) die("fsync");
    if (close(fd) < 0) die("close");
    return 0;
  }
  if (!strcmp(wl, "drop")) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open");
    fdatasync(fd);
    int e = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    if (e != 0) { errno = e; die("posix_fadvise"); }
    close(fd);
    return 0;
  }

  int is_write = !strcmp(wl, "write");
  if (!is_write && strcmp(wl, "seq") && strcmp(wl, "hot") && strcmp(wl, "rand")) usage();

  off_t fsize = 0;
  if (!is_write) {
    struct stat st;
    if (stat(path, &st) < 0) die("stat");
    fsize = st.st_size;
    if (fsize < (off_t)bs) { fprintf(stderr, "bench: file smaller than bs\n"); return 1; }
  }

  unsigned char *buf = xalloc(bs);
  uint64_t h = 0xCBF29CE484222325ULL;
  unsigned long long bytes = 0;
  size_t done_ops = 0;
  int vtpc_fds = 0;

  struct meas m0, m1;
  mark(&m0);

  if (is_write) {
    int flags = (g_direct ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC | (g_direct ? O_DIRECT : 0);
    int fd = open(path, flags, 0644);
    if (fd < 0) die("open");
    vtpc_fds = count_fds(path);
    size_t nblocks = total / bs;
    if (ops == 0) ops = nblocks;
    for (size_t i = 0; i < bs; i++) buf[i] = (unsigned char)(i * 31 + 7);
    for (size_t i = 0; i < ops; i++) {
      off_t off = rand_wr ? (off_t)(rnd() % nblocks) * (off_t)bs : (off_t)(i % nblocks) * (off_t)bs;
      uint64_t stamp = i;
      memcpy(buf, &stamp, bs < 8 ? bs : 8);
      if (io_write(fd, buf, bs, off) < 0) die("write");
      bytes += bs;
      done_ops++;
    }
    if (g_direct && ftruncate(fd, g_size) < 0) die("ftruncate");
    if (do_fsync && fsync(fd) < 0) die("fsync");
    if (close(fd) < 0) die("close");
  } else {
    int fd = open(path, O_RDONLY | (g_direct ? O_DIRECT : 0));
    if (fd < 0) die("open");
    vtpc_fds = count_fds(path);

    if (!strcmp(wl, "seq")) {
      size_t limit = ops ? ops : (size_t)-1;
      off_t off = 0;
      while (done_ops < limit) {
        ssize_t r = io_read(fd, buf, bs, off);
        if (r < 0) die("read");
        if (r == 0) break;
        h = cks(h, buf, (size_t)r);
        off += r;
        bytes += (unsigned long long)r;
        done_ops++;
      }
    } else {
      off_t region = !strcmp(wl, "hot") && (off_t)hot < fsize ? (off_t)hot : fsize;
      size_t nblocks = (size_t)(region / (off_t)bs);
      if (nblocks == 0) nblocks = 1;
      if (ops == 0) ops = 100000;
      for (; done_ops < ops; done_ops++) {
        off_t off = (off_t)(rnd() % nblocks) * (off_t)bs;
        ssize_t r = io_read(fd, buf, bs, off);
        if (r < 0) die("read");
        h = cks(h, buf, (size_t)r);
        bytes += (unsigned long long)r;
      }
    }
    if (close(fd) < 0) die("close");
  }

  mark(&m1);

  double wall = (double)(m1.t.tv_sec - m0.t.tv_sec) + (double)(m1.t.tv_nsec - m0.t.tv_nsec) / 1e9;
  double user = tvsec(m1.ru.ru_utime) - tvsec(m0.ru.ru_utime);
  double sys = tvsec(m1.ru.ru_stime) - tvsec(m0.ru.ru_stime);

  printf("wl=%s bs=%zu ops=%zu bytes=%llu wall=%.4f user=%.4f sys=%.4f "
         "inblk=%ld outblk=%ld ctxsw=%ld vtpc=%s cksum=%016llx\n",
         wl, bs, done_ops, bytes, wall, user, sys,
         m1.ru.ru_inblock - m0.ru.ru_inblock, m1.ru.ru_oublock - m0.ru.ru_oublock,
         m1.ru.ru_nvcsw - m0.ru.ru_nvcsw, vtpc_fds >= 2 ? "on" : "off",
         is_write ? 0ULL : (unsigned long long)h);
  return 0;
}
