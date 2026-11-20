// bionic pthread ABI on libnx kernel primitives.
//
// bionic arm64 object sizes: mutex 40, cond 48, rwlock 56, attr 56, sem 16,
// key/once int, pthread_t long. Zero-initialised objects must be valid
// (PTHREAD_*_INITIALIZER), so the libnx primitives live in place:
//   mutex:  int state (type bits) | Mutex | owner handle | recursion count
//   cond:   CondVar
//   rwlock: RwLock (32 bytes)
//   sem:    Semaphore (16 bytes)
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>
#include "port.h"

#define B_ETIMEDOUT 110
#define B_EBUSY 16
#define B_EINVAL 22
#define B_EDEADLK 35
#define B_EPERM 1

typedef struct {
    int32_t state;   // 0x4000 recursive, 0x8000 error-check (bionic initializer bits)
    Mutex lock;
    uint64_t owner;
    uint32_t count;
} bmutex;
_Static_assert(sizeof(bmutex) <= 40, "mutex size");
_Static_assert(sizeof(RwLock) <= 56, "rwlock size");
_Static_assert(sizeof(Semaphore) <= 16, "sem size");

static inline uint64_t self_tag(void) { return (uint64_t)threadGetCurHandle(); }

// ---- cooperative thread exit at shutdown
// Horizon cannot end another thread, so on quit every game thread is woken
// and ends itself at the next blocking call it makes (a checkpoint).
static volatile bool g_threads_exiting;
static bool is_game_thread(void);
static void game_thread_exit(void) __attribute__((noreturn));

void bionic_thread_checkpoint(void)
{
    if (g_threads_exiting && is_game_thread())
        game_thread_exit();
}

// what game threads are currently blocked on, so shutdown can wake them
#define MAX_WAITS 256
static CondVar *g_wait_cv[MAX_WAITS];
static Semaphore *g_wait_sem[MAX_WAITS];
static Mutex g_wait_lock;

static int wait_register(void **table, void *obj)
{
    mutexLock(&g_wait_lock);
    for (int i = 0; i < MAX_WAITS; i++) {
        if (!table[i]) { table[i] = obj; mutexUnlock(&g_wait_lock); return i; }
    }
    mutexUnlock(&g_wait_lock);
    return -1;
}

static void wait_unregister(void **table, int slot)
{
    if (slot < 0) return;
    mutexLock(&g_wait_lock);
    table[slot] = NULL;
    mutexUnlock(&g_wait_lock);
}

int b_pthread_mutexattr_init(long *a) { *a = 0; return 0; }
int b_pthread_mutexattr_destroy(long *a) { (void)a; return 0; }
int b_pthread_mutexattr_settype(long *a, int type) { *a = type; return 0; }

int b_pthread_mutex_init(bmutex *m, const long *attr)
{
    memset(m, 0, sizeof(*m));
    long type = attr ? *attr : 0;
    m->state = type == 1 ? 0x4000 : type == 2 ? 0x8000 : 0;
    return 0;
}

int b_pthread_mutex_destroy(bmutex *m) { (void)m; return 0; }

int b_pthread_mutex_lock(bmutex *m)
{
    bionic_thread_checkpoint();
    uint64_t me = self_tag();
    if (m->state & 0xc000) {
        if (m->owner == me) {
            if (m->state & 0x4000) { m->count++; return 0; }
            return B_EDEADLK;
        }
    }
    if (perf_on_main()) {
        uint64_t t = time_ns();
        mutexLock(&m->lock);
        perf_add_wait(time_ns() - t);
    } else {
        mutexLock(&m->lock);
    }
    m->owner = me;
    m->count = 1;
    return 0;
}

int b_pthread_mutex_trylock(bmutex *m)
{
    uint64_t me = self_tag();
    if ((m->state & 0x4000) && m->owner == me) { m->count++; return 0; }
    if (!mutexTryLock(&m->lock)) return B_EBUSY;
    m->owner = me;
    m->count = 1;
    return 0;
}

int b_pthread_mutex_unlock(bmutex *m)
{
    if (m->state & 0xc000) {
        if (m->owner != self_tag()) return B_EPERM;
        if (--m->count > 0) return 0;
    }
    m->owner = 0;
    m->count = 0;
    mutexUnlock(&m->lock);
    return 0;
}

// ------------------------------------------------------------------ cond

