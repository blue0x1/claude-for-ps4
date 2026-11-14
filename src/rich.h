// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Lays out Claude's replies with light Markdown: headings, bold, inline code,
// code blocks, bullet and numbered lists, quotes and rules.
#pragma once
#include <stdint.h>

#include "font.h"

enum { RF_TEXT, RF_BOLD, RF_HEAD, RF_MONO, RF_COUNT };

typedef struct {
    Font    *font[RF_COUNT];
    uint32_t text, dim, code_text, code_bg, inline_bg, rule;
} RichStyle;

typedef struct {
    int         x, y;       // relative to the top left of the message
    short       font;
    uint32_t    color;
    int         off, len;   // into the message text
    const char *lit;        // or a literal (bullets)
} RichRun;

typedef struct {
    int x, y, w, h, radius;
    uint32_t color;
} RichBox;

typedef struct {
    RichRun *runs;
    int      nruns, cap_runs;
    RichBox *boxes;
    int      nboxes, cap_boxes;
    int      height;
} Rich;

// Returns NULL only when out of memory.
Rich *rich_layout(const RichStyle *st, const char *text, int len, int width);
void  rich_free(Rich *r);
// Draws at (x, y); runs outside clip are skipped.
void  rich_draw(const Rich *r, const RichStyle *st, const char *text, Canvas *c, int x, int y, const Rect *clip);
