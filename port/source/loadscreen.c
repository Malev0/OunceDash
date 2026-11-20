// First-launch screen while the -uhd icon cache is built (uhd_pack.c).
//
// Shown after the display is up and before the game starts, only when the
// cache has to be (re)built. It keeps the applet loop running (HOME and
// closing work) and draws the title, a progress bar and the file count.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>
#include "port.h"

static GLuint g_prog, g_white;
static GLint g_u_tex, g_u_color;

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
        "precision mediump float;\nvarying vec2 v_uv;\nuniform sampler2D u_tex;\nuniform vec4 u_color;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color; }\n";
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(g_prog, 0, "a_pos");
    glBindAttribLocation(g_prog, 1, "a_uv");
    glLinkProgram(g_prog);
    g_u_tex = glGetUniformLocation(g_prog, "u_tex");
    g_u_color = glGetUniformLocation(g_prog, "u_color");
    static const uint8_t white[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static void quad(GLuint tex, float x0, float y0, float x1, float y1, int w, int h,
                 float r, float g, float b, float a)
{
    float sx = 2.0f / w, sy = 2.0f / h;
    GLfloat pos[8] = { x0 * sx - 1, 1 - y0 * sy, x1 * sx - 1, 1 - y0 * sy,
                       x0 * sx - 1, 1 - y1 * sy, x1 * sx - 1, 1 - y1 * sy };
    static const GLfloat uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform4f(g_u_color, r, g, b, a);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Text centred at (cx, y), height px; returns nothing if fonts are missing.
static void text(const char *s, float px, float cx, float y, int w, int h, float r, float g, float b)
{
    int tw, th;
    uint8_t *rgba = text_render_rgba(s, px, &tw, &th);
    if (!rgba) return;
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(rgba);
    quad(t, cx - tw / 2.0f, y, cx + tw / 2.0f, y + th, w, h, r, g, b, 1);
    glDeleteTextures(1, &t);
}

static void draw(int w, int h, size_t done, size_t total, bool writing)
{
    float u = h / 1080.0f;
    glViewport(0, 0, w, h);
    glClearColor(0.094f, 0.110f, 0.149f, 1);  // the icon's steel
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog);
    glUniform1i(g_u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    text("OunceDash", 96 * u, w / 2.0f, h * 0.30f, w, h, 1.0f, 0.86f, 0.87f);
    text("Preparing sharp textures (first launch only)", 40 * u, w / 2.0f, h * 0.30f + 130 * u, w, h,
         0.85f, 0.87f, 0.92f);

    float bw = 900 * u, bh = 28 * u, bx = (w - bw) / 2, by = h * 0.56f;
    float frac = total ? (float)done / total : 0;
    if (writing) frac = 1;
    quad(g_white, bx - 4 * u, by - 4 * u, bx + bw + 4 * u, by + bh + 4 * u, w, h, 0.05f, 0.06f, 0.09f, 1);
    quad(g_white, bx, by, bx + bw * frac, by + bh, w, h, 0.90f, 0.20f, 0.25f, 1);

    char line[96];
    if (writing) snprintf(line, sizeof(line), "Saving the cache...");
    else snprintf(line, sizeof(line), "%zu / %zu files (%d%%)", done, total, (int)(frac * 100));
    text(line, 34 * u, w / 2.0f, by + bh + 30 * u, w, h, 0.75f, 0.78f, 0.85f);
}

// Returns false if the app was asked to close meanwhile.
bool loadscreen_run(void)
{
    if (!uhd_pack_building()) return true;
    if (!g_prog) init_gl();
    LOG("loadscreen: building the icon cache before the game starts\n");
    uint64_t t = time_ns();
    while (!uhd_pack_finished()) {
        if (!appletMainLoop()) return false;
        size_t done, total;
        bool writing;
        uhd_pack_progress(&done, &total, &writing);
        int w, h;
        display_size(&w, &h);
        draw(w, h, done, total, writing);
        display_present();
    }
    LOG("loadscreen: done in %lu s\n", (unsigned long)((time_ns() - t) / 1000000000ULL));
    return true;
}