int b_pthread_cond_init(CondVar *c, const void *attr) { (void)attr; condvarInit(c); return 0; }
int b_pthread_cond_destroy(CondVar *c) { (void)c; return 0; }
int b_pthread_cond_signal(CondVar *c) { condvarWakeOne(c); return 0; }
int b_pthread_cond_broadcast(CondVar *c) { condvarWakeAll(c); return 0; }

static int cond_wait_ns(CondVar *c, bmutex *m, int64_t timeout)
{
    uint32_t count = m->count;
    m->owner = 0;
    m->count = 0;
    int slot = wait_register((void **)g_wait_cv, c);
    uint64_t t = time_ns();
    Result rc = timeout < 0 ? condvarWait(c, &m->lock) : condvarWaitTimeout(c, &m->lock, timeout);
    if (perf_on_main()) perf_add_wait(time_ns() - t);
    wait_unregister((void **)g_wait_cv, slot);
    if (g_threads_exiting && is_game_thread()) {
        mutexUnlock(&m->lock); // don't die holding the game's mutex
        game_thread_exit();
    }
    m->owner = self_tag();
    m->count = count;
    return R_SUCCEEDED(rc) ? 0 : B_ETIMEDOUT;
}

int b_pthread_cond_wait(CondVar *c, bmutex *m) { return cond_wait_ns(c, m, -1); }

int b_pthread_cond_timedwait(CondVar *c, bmutex *m, const struct timespec *abs)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t ns = (int64_t)(abs->tv_sec - now.tv_sec) * 1000000000LL + (abs->tv_nsec - now.tv_nsec);
    if (ns <= 0) {
        // still release/reacquire to give other threads a chance
        ns = 0;
    }
    return cond_wait_ns(c, m, ns);
}

// ------------------------------------------------------------------ rwlock

int b_pthread_rwlock_init(RwLock *l, const void *a) { (void)a; rwlockInit(l); return 0; }
int b_pthread_rwlock_destroy(RwLock *l) { (void)l; return 0; }
int b_pthread_rwlock_rdlock(RwLock *l) { rwlockReadLock(l); return 0; }
int b_pthread_rwlock_wrlock(RwLock *l) { rwlockWriteLock(l); return 0; }
int b_pthread_rwlock_unlock(RwLock *l)
{
    if (rwlockIsWriteLockHeldByCurrentThread(l)) rwlockWriteUnlock(l);
    else rwlockReadUnlock(l);
    return 0;
}

// ------------------------------------------------------------------ semaphores

int b_sem_init(Semaphore *s, int pshared, unsigned value) { (void)pshared; semaphoreInit(s, value); return 0; }
int b_sem_destroy(Semaphore *s) { (void)s; return 0; }
int b_sem_post(Semaphore *s) { semaphoreSignal(s); return 0; }
int b_sem_wait(Semaphore *s)
{
    bionic_thread_checkpoint();
    int slot = wait_register((void **)g_wait_sem, s);
    uint64_t t = time_ns();
    semaphoreWait(s);
    if (perf_on_main()) perf_add_wait(time_ns() - t);
    wait_unregister((void **)g_wait_sem, slot);
    bionic_thread_checkpoint();
    return 0;
}

// ------------------------------------------------------------------ bionic TLS

// Android arm64 code reads its thread's TLS slots through TPIDR_EL0
// (stack protector: `mrs x8, tpidr_el0; ldr x8, [x8, #40]`, slot 5).
// libnx keeps its own thread pointer in TPIDRRO_EL0 and leaves TPIDR_EL0 at
// 0, so every thread that runs game code points TPIDR_EL0 at this block.
// Only read-only slots are used, so one block serves all threads.
//   slot -1 BIONIC_TLS, 0 DTV, 1 THREAD_ID, 2 APP, 3 OPENGL, 4 OPENGL_API,
//   5 STACK_GUARD, 6 SANITIZER, 7 ART_THREAD_SELF
extern uint64_t b_stack_chk_guard;
static uint64_t g_bionic_tls[1 + 16] __attribute__((aligned(16)));

void bionic_tls_setup_thread(void)
{
    uint64_t *slots = &g_bionic_tls[1];
    slots[5] = b_stack_chk_guard;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(slots));
}

bool bionic_tls_ready(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v == (uint64_t)&g_bionic_tls[1];
}

// ------------------------------------------------------------------ threads

#define MAX_KEYS 256

