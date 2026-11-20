// Sampling profiler for GD's update (config.ini: profile=1).
//
// A thread on another core pauses the game thread about 1000 times a second
// (svcSetThreadActivity), reads where it is (svcGetThreadContext3), and lets
// it run again. Only samples taken during GD's update (the "logic" part of a
// frame, see perf.c) are kept. Every ~10 s of samples the log gets the
// functions the update spends its time in:
//   - game/FMOD code by exported symbol
//   - loader code (the C library and the Android shims the game calls) as
//     "loader+offset", to be looked up in this build's OunceDash.elf, with
//     the game functions that called into it
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"

#define MAX_SAMPLES 16384
#define REPORT_SAMPLES 8000   // ~10 s of update time at ~1000 Hz
#define TOP 25

// Loader addresses are logged relative to main(); add main's address from
// `nm OunceDash.elf` to look them up.
extern int main(int argc, char **argv);
#define LOADER_ANCHOR ((uintptr_t)&main)

static Thread g_thread;
static bool g_started;
static volatile bool g_stop;
static uint64_t g_pc[MAX_SAMPLES], g_lr[MAX_SAMPLES], g_gd[MAX_SAMPLES];
static size_t g_n;
static uint64_t g_paused_fail;

typedef struct { uintptr_t key; const char *module, *name; unsigned count; } Bucket;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static int cmp_bucket(const void *a, const void *b)
{
    return (int)((const Bucket *)b)->count - (int)((const Bucket *)a)->count;
}

// Groups addresses into functions (game/FMOD) or 64-byte loader blocks.
static size_t group(uint64_t *addrs, size_t n, Bucket *out, size_t max)
{
    qsort(addrs, n, sizeof(uint64_t), cmp_u64);
    size_t nb = 0;
    uintptr_t loader = LOADER_ANCHOR;
    for (size_t i = 0; i < n; i++) {
        const char *module, *name;
        uintptr_t start;
        uintptr_t key;
        if (so_function_at(addrs[i], &module, &name, &start)) {
            key = start;
        } else {
            key = (addrs[i] & ~(uint64_t)63);
            module = "loader";
            name = NULL;
            start = key;
        }
        if (nb && out[nb - 1].key == key) { out[nb - 1].count++; continue; }
        // sorted input: the same function can reappear only for loader blocks
        size_t j;
        for (j = 0; j < nb; j++) if (out[j].key == key) break;
        if (j < nb) { out[j].count++; continue; }
        if (nb == max) continue;
        out[nb++] = (Bucket){ key, module, name, 1 };
        (void)loader;
    }
    qsort(out, nb, sizeof(Bucket), cmp_bucket);
    return nb;
}

static void report(size_t n)
{
    static Bucket b[4096];
    static uint64_t tmp[MAX_SAMPLES];
    memcpy(tmp, g_pc, n * sizeof(uint64_t));
    size_t nb = group(tmp, n, b, 4096);
    uintptr_t loader = LOADER_ANCHOR;
    LOG("profile: %zu samples of GD's update (%lu missed)\n", n, (unsigned long)g_paused_fail);
    unsigned in_loader = 0;
    for (size_t i = 0; i < nb; i++) if (!b[i].name) in_loader += b[i].count;
    for (size_t i = 0; i < nb && i < TOP; i++) {
        if (b[i].name)
            LOG("  %5.1f%%  %s %s\n", 100.0 * b[i].count / n, b[i].module, b[i].name);
        else
            LOG("  %5.1f%%  main%+ld\n", 100.0 * b[i].count / n, (long)(b[i].key - loader));
    }
    LOG("  loader code total: %.1f%%\n", 100.0 * in_loader / n);
    // which game functions call into loader code (by return address)
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        const char *module, *name;
        uintptr_t start;
        if (!so_function_at(g_pc[i], &module, &name, &start)) tmp[m++] = g_lr[i];
    }
    // the nearest game function up the call stack of each loader sample
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *module, *name;
        uintptr_t start;
        if (!so_function_at(g_pc[i], &module, &name, &start) && g_gd[i]) tmp[k++] = g_gd[i];
    }
    if (k) {
        size_t nk = group(tmp, k, b, 4096);
        LOG("  game functions whose calls end in loader code (from the stack):\n");
        for (size_t i = 0; i < nk && i < 15; i++)
            if (b[i].name) LOG("  %5.1f%%  %s %s\n", 100.0 * b[i].count / n, b[i].module, b[i].name);
    }
    m = 0;
    for (size_t i = 0; i < n; i++) {
        const char *module, *name;
        uintptr_t start;
        if (!so_function_at(g_pc[i], &module, &name, &start)) tmp[m++] = g_lr[i];
    }
    if (m) {
        nb = group(tmp, m, b, 4096);
        LOG("  callers of loader code:\n");
        for (size_t i = 0; i < nb && i < 12; i++) {
            if (b[i].name)
                LOG("  %5.1f%%  %s %s\n", 100.0 * b[i].count / n, b[i].module, b[i].name);
            else
                LOG("  %5.1f%%  main%+ld\n", 100.0 * b[i].count / n, (long)(b[i].key - loader));
        }
    }
}

