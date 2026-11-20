// Vertex buffers that the game rewrites every frame, rotated through three
// real GL buffers (config.ini: gl_buffer_ring, default 1).
//
// Each cocos2d particle system copies its quads into its vertex buffer every
// frame (CCParticleSystemQuad::postStep: glBufferSubData of the whole
// buffer), during GD's update. That buffer was drawn in the previous frame,
// so before writing Mesa's threaded context asks the driver whether the GPU
// is done with it, and while the GPU is still on that frame the driver asks
// the system each time (a GPU sync counter read through a service call).
// With many particle systems that was 4-12% of GD's update in levels.
//
// So a buffer that gets a whole-buffer update is given two more real
// buffers, and each whole update goes to the next one: the buffer written is
// the one drawn three frames ago, which the driver already knows is idle.
// The game keeps using its own buffer name; binding it binds the current
// real buffer. Partial updates go to the current buffer. GD 2.2 uses no
// vertex array objects, buffer mapping or buffer queries, which would see
// the real buffers.
#include <stdlib.h>
#include <GLES2/gl2.h>
#include "port.h"

#define RING 3

typedef struct {
    GLuint name;              // the game's buffer name (also real[0]); 0 = empty slot
    GLuint real[RING];
    GLsizeiptr real_size[RING];
    GLsizeiptr size;          // size from the game's last glBufferData
    GLenum usage;
    int cur;
} Buf;

static Buf *g_bufs;
static size_t g_cap, g_count;
static GLuint g_bound_array;      // game name bound to GL_ARRAY_BUFFER
static uint64_t g_rotations;

static Buf *find(GLuint name)
{
    if (!g_cap || !name) return NULL;
    size_t j = (name * 2654435761u) & (g_cap - 1);
    while (g_bufs[j].name) {
        if (g_bufs[j].name == name) return &g_bufs[j];
        j = (j + 1) & (g_cap - 1);
    }
    return NULL;
}

static Buf *insert(GLuint name)
{
    Buf *b = find(name);
    if (b) return b;
    if ((g_count + 1) * 2 > g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 256;
        Buf *t = calloc(cap, sizeof(Buf));
        if (!t) return NULL;
        for (size_t i = 0; i < g_cap; i++) {
            if (!g_bufs[i].name) continue;
            size_t j = (g_bufs[i].name * 2654435761u) & (cap - 1);
            while (t[j].name) j = (j + 1) & (cap - 1);
            t[j] = g_bufs[i];
        }
        free(g_bufs);
        g_bufs = t;
        g_cap = cap;
    }
    size_t j = (name * 2654435761u) & (g_cap - 1);
    while (g_bufs[j].name) j = (j + 1) & (g_cap - 1);
    g_bufs[j] = (Buf){ .name = name };
    g_bufs[j].real[0] = name;
    g_count++;
    return &g_bufs[j];
}

// Removes an entry, re-inserting the run of entries after it.
static void erase(Buf *b)
{
    size_t j = (size_t)(b - g_bufs);
    g_bufs[j].name = 0;
    g_count--;
    for (size_t k = (j + 1) & (g_cap - 1); g_bufs[k].name; k = (k + 1) & (g_cap - 1)) {
        Buf moved = g_bufs[k];
        g_bufs[k].name = 0;
        g_count--;
        Buf *n = insert(moved.name);
        if (n) *n = moved;
    }
}

void b_glBindBuffer(GLenum target, GLuint buffer)
{
    if (target != GL_ARRAY_BUFFER) { glBindBuffer(target, buffer); return; }
    g_bound_array = buffer;
    Buf *b = find(buffer);
    glBindBuffer(target, b ? b->real[b->cur] : buffer);
}

void b_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    glBufferData(target, size, data, usage);
    if (target != GL_ARRAY_BUFFER || !g_bound_array || !g_config.gl_buffer_ring) return;
    Buf *b = insert(g_bound_array);
    if (!b) return;
    b->size = size;
    b->usage = usage;
    b->real_size[b->cur] = size;
}

void b_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    Buf *b = target == GL_ARRAY_BUFFER ? find(g_bound_array) : NULL;
    if (!b || offset != 0 || size != b->size || size <= 0) {
        glBufferSubData(target, offset, size, data);
        return;
    }
    // a whole-buffer update: write the next buffer of the ring
    int next = (b->cur + 1) % RING;
    if (!b->real[next]) glGenBuffers(1, &b->real[next]);
    b->cur = next;
    glBindBuffer(GL_ARRAY_BUFFER, b->real[next]);
    if (b->real_size[next] != size) {
        glBufferData(GL_ARRAY_BUFFER, size, data, b->usage);
        b->real_size[next] = size;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, size, data);
    }
    if (++g_rotations == 1) LOG("glbuf: rotating per-frame vertex buffers (gl_buffer_ring=1)\n");
}

void b_glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
    for (GLsizei i = 0; i < n; i++) {
        Buf *b = find(buffers[i]);
        if (!b) continue;
        for (int r = 1; r < RING; r++)
            if (b->real[r]) glDeleteBuffers(1, &b->real[r]);
        if (g_bound_array == buffers[i]) g_bound_array = 0;
        erase(b);
    }
    glDeleteBuffers(n, buffers);
}
