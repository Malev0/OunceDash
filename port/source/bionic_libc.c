// bionic (Android libc) compatibility for the functions whose ABI differs
// from newlib: stdio streams, stat/dirent layouts, open flags, locale,
// clocks, mmap and the process/user stubs. Paths are redirected to the SD
// card (see path_translate).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "port.h"

// ------------------------------------------------------------------ paths

const char *path_translate(const char *in, char *out, size_t out_size)
{
    static const struct { const char *from, *to; } map[] = {
        { "/data/data/" ANDROID_PACKAGE "/files/", GD_SAVE_DIR "/" },
        { "/data/user/0/" ANDROID_PACKAGE "/files/", GD_SAVE_DIR "/" },
        { "/data/data/" ANDROID_PACKAGE "/", GD_SAVE_DIR "/" },
        { "/data/user/0/" ANDROID_PACKAGE "/", GD_SAVE_DIR "/" },
        { "/data/data/" ANDROID_PACKAGE "/files", GD_SAVE_DIR },
        { "/data/data/" ANDROID_PACKAGE, GD_SAVE_DIR },
        { "/sdcard/", GD_ROOT "/sdcard/" },
        { "/storage/emulated/0/", GD_ROOT "/sdcard/" },
    };
    if (!in)
        return NULL;
    if (strncmp(in, "sdmc:", 5) == 0 || strncmp(in, "romfs:", 6) == 0) {
        snprintf(out, out_size, "%s", in);
        return out;
    }
    if (strcmp(in, ANDROID_APK_PATH) == 0) {
        snprintf(out, out_size, "%s", g_config.apk_path);
        return out;
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        size_t n = strlen(map[i].from);
        if (strncmp(in, map[i].from, n) == 0) {
            snprintf(out, out_size, "%s%s", map[i].to, in + n);
            return out;
        }
    }
    if (in[0] == '/')
        snprintf(out, out_size, GD_ROOT "/fs%s", in);
    else
        snprintf(out, out_size, GD_SAVE_DIR "/%s", in);
    return out;
}

int mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' && p[-1] != ':') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return mkdir(tmp, 0777);
}

// ------------------------------------------------------------------ stdio

// bionic exports stdin/stdout/stderr as &__sF[0..2], each sizeof(FILE)=152.
#define BIONIC_FILE_SIZE 152

static int rand_cookie_read(void *c, char *buf, _READ_WRITE_BUFSIZE_TYPE n);
static int rand_cookie_close(void *c);
bool rand_fd_is(int fd);
uint8_t bionic_sF[3 * BIONIC_FILE_SIZE] __attribute__((aligned(16)));

static inline FILE *F(void *p)
{
    uint8_t *b = p;
    if (b >= bionic_sF && b < bionic_sF + sizeof(bionic_sF)) {
        switch ((b - bionic_sF) / BIONIC_FILE_SIZE) {
        case 0: return stdin;
        case 1: return stdout;
        default: return stderr;
        }
    }
    return p;
}

