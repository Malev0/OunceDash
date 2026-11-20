// Cocos2dxBitmap.createTextBitmapShadowStroke: renders CCLabelTTF text with
// the console's shared system fonts (nothing is bundled) and hands the
// bitmap back through nativeInitBitmapDC (A,R,G,B bytes per pixel, which
// the game rotates into RGBA).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "port.h"

#define MAX_FONTS 6
static stbtt_fontinfo g_fonts[MAX_FONTS];
static int g_font_count;

void text_init(void)
{
    static const PlSharedFontType types[] = {
        PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified,
        PlSharedFontType_ExtChineseSimplified, PlSharedFontType_ChineseTraditional,
        PlSharedFontType_KO, PlSharedFontType_NintendoExt,
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        PlFontData fd;
        if (R_FAILED(plGetSharedFontByType(&fd, types[i]))) continue;
        if (stbtt_InitFont(&g_fonts[g_font_count], fd.address, 0))
            g_font_count++;
    }
    LOG("text: %d shared fonts\n", g_font_count);
}

static int utf8_next(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    int c = *p++;
    if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 0x3f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); p += 3; }
    else if (c >= 0xe0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f); p += 2; }
    else if (c >= 0xc0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 0x3f); p += 1; }
    *pp = (const char *)p;
    return c;
}

static int font_for(int cp, int *glyph)
{
    for (int i = 0; i < g_font_count; i++) {
        int g = stbtt_FindGlyphIndex(&g_fonts[i], cp);
        if (g) { *glyph = g; return i; }
    }
    *glyph = 0;
    return 0;
}

typedef struct { const char *start, *end; float width; } Line;

static float measure(const char *s, const char *e, float px)
{
    float w = 0;
    while (s < e) {
        int g, cp = utf8_next(&s);
        int f = font_for(cp, &g);
        float scale = stbtt_ScaleForPixelHeight(&g_fonts[f], px);
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&g_fonts[f], g, &adv, &lsb);
        w += adv * scale;
    }
    return w;
}

static int layout(const char *text, float px, int max_w, Line *lines, int max_lines)
{
    int n = 0;
    const char *p = text;
    while (*p && n < max_lines) {
        const char *ls = p, *last_space = NULL;
        while (*p && *p != '\n') {
            if (*p == ' ') last_space = p;
            const char *q = p;
            utf8_next(&q);
            if (max_w > 0 && measure(ls, q, px) > max_w && p > ls) {
                if (last_space && last_space > ls) p = last_space + 1;
                break;
            }
            p = q;
        }
        const char *le = p;
        while (le > ls && le[-1] == ' ') le--;
        lines[n].start = ls;
        lines[n].end = le;
        lines[n].width = measure(ls, le, px);
        n++;
        if (*p == '\n') p++;
    }
    if (n == 0) { lines[0].start = lines[0].end = text; lines[0].width = 0; n = 1; }
    return n;
}

// One line of white text with a dark outline, as straight-alpha RGBA, for
// the loader's own overlays.
uint8_t *text_render_rgba(const char *text, float px, int *out_w, int *out_h)
{
    if (!g_font_count) return NULL;
    int pad = (int)(px / 10) + 2;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&g_fonts[0], &asc, &desc, &gap);
    float sc0 = stbtt_ScaleForPixelHeight(&g_fonts[0], px);
    float base = asc * sc0 + pad;
    int W = (int)ceilf(measure(text, text + strlen(text), px)) + pad * 2;
    int H = (int)ceilf((asc - desc) * sc0) + pad * 2;
    uint8_t *cov = calloc((size_t)W * H, 1);
    float x = pad;
    for (const char *s = text; *s;) {
        int gi, cp = utf8_next(&s);
        int f = font_for(cp, &gi);
        float sc = stbtt_ScaleForPixelHeight(&g_fonts[f], px);
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetGlyphHMetrics(&g_fonts[f], gi, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(&g_fonts[f], gi, sc, sc, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            uint8_t *gb = malloc((size_t)gw * gh);
            stbtt_MakeGlyphBitmap(&g_fonts[f], gb, gw, gh, gw, sc, sc, gi);
            int ox = (int)floorf(x) + x0, oy = (int)floorf(base) + y0;
            for (int yy = 0; yy < gh; yy++)
                for (int xx = 0; xx < gw; xx++) {
                    int tx = ox + xx, ty = oy + yy;
                    if (tx < 0 || ty < 0 || tx >= W || ty >= H) continue;
                    int v = cov[ty * W + tx] + gb[yy * gw + xx];
                    cov[ty * W + tx] = v > 255 ? 255 : v;
                }
            free(gb);
        }
        x += adv * sc;
    }
    int r = pad - 1;
    uint8_t *out = malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++)
        for (int xx = 0; xx < W; xx++) {
            int m = 0;
            for (int dy = -r; dy <= r && m < 255; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    int sx = xx + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= W || sy >= H || dx * dx + dy * dy > r * r) continue;
                    if (cov[sy * W + sx] > m) m = cov[sy * W + sx];
                }
            float a = cov[y * W + xx] / 255.0f, o = m / 255.0f * 0.85f;
            float alpha = a + o * (1 - a);
            float lum = alpha > 0 ? a / alpha : 0; // white text over black outline
            uint8_t *p = out + ((size_t)y * W + xx) * 4;
            p[0] = p[1] = p[2] = (uint8_t)(lum * 255.0f + 0.5f);
            p[3] = (uint8_t)(alpha * 255.0f + 0.5f);
        }
    free(cov);
    *out_w = W;
    *out_h = H;
    return out;
}

