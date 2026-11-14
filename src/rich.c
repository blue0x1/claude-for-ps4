// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "rich.h"

#include <stdlib.h>
#include <string.h>

#define CODE_PAD   24
#define LIST_INDENT 48

static void add_run(Rich *r, int x, int y, int font, uint32_t color, int off, int len, const char *lit)
{
    if (r->nruns == r->cap_runs) {
        int cap = r->cap_runs ? r->cap_runs * 2 : 64;
        RichRun *n = realloc(r->runs, sizeof(RichRun) * (size_t)cap);
        if (!n)
            return;
        r->runs = n;
        r->cap_runs = cap;
    }
    r->runs[r->nruns++] = (RichRun){ x, y, (short)font, color, off, len, lit };
}

static void add_box(Rich *r, int x, int y, int w, int h, int radius, uint32_t color)
{
    if (r->nboxes == r->cap_boxes) {
        int cap = r->cap_boxes ? r->cap_boxes * 2 : 16;
        RichBox *n = realloc(r->boxes, sizeof(RichBox) * (size_t)cap);
        if (!n)
            return;
        r->boxes = n;
        r->cap_boxes = cap;
    }
    r->boxes[r->nboxes++] = (RichBox){ x, y, w, h, radius, color };
}

// Length in bytes of the longest prefix of s[0..len) that fits in width.
static int fit_bytes(Font *f, const char *s, int len, int width)
{
    int i = 0, last = 0;
    while (i < len) {
        int j = i;
        utf8_next(s, len, &j);
        if (font_text_width(f, s, j) > width)
            break;
        last = j;
        i = j;
    }
    if (!last && len) {  // always place at least one character
        int j = 0;
        utf8_next(s, len, &j);
        last = j;
    }
    return last;
}

typedef struct {
    const RichStyle *st;
    Rich            *r;
    const char      *text;
    int              x0, width;  // line start and right edge
    int              x, y, line_h;
    int              line_used;
} Flow;

static void flow_newline(Flow *f)
{
    f->y += f->line_h;
    f->x = f->x0;
    f->line_used = 0;
}

// Places one word (may end with a space). Breaks very long words.
static void flow_word(Flow *f, int off, int len, int font, uint32_t color, int code)
{
    Font *fn = f->st->font[font];
    const char *s = f->text + off;
    int w = font_text_width(fn, s, len);
    // A trailing space does not need to fit.
    int wfit = (len && s[len - 1] == ' ') ? font_text_width(fn, s, len - 1) : w;
    if (f->line_used && f->x + wfit > f->width)
        flow_newline(f);
    if (!f->line_used) {
        while (len && s[0] == ' ') {  // no leading spaces after a wrap
            s++;
            off++;
            len--;
        }
        if (!len)
            return;
        w = font_text_width(fn, s, len);
    }
    while (f->x + w > f->width && len) {
        int n = fit_bytes(fn, s, len, f->width - f->x);
        if (f->line_used && n < len && f->x > f->x0) {
            flow_newline(f);
            continue;
        }
        int pw = font_text_width(fn, s, n);
        int dy = (f->st->font[RF_TEXT]->ascent - fn->ascent);
        if (code)
            add_box(f->r, f->x - 4, f->y + dy + 2, pw + 8, fn->line_h - 2, 6, f->st->inline_bg);
        add_run(f->r, f->x, f->y + dy, font, color, off, n, NULL);
        s += n;
        off += n;
        len -= n;
        f->line_used = 1;
        flow_newline(f);
        w = font_text_width(fn, s, len);
    }
    if (!len)
        return;
    int dy = (font == RF_MONO) ? (f->st->font[RF_TEXT]->ascent - fn->ascent) : 0;
    if (font == RF_HEAD)
        dy = 0;
    if (code) {
        int tw = font_text_width(fn, s, (s[len - 1] == ' ') ? len - 1 : len);
        add_box(f->r, f->x - 4, f->y + dy + 2, tw + 8, fn->line_h - 2, 6, f->st->inline_bg);
    }
    add_run(f->r, f->x, f->y + dy, font, color, off, len, NULL);
    f->x += w;
    f->line_used = 1;
}