FILE *b_fopen(const char *path, const char *mode)
{
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random") || !strcmp(path, "/dev/srandom"))
        return funopen(NULL, rand_cookie_read, NULL, NULL, rand_cookie_close);
    char p[512];
    path_translate(path, p, sizeof(p));
    FILE *f = fopen(p, mode);
    if (!f && (strchr(mode, 'w') || strchr(mode, 'a'))) {
        // create missing directories for writes
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", p);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; mkdir_p(dir); f = fopen(p, mode); }
    }
    return f;
}
FILE *b_fdopen(int fd, const char *mode) { return fdopen(fd, mode); }
int b_fclose(void *f) { FILE *r = F(f); return (r == stdin || r == stdout || r == stderr) ? 0 : fclose(r); }
int b_feof(void *f) { return feof(F(f)); }
int b_ferror(void *f) { return ferror(F(f)); }
int b_fflush(void *f) { return fflush(f ? F(f) : NULL); }
char *b_fgets(char *s, int n, void *f) { return fgets(s, n, F(f)); }
int b_fileno(void *f) { return fileno(F(f)); }
int b_fputc(int c, void *f) { return fputc(c, F(f)); }
int b_fputs(const char *s, void *f) { return fputs(s, F(f)); }
size_t b_fread(void *p, size_t s, size_t n, void *f) { return fread(p, s, n, F(f)); }
size_t b_fwrite(const void *p, size_t s, size_t n, void *f) { return fwrite(p, s, n, F(f)); }
int b_fseek(void *f, long o, int w) { return fseek(F(f), o, w); }
int b_fseeko(void *f, off_t o, int w) { return fseeko(F(f), o, w); }
long b_ftell(void *f) { return ftell(F(f)); }
off_t b_ftello(void *f) { return ftello(F(f)); }
int b_getc(void *f) { return getc(F(f)); }
int b_ungetc(int c, void *f) { return ungetc(c, F(f)); }
void b_setbuf(void *f, char *buf) { setbuf(F(f), buf); }
int b_vfprintf(void *f, const char *fmt, va_list ap)
{
    FILE *r = F(f);
    if (r == stdout || r == stderr) {
        char buf[2048];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        LOG("%s", buf);
        return n;
    }
    return vfprintf(r, fmt, ap);
}
int b_fprintf(void *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = b_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}
int b_printf(const char *fmt, ...)
{
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOG("%s", buf);
    return n;
}

// ------------------------------------------------------------------ fd io

#define L_O_ACCMODE 3
#define L_O_CREAT 0100
#define L_O_EXCL 0200
#define L_O_TRUNC 01000
#define L_O_APPEND 02000
#define L_O_NONBLOCK 04000

int flags_from_linux(int lf)
{
    int f = lf & L_O_ACCMODE;
    if (lf & L_O_CREAT) f |= O_CREAT;
    if (lf & L_O_EXCL) f |= O_EXCL;
    if (lf & L_O_TRUNC) f |= O_TRUNC;
    if (lf & L_O_APPEND) f |= O_APPEND;
    if (lf & L_O_NONBLOCK) f |= O_NONBLOCK;
    return f;
}

int flags_to_linux(int f)
{
    int lf = f & O_ACCMODE;
    if (f & O_CREAT) lf |= L_O_CREAT;
    if (f & O_EXCL) lf |= L_O_EXCL;
    if (f & O_TRUNC) lf |= L_O_TRUNC;
    if (f & O_APPEND) lf |= L_O_APPEND;
    if (f & O_NONBLOCK) lf |= L_O_NONBLOCK;
    return lf;
}

// ---- /dev/urandom, /dev/random, /dev/srandom
// OpenSSL seeds its PRNG from these (and libc++'s random_device reads them);
// without them every TLS handshake fails with "PRNG not seeded". They are
// fake descriptors backed by the console's CSPRNG.
#define RAND_FD_BASE 0x7ff00000
#define RAND_FD_COUNT 32
static bool g_rand_fd_used[RAND_FD_COUNT];
static Mutex g_rand_lock;

static bool is_random_device(const char *path)
{
    return path && (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random") ||
                    !strcmp(path, "/dev/srandom"));
}

bool rand_fd_is(int fd) { return fd >= RAND_FD_BASE && fd < RAND_FD_BASE + RAND_FD_COUNT; }

static int rand_fd_open(void)
{
    mutexLock(&g_rand_lock);
    for (int i = 0; i < RAND_FD_COUNT; i++) {
        if (!g_rand_fd_used[i]) {
            g_rand_fd_used[i] = true;
            mutexUnlock(&g_rand_lock);
            return RAND_FD_BASE + i;
        }
    }
    mutexUnlock(&g_rand_lock);
    errno = 24; // EMFILE
    return -1;
}

