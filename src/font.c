// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#define STB_TRUETYPE_IMPLEMENTATION
#include "font.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct Glyph {
    unsigned cp;      // 0 = empty slot
    int      advance;
    int      w, h, xoff, yoff;
    unsigned char *bitmap;
};

#define CACHE_SIZE 4096

int font_init(Font *f, const unsigned char *data, float px)
{
    memset(f, 0, sizeof(*f));
    if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0)))
        return 0;
    f->scale = stbtt_ScaleForPixelHeight(&f->info, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    f->ascent = (int)ceilf(asc * f->scale);
    f->line_h = (int)ceilf((asc - desc + gap) * f->scale);
    f->cache = calloc(CACHE_SIZE, sizeof(Glyph));
    f->cache_size = f->cache ? CACHE_SIZE : 0;
    return 1;
}

static void render_glyph(Font *f, unsigned cp, Glyph *g)
{
    int adv, lsb;
    int gi = stbtt_FindGlyphIndex(&f->info, (int)cp);
    if (gi == 0 && cp != ' ')
        gi = stbtt_FindGlyphIndex(&f->info, 0xFFFD);
    stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
    g->cp = cp;
    g->advance = (int)lroundf(adv * f->scale);
    g->bitmap = stbtt_GetGlyphBitmap(&f->info, f->scale, f->scale, gi, &g->w, &g->h, &g->xoff, &g->yoff);
}

static Glyph *get_glyph(Font *f, unsigned cp, Glyph *tmp)
{
    if (cp == 0)
        cp = 0xFFFD;
    if (f->cache_size) {
        unsigned h = (cp * 2654435761u) & (unsigned)(f->cache_size - 1);
        for (int probe = 0; probe < 32; probe++) {
            Glyph *g = &f->cache[(h + (unsigned)probe) & (unsigned)(f->cache_size - 1)];
            if (g->cp == cp)
                return g;
            if (g->cp == 0) {
                render_glyph(f, cp, g);
                return g;
            }
        }
    }
    // Table crowded: render without caching (caller frees).
    render_glyph(f, cp, tmp);
    return tmp;
}

static void release_tmp(Glyph *g, Glyph *tmp)
{
    if (g == tmp && tmp->bitmap) {
        stbtt_FreeBitmap(tmp->bitmap, NULL);
        tmp->bitmap = NULL;
    }
}

unsigned utf8_next(const char *s, int len, int *i)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned c = p[*i];
    int n = 0;
    unsigned cp;
    if (c < 0x80) { (*i)++; return c; }
    else if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { (*i)++; return 0xFFFD; }
    if (*i + n >= len) { (*i)++; return 0xFFFD; }
    for (int k = 1; k <= n; k++) {
        if ((p[*i + k] & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }
        cp = (cp << 6) | (p[*i + k] & 0x3F);
    }
    *i += n + 1;
    return cp;
}

int font_text_width(Font *f, const char *s, int len)
{
    int w = 0, i = 0;
    Glyph tmp = {0};
    while (i < len) {
        unsigned cp = utf8_next(s, len, &i);
        if (cp == '\t')
            cp = ' ';
        Glyph *g = get_glyph(f, cp, &tmp);
        w += g->advance;
        release_tmp(g, &tmp);
    }
    return w;
}

static inline uint32_t blend(const Canvas *c, uint32_t dst, uint32_t rgb, unsigned a)
{
    unsigned dr = (dst >> c->rshift) & 0xFF, dg = (dst >> c->gshift) & 0xFF, db = (dst >> c->bshift) & 0xFF;
    unsigned sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    unsigned r = (sr * a + dr * (255 - a)) / 255;
    unsigned g = (sg * a + dg * (255 - a)) / 255;
    unsigned b = (sb * a + db * (255 - a)) / 255;
    return (dst & ~((0xFFu << c->rshift) | (0xFFu << c->gshift) | (0xFFu << c->bshift))) |
           (r << c->rshift) | (g << c->gshift) | (b << c->bshift);
}

static Rect clip_rect(const Canvas *c, const Rect *clip)
{
    Rect r = { 0, 0, c->w, c->h };
    if (clip) {
        int x0 = clip->x > 0 ? clip->x : 0, y0 = clip->y > 0 ? clip->y : 0;
        int x1 = clip->x + clip->w < c->w ? clip->x + clip->w : c->w;
        int y1 = clip->y + clip->h < c->h ? clip->y + clip->h : c->h;
        r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
    }
    return r;
}

