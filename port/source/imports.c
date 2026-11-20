// Symbol table for the Android libraries' imports.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/time.h>
#include <GLES2/gl2.h>
#include "port.h"

// ---- declarations of the shims (bionic_*.c, android.c)
#define X(name) extern char b_##name[];
#define SHIMS \
    X(fopen) X(fdopen) X(fclose) X(feof) X(ferror) X(fflush) X(fgets) X(fileno) X(fputc) X(fputs) \
    X(fread) X(fwrite) X(fseek) X(fseeko) X(ftell) X(ftello) X(getc) X(ungetc) X(setbuf) \
    X(vfprintf) X(fprintf) X(printf) X(open) X(access) X(mkdir) X(remove) X(rename) X(chmod) \
    X(stat) X(fstat) X(read) X(close) X(lseek) X(opendir) X(readdir) X(closedir) X(fcntl) X(mmap) X(munmap) X(mprotect) \
    X(madvise) X(mlock) X(clock_gettime) X(clock) X(sysconf) X(nanosleep) X(usleep) X(sched_yield) \
    X(localeconv) X(setlocale) X(newlocale) X(uselocale) X(freelocale) X(ctype_get_mb_cur_max) \
    X(isdigit_l) X(islower_l) X(isupper_l) X(isxdigit_l) X(tolower_l) X(toupper_l) \
    X(iswalpha_l) X(iswblank_l) X(iswcntrl_l) X(iswdigit_l) X(iswlower_l) X(iswprint_l) \
    X(iswpunct_l) X(iswspace_l) X(iswupper_l) X(iswxdigit_l) X(towlower_l) X(towupper_l) \
    X(strcoll_l) X(strxfrm_l) X(wcscoll_l) X(wcsxfrm_l) X(strftime_l) X(strtold_l) X(strtoll_l) \
    X(strtoull_l) X(strerror_r) X(isnanf) X(signbit) X(getenv) X(getauxval) X(ret0) X(retm1) \
    X(retnull) X(getpid) X(gethostname) X(umask) X(signal) X(getpwuid_r) X(alarm) X(syslog) \
    X(abort) X(exit) X(android_set_abort_message) X(assert2) X(android_log_print) \
    X(android_log_write) X(android_log_vprint) X(cxa_atexit) X(cxa_finalize) X(cxa_thread_atexit_impl) \
    X(stack_chk_fail) X(dl_iterate_phdr) X(basename) X(memrchr) X(gmtime_r) \
    X(pthread_mutexattr_init) X(pthread_mutexattr_destroy) X(pthread_mutexattr_settype) \
    X(pthread_mutex_init) X(pthread_mutex_destroy) X(pthread_mutex_lock) X(pthread_mutex_trylock) \
    X(pthread_mutex_unlock) X(pthread_cond_init) X(pthread_cond_destroy) X(pthread_cond_signal) \
    X(pthread_cond_broadcast) X(pthread_cond_wait) X(pthread_cond_timedwait) X(pthread_rwlock_init) \
    X(pthread_rwlock_destroy) X(pthread_rwlock_rdlock) X(pthread_rwlock_wrlock) X(pthread_rwlock_unlock) \
    X(sem_init) X(sem_destroy) X(sem_post) X(sem_wait) X(pthread_attr_init) X(pthread_attr_destroy) \
    X(pthread_attr_setstacksize) X(pthread_attr_setdetachstate) X(pthread_create) X(pthread_join) \
    X(pthread_detach) X(pthread_self) X(pthread_equal) X(pthread_key_create) X(pthread_key_delete) \
    X(pthread_getspecific) X(pthread_setspecific) X(pthread_once) X(gettid) X(syscall) X(setpriority) \
    X(socket) X(connect) X(bind) X(listen) X(accept) X(send) X(recv) X(sendto) X(recvfrom) \
    X(shutdown) X(getpeername) X(getsockname) X(setsockopt) X(getsockopt) X(poll) X(select) \
    X(ioctl) X(getaddrinfo) X(freeaddrinfo) X(gai_strerror) X(getnameinfo) X(inet_pton) \
    X(inet_ntop) X(gethostbyname) X(if_nametoindex) \
    X(sigsetjmp) X(siglongjmp)