static void text_create_bitmap_inner(const char *text, const char *font, int font_size,
                        float r, float g, float b, int align, int width, int height,
                        bool stroke, float sr, float sg, float sb, float stroke_size)
{
    (void)font;
    if (!g_game.nativeInitBitmapDC) return;
    float px = font_size > 0 ? (float)font_size : 20.0f;
    int pad = stroke ? (int)ceilf(stroke_size) : 0;
    Line lines[256];
    int nl = g_font_count ? layout(text, px, width, lines, 256) : 1;
    if (!g_font_count) { lines[0].start = lines[0].end = text; lines[0].width = 0; }

    float ascent_f = px * 0.8f, line_h = px * 1.2f;
    if (g_font_count) {
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&g_fonts[0], &asc, &desc, &gap);
        float sc = stbtt_ScaleForPixelHeight(&g_fonts[0], px);
        ascent_f = asc * sc;
        line_h = (asc - desc + gap) * sc;
    }
    float text_w = 0;
    for (int i = 0; i < nl; i++) if (lines[i].width > text_w) text_w = lines[i].width;
    float text_h = line_h * nl;

    int W = width > 0 ? width : (int)ceilf(text_w) + pad * 2;
    int H = height > 0 ? height : (int)ceilf(text_h) + pad * 2;
    if (W < 1) W = 1;
    if (H < 1) H = 1;

    uint8_t *cov = calloc((size_t)W * H, 1);
    int halign = align & 0x0f, valign = (align >> 4) & 0x0f;
    float y0 = valign == 1 ? pad : valign == 2 ? H - text_h - pad : (H - text_h) / 2;

    for (int li = 0; li < nl && g_font_count; li++) {
        float x = halign == 1 ? pad : halign == 2 ? W - lines[li].width - pad : (W - lines[li].width) / 2;
        float base = y0 + li * line_h + ascent_f;
        for (const char *s = lines[li].start; s < lines[li].end;) {
            int gi, cp = utf8_next(&s);
            int f = font_for(cp, &gi);
            float sc = stbtt_ScaleForPixelHeight(&g_fonts[f], px);
            int adv, lsb, x0, y0b, x1, y1;
            stbtt_GetGlyphHMetrics(&g_fonts[f], gi, &adv, &lsb);
            stbtt_GetGlyphBitmapBox(&g_fonts[f], gi, sc, sc, &x0, &y0b, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0b;
            if (gw > 0 && gh > 0) {
                uint8_t *gb = malloc((size_t)gw * gh);
                stbtt_MakeGlyphBitmap(&g_fonts[f], gb, gw, gh, gw, sc, sc, gi);
                int ox = (int)floorf(x) + x0, oy = (int)floorf(base) + y0b;
                for (int yy = 0; yy < gh; yy++) {
                    int ty = oy + yy;
                    if (ty < 0 || ty >= H) continue;
                    for (int xx = 0; xx < gw; xx++) {
                        int tx = ox + xx;
                        if (tx < 0 || tx >= W) continue;
                        int v = cov[ty * W + tx] + gb[yy * gw + xx];
                        cov[ty * W + tx] = v > 255 ? 255 : v;
                    }
                }
                free(gb);
            }
            x += adv * sc;
        }
    }

    uint8_t *outline = NULL;
    if (stroke && pad > 0) {
        outline = calloc((size_t)W * H, 1);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int m = 0;
                for (int dy = -pad; dy <= pad && m < 255; dy++)
                    for (int dx = -pad; dx <= pad; dx++) {
                        if (dx * dx + dy * dy > pad * pad) continue;
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                        if (cov[yy * W + xx] > m) m = cov[yy * W + xx];
                    }
                outline[y * W + x] = (uint8_t)m;
            }
    }

    uint8_t *px_out = malloc((size_t)W * H * 4);
    for (int i = 0; i < W * H; i++) {
        float a = cov[i] / 255.0f;
        float cr = r * a, cg = g * a, cb = b * a, ca = a;
        if (outline) {
            float oa = outline[i] / 255.0f * (1 - a);
            cr += sr * oa; cg += sg * oa; cb += sb * oa; ca += oa;
        }
        // premultiplied, A R G B byte order
        px_out[i * 4 + 0] = (uint8_t)(ca * 255.0f + 0.5f);
        px_out[i * 4 + 1] = (uint8_t)(cr * 255.0f + 0.5f);
        px_out[i * 4 + 2] = (uint8_t)(cg * 255.0f + 0.5f);
        px_out[i * 4 + 3] = (uint8_t)(cb * 255.0f + 0.5f);
    }
    jbyteArray arr = jni_new_byte_array(px_out, W * H * 4);
    g_game.nativeInitBitmapDC(g_env, jni_find_class("org/cocos2dx/lib/Cocos2dxBitmap"), W, H, arr);
    free(px_out);
    free(outline);
    free(cov);
}

// timed for the frame log (perf.c)
void text_create_bitmap(const char *text, const char *font, int font_size,
                        float r, float g, float b, int align, int width, int height,
                        bool stroke, float sr, float sg, float sb, float stroke_size)
{
    uint64_t t = time_ns();
    text_create_bitmap_inner(text, font, font_size, r, g, b, align, width, height,
                             stroke, sr, sg, sb, stroke_size);
    perf_add_text(time_ns() - t);
}