typedef struct bthread {
    Thread thread;
    void *(*fn)(void *);
    void *arg;
    void *ret;
    bool started_by_us;
    bool detached;
    void *tls[MAX_KEYS];
} bthread;

// every thread pthread_create started, for shutdown
#define MAX_TRACKED 256
static struct bthread *g_tracked[MAX_TRACKED];
static Mutex g_tracked_lock;

static void (*g_key_dtor[MAX_KEYS])(void *);
static bool g_key_used[MAX_KEYS];
static Mutex g_key_lock;
static __thread bthread *t_self;
static bthread g_main_thread;

static bthread *current(void)
{
    if (!t_self) {
        // Threads not created through pthread_create (main, audio).
        if (!g_main_thread.started_by_us && threadGetCurHandle() == envGetMainThreadHandle()) {
            t_self = &g_main_thread;
        } else {
            t_self = calloc(1, sizeof(bthread));
        }
    }
    return t_self;
}

static void run_key_destructors(bthread *t)
{
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (int k = 0; k < MAX_KEYS; k++) {
            if (t->tls[k] && g_key_dtor[k]) {
                void *v = t->tls[k];
                t->tls[k] = NULL;
                g_key_dtor[k](v);
                any = true;
            }
        }
        if (!any) break;
    }
}

static void thread_entry(void *p)
{
    bthread *t = p;
    bionic_tls_setup_thread();
    t_self = t;
    t->ret = t->fn(t->arg);
    run_key_destructors(t);
}

// bionic pthread_attr_t: flags(u32), stack_base, stack_size, guard_size, policy, priority
typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t policy, priority;
    char reserved[16];
} battr;
_Static_assert(sizeof(battr) == 56, "attr size");

int b_pthread_attr_init(battr *a) { memset(a, 0, sizeof(*a)); a->stack_size = 1 << 20; return 0; }
int b_pthread_attr_destroy(battr *a) { (void)a; return 0; }
int b_pthread_attr_setstacksize(battr *a, size_t s) { a->stack_size = s; return 0; }
int b_pthread_attr_setdetachstate(battr *a, int d) { if (d) a->flags |= 1; else a->flags &= ~1u; return 0; }

// Horizon does not time-slice threads of equal priority on one core, and
// Android code busy-waits on worker threads (FMOD loading, curl), which
// expects Linux scheduling. Worker threads therefore start on the cores the
// main thread is not using, round-robin, and may migrate between them.
static u64 g_core_mask;
static int g_main_core = -1;
static int g_next_core;

static int pick_core(void)
{
    if (!g_core_mask) {
        // cores.c: the worker cores exclude the game thread's and the render core
        g_core_mask = cores_worker_mask();
        g_main_core = cores_main();
        if (!g_core_mask) g_core_mask = 1ULL << svcGetCurrentProcessorNumber();
        LOG("threads: game workers on cores %#lx, game thread on core %d\n", (unsigned long)g_core_mask, g_main_core);
    }
    for (int tries = 0; tries < 64; tries++) {
        int c = g_next_core++ % 64;
        if ((g_core_mask >> c) & 1) return c;
    }
    return g_main_core;
}

int thread_core_mask(void) { if (!g_core_mask) pick_core(); return (int)g_core_mask; }

