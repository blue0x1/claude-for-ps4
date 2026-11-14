// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Screen drawing. No console APIs here, so it also builds on a PC for previews.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "plat.h"
#include "qrcodegen.h"
#include "ui.h"



// ---- drawing ------------------------------------------------------------

static Font *msg_font(const Msg *m)
{
    return (m->role == ROLE_USER || m->role == ROLE_ASSISTANT) ? &g_body : &g_small;
}

static int msg_text_width(const Msg *m)
{
    return m->role == ROLE_USER ? TEXT_W - 2 * PAD_X : TEXT_W;
}

static void ensure_layout(Msg *m)
{
    int width = msg_text_width(m);
    if (m->line_start && m->layout_len == m->text.len && m->layout_width == width)
        return;
    free(m->line_start);
    free(m->line_len);
    m->nlines = layout_wrap(msg_font(m), m->text.data ? m->text.data : "", (int)m->text.len, width,
                            &m->line_start, &m->line_len);
    m->layout_len = m->text.len;
    m->layout_width = width;
}

static int msg_height(const Msg *m)
{
    Font *f = msg_font(m);
    int h = m->nlines * f->line_h;
    if (m->role == ROLE_USER || m->role == ROLE_ASSISTANT)
        h += g_small.line_h + 6;     // name label
    if (m->role == ROLE_USER)
        h += 2 * 16;                 // bubble padding
    return h + 28;                   // gap below
}

static void draw_text_lines(Canvas *c, const Msg *m, int x, int y, uint32_t color, const Rect *clip)
{
    Font *f = msg_font(m);
    for (int i = 0; i < m->nlines; i++, y += f->line_h) {
        if (y + f->line_h < clip->y || y > clip->y + clip->h)
            continue;
        font_draw(f, c, x, y, m->text.data + m->line_start[i], m->line_len[i], color, clip);
    }
}

void draw_center(Canvas *c, Font *f, int y, const char *s, uint32_t color)
{
    int w = font_text_width(f, s, (int)strlen(s));
    font_draw(f, c, (SCREEN_W - w) / 2, y, s, (int)strlen(s), color, NULL);
}

static void draw_confirm(Canvas *c)
{
    const char *text = g_chat.confirm_text;
    int *starts, *lens;
    int n = layout_wrap(&g_body, text, (int)strlen(text), 1100, &starts, &lens);
    int h = 120 + n * g_body.line_h + 90;
    Rect panel = { (SCREEN_W - 1240) / 2, (SCREEN_H - h) / 2, 1240, h };
    canvas_fill(c, (Rect){ panel.x - 3, panel.y - 3, panel.w + 6, panel.h + 6 }, COL_ACCENT, NULL);
    canvas_fill(c, panel, COL_PANEL, NULL);
    font_draw(&g_title, c, panel.x + 70, panel.y + 30, "Claude wants to do this:", 24, COL_ACCENT, NULL);
    int y = panel.y + 110;
    for (int i = 0; i < n; i++, y += g_body.line_h)
        font_draw(&g_body, c, panel.x + 70, y, text + starts[i], lens[i], COL_TEXT, NULL);
    font_draw(&g_small, c, panel.x + 70, panel.y + h - 60, "Cross: allow      Circle: deny", 30, COL_DIM, NULL);
    free(starts);
    free(lens);
}


static void draw_setup(Canvas *c)
{
    Rect panel = { 260, 150, SCREEN_W - 520, SCREEN_H - 300 };
    canvas_fill(c, (Rect){ panel.x - 3, panel.y - 3, panel.w + 6, panel.h + 6 }, COL_ACCENT, NULL);
    canvas_fill(c, panel, COL_PANEL, NULL);

    // QR code with the setup URL
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    int qx = panel.x + 70, qy = panel.y + 90, qsize = 0;
    if (qrcodegen_encodeText(g_setup.url, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                             qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        int n = qrcodegen_getSize(qr), quiet = 4, mod = 520 / (n + 2 * quiet);
        qsize = mod * (n + 2 * quiet);
        canvas_fill(c, (Rect){ qx, qy, qsize, qsize }, 0xFFFFFF, NULL);
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                if (qrcodegen_getModule(qr, x, y))
                    canvas_fill(c, (Rect){ qx + (x + quiet) * mod, qy + (y + quiet) * mod, mod, mod }, 0x000000, NULL);
    }

    int tx = qx + qsize + 70, y = panel.y + 90;
    font_draw(&g_title, c, tx, y, "Set up from your phone", 22, COL_ACCENT, NULL);
    y += 90;
    static const char *const lines[] = {
        "1. Connect your phone to the same Wi-Fi.",
        "2. Scan the code, or open this address:",
    };
    for (int i = 0; i < 2; i++, y += g_body.line_h + 8)
        font_draw(&g_body, c, tx, y, lines[i], (int)strlen(lines[i]), COL_TEXT, NULL);
    font_draw(&g_body, c, tx + 40, y, g_setup.url, (int)strlen(g_setup.url), COL_TOOL, NULL);
    y += g_body.line_h + 30;
    const char *l3 = "3. Type this PIN on the page:";
    font_draw(&g_body, c, tx, y, l3, (int)strlen(l3), COL_TEXT, NULL);
    y += g_body.line_h + 16;
    char pin[16];
    snprintf(pin, sizeof(pin), "%.3s %.3s", g_setup.pin, g_setup.pin + 3);
    font_draw(&g_title, c, tx + 40, y, pin, (int)strlen(pin), COL_ACCENT, NULL);
    y += g_title.line_h + 40;
    const char *l4 = "4. Enter your API key, or your PC bridge details.";
    font_draw(&g_body, c, tx, y, l4, (int)strlen(l4), COL_TEXT, NULL);
    const char *foot = "Circle: close";
    font_draw(&g_small, c, panel.x + 70, panel.y + panel.h - 70, foot, (int)strlen(foot), COL_DIM, NULL);
}

void render(Canvas *c, int frame)
{
    canvas_fill(c, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, COL_BG, NULL);

    // Header
    canvas_fill(c, (Rect){ 0, 0, SCREEN_W, HEADER_H }, COL_HEADER, NULL);
    font_draw(&g_title, c, MARGIN_X, (HEADER_H - g_title.line_h) / 2, "Claude", 6, COL_ACCENT, NULL);
    char right[160];
    if (!g_cfg_ok)
        snprintf(right, sizeof(right), "not set up");
    else if (g_cfg.bridge)
        snprintf(right, sizeof(right), "PC bridge (Claude account, chat only)");
    else
        snprintf(right, sizeof(right), "%s%s", g_cfg.model, plat_jailbroken() ? "" : "   (sandboxed)");
    int rw = font_text_width(&g_small, right, (int)strlen(right));
    font_draw(&g_small, c, SCREEN_W - MARGIN_X - rw, (HEADER_H - g_small.line_h) / 2, right,
              (int)strlen(right), COL_DIM, NULL);

    Rect view = { 0, HEADER_H + 10, SCREEN_W, SCREEN_H - HEADER_H - FOOTER_H - 20 };

    pthread_mutex_lock(&g_chat.lock);
    int total = 0;
    for (int i = 0; i < g_chat.count; i++) {
        ensure_layout(&g_chat.msgs[i]);
        total += msg_height(&g_chat.msgs[i]);
    }
    int thinking = g_chat.busy && !g_chat.got_text && g_chat.confirm != CONFIRM_PENDING;
    if (thinking)
        total += g_body.line_h + 20;

    if (!g_chat.count) {
        draw_center(c, &g_title, 380, "Claude for PS4", COL_TEXT);
        if (g_cfg_ok) {
            draw_center(c, &g_body, 470, "Press Cross to start typing.", COL_DIM);
            draw_center(c, &g_small, 530,
                        "Ask anything, or ask Claude to check your storage, find .pkg files, back up saves...",
                        COL_DIM);
        }
    }

    g_max_scroll = total > view.h ? total - view.h : 0;
    if (g_scroll > g_max_scroll)
        g_scroll = g_max_scroll;
    if (g_scroll < 0)
        g_scroll = 0;
    int y = (total > view.h) ? view.y + view.h - total + g_scroll : view.y;

    for (int i = 0; i < g_chat.count; i++) {
        Msg *m = &g_chat.msgs[i];
        int h = msg_height(m);
        if (y + h >= view.y && y <= view.y + view.h) {
            if (m->role == ROLE_USER) {
                Rect bubble = { MARGIN_X - PAD_X, y + g_small.line_h + 6, TEXT_W + 2 * PAD_X,
                                m->nlines * g_body.line_h + 32 };
                font_draw(&g_small, c, MARGIN_X, y, "You", 3, COL_DIM, &view);
                canvas_fill(c, bubble, COL_USERBG, &view);
                draw_text_lines(c, m, MARGIN_X, bubble.y + 16, COL_TEXT, &view);
            } else if (m->role == ROLE_ASSISTANT) {
                font_draw(&g_small, c, MARGIN_X, y, "Claude", 6, COL_ACCENT, &view);
                draw_text_lines(c, m, MARGIN_X, y + g_small.line_h + 6, COL_TEXT, &view);
            } else {
                draw_text_lines(c, m, MARGIN_X, y, m->role == ROLE_TOOL ? COL_TOOL : COL_NOTICE, &view);
            }
        }
        y += h;
    }
    if (thinking) {
        static const char *dots[] = { "Claude is thinking", "Claude is thinking.", "Claude is thinking..",
                                      "Claude is thinking..." };
        const char *s = dots[(frame / 20) % 4];
        font_draw(&g_body, c, MARGIN_X, y, s, (int)strlen(s), COL_DIM, &view);
    }
    pthread_mutex_unlock(&g_chat.lock);

    // Footer
    canvas_fill(c, (Rect){ 0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H }, COL_HEADER, NULL);
    const char *hint;
    if (g_setup.active)
        hint = "Circle: close phone setup";
    else if (!g_cfg_ok)
        hint = "Options: set up from phone    Triangle: reload claude.ini";
    else if (g_chat.confirm == CONFIRM_PENDING)
        hint = "Cross: allow    Circle: deny";
    else if (g_chat.busy)
        hint = "Circle: stop    Up/Down or right stick: scroll";
    else
        hint = "Cross: type    Square: new chat    Options: phone setup    Triangle: reload    Up/Down, L1/R1: scroll";
    font_draw(&g_small, c, MARGIN_X, SCREEN_H - FOOTER_H + (FOOTER_H - g_small.line_h) / 2, hint,
              (int)strlen(hint), COL_DIM, NULL);

    if (g_chat.confirm == CONFIRM_PENDING)
        draw_confirm(c);
    else if (g_setup.active)
        draw_setup(c);
}

