// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// TrueType text drawing into a 32-bit framebuffer (stb_truetype).
#pragma once
#include <stdint.h>

#include "stb_truetype.h"

typedef struct {
    uint32_t *px;
    int w, h, stride;          // stride in pixels
    int rshift, gshift, bshift;
    uint32_t amask;            // set on every filled pixel
} Canvas;

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct Glyph Glyph;

typedef struct {
    stbtt_fontinfo info;
    float  scale;
    int    ascent;      // pixels
    int    line_h;      // pixels
    Glyph *cache;       // open addressing table
    int    cache_size;
} Font;

// data must stay alive while the font is used.
int  font_init(Font *f, const unsigned char *data, float px);
int  font_text_width(Font *f, const char *s, int len);
// Draws one line of text with its top at y. Returns the x after the text.
int  font_draw(Font *f, Canvas *c, int x, int y, const char *s, int len, uint32_t rgb, const Rect *clip);

void canvas_fill(Canvas *c, Rect r, uint32_t rgb, const Rect *clip);
void canvas_round(Canvas *c, Rect r, int radius, uint32_t rgb, const Rect *clip);
void canvas_round_border(Canvas *c, Rect r, int radius, int width, uint32_t border, uint32_t fill, const Rect *clip);
void canvas_dim(Canvas *c, Rect r, unsigned keep);
// Draws an n x n alpha mask scaled to r, in one colour.
void canvas_mask(Canvas *c, Rect r, const unsigned char *mask, int n, uint32_t rgb, const Rect *clip);

// UTF-8 decode; advances *i. Invalid bytes decode as U+FFFD.
unsigned utf8_next(const char *s, int len, int *i);
