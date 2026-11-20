// Top-left frame-rate counter, toggled with the - button.
//
// The counter is drawn by the loader on top of the game's frame, from the
// loader's own frame timing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>
#include "port.h"

// ------------------------------------------------------------------ measuring

static uint64_t g_window_start, g_window_frames;
static int g_fps_value = -1;
static bool g_enabled;
static bool g_loaded;

// Toggled with the - button; remembered in the loader's preferences.
void fps_toggle(void)
{
    g_enabled = !g_enabled;
    prefs_set("ounce_show_fps", g_enabled ? "1" : "0");
    prefs_flush();
    LOG("fps counter: %s\n", g_enabled ? "on" : "off");
}

void fps_frame(void)
{
    uint64_t now = time_ns();
    if (!g_window_start) g_window_start = now;
    g_window_frames++;
    if (now - g_window_start >= 500000000ULL) {
        g_fps_value = (int)(g_window_frames * 1000000000ULL / (now - g_window_start) + 0);
        g_window_start = now;
        g_window_frames = 0;
    }
    if (!g_loaded) {
        const char *v = prefs_get("ounce_show_fps");
        g_enabled = v && v[0] == '1';
        g_loaded = true;
    }
}

// ------------------------------------------------------------------ drawing

static GLuint g_prog, g_tex;
static GLint g_u_tex;
static int g_tex_w, g_tex_h, g_shown_value = -2;

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static void init_gl(void)
{
    const char *vs =
        "attribute vec2 a_pos;\nattribute vec2 a_uv;\nvarying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    const char *fs =
        "precision mediump float;\nvarying vec2 v_uv;\nuniform sampler2D u_tex;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(g_prog, 0, "a_pos");
    glBindAttribLocation(g_prog, 1, "a_uv");
    glLinkProgram(g_prog);
    g_u_tex = glGetUniformLocation(g_prog, "u_tex");
    glGenTextures(1, &g_tex);
}

static void update_texture(void)
{
    char label[32];
    if (g_fps_value < 0) snprintf(label, sizeof(label), "FPS: --");
    else snprintf(label, sizeof(label), "FPS: %d", g_fps_value);
    int w, h;
    uint8_t *rgba = text_render_rgba(label, 30.0f * g_screen_h / 1080.0f, &w, &h);
    if (!rgba) return;
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(rgba);
    g_tex_w = w;
    g_tex_h = h;
    g_shown_value = g_fps_value;
}

void fps_draw(void)
{
    if (!g_enabled) return;
    if (!g_prog) init_gl();

    // save what cocos2d caches
    GLint prog, buf, tex, active, viewport[4], bsrc, bdst, unpack;
    GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean stencil = glIsEnabled(GL_STENCIL_TEST), depth = glIsEnabled(GL_DEPTH_TEST);
    GLint enabled[3];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
    for (int i = 0; i < 3; i++) glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);

    if (g_shown_value != g_fps_value) update_texture();

    if (g_tex_w > 0) {
        glViewport(0, 0, g_screen_w, g_screen_h);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(g_prog);
        glUniform1i(g_u_tex, 0);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDisableVertexAttribArray(2);

        float margin = 12.0f * g_screen_h / 1080.0f;
        float x0 = margin, y0 = margin, x1 = x0 + g_tex_w, y1 = y0 + g_tex_h;
        float sx = 2.0f / g_screen_w, sy = 2.0f / g_screen_h;
        GLfloat pos[8] = { x0 * sx - 1, 1 - y0 * sy, x1 * sx - 1, 1 - y0 * sy,
                           x0 * sx - 1, 1 - y1 * sy, x1 * sx - 1, 1 - y1 * sy };
        static const GLfloat uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    for (int i = 0; i < 3; i++) {
        if (enabled[i]) glEnableVertexAttribArray(i);
        else glDisableVertexAttribArray(i);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glActiveTexture(active);
    glUseProgram(prog);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBlendFunc(bsrc, bdst);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
    if (!blend) glDisable(GL_BLEND);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (depth) glEnable(GL_DEPTH_TEST);
}