static int rand_cookie_read(void *c, char *buf, _READ_WRITE_BUFSIZE_TYPE n) { (void)c; randomGet(buf, n); return n; }
static int rand_cookie_close(void *c) { (void)c; return 0; }

int b_open(const char *path, int flags, int mode)
{
    if (is_random_device(path)) return rand_fd_open();
    char p[512];
    path_translate(path, p, sizeof(p));
    return open(p, flags_from_linux(flags), mode);
}

ssize_t b_read(int fd, void *buf, size_t n)
{
    if (rand_fd_is(fd)) { randomGet(buf, n); return (ssize_t)n; }
    return read(fd, buf, n);
}

int b_close(int fd)
{
    if (rand_fd_is(fd)) {
        mutexLock(&g_rand_lock);
        g_rand_fd_used[fd - RAND_FD_BASE] = false;
        mutexUnlock(&g_rand_lock);
        return 0;
    }
    return close(fd);
}

off_t b_lseek(int fd, off_t off, int whence)
{
    if (rand_fd_is(fd)) return 0;
    return lseek(fd, off, whence);
}

int b_access(const char *path, int mode)
{
    char p[512];
    struct stat st;
    (void)mode;
    path_translate(path, p, sizeof(p));
    return stat(p, &st);
}

int b_mkdir(const char *path, int mode)
{
    char p[512];
    path_translate(path, p, sizeof(p));
    int rc = mkdir(p, mode);
    if (rc != 0 && errno == ENOENT) rc = mkdir_p(p);
    return rc;
}

int b_remove(const char *path)
{
    char p[512];
    path_translate(path, p, sizeof(p));
    return remove(p);
}

int b_rename(const char *a, const char *b)
{
    char pa[512], pb[512];
    path_translate(a, pa, sizeof(pa));
    path_translate(b, pb, sizeof(pb));
    remove(pb); // POSIX rename replaces the destination
    return rename(pa, pb);
}

int b_chmod(const char *path, int mode) { (void)path; (void)mode; return 0; }

// bionic arm64 struct stat
typedef struct {
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, pad1;
    int64_t st_size;
    int32_t st_blksize, pad2;
    int64_t st_blocks;
    struct { int64_t tv_sec, tv_nsec; } st_atim, st_mtim, st_ctim;
    uint32_t unused4, unused5;
} bionic_stat;

static void convert_stat(const struct stat *s, bionic_stat *b)
{
    memset(b, 0, sizeof(*b));
    b->st_dev = s->st_dev;
    b->st_ino = s->st_ino;
    b->st_mode = s->st_mode;
    b->st_nlink = s->st_nlink ? s->st_nlink : 1;
    b->st_size = s->st_size;
    b->st_blksize = 4096;
    b->st_blocks = (s->st_size + 511) / 512;
    b->st_atim.tv_sec = s->st_atime;
    b->st_mtim.tv_sec = s->st_mtime;
    b->st_ctim.tv_sec = s->st_ctime;
}

int b_stat(const char *path, bionic_stat *out)
{
    char p[512];
    struct stat st;
    path_translate(path, p, sizeof(p));
    int rc = stat(p, &st);
    if (rc == 0) convert_stat(&st, out);
    return rc;
}

int b_fstat(int fd, bionic_stat *out)
{
    if (rand_fd_is(fd)) {
        memset(out, 0, sizeof(*out));
        out->st_mode = 0020444;          // S_IFCHR, r--r--r--
        out->st_rdev = (1 << 8) | 9;     // 1:9, Linux's /dev/urandom
        out->st_ino = 1000 + (fd - 0x7ff00000);
        out->st_dev = 5;
        out->st_nlink = 1;
        return 0;
    }
    struct stat st;
    int rc = fstat(fd, &st);
    if (rc == 0) convert_stat(&st, out);
    return rc;
}

// bionic struct dirent
typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} bionic_dirent;

typedef struct {
    DIR *dir;
    char path[512];
    bionic_dirent ent;
} bionic_DIR;