SHIMS
#undef X

// defined below
void *b_dlopen(const char *name, int flags);
void *b_dlsym(void *h, const char *name);
int b_dlclose(void *h);
char *b_dlerror(void);
int b_cxa_guard_acquire(uint64_t *g);
void b_cxa_guard_release(uint64_t *g);
void b_cxa_pure_virtual(void);
void b_fd_set_chk(int fd, void *set, size_t size);
void b_sincos(double x, double *s, double *c);
void b_sincosf(float x, float *s, float *c);
// timed GL entry points (perf.c)
void b_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                    GLint border, GLenum format, GLenum type, const void *pixels);
void b_glLinkProgram(GLuint program);
void b_glClear(GLbitfield mask);
// per-frame vertex buffer ring (glbuf.c)
void b_glBindBuffer(GLenum target, GLuint buffer);
void b_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void b_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
void b_glDeleteBuffers(GLsizei n, const GLuint *buffers);

extern int *__errno(void);
extern int vasprintf(char **, const char *, va_list);
extern uint8_t bionic_sF[];
extern uint64_t b_stack_chk_guard;

typedef struct { const char *name; uintptr_t addr; } Sym;

#define D(n) { #n, (uintptr_t)&(n) }
#define W(n) { #n, (uintptr_t)b_##n }
#define R(n, f) { n, (uintptr_t)(f) }