int font_draw(Font *f, Canvas *c, int x, int y, const char *s, int len, uint32_t rgb, const Rect *clip)
{
    Rect cr = clip_rect(c, clip);
    int baseline = y + f->ascent;
    int i = 0;
    Glyph tmp = {0};
    while (i < len) {
        unsigned cp = utf8_next(s, len, &i);
        if (cp == '\t')
            cp = ' ';
        Glyph *g = get_glyph(f, cp, &tmp);
        int gx = x + g->xoff, gy = baseline + g->yoff;
        for (int row = 0; row < g->h; row++) {
            int py = gy + row;
            if (py < cr.y || py >= cr.y + cr.h)
                continue;
            uint32_t *dst = c->px + (size_t)py * (size_t)c->stride;
            const unsigned char *src = g->bitmap + row * g->w;
            for (int col = 0; col < g->w; col++) {
                int pxx = gx + col;
                unsigned a = src[col];
                if (!a || pxx < cr.x || pxx >= cr.x + cr.w)
                    continue;
                dst[pxx] = blend(c, dst[pxx], rgb, a);
            }
        }
        x += g->advance;
        release_tmp(g, &tmp);
    }
    return x;
}

void canvas_fill(Canvas *c, Rect r, uint32_t rgb, const Rect *clip)
{
    Rect cr = clip_rect(c, clip);
    int x0 = r.x > cr.x ? r.x : cr.x, y0 = r.y > cr.y ? r.y : cr.y;
    int x1 = r.x + r.w < cr.x + cr.w ? r.x + r.w : cr.x + cr.w;
    int y1 = r.y + r.h < cr.y + cr.h ? r.y + r.h : cr.y + cr.h;
    uint32_t v = c->amask | (((rgb >> 16) & 0xFF) << c->rshift) | (((rgb >> 8) & 0xFF) << c->gshift) |
                 ((rgb & 0xFF) << c->bshift);
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (size_t)y * (size_t)c->stride;
        for (int x = x0; x < x1; x++)
            row[x] = v;
    }
}

// Rounded rectangle with smooth corners.
void canvas_round(Canvas *c, Rect r, int radius, uint32_t rgb, const Rect *clip)
{
    if (radius * 2 > r.w)
        radius = r.w / 2;
    if (radius * 2 > r.h)
        radius = r.h / 2;
    if (radius <= 0) {
        canvas_fill(c, r, rgb, clip);
        return;
    }
    // Middle band and the side strips between the corners.
    canvas_fill(c, (Rect){ r.x, r.y + radius, r.w, r.h - 2 * radius }, rgb, clip);
    canvas_fill(c, (Rect){ r.x + radius, r.y, r.w - 2 * radius, radius }, rgb, clip);
    canvas_fill(c, (Rect){ r.x + radius, r.y + r.h - radius, r.w - 2 * radius, radius }, rgb, clip);
    Rect cr = clip_rect(c, clip);
    float rr = (float)radius;
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            // Distance from the corner circle centre, sampled at the pixel centre.
            float fx = rr - (float)dx - 0.5f, fy = rr - (float)dy - 0.5f;
            float d = sqrtf(fx * fx + fy * fy);
            float cov = rr - d + 0.5f;
            if (cov <= 0.f)
                continue;
            unsigned a = cov >= 1.f ? 255u : (unsigned)(cov * 255.f);
            int xs[2] = { r.x + dx, r.x + r.w - 1 - dx };
            int ys[2] = { r.y + dy, r.y + r.h - 1 - dy };
            for (int k = 0; k < 4; k++) {
                int px = xs[k & 1], py = ys[k >> 1];
                if (px < cr.x || px >= cr.x + cr.w || py < cr.y || py >= cr.y + cr.h)
                    continue;
                uint32_t *p = c->px + (size_t)py * (size_t)c->stride + px;
                *p = blend(c, *p, rgb, a);
            }
        }
    }
}

// Rounded outline: outer shape in border colour, inner shape in fill colour.
void canvas_round_border(Canvas *c, Rect r, int radius, int width, uint32_t border, uint32_t fill, const Rect *clip)
{
    canvas_round(c, r, radius, border, clip);
    canvas_round(c, (Rect){ r.x + width, r.y + width, r.w - 2 * width, r.h - 2 * width },
                 radius > width ? radius - width : 0, fill, clip);
}

// Darkens an area (used behind dialogs). keep is 0..255 of the original brightness.
void canvas_dim(Canvas *c, Rect r, unsigned keep)
{
    Rect cr = clip_rect(c, &r);
    for (int y = cr.y; y < cr.y + cr.h; y++) {
        uint32_t *row = c->px + (size_t)y * (size_t)c->stride;
        for (int x = cr.x; x < cr.x + cr.w; x++)
            row[x] = blend(c, row[x], 0x000000, 255u - keep);
    }
}