static void prof_thread(void *arg)
{
    (void)arg;
    Handle h = perf_main_handle();
    while (!g_stop) {
        svcSleepThread(1000000);
        if (!perf_in_logic()) continue;
        if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) { g_paused_fail++; continue; }
        ThreadContext ctx;
        Result rc = svcGetThreadContext3(&ctx, h);
        uint64_t gd = 0;
        if (R_SUCCEEDED(rc)) {
            const char *module, *name;
            uintptr_t start;
            if (!so_function_at(ctx.pc.x, &module, &name, &start)) {
                // Frame records {previous fp, return address} on the game
                // thread's stack; the first return address inside the game
                // is the game function whose call led here.
                if (so_function_at(ctx.lr, &module, &name, &start)) gd = ctx.lr;
                uint64_t fp = ctx.fp, sp = ctx.sp;
                for (int d = 0; !gd && d < 32; d++) {
                    if (fp < sp || fp >= sp + 0x200000 || (fp & 15)) break;
                    uint64_t next = ((uint64_t *)fp)[0], ret = ((uint64_t *)fp)[1];
                    if (so_function_at(ret, &module, &name, &start)) gd = ret;
                    if (next <= fp) break;
                    fp = next;
                }
            }
        }
        svcSetThreadActivity(h, ThreadActivity_Runnable);
        if (R_FAILED(rc)) { g_paused_fail++; continue; }
        g_pc[g_n] = ctx.pc.x;
        g_lr[g_n] = ctx.lr;
        g_gd[g_n] = gd;
        if (++g_n >= REPORT_SAMPLES) {
            svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3b);
            report(g_n);
            svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);
            g_n = 0;
            g_paused_fail = 0;
        }
    }
}

void prof_start(void)
{
    if (!g_config.profile || !perf_main_handle()) return;
    // a worker core other than the last one (the audio mixer's)
    u64 mask = cores_worker_mask();
    int last = -1, core = -2;
    for (int c = 63; c >= 0; c--) if ((mask >> c) & 1) { last = c; break; }
    for (int c = last - 1; c >= 0; c--) if ((mask >> c) & 1) { core = c; break; }
    if (core < 0) core = last >= 0 ? last : -2;
    if (R_SUCCEEDED(threadCreate(&g_thread, prof_thread, NULL, NULL, 0x8000, 0x20, core)))
        g_started = R_SUCCEEDED(threadStart(&g_thread));
    LOG("profile: sampler %s on core %d (main() at %p)\n", g_started ? "started" : "FAILED", core,
        (void *)LOADER_ANCHOR);
}

void prof_stop(void)
{
    if (!g_started) return;
    g_stop = true;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    g_started = false;
}