static const Sym g_syms[] = {
    // --- bionic internals
    R("__android_log_print", b_android_log_print),
    R("__android_log_write", b_android_log_write),
    R("__android_log_vprint", b_android_log_vprint),
    R("__ctype_get_mb_cur_max", b_ctype_get_mb_cur_max),
    R("__cxa_atexit", b_cxa_atexit),
    R("__cxa_finalize", b_cxa_finalize),
    R("__cxa_thread_atexit_impl", b_cxa_thread_atexit_impl),
    R("__cxa_guard_acquire", b_cxa_guard_acquire),
    R("__cxa_guard_release", b_cxa_guard_release),
    R("__cxa_pure_virtual", b_cxa_pure_virtual),
    R("__errno", __errno),
    R("__isnanf", b_isnanf),
    R("__sF", bionic_sF),
    R("__signbit", b_signbit),
    R("__stack_chk_fail", b_stack_chk_fail),
    R("__stack_chk_guard", &b_stack_chk_guard),
    R("__assert2", b_assert2),
    R("__FD_SET_chk", b_fd_set_chk),
    R("_ZdlPv", free),
    R("android_set_abort_message", b_android_set_abort_message),

    // --- stdio / files
    W(fopen), W(fdopen), W(fclose), W(feof), W(ferror), W(fflush), W(fgets), W(fileno),
    W(fputc), W(fputs), W(fread), W(fwrite), W(fseek), W(fseeko), W(ftell), W(ftello),
    W(getc), W(ungetc), W(setbuf), W(vfprintf), W(fprintf), W(printf),
    W(open), W(access), W(mkdir), W(remove), W(rename), W(chmod), W(stat), W(fstat),
    W(opendir), W(readdir), W(closedir), W(fcntl),
    W(close), W(read), D(write), W(lseek),
    D(snprintf), D(sprintf), D(vsnprintf), D(vasprintf), D(sscanf), D(vsscanf), D(swprintf),

    // --- memory
    D(malloc), D(calloc), D(realloc), D(free),
    W(mmap), W(munmap), W(mprotect), W(madvise), W(mlock),

    // --- strings
    D(memchr), D(memcmp), D(memcpy), D(memmove), D(memset), W(memrchr),
    D(strcasecmp), D(strcat), D(strchr), D(strcmp), D(strcpy), D(strcspn), D(strdup),
    D(strerror), W(strerror_r), D(strlen), D(strncasecmp), D(strncmp), D(strncpy), D(strpbrk),
    D(strrchr), D(strspn), D(strstr), D(strtok), D(strtok_r), D(strftime),
    D(strtod), D(strtof), D(strtol), D(strtold), D(strtoll), D(strtoul), D(strtoull),
    D(atof), D(atoi), D(atoll), W(basename), D(bsearch), D(qsort),
    D(btowc), D(wctob), D(mbrlen), D(mbrtowc), D(mbsnrtowcs), D(mbsrtowcs), D(mbtowc),
    D(wcrtomb), D(wcslen), D(wcsnrtombs), D(wcstod), D(wcstof), D(wcstol), D(wcstold),
    D(wcstoll), D(wcstoul), D(wcstoull), D(wmemchr), D(wmemcmp), D(wmemcpy), D(wmemmove), D(wmemset),

    // --- ctype / locale
    D(isalnum), D(isalpha), D(isascii), D(isgraph), D(islower), D(isprint), D(isspace),
    D(isupper), D(isxdigit), D(tolower), D(toupper),
    W(isdigit_l), W(islower_l), W(isupper_l), W(isxdigit_l), W(tolower_l), W(toupper_l),
    W(iswalpha_l), W(iswblank_l), W(iswcntrl_l), W(iswdigit_l), W(iswlower_l), W(iswprint_l),
    W(iswpunct_l), W(iswspace_l), W(iswupper_l), W(iswxdigit_l), W(towlower_l), W(towupper_l),
    W(strcoll_l), W(strxfrm_l), W(wcscoll_l), W(wcsxfrm_l), W(strftime_l), W(strtold_l),
    W(strtoll_l), W(strtoull_l), W(localeconv), W(setlocale), W(newlocale), W(uselocale),
    W(freelocale),

    // --- math
    D(abs), D(acos), D(acosf), D(asinf), D(atan2), D(atan2f), D(atanf), D(cos), D(cosf),
    D(exp), D(expf), D(exp2f), D(fmax), D(fmin), D(fmod), D(fmodf), D(frexp), D(ldexp), D(ldexpf),
    D(log), D(log10), D(log10f), D(logf), D(lround), D(modf), D(pow), D(powf), D(sin), D(sinf),
    D(sqrt), D(sqrtf), D(tanf), W(sincos), W(sincosf), D(rand), D(srand),

    // --- time
    D(time), D(difftime), D(gmtime), W(gmtime_r), D(localtime), D(gettimeofday),
    W(clock_gettime), W(clock), W(nanosleep), W(usleep), W(sched_yield), W(sysconf),

    // --- setjmp
    D(setjmp), D(longjmp), W(sigsetjmp), W(siglongjmp),

    // --- process / user / signals (stubs)
    W(abort), W(exit), W(getenv), W(getauxval), W(getpid), W(gethostname), W(umask),
    W(signal), W(getpwuid_r), W(alarm), W(syslog),
    R("getuid", b_ret0), R("geteuid", b_ret0), R("getgid", b_ret0), R("getegid", b_ret0),
    R("setuid", b_ret0), R("setgid", b_ret0), R("initgroups", b_ret0),
    R("getpwuid", b_retnull), R("openlog", b_ret0), R("closelog", b_ret0),
    R("sigaction", b_ret0), R("sigaddset", b_ret0), R("sigdelset", b_ret0),
    R("sigemptyset", b_ret0), R("sigfillset", b_ret0), R("sigprocmask", b_ret0),
    R("kill", b_retm1), R("fork", b_retm1), R("execl", b_retm1), R("waitpid", b_retm1),
    R("dup2", b_retm1), R("pipe", b_retm1), R("socketpair", b_retm1),
    R("tcgetattr", b_retm1), R("tcsetattr", b_retm1),

    // --- dynamic linker
    W(dlopen), W(dlsym), W(dlclose), W(dlerror), W(dl_iterate_phdr),

    // --- threads
    W(pthread_mutexattr_init), W(pthread_mutexattr_destroy), W(pthread_mutexattr_settype),
    W(pthread_mutex_init), W(pthread_mutex_destroy), W(pthread_mutex_lock), W(pthread_mutex_trylock),
    W(pthread_mutex_unlock), W(pthread_cond_init), W(pthread_cond_destroy), W(pthread_cond_signal),
    W(pthread_cond_broadcast), W(pthread_cond_wait), W(pthread_cond_timedwait),
    W(pthread_rwlock_init), W(pthread_rwlock_destroy), W(pthread_rwlock_rdlock),
    W(pthread_rwlock_wrlock), W(pthread_rwlock_unlock), W(sem_init), W(sem_destroy), W(sem_post),
    W(sem_wait), W(pthread_attr_init), W(pthread_attr_destroy), W(pthread_attr_setstacksize),
    W(pthread_attr_setdetachstate), W(pthread_create), W(pthread_join), W(pthread_detach),
    W(pthread_self), W(pthread_equal), W(pthread_key_create), W(pthread_key_delete),
    W(pthread_getspecific), W(pthread_setspecific), W(pthread_once), W(gettid), W(syscall),
    W(setpriority),

    // --- network
    W(socket), W(connect), W(bind), W(listen), W(accept), W(send), W(recv), W(sendto),
    W(recvfrom), W(shutdown), W(getpeername), W(getsockname), W(setsockopt), W(getsockopt),
    W(poll), W(select), W(ioctl), W(getaddrinfo), W(freeaddrinfo), W(gai_strerror),
    W(getnameinfo), W(inet_pton), W(inet_ntop), W(gethostbyname), W(if_nametoindex),

    // --- OpenGL ES 2
    D(glActiveTexture), D(glAttachShader), D(glBindAttribLocation), W(glBindBuffer),
    D(glBindFramebuffer), D(glBindRenderbuffer), D(glBindTexture), D(glBlendEquation),
    D(glBlendFunc), W(glBufferData), W(glBufferSubData), D(glCheckFramebufferStatus), W(glClear),
    D(glClearColor), D(glClearDepthf), D(glClearStencil), D(glCompileShader),
    D(glCompressedTexImage2D), D(glCreateProgram), D(glCreateShader), W(glDeleteBuffers),
    D(glDeleteFramebuffers), D(glDeleteProgram), D(glDeleteRenderbuffers), D(glDeleteShader),
    D(glDeleteTextures), D(glDepthFunc), D(glDepthMask), D(glDisable),
    D(glDisableVertexAttribArray), D(glDrawArrays), D(glDrawElements), D(glEnable),
    D(glEnableVertexAttribArray), D(glFramebufferRenderbuffer), D(glFramebufferTexture2D),
    D(glGenBuffers), D(glGenFramebuffers), D(glGenRenderbuffers), D(glGenTextures),
    D(glGenerateMipmap), D(glGetBooleanv), D(glGetError), D(glGetFloatv), D(glGetIntegerv),
    D(glGetProgramInfoLog), D(glGetProgramiv), D(glGetShaderInfoLog), D(glGetShaderSource),
    D(glGetShaderiv), D(glGetString), D(glGetUniformLocation), D(glIsEnabled), D(glLineWidth),
    W(glLinkProgram), D(glPixelStorei), D(glReadPixels), D(glRenderbufferStorage), D(glScissor),
    D(glShaderSource), D(glStencilFunc), D(glStencilMask), D(glStencilOp), W(glTexImage2D),
    D(glTexParameteri), D(glUniform1f), D(glUniform1i), D(glUniform2f), D(glUniform2fv),
    D(glUniform2i), D(glUniform2iv), D(glUniform3f), D(glUniform3fv), D(glUniform3i),
    D(glUniform3iv), D(glUniform4f), D(glUniform4fv), D(glUniform4i), D(glUniform4iv),
    D(glUniformMatrix3fv), D(glUniformMatrix4fv), D(glUseProgram), D(glVertexAttribPointer),
    D(glViewport), D(glTexSubImage2D), D(glFinish), D(glFlush), D(glColorMask),
    D(glBlendFuncSeparate), D(glBlendEquationSeparate), D(glTexParameterf), D(glCullFace),
    D(glFrontFace), D(glIsTexture), D(glIsBuffer), D(glGetAttribLocation), D(glHint),
};