void *b_opendir(const char *path)
{
    char p[512];
    path_translate(path, p, sizeof(p));
    DIR *d = opendir(p);
    if (!d) return NULL;
    bionic_DIR *bd = calloc(1, sizeof(*bd));
    bd->dir = d;
    snprintf(bd->path, sizeof(bd->path), "%s", p);
    return bd;
}

bionic_dirent *b_readdir(void *h)
{
    bionic_DIR *bd = h;
    struct dirent *e = readdir(bd->dir);
    if (!e) return NULL;
    memset(&bd->ent, 0, sizeof(bd->ent));
    snprintf(bd->ent.d_name, sizeof(bd->ent.d_name), "%s", e->d_name);
    bd->ent.d_reclen = sizeof(bd->ent);
    char full[800];
    struct stat st;
    snprintf(full, sizeof(full), "%s/%s", bd->path, e->d_name);
    if (stat(full, &st) == 0)
        bd->ent.d_type = S_ISDIR(st.st_mode) ? 4 : 8;
    return &bd->ent;
}

int b_closedir(void *h)
{
    bionic_DIR *bd = h;
    int rc = closedir(bd->dir);
    free(bd);
    return rc;
}

// fcntl for regular files (sockets are handled in bionic_net.c)
int net_fcntl(int fd, int cmd, long arg, bool *handled);
int b_fcntl(int fd, int cmd, long arg)
{
    if (rand_fd_is(fd)) return 0;
    bool handled = false;
    int rc = net_fcntl(fd, cmd, arg, &handled);
    if (handled) return rc;
    switch (cmd) {
    case 1: /* F_GETFD */ return 0;
    case 2: /* F_SETFD */ return 0;
    case 3: { /* F_GETFL */ int f = fcntl(fd, F_GETFL); return f < 0 ? f : flags_to_linux(f); }
    case 4: /* F_SETFL */ return fcntl(fd, F_SETFL, flags_from_linux((int)arg));
    default: return 0;
    }
}

// ------------------------------------------------------------------ mmap

typedef struct MapRegion { void *addr; size_t len; struct MapRegion *next; } MapRegion;
static MapRegion *g_maps;
static Mutex g_map_lock;

void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)addr; (void)prot;
    len = (len + 0xfff) & ~(size_t)0xfff;
    void *p = aligned_alloc(0x1000, len);
    if (!p) { errno = ENOMEM; return (void *)-1; }
    memset(p, 0, len);
    if (!(flags & 0x20 /* MAP_ANONYMOUS */) && fd >= 0) {
        off_t cur = lseek(fd, 0, SEEK_CUR);
        lseek(fd, off, SEEK_SET);
        ssize_t got = read(fd, p, len);
        (void)got;
        lseek(fd, cur, SEEK_SET);
    }
    MapRegion *r = malloc(sizeof(*r));
    r->addr = p; r->len = len;
    mutexLock(&g_map_lock);
    r->next = g_maps; g_maps = r;
    mutexUnlock(&g_map_lock);
    return p;
}

int b_munmap(void *addr, size_t len)
{
    (void)len;
    mutexLock(&g_map_lock);
    for (MapRegion **pp = &g_maps; *pp; pp = &(*pp)->next) {
        if ((*pp)->addr == addr) {
            MapRegion *r = *pp;
            *pp = r->next;
            mutexUnlock(&g_map_lock);
            free(r->addr);
            free(r);
            return 0;
        }
    }
    mutexUnlock(&g_map_lock);
    return 0; // partial unmaps are ignored
}

int b_mprotect(void *a, size_t l, int p) { (void)a; (void)l; (void)p; return 0; }
int b_madvise(void *a, size_t l, int p) { (void)a; (void)l; (void)p; return 0; }
int b_mlock(const void *a, size_t l) { (void)a; (void)l; return 0; }

// ------------------------------------------------------------------ time

uint64_t time_ns(void)
{
    return armTicksToNs(armGetSystemTick());
}

int b_clock_gettime(int clk, struct timespec *ts)
{
    if (clk == 0 /* CLOCK_REALTIME */)
        return clock_gettime(CLOCK_REALTIME, ts);
    uint64_t ns = time_ns();
    ts->tv_sec = ns / 1000000000ULL;
    ts->tv_nsec = ns % 1000000000ULL;
    return 0;
}

long b_clock(void)
{
    // bionic CLOCKS_PER_SEC is 1000000
    return (long)(time_ns() / 1000);
}

long b_sysconf(int name)
{
    switch (name) {
    case 0x27: /* _SC_PAGESIZE */ return 4096;
    case 0x28: /* _SC_PAGE_SIZE */ return 4096;
    case 0x60: /* _SC_NPROCESSORS_CONF */ return 6;
    case 0x61: /* _SC_NPROCESSORS_ONLN */ return 6;
    case 6: /* _SC_CLK_TCK */ return 100;
    case 0x62: /* _SC_PHYS_PAGES */ return (8ULL << 30) / 4096;
    default: return -1;
    }
}

int b_nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    bionic_thread_checkpoint();
    uint64_t t = time_ns();
    svcSleepThread((s64)req->tv_sec * 1000000000LL + req->tv_nsec);
    if (perf_on_main()) perf_add_wait(time_ns() - t);
    bionic_thread_checkpoint();
    return 0;
}

int b_usleep(unsigned long us)
{
    bionic_thread_checkpoint();
    uint64_t t = time_ns();
    svcSleepThread((s64)us * 1000);
    if (perf_on_main()) perf_add_wait(time_ns() - t);
    bionic_thread_checkpoint();
    return 0;
}
int b_sched_yield(void) { bionic_thread_checkpoint(); svcSleepThread(0); return 0; }

// ------------------------------------------------------------------ locale

// bionic's struct lconv
typedef struct {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol,
         *mon_decimal_point, *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes,
         n_sep_by_space, p_sign_posn, n_sign_posn, int_p_cs_precedes, int_p_sep_by_space,
         int_n_cs_precedes, int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
} bionic_lconv;

static bionic_lconv g_lconv = {
    ".", "", "", "", "", "", "", "", "", "",
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
};
static int g_locale_obj = 1;

void *b_localeconv(void) { return &g_lconv; }
char *b_setlocale(int cat, const char *loc) { (void)cat; (void)loc; return "C"; }
void *b_newlocale(int mask, const char *loc, void *base) { (void)mask; (void)loc; (void)base; return &g_locale_obj; }
void *b_uselocale(void *l) { (void)l; return &g_locale_obj; }
void b_freelocale(void *l) { (void)l; }
size_t b_ctype_get_mb_cur_max(void) { return 1; }

#define L1(name, call) int b_##name(int c, void *l) { (void)l; return call(c); }
L1(isdigit_l, isdigit) L1(islower_l, islower) L1(isupper_l, isupper) L1(isxdigit_l, isxdigit)
L1(tolower_l, tolower) L1(toupper_l, toupper)
#define W1(name, call) int b_##name(wint_t c, void *l) { (void)l; return call(c); }
W1(iswalpha_l, iswalpha) W1(iswblank_l, iswblank) W1(iswcntrl_l, iswcntrl) W1(iswdigit_l, iswdigit)
W1(iswlower_l, iswlower) W1(iswprint_l, iswprint) W1(iswpunct_l, iswpunct) W1(iswspace_l, iswspace)
W1(iswupper_l, iswupper) W1(iswxdigit_l, iswxdigit)
wint_t b_towlower_l(wint_t c, void *l) { (void)l; return towlower(c); }
wint_t b_towupper_l(wint_t c, void *l) { (void)l; return towupper(c); }
int b_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
size_t b_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
int b_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscmp(a, b); }
size_t b_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }
size_t b_strftime_l(char *s, size_t n, const char *f, const struct tm *t, void *l) { (void)l; return strftime(s, n, f, t); }
long double b_strtold_l(const char *s, char **e, void *l) { (void)l; return strtold(s, e); }
long long b_strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
unsigned long long b_strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }

int b_strerror_r(int err, char *buf, size_t n)
{
    snprintf(buf, n, "%s", strerror(err));
    return 0;
}

int b_isnanf(float f) { return __builtin_isnan(f); }
int b_signbit(double d) { return __builtin_signbit(d); }

// ------------------------------------------------------------------ misc stubs

char *b_getenv(const char *name)
{
    if (strcmp(name, "HOME") == 0) return ANDROID_FILES_DIR;
    if (strcmp(name, "TMPDIR") == 0) return ANDROID_DATA_DIR "cache";
    return NULL;
}

unsigned long b_getauxval(unsigned long type)
{
    if (type == 16 /* AT_HWCAP */)
        return (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7); // fp asimd aes pmull sha1 sha2 crc32
    if (type == 6 /* AT_PAGESZ */) return 4096;
    return 0;
}

int b_ret0(void) { return 0; }
int b_retm1(void) { errno = ENOSYS; return -1; }
void *b_retnull(void) { return NULL; }
int b_getpid(void) { return 1000; }
int b_gethostname(char *name, size_t len) { snprintf(name, len, "switch2"); return 0; }
int b_umask(int m) { (void)m; return 022; }
void *b_signal(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
int b_getpwuid_r(int uid, void *pw, char *buf, size_t n, void **res) { (void)uid; (void)pw; (void)buf; (void)n; *res = NULL; return ENOENT; }
unsigned b_alarm(unsigned s) { (void)s; return 0; }
void b_syslog(int p, const char *fmt, ...) { (void)p; (void)fmt; }

void b_abort(void)
{
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    char buf[256];
    LOG("abort() called from %s\n", so_describe_address(lr, buf, sizeof(buf)));
    fatal_error("The game called abort() from %s.", buf);
}

void b_exit(int code)
{
    LOG("exit(%d) called by the game\n", code);
    g_running = false;
    log_flush();
    exit(code);
}

void b_android_set_abort_message(const char *msg) { LOG("abort message: %s\n", msg); }

void b_assert2(const char *file, int line, const char *func, const char *expr)
{
    fatal_error("Assertion failed: %s\n%s:%d (%s)", expr, file, line, func);
}

int b_android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOG("[%d %s] %s\n", prio, tag ? tag : "", buf);
    return 0;
}

int b_android_log_write(int prio, const char *tag, const char *text)
{
    LOG("[%d %s] %s\n", prio, tag ? tag : "", text);
    return 0;
}

int b_android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    LOG("[%d %s] %s\n", prio, tag ? tag : "", buf);
    return 0;
}

// The game never unloads, so atexit-style registrations are kept but unused.
int b_cxa_atexit(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
void b_cxa_finalize(void *dso) { (void)dso; }
int b_cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso) { (void)fn; (void)obj; (void)dso; return 0; }

uint64_t b_stack_chk_guard = 0x5f3759df8badf00dULL;
void b_stack_chk_fail(void)
{
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    char buf[256];
    fatal_error("Stack corruption detected in %s.", so_describe_address(lr, buf, sizeof(buf)));
}

int b_dl_iterate_phdr(bionic_phdr_cb cb, void *data) { return so_iterate_phdr(cb, data); }

char *b_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return (char *)(s ? s + 1 : path);
}

void *b_memrchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s + n;
    while (n--) if (*--p == (uint8_t)c) return (void *)p;
    return NULL;
}

// time functions with struct tm are layout-compatible for the first nine
// fields; bionic's extra tm_gmtoff/tm_zone are left untouched by newlib.
struct tm *b_gmtime_r(const time_t *t, struct tm *out) { return gmtime_r(t, out); }