// Splits a segment into words and flows them.
static void flow_segment(Flow *f, int off, int len, int font, uint32_t color, int code)
{
    int i = 0;
    while (i < len) {
        int j = i;
        while (j < len && f->text[off + j] != ' ')
            j++;
        while (j < len && f->text[off + j] == ' ')
            j++;
        flow_word(f, off + i, j - i, font, color, code);
        i = j;
    }
}

static int find(const char *s, int from, int end, const char *pat)
{
    int pl = (int)strlen(pat);
    for (int i = from; i + pl <= end; i++)
        if (!memcmp(s + i, pat, (size_t)pl))
            return i;
    return -1;
}

// Inline Markdown for one source line: **bold** and `code`.
static void flow_inline(Flow *f, int off, int len, int base_font, uint32_t color)
{
    const char *t = f->text;
    int end = off + len, i = off, seg = off;
    int bold_font = base_font == RF_TEXT ? RF_BOLD : base_font;
    while (i < end) {
        if (t[i] == '`') {
            int close = find(t, i + 1, end, "`");
            if (close > i + 1) {
                if (i > seg)
                    flow_segment(f, seg, i - seg, base_font, color, 0);
                flow_segment(f, i + 1, close - i - 1, RF_MONO, f->st->code_text, 1);
                i = seg = close + 1;
                continue;
            }
        } else if (t[i] == '*' && i + 1 < end && t[i + 1] == '*') {
            int close = find(t, i + 2, end, "**");
            if (close > i + 2) {
                if (i > seg)
                    flow_segment(f, seg, i - seg, base_font, color, 0);
                flow_segment(f, i + 2, close - i - 2, bold_font, color, 0);
                i = seg = close + 2;
                continue;
            }
        }
        i++;
    }
    if (end > seg)
        flow_segment(f, seg, end - seg, base_font, color, 0);
}

static int starts_with(const char *s, int len, const char *p)
{
    int pl = (int)strlen(p);
    return len >= pl && !memcmp(s, p, (size_t)pl);
}