static int cmp_sym(const void *a, const void *b)
{
    return strcmp(((const Sym *)a)->name, ((const Sym *)b)->name);
}

static Sym *g_sorted;
static size_t g_count;

static void ensure_sorted(void)
{
    if (g_sorted) return;
    g_count = sizeof(g_syms) / sizeof(g_syms[0]);
    g_sorted = malloc(sizeof(g_syms));
    memcpy(g_sorted, g_syms, sizeof(g_syms));
    qsort(g_sorted, g_count, sizeof(Sym), cmp_sym);
}

uintptr_t imports_lookup(const char *name)
{
    ensure_sorted();
    Sym key = { name, 0 };
    Sym *s = bsearch(&key, g_sorted, g_count, sizeof(Sym), cmp_sym);
    return s ? s->addr : 0;
}

uintptr_t imports_resolve(const char *name, so_module *requester)
{
    uintptr_t a = imports_lookup(name);
    if (a) return a;
    // other loaded Android libraries (FMOD_* from libfmod.so)
    for (const char **lib = (const char *[]){ "libfmod.so", "libcocos2dcpp.so", NULL }; *lib; lib++) {
        so_module *m = so_find(*lib);
        if (m && m != requester) {
            a = so_symbol(m, name);
            if (a) return a;
        }
    }
    return 0;
}

void unresolved_import_stub(void)
{
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    char buf[256];
    fatal_error("The game called a function this loader does not provide.\nCaller: %s",
                so_describe_address(lr, buf, sizeof(buf)));
}

// ------------------------------------------------------------------ libdl

#define H_LIBC    ((void *)0x1001)
#define H_ANDROID ((void *)0x1002)
#define H_AAUDIO  ((void *)0x1003)
#define H_LOG     ((void *)0x1004)
static __thread const char *t_dlerror;

void *b_dlopen(const char *name, int flags)
{
    (void)flags;
    if (!name) return H_LIBC;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (!strcmp(base, "libandroid.so")) return H_ANDROID;
    if (!strcmp(base, "libaaudio.so")) return H_AAUDIO;
    if (!strcmp(base, "liblog.so")) return H_LOG;
    if (!strcmp(base, "libc.so") || !strcmp(base, "libm.so") || !strcmp(base, "libdl.so"))
        return H_LIBC;
    so_module *m = so_find(base);
    if (m) return m;
    // OpenSL ES is not provided, so FMOD uses AAudio.
    LOG("dlopen(%s): not available\n", name);
    t_dlerror = "library not found";
    return NULL;
}

void *b_dlsym(void *h, const char *name)
{
    uintptr_t a = 0;
    if (h == H_ANDROID) a = android_lookup("libandroid.so", name);
    else if (h == H_AAUDIO) a = android_lookup("libaaudio.so", name);
    else if (h == H_LOG || h == H_LIBC || h == NULL || h == (void *)-1) {
        a = imports_lookup(name);
        if (!a) a = android_lookup(NULL, name);
        if (!a) a = imports_resolve(name, NULL);
    } else {
        a = so_symbol((so_module *)h, name);
    }
    if (!a) {
        LOG("dlsym(%s): not found\n", name);
        t_dlerror = "symbol not found";
    }
    return (void *)a;
}

int b_dlclose(void *h) { (void)h; return 0; }
char *b_dlerror(void) { const char *e = t_dlerror; t_dlerror = NULL; return (char *)e; }

// ------------------------------------------------------------------ misc ABI helpers

static RMutex g_guard_lock;

int b_cxa_guard_acquire(uint64_t *g)
{
    if (__atomic_load_n((uint8_t *)g, __ATOMIC_ACQUIRE)) return 0;
    rmutexLock(&g_guard_lock);
    if (*(uint8_t *)g) { rmutexUnlock(&g_guard_lock); return 0; }
    return 1;
}

void b_cxa_guard_release(uint64_t *g)
{
    __atomic_store_n((uint8_t *)g, 1, __ATOMIC_RELEASE);
    rmutexUnlock(&g_guard_lock);
}

void b_cxa_pure_virtual(void) { fatal_error("Pure virtual function called."); }

void b_fd_set_chk(int fd, void *set, size_t size)
{
    (void)size;
    ((uint64_t *)set)[fd / 64] |= 1ULL << (fd % 64);
}

void b_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
void b_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
