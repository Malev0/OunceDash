// Core layout.
//
// GD runs its game logic and records its draw calls together on one thread
// (CCDirector::drawScene updates the scheduler, then draws), so the two
// cannot be pulled apart without changing how the game steps. What can be
// split is the work behind the draw calls: the GL driver (Mesa/zink) records
// the game's GL calls cheaply and replays them on its own threads, which
// translate them to Vulkan and submit them to the GPU.
//
//   main core    the game thread: logic + GL call recording (pinned)
//   render core  the GL/Vulkan driver threads (preferred core; they may move
//                to the worker cores but never onto the main core)
//   other cores  the game's own worker threads (loading, networking, audio
//                decoding); the audio mixer stays on the last core
//
// The driver threads are created through newlib's pthread_create, which
// calls __syscall_thread_create; the link wraps that symbol (--wrap) so each
// new loader-side thread can be placed. libnx creates them on the main
// core at priority 0x3B (below the game's 0x2c), where they compete with
// the game thread for the same core.
#include <stdio.h>
#include "port.h"

#define GAME_PRIORITY 0x2c

static u64 g_mask;
static int g_main = -1, g_render = -1;
static u64 g_worker_mask;
static int g_render_threads;

void cores_init(void)
{
    if (R_FAILED(svcGetInfo(&g_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !g_mask)
        g_mask = 1ULL << svcGetCurrentProcessorNumber();
    g_main = svcGetCurrentProcessorNumber();
    int n = 0;
    for (int c = 0; c < 64; c++) if ((g_mask >> c) & 1) n++;
    if (n >= 3) {
        for (int c = 0; c < 64; c++)
            if (((g_mask >> c) & 1) && c != g_main) { g_render = c; break; }
        g_worker_mask = g_mask & ~(1ULL << g_main) & ~(1ULL << g_render);
        // keep the game thread on its core
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, g_main, 1ULL << g_main);
    } else {
        // too few cores to dedicate one: everything but the main core
        g_worker_mask = (g_mask & ~(1ULL << g_main)) ? g_mask & ~(1ULL << g_main) : g_mask;
    }
    LOG("cores: mask %#lx; game thread core %d, render core %d, game workers %#lx\n",
        (unsigned long)g_mask, g_main, g_render, (unsigned long)g_worker_mask);
}

int cores_main(void) { return g_main; }
u64 cores_worker_mask(void) { return g_worker_mask; }

// newlib/libnx pthread creation (the GL driver's threads)
struct __pthread_t;
int __real___syscall_thread_create(struct __pthread_t **thread, void *(*func)(void *), void *arg,
                                   void *stack_addr, size_t stack_size);

int __wrap___syscall_thread_create(struct __pthread_t **thread, void *(*func)(void *), void *arg,
                                   void *stack_addr, size_t stack_size)
{
    int rc = __real___syscall_thread_create(thread, func, arg, stack_addr, stack_size);
    if (rc != 0 || !*thread || g_render < 0) return rc;
    // struct __pthread_t begins with the libnx Thread
    Handle h = ((Thread *)*thread)->handle;
    u64 allowed = g_mask & ~(1ULL << g_main);
    Result r1 = svcSetThreadCoreMask(h, g_render, allowed);
    Result r2 = svcSetThreadPriority(h, GAME_PRIORITY);
    LOG("cores: driver thread %d -> core %d (0x%x, 0x%x)\n", ++g_render_threads, g_render, r1, r2);
    return rc;
}