Rich *rich_layout(const RichStyle *st, const char *text, int len, int width)
{
    Rich *r = calloc(1, sizeof(Rich));
    if (!r)
        return NULL;
    Font *body = st->font[RF_TEXT], *mono = st->font[RF_MONO], *head = st->font[RF_HEAD];
    int y = 0, in_code = 0, code_top = 0, prev_blank = 1, first = 1;
    int pos = 0;
    while (pos <= len) {
        int e = pos;
        while (e < len && text[e] != '\n')
            e++;
        if (pos == len && pos > 0 && text[len - 1] == '\n' && !in_code)
            break;
        const char *ln = text + pos;
        int ll = e - pos;
        if (ll && ln[ll - 1] == '\r')
            ll--;
        int ind = 0;
        while (ind < ll && ln[ind] == ' ')
            ind++;
        const char *tl = ln + ind;
        int tll = ll - ind;

        if (starts_with(tl, tll, "```")) {
            if (!in_code) {
                in_code = 1;
                if (!first)
                    y += body->line_h / 3;
                code_top = y;
                y += CODE_PAD;
            } else {
                in_code = 0;
                y += CODE_PAD;
                add_box(r, 0, code_top, width, y - code_top, 14, st->code_bg);
                y += body->line_h / 3;
            }
            first = 0;
            prev_blank = 0;
        } else if (in_code) {
            int i = 0;
            if (!ll)
                y += mono->line_h;
            while (i < ll) {
                int n = fit_bytes(mono, ln + i, ll - i, width - 2 * CODE_PAD);
                add_run(r, CODE_PAD, y, RF_MONO, st->code_text, pos + i, n, NULL);
                i += n;
                y += mono->line_h;
            }
        } else if (!tll) {
            if (!prev_blank)
                y += body->line_h / 2;
            prev_blank = 1;
        } else {
            Flow f = { st, r, text, 0, width, 0, y, body->line_h, 0 };
            int hashes = 0;
            while (hashes < tll && hashes < 6 && tl[hashes] == '#')
                hashes++;
            if (hashes && hashes < tll && tl[hashes] == ' ') {
                if (!first)
                    y += body->line_h / 3;
                f.y = y;
                int font = hashes <= 2 ? RF_HEAD : RF_BOLD;
                f.line_h = st->font[font]->line_h;
                flow_inline(&f, pos + ind + hashes + 1, tll - hashes - 1, font, st->text);
                y = f.y + f.line_h + 6;
                (void)head;
            } else if ((tll == 3 || (tll > 3 && tl[3] == ' ')) &&
                       (starts_with(tl, tll, "---") || starts_with(tl, tll, "***") || starts_with(tl, tll, "___"))) {
                add_box(r, 0, y + body->line_h / 2, width, 2, 0, st->rule);
                y += body->line_h;
            } else if (tll >= 2 && (tl[0] == '-' || tl[0] == '*' || tl[0] == '+') && tl[1] == ' ') {
                int level = ind / 2 > 3 ? 3 : ind / 2;
                int x0 = LIST_INDENT * (level + 1);
                add_run(r, x0 - 34, y, RF_TEXT, st->dim, 0, 0, level ? "\xE2\x97\xA6" : "\xE2\x80\xA2");
                f.x0 = f.x = x0;
                flow_inline(&f, pos + ind + 2, tll - 2, RF_TEXT, st->text);
                y = f.y + f.line_h;
            } else if (tll >= 3 && tl[0] >= '0' && tl[0] <= '9') {
                int d = 0;
                while (d < tll && d < 4 && tl[d] >= '0' && tl[d] <= '9')
                    d++;
                if (d < tll - 1 && (tl[d] == '.' || tl[d] == ')') && tl[d + 1] == ' ') {
                    int level = ind / 2 > 3 ? 3 : ind / 2;
                    int x0 = LIST_INDENT * (level + 1) + 8;
                    int nw = font_text_width(body, tl, d + 1);
                    add_run(r, x0 - 14 - nw, y, RF_TEXT, st->dim, pos + ind, d + 1, NULL);
                    f.x0 = f.x = x0;
                    flow_inline(&f, pos + ind + d + 2, tll - d - 2, RF_TEXT, st->text);
                } else {
                    flow_inline(&f, pos + ind, tll, RF_TEXT, st->text);
                }
                y = f.y + f.line_h;
            } else if (tl[0] == '>') {
                int skip = (tll > 1 && tl[1] == ' ') ? 2 : 1;
                int top = y;
                f.x0 = f.x = 28;
                flow_inline(&f, pos + ind + skip, tll - skip, RF_TEXT, st->dim);
                y = f.y + f.line_h;
                add_box(r, 0, top + 4, 5, y - top - 8, 2, st->rule);
            } else {
                flow_inline(&f, pos + ind, tll, RF_TEXT, st->text);
                y = f.y + f.line_h;
            }
            first = 0;
            prev_blank = 0;
        }
        if (e >= len)
            break;
        pos = e + 1;
    }
    if (in_code) {  // still streaming the code block
        y += CODE_PAD;
        add_box(r, 0, code_top, width, y - code_top, 14, st->code_bg);
    }
    r->height = y;
    return r;
}

void rich_free(Rich *r)
{
    if (!r)
        return;
    free(r->runs);
    free(r->boxes);
    free(r);
}

void rich_draw(const Rich *r, const RichStyle *st, const char *text, Canvas *c, int x, int y, const Rect *clip)
{
    int top = clip ? clip->y : 0, bottom = clip ? clip->y + clip->h : c->h;
    for (int i = 0; i < r->nboxes; i++) {
        const RichBox *b = &r->boxes[i];
        if (y + b->y + b->h < top || y + b->y > bottom)
            continue;
        canvas_round(c, (Rect){ x + b->x, y + b->y, b->w, b->h }, b->radius, b->color, clip);
    }
    for (int i = 0; i < r->nruns; i++) {
        const RichRun *u = &r->runs[i];
        Font *f = st->font[u->font];
        if (y + u->y + f->line_h < top || y + u->y > bottom)
            continue;
        if (u->lit)
            font_draw(f, c, x + u->x, y + u->y, u->lit, (int)strlen(u->lit), u->color, clip);
        else
            font_draw(f, c, x + u->x, y + u->y, text + u->off, u->len, u->color, clip);
    }
}