int b_pthread_create(long *out, const battr *attr, void *(*fn)(void *), void *arg)
{
    bthread *t = calloc(1, sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    t->started_by_us = true;
    t->detached = attr && (attr->flags & 1);
    size_t stack = attr && attr->stack_size ? attr->stack_size : (1 << 20);
    if (stack < 0x40000) stack = 0x40000;
    stack = (stack + 0xfff) & ~(size_t)0xfff;
    int core = pick_core();
    Result rc = threadCreate(&t->thread, thread_entry, t, NULL, stack, 0x2c, core);
    if (R_FAILED(rc)) {
        LOG("pthread_create failed: 0x%x\n", rc);
        free(t);
        return 11; // EAGAIN
    }
    // allow migration to any core the process owns, ideal core `core`
    svcSetThreadCoreMask(t->thread.handle, core, g_core_mask);
    LOG("pthread_create: thread %p on core %d (entry %p)\n", (void *)t, core, (void *)fn);
    mutexLock(&g_tracked_lock);
    for (int i = 0; i < MAX_TRACKED; i++) if (!g_tracked[i]) { g_tracked[i] = t; break; }
    mutexUnlock(&g_tracked_lock);
    threadStart(&t->thread);
    *out = (long)t;
    return 0;
}

int b_pthread_join(long th, void **ret)
{
    bthread *t = (bthread *)th;
    threadWaitForExit(&t->thread);
    mutexLock(&g_tracked_lock);
    for (int i = 0; i < MAX_TRACKED; i++) if (g_tracked[i] == t) g_tracked[i] = NULL;
    mutexUnlock(&g_tracked_lock);
    threadClose(&t->thread);
    if (ret) *ret = t->ret;
    free(t);
    return 0;
}

static bool is_game_thread(void)
{
    return t_self && t_self->started_by_us;
}

static void game_thread_exit(void)
{
    threadExit();
}

// Ends every thread the game started, before its code is unmapped: sets the
// exit flag, keeps waking whatever they wait on, and closes each one that
// has ended (which also frees its stack). Returns how many are still alive.
int threads_exit_all(uint64_t timeout_ns)
{
    g_threads_exiting = true;
    uint64_t start = time_ns();
    int alive = 0;
    for (;;) {
        mutexLock(&g_wait_lock);
        for (int i = 0; i < MAX_WAITS; i++) {
            if (g_wait_cv[i]) condvarWakeAll(g_wait_cv[i]);
            if (g_wait_sem[i]) semaphoreSignal(g_wait_sem[i]);
        }
        mutexUnlock(&g_wait_lock);

        alive = 0;
        mutexLock(&g_tracked_lock);
        for (int i = 0; i < MAX_TRACKED; i++) {
            bthread *t = g_tracked[i];
            if (!t) continue;
            if (R_SUCCEEDED(svcWaitSynchronizationSingle(t->thread.handle, 0))) {
                threadClose(&t->thread);
                g_tracked[i] = NULL;
                free(t);
            } else {
                alive++;
            }
        }
        mutexUnlock(&g_tracked_lock);
        if (!alive || time_ns() - start > timeout_ns) break;
        svcSleepThread(20000000LL);
    }
    if (alive) {
        mutexLock(&g_tracked_lock);
        for (int i = 0; i < MAX_TRACKED; i++) {
            bthread *t = g_tracked[i];
            if (!t) continue;
            char buf[256];
            LOG("shutdown: thread %p (entry %s) did not end\n", (void *)t,
                so_describe_address((uintptr_t)t->fn, buf, sizeof(buf)));
        }
        mutexUnlock(&g_tracked_lock);
    }
    LOG("shutdown: game threads still running: %d\n", alive);
    return alive;
}

int b_pthread_detach(long th) { ((bthread *)th)->detached = true; return 0; }
long b_pthread_self(void) { return (long)current(); }
int b_pthread_equal(long a, long b) { return a == b; }

int b_pthread_key_create(int *key, void (*dtor)(void *))
{
    mutexLock(&g_key_lock);
    for (int k = 0; k < MAX_KEYS; k++) {
        if (!g_key_used[k]) {
            g_key_used[k] = true;
            g_key_dtor[k] = dtor;
            *key = k;
            mutexUnlock(&g_key_lock);
            return 0;
        }
    }
    mutexUnlock(&g_key_lock);
    return 11;
}

int b_pthread_key_delete(int key)
{
    if (key < 0 || key >= MAX_KEYS) return B_EINVAL;
    g_key_used[key] = false;
    g_key_dtor[key] = NULL;
    return 0;
}

void *b_pthread_getspecific(int key)
{
    if (key < 0 || key >= MAX_KEYS) return NULL;
    return current()->tls[key];
}

int b_pthread_setspecific(int key, const void *v)
{
    if (key < 0 || key >= MAX_KEYS) return B_EINVAL;
    current()->tls[key] = (void *)v;
    return 0;
}

int b_pthread_once(atomic_int *once, void (*fn)(void))
{
    int expected = 0;
    if (atomic_compare_exchange_strong(once, &expected, 1)) {
        fn();
        atomic_store(once, 2);
        return 0;
    }
    while (atomic_load(once) != 2)
        svcSleepThread(100000);
    return 0;
}

int b_gettid(void) { return (int)threadGetCurHandle(); }

long b_syscall(long n, long a, long b, long c)
{
    (void)a; (void)b; (void)c;
    if (n == 178 /* gettid */) return b_gettid();
    return -1;
}

int b_setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }
