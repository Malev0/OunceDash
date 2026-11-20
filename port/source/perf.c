// Frame timing for the log, to tell where slow frames come from.
//
// Each frame is split into
//   game  - input + nativeRender on the game thread (core 0), split at the
//           frame's first glClear (CCDirector::drawScene updates the game,
//           then clears, then draws) into
//             logic - input and GD's update
//             draw  - GD recording the frame's draw calls
//   swap  - eglSwapBuffers: the GL driver finishing the frame, waiting for
//           the GPU and for the display's next refresh
// and the work done meanwhile is counted: game files read (getFileData,
// with -uhd textures on), texture uploads (glTexImage2D) and shader program
// links (glLinkProgram, where the driver compiles shaders it has not cached).
//
// A frame much longer than the refresh interval is logged with that
// breakdown. Every 1200 frames a summary line gives averages and maxima.
// High "game" means the CPU side of GD is the limit; high "swap" with low
// "game" means the driver or the GPU is.
#include <stdio.h>
#include <string.h>
#include <GLES2/gl2.h>
#include "port.h"

static volatile uint64_t g_file_ns, g_file_count, g_file_bytes;
// game-thread only
static Handle g_main_handle;
static uint64_t g_wait_ns, g_java_ns, g_java_count, g_text_ns, g_text_count;
static uint64_t g_prev_wait_ns, g_prev_java_ns, g_prev_java_count, g_prev_text_ns, g_prev_text_count;

void perf_init_main(void) { g_main_handle = threadGetCurHandle(); }
Handle perf_main_handle(void) { return g_main_handle; }
static volatile bool g_in_logic;
void perf_frame_begin(void) { g_in_logic = true; }
bool perf_in_logic(void) { return g_in_logic; }
bool perf_on_main(void) { return g_main_handle && threadGetCurHandle() == g_main_handle; }
void perf_add_wait(uint64_t ns) { g_wait_ns += ns; }
void perf_add_java(uint64_t ns) { g_java_ns += ns; g_java_count++; }
void perf_add_text(uint64_t ns) { g_text_ns += ns; g_text_count++; }
static uint64_t g_tex_ns, g_tex_count, g_tex_bytes, g_link_ns, g_link_count;

void perf_add_file(uint64_t ns, uint64_t bytes)
{
    __atomic_add_fetch(&g_file_ns, ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_file_count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_file_bytes, bytes, __ATOMIC_RELAXED);
}

// GL entry points the game imports (imports.c), timed. GL runs on the game
// thread only, so plain counters suffice.
void b_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                    GLint border, GLenum format, GLenum type, const void *pixels)
{
    uint64_t t = time_ns();
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    g_tex_ns += time_ns() - t;
    g_tex_count++;
    g_tex_bytes += (uint64_t)width * height * 4;
}

// The first glClear of a frame marks the end of the game update in
// CCDirector::drawScene.
static uint64_t g_t_clear;
void b_glClear(GLbitfield mask)
{
    if (!g_t_clear) { g_t_clear = time_ns(); g_in_logic = false; }
    glClear(mask);
}

void b_glLinkProgram(GLuint program)
{
    uint64_t t = time_ns();
    glLinkProgram(program);
    g_link_ns += time_ns() - t;
    g_link_count++;
}

// ------------------------------------------------------------------ per frame

static struct {
    uint64_t frames, slow, game_sum, swap_sum, game_max, swap_max, total_max;
    uint64_t logic_sum, draw_sum, logic_max, draw_max;
    uint64_t wait_sum, java_sum, java_count, text_sum, text_count;
} g_win;
static uint64_t g_last_slow_log, g_slow_unlogged;
static uint64_t g_prev_file_ns, g_prev_file_count, g_prev_file_bytes;
static uint64_t g_prev_tex_ns, g_prev_tex_count, g_prev_tex_bytes, g_prev_link_ns, g_prev_link_count;

void perf_frame(uint64_t t_start, uint64_t t_game, uint64_t t_swap_start, uint64_t t_end)
{
    uint64_t game = t_game - t_start, swap = t_end - t_swap_start, total = t_end - t_start;
    uint64_t t_clear = g_t_clear >= t_start && g_t_clear <= t_game ? g_t_clear : t_game;
    uint64_t logic = t_clear - t_start, draw = t_game - t_clear;
    g_t_clear = 0;
    g_in_logic = false;
    uint64_t budget = 1000000000ULL / (g_config.fps > 0 ? g_config.fps : 120);

    g_win.frames++;
    g_win.game_sum += game;
    g_win.swap_sum += swap;
    if (game > g_win.game_max) g_win.game_max = game;
    if (swap > g_win.swap_max) g_win.swap_max = swap;
    if (total > g_win.total_max) g_win.total_max = total;
    uint64_t wait = g_wait_ns - g_prev_wait_ns, java = g_java_ns - g_prev_java_ns;
    uint64_t java_n = g_java_count - g_prev_java_count, text = g_text_ns - g_prev_text_ns;
    uint64_t text_n = g_text_count - g_prev_text_count;
    g_win.wait_sum += wait;
    g_win.java_sum += java;
    g_win.java_count += java_n;
    g_win.text_sum += text;
    g_win.text_count += text_n;
    g_win.logic_sum += logic;
    g_win.draw_sum += draw;
    if (logic > g_win.logic_max) g_win.logic_max = logic;
    if (draw > g_win.draw_max) g_win.draw_max = draw;

    uint64_t file_ns = g_file_ns, file_count = g_file_count, file_bytes = g_file_bytes;
    // a frame that took more than 1.5 refresh intervals missed at least one
    if (total > budget * 3 / 2) {
        g_win.slow++;
        if (t_end - g_last_slow_log > 250000000ULL) { // at most 4 lines a second
            LOG("slow frame: %.1f ms (logic %.1f, draw %.1f, swap %.1f) | files %lu (%lu KB, %.1f ms) | "
                "textures %lu (%lu KB, %.1f ms) | shader links %lu (%.1f ms) | "
                "waiting %.1f ms | java %lu (%.1f ms) | text %lu (%.1f ms)%s\n",
                total / 1e6, logic / 1e6, draw / 1e6, swap / 1e6,
                (unsigned long)(file_count - g_prev_file_count),
                (unsigned long)((file_bytes - g_prev_file_bytes) >> 10), (file_ns - g_prev_file_ns) / 1e6,
                (unsigned long)(g_tex_count - g_prev_tex_count),
                (unsigned long)((g_tex_bytes - g_prev_tex_bytes) >> 10), (g_tex_ns - g_prev_tex_ns) / 1e6,
                (unsigned long)(g_link_count - g_prev_link_count), (g_link_ns - g_prev_link_ns) / 1e6,
                wait / 1e6, (unsigned long)java_n, java / 1e6, (unsigned long)text_n, text / 1e6,
                g_slow_unlogged ? " (+ earlier slow frames not logged)" : "");
            g_last_slow_log = t_end;
            g_slow_unlogged = 0;
        } else {
            g_slow_unlogged++;
        }
    }
    g_prev_file_ns = file_ns; g_prev_file_count = file_count; g_prev_file_bytes = file_bytes;
    g_prev_tex_ns = g_tex_ns; g_prev_tex_count = g_tex_count; g_prev_tex_bytes = g_tex_bytes;
    g_prev_link_ns = g_link_ns; g_prev_link_count = g_link_count;
    g_prev_wait_ns = g_wait_ns; g_prev_java_ns = g_java_ns; g_prev_java_count = g_java_count;
    g_prev_text_ns = g_text_ns; g_prev_text_count = g_text_count;

    if (g_win.frames >= 1200) {
        LOG("frames: logic avg %.2f ms max %.1f | draw avg %.2f ms max %.1f | game avg %.2f ms max %.1f | "
            "swap avg %.2f ms max %.1f | worst %.1f ms | %lu of %lu over budget (%.2f ms) | %dx%d | "
            "per frame: waiting %.2f ms, java %.1f calls %.2f ms, text %.2f ms\n",
            g_win.logic_sum / 1e6 / g_win.frames, g_win.logic_max / 1e6,
            g_win.draw_sum / 1e6 / g_win.frames, g_win.draw_max / 1e6,
            g_win.game_sum / 1e6 / g_win.frames, g_win.game_max / 1e6,
            g_win.swap_sum / 1e6 / g_win.frames, g_win.swap_max / 1e6, g_win.total_max / 1e6,
            (unsigned long)g_win.slow, (unsigned long)g_win.frames, budget / 1e6, g_screen_w, g_screen_h,
            g_win.wait_sum / 1e6 / g_win.frames, (double)g_win.java_count / g_win.frames,
            g_win.java_sum / 1e6 / g_win.frames, g_win.text_sum / 1e6 / g_win.frames);
        memset(&g_win, 0, sizeof(g_win));
    }
}
