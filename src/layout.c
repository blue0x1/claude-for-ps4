// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "layout.h"

#include <stdlib.h>

typedef struct {
    int *s, *l, n, cap;
} Lines;

static void push(Lines *L, int start, int len)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 16;
        L->s = realloc(L->s, sizeof(int) * (size_t)L->cap);
        L->l = realloc(L->l, sizeof(int) * (size_t)L->cap);
        if (!L->s || !L->l)
            abort();
    }
    L->s[L->n] = start;
    L->l[L->n] = len;
    L->n++;
}

static void wrap_paragraph(Font *f, const char *t, int start, int end, int width, Lines *L)
{
    int line = start;
    while (line < end) {
        int i = line, w = 0, last_space = -1, fit_end = line;
        while (i < end) {
            int prev = i;
            unsigned cp = utf8_next(t, end, &i);
            int cw = font_text_width(f, t + prev, i - prev);
            if (w + cw > width && fit_end > line) {
                i = prev;
                break;
            }
            w += cw;
            fit_end = i;
            if (cp == ' ')
                last_space = i;
        }
        if (i >= end) {
            push(L, line, end - line);
            return;
        }
        int brk = (last_space > line) ? last_space : fit_end;
        int len = brk - line;
        // Drop the trailing space from the shown line.
        while (len > 0 && t[line + len - 1] == ' ')
            len--;
        push(L, line, len);
        line = brk;
        while (line < end && t[line] == ' ')
            line++;
    }
}

int layout_wrap(Font *f, const char *text, int len, int width, int **starts, int **lens)
{
    Lines L = { 0 };
    int p = 0;
    while (p <= len) {
        int e = p;
        while (e < len && text[e] != '\n')
            e++;
        if (e == p)
            push(&L, p, 0);
        else
            wrap_paragraph(f, text, p, e, width, &L);
        p = e + 1;
    }
    if (!L.n)
        push(&L, 0, 0);
    *starts = L.s;
    *lens = L.l;
    return L.n;
}
