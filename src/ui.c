// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Screen drawing. No console APIs here, so it also builds on a PC for previews.
// Layout: a sidebar on the left, the chat column in the middle and a message
// box at the bottom, in a warm dark theme.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "layout.h"
#include "plat.h"
#include "qrcodegen.h"
#include "rich.h"
#include "ui.h"
#include "version.h"

Picker g_picker;

static RichStyle g_rich;

static void free_layout(Msg *m)
{
    rich_free(m->rich);
    m->rich = NULL;
}

void ui_init(void)
{
    g_rich.font[RF_TEXT] = &g_serif;
    g_rich.font[RF_BOLD] = &g_serif_b;
    g_rich.font[RF_HEAD] = &g_head;
    g_rich.font[RF_MONO] = &g_mono;
    g_rich.text = COL_TEXT;
    g_rich.dim = COL_DIM;
    g_rich.code_text = COL_CODE;
    g_rich.code_bg = COL_CODEBG;
    g_rich.inline_bg = COL_INLINEBG;
    g_rich.rule = COL_LINE;
    chat_free_layout = free_layout;
}

// ---- model and effort picker ---------------------------------------------

static const char *const MODEL_DESC[] = {
    "Strong at complex work. Default.", "Fast and capable", "Fastest, lowest cost",
    "Most capable, slower, costs more", "Previous Opus",
};
static const char *const BRIDGE_DESC[] = {
    "Whatever Claude Code uses", "Latest Opus in Claude Code", "Latest Sonnet in Claude Code",
    "Latest Haiku in Claude Code", "Latest Fable in Claude Code",
};
static const char *const EFFORT_NAMES[] = { "Default", "Low", "Medium", "High", "Extra high", "Max" };
static const char *const EFFORT_DESC[] = {
    "Model's own setting", "Quick answers", "Balanced", "Thinks more", "For hard problems", "Deepest, slowest",
};

static const char *const *picker_list(int col)
{
    if (col == 1)
        return CONFIG_EFFORTS;
    return g_cfg.bridge ? CONFIG_BRIDGE_MODELS : CONFIG_MODELS;
}

int picker_count(int col)
{
    const char *const *l = picker_list(col);
    int n = 0;
    while (l[n])
        n++;
    return n;
}

const char *picker_value(int col, int row)
{
    return picker_list(col)[row];
}

int picker_current(int col)
{
    const char *cur = col == 1 ? g_cfg.effort : config_active_model(&g_cfg);
    const char *const *l = picker_list(col);
    for (int i = 0; l[i]; i++)
        if (!strcmp(l[i], cur))
            return i;
    return -1;
}

const char *effort_name(const char *e)
{
    for (int i = 0; CONFIG_EFFORTS[i]; i++)
        if (!strcmp(e, CONFIG_EFFORTS[i]))
            return EFFORT_NAMES[i];
    return e;
}

static const char *bridge_model_name(const char *m)
{
    static const char *const names[] = { "PC default", "Opus", "Sonnet", "Haiku", "Fable" };
    for (int i = 0; CONFIG_BRIDGE_MODELS[i]; i++)
        if (!strcmp(m, CONFIG_BRIDGE_MODELS[i]))
            return names[i];
    return m;
}

static const char *active_model_name(void)
{
    return g_cfg.bridge ? bridge_model_name(g_cfg.bridge_model) : config_model_name(g_cfg.model);
}

// ---- small drawing helpers ------------------------------------------------

static int text_w(Font *f, const char *s)
{
    return font_text_width(f, s, (int)strlen(s));
}

static void text(Canvas *c, Font *f, int x, int y, const char *s, uint32_t col)
{
    font_draw(f, c, x, y, s, (int)strlen(s), col, NULL);
}

void draw_center(Canvas *c, Font *f, int y, const char *s, uint32_t color)
{
    text(c, f, (SCREEN_W - text_w(f, s)) / 2, y, s, color);
}

static void line(Canvas *c, int x0, int y0, int x1, int y1, int thick, uint32_t col)
{
    int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (x1 - x0) * i / (steps ? steps : 1), y = y0 + (y1 - y0) * i / (steps ? steps : 1);
        canvas_round(c, (Rect){ x - thick / 2, y - thick / 2, thick, thick }, thick / 2, col, NULL);
    }
}

// Controller face buttons drawn as shapes, so no font glyphs are needed.
enum { BTN_CROSS, BTN_CIRCLE, BTN_SQUARE, BTN_TRIANGLE };

static void draw_button(Canvas *c, int kind, int x, int y, int s)
{
    canvas_round(c, (Rect){ x, y, s, s }, s / 2, 0x45443F, NULL);
    int m = s / 4, t = s / 12 > 2 ? s / 12 : 2;
    switch (kind) {
    case BTN_CROSS:
        line(c, x + m, y + m, x + s - m, y + s - m, t, 0x8CB4E8);
        line(c, x + s - m, y + m, x + m, y + s - m, t, 0x8CB4E8);
        break;
    case BTN_CIRCLE:
        canvas_round_border(c, (Rect){ x + m, y + m, s - 2 * m, s - 2 * m }, s, t, 0xE8828A, 0x45443F, NULL);
        break;
    case BTN_SQUARE:
        canvas_round_border(c, (Rect){ x + m + 1, y + m + 1, s - 2 * m - 2, s - 2 * m - 2 }, 2, t, 0xD99AC8, 0x45443F,
                            NULL);
        break;
    case BTN_TRIANGLE:
        line(c, x + s / 2, y + m, x + s - m, y + s - m - 1, t, 0x7FCBB0);
        line(c, x + s - m, y + s - m - 1, x + m, y + s - m - 1, t, 0x7FCBB0);
        line(c, x + m, y + s - m - 1, x + s / 2, y + m, t, 0x7FCBB0);
        break;
    }
}

// A text key cap such as R2 or OPTIONS.
static int draw_key(Canvas *c, int x, int y, const char *label)
{
    int w = text_w(&g_tiny, label) + 20;
    canvas_round(c, (Rect){ x, y, w, g_tiny.line_h + 6 }, 8, 0x45443F, NULL);
    text(c, &g_tiny, x + 10, y + 3, label, COL_TEXT);
    return w;
}

// The app mark: an orange tile with a speech bubble.
static void draw_mark(Canvas *c, int x, int y, int s)
{
    canvas_round(c, (Rect){ x, y, s, s }, s / 4, COL_ACCENT, NULL);
    int bw = s * 60 / 100, bh = s * 42 / 100, bx = x + (s - bw) / 2, by = y + s * 24 / 100;
    canvas_round(c, (Rect){ bx, by, bw, bh }, bh / 3, 0xFFF7F0, NULL);
    for (int i = 0; i < s / 7; i++)  // tail
        canvas_fill(c, (Rect){ bx + bw / 5, by + bh + i, s / 7 - i, 1 }, 0xFFF7F0, NULL);
    int d = s / 11 > 2 ? s / 11 : 2;
    for (int k = 0; k < 3; k++)
        canvas_round(c, (Rect){ bx + bw / 2 - 2 * d - d / 2 + k * 2 * d, by + bh / 2 - d / 2, d, d }, d / 2,
                     COL_ACCENT, NULL);
}

// ---- messages -------------------------------------------------------------

#define USER_MAX_W 860
#define USER_PAD_X 28
#define USER_PAD_Y 18
#define MSG_GAP    36

static void ensure_layout(Msg *m)
{
    if (m->role == ROLE_ASSISTANT) {
        if (m->rich && m->rich_len == m->text.len && m->rich_width == COL_W)
            return;
        rich_free(m->rich);
        m->rich = rich_layout(&g_rich, m->text.data ? m->text.data : "", (int)m->text.len, COL_W);
        m->rich_len = m->text.len;
        m->rich_width = COL_W;
        return;
    }
    Font *f = m->role == ROLE_USER ? &g_body : &g_small;
    int width = m->role == ROLE_USER ? USER_MAX_W : COL_W - 60;
    if (m->line_start && m->layout_len == m->text.len && m->layout_width == width)
        return;
    free(m->line_start);
    free(m->line_len);
    m->nlines = layout_wrap(f, m->text.data ? m->text.data : "", (int)m->text.len, width, &m->line_start,
                            &m->line_len);
    m->layout_len = m->text.len;
    m->layout_width = width;
}

static int msg_height(const Msg *m)
{
    switch (m->role) {
    case ROLE_USER:
        return m->nlines * g_body.line_h + 2 * USER_PAD_Y + MSG_GAP;
    case ROLE_ASSISTANT:
        return (m->rich ? ((Rich *)m->rich)->height : 0) + MSG_GAP;
    default:
        return m->nlines * g_small.line_h + 20 + 16;
    }
}

static void draw_lines(Canvas *c, Font *f, const Msg *m, int x, int y, uint32_t col, const Rect *clip)
{
    for (int i = 0; i < m->nlines; i++, y += f->line_h) {
        if (y + f->line_h < clip->y || y > clip->y + clip->h)
            continue;
        font_draw(f, c, x, y, m->text.data + m->line_start[i], m->line_len[i], col, clip);
    }
}

static void draw_msg(Canvas *c, const Msg *m, int y, const Rect *view, int last_reply)
{
    if (m->role == ROLE_USER) {
        int w = 0;
        for (int i = 0; i < m->nlines; i++) {
            int lw = font_text_width(&g_body, m->text.data + m->line_start[i], m->line_len[i]);
            if (lw > w)
                w = lw;
        }
        int bw = w + 2 * USER_PAD_X, bh = m->nlines * g_body.line_h + 2 * USER_PAD_Y;
        int bx = COL_X + COL_W - bw;
        canvas_round(c, (Rect){ bx, y, bw, bh }, 24, COL_USERBG, view);
        draw_lines(c, &g_body, m, bx + USER_PAD_X, y + USER_PAD_Y, COL_TEXT, view);
    } else if (m->role == ROLE_ASSISTANT) {
        if (m->rich)
            rich_draw((Rich *)m->rich, &g_rich, m->text.data ? m->text.data : "", c, COL_X, y, view);
        if (last_reply && m->rich && ((Rich *)m->rich)->height > 0) {
            int my = y + ((Rich *)m->rich)->height + 6;
            if (my > view->y && my + 30 < view->y + view->h)
                draw_mark(c, COL_X, my, 30);
        }
    } else {
        int h = m->nlines * g_small.line_h + 20;
        uint32_t col = m->role == ROLE_TOOL ? COL_TOOL : COL_NOTICE;
        int w = 0;
        for (int i = 0; i < m->nlines; i++) {
            int lw = font_text_width(&g_small, m->text.data + m->line_start[i], m->line_len[i]);
            if (lw > w)
                w = lw;
        }
        if (m->role == ROLE_TOOL) {
            canvas_round(c, (Rect){ COL_X, y, w + 64, h }, 14, COL_TOOLBG, view);
            canvas_round(c, (Rect){ COL_X + 18, y + h / 2 - 5, 10, 10 }, 5, COL_ACCENT, view);
            draw_lines(c, &g_small, m, COL_X + 42, y + 10, col, view);
        } else {
            canvas_fill(c, (Rect){ COL_X, y + 4, 4, h - 8 }, col, view);
            draw_lines(c, &g_small, m, COL_X + 22, y + 10, col, view);
        }
    }
}

// ---- sidebar ----------------------------------------------------------------

static void side_row(Canvas *c, int y, int icon, const char *label, const char *key, int button)
{
    int x = 32;
    canvas_round(c, (Rect){ x, y, SIDEBAR_W - 64, 60 }, 14, COL_SIDEROW, NULL);
    int lx;
    if (button >= 0) {
        draw_button(c, button, x + 14, y + 12, 36);
        lx = x + 66;
    } else {
        lx = x + 12 + draw_key(c, x + 12, y + 14, key) + 16;
    }
    (void)icon;
    text(c, &g_small, lx, y + (60 - g_small.line_h) / 2, label, COL_TEXT);
}

static void draw_sidebar(Canvas *c)
{
    canvas_fill(c, (Rect){ 0, 0, SIDEBAR_W, SCREEN_H }, COL_SIDEBAR, NULL);
    canvas_fill(c, (Rect){ SIDEBAR_W - 1, 0, 1, SCREEN_H }, COL_LINE, NULL);
    draw_mark(c, 36, 40, 56);
    text(c, &g_head, 110, 40, "Claude", COL_TEXT);
    text(c, &g_tiny, 112, 40 + g_head.line_h - 4, "for PlayStation 4", COL_DIM);

    int y = 150;
    side_row(c, y, 0, "New chat", NULL, BTN_SQUARE);
    y += 72;
    if (g_cfg_ok) {
        side_row(c, y, 0, "Model and effort", "R2", -1);
        y += 72;
    }
    side_row(c, y, 0, "Use your phone", "OPTIONS", -1);
    y += 72;
    side_row(c, y, 0, "Reload settings", NULL, BTN_TRIANGLE);
    y += 100;

    text(c, &g_tiny, 40, y, "CONNECTION", COL_FAINT);
    y += g_tiny.line_h + 12;
    char buf[200];
    if (!g_cfg_ok) {
        text(c, &g_small, 40, y, "Not set up yet", COL_NOTICE);
        y += g_small.line_h + 8;
    } else {
        text(c, &g_small, 40, y, g_cfg.bridge ? "PC bridge (Claude account)" : "API key", COL_TEXT);
        y += g_small.line_h + 8;
        snprintf(buf, sizeof(buf), "%s, %s effort", active_model_name(), effort_name(g_cfg.effort));
        text(c, &g_small, 40, y, buf, COL_DIM);
        y += g_small.line_h + 8;
        const char *tools = g_cfg.bridge ? "Console tools off (chat only)"
                          : plat_jailbroken() ? "Console tools on" : "Tools limited (sandboxed)";
        text(c, &g_small, 40, y, tools, COL_DIM);
        y += g_small.line_h + 8;
    }
    if (g_setup.active && g_setup.url[0]) {
        snprintf(buf, sizeof(buf), "Phone: %s", g_setup.url + 7);  // without http://
        size_t n = strlen(buf);
        if (n && buf[n - 1] == '/')
            buf[n - 1] = 0;
        text(c, &g_small, 40, y, buf, COL_DIM);
    }

    int by = SCREEN_H - 150;
    canvas_fill(c, (Rect){ 32, by - 20, SIDEBAR_W - 64, 1 }, COL_LINE, NULL);
    text(c, &g_tiny, 40, by, "Up/Down, L1/R1, right stick: scroll", COL_FAINT);
    text(c, &g_tiny, 40, by + g_tiny.line_h + 6, "Circle: stop a reply", COL_FAINT);
    text(c, &g_tiny, 40, by + 2 * (g_tiny.line_h + 6), "Version " APP_VERSION, COL_FAINT);
}

// ---- composer ---------------------------------------------------------------

static void draw_composer(Canvas *c, int empty_chat)
{
    Rect r = { COMP_X, COMP_Y, COMP_W, COMP_H };
    canvas_round_border(c, r, 26, 2, COL_COMPBORDER, COL_COMPOSER, NULL);
    const char *ph;
    int btn = BTN_CROSS;
    if (!g_cfg_ok) {
        ph = "Set up first: press OPTIONS and use your phone";
        btn = -1;
    } else if (g_chat.busy) {
        ph = "Claude is replying. Press Circle to stop.";
        btn = BTN_CIRCLE;
    } else {
        ph = empty_chat ? "How can I help you today?" : "Reply to Claude...";
    }
    int tx = r.x + 30;
    if (btn >= 0) {
        draw_button(c, btn, tx, r.y + 22, 40);
        tx += 56;
    }
    text(c, &g_body, tx, r.y + 26, ph, COL_DIM);

    int by = r.y + r.h - g_small.line_h - 18;
    if (g_cfg_ok) {
        text(c, &g_tiny, r.x + 30, by + 4, g_cfg.bridge ? "Chat only" : "Tools on", COL_FAINT);
        char buf[160];
        if (g_cfg.effort[0])
            snprintf(buf, sizeof(buf), "%s  \xC2\xB7  %s effort", active_model_name(), effort_name(g_cfg.effort));
        else
            snprintf(buf, sizeof(buf), "%s", active_model_name());
        int kw = text_w(&g_tiny, "R2") + 20;
        int w = text_w(&g_small, buf);
        int x = r.x + r.w - 30 - kw - 14 - w;
        text(c, &g_small, x, by, buf, COL_DIM);
        draw_key(c, r.x + r.w - 30 - kw, by + 2, "R2");
    }
}

// ---- dialogs ----------------------------------------------------------------

static Rect card(Canvas *c, int w, int h)
{
    canvas_dim(c, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, 110);
    Rect r = { MAIN_X + (MAIN_W - w) / 2, (SCREEN_H - h) / 2, w, h };
    canvas_round_border(c, r, 28, 2, COL_COMPBORDER, COL_PANEL, NULL);
    return r;
}

static int pill(Canvas *c, int x, int y, int button, const char *label, int primary)
{
    int w = text_w(&g_body, label) + 120, h = 76;
    if (primary)
        canvas_round(c, (Rect){ x, y, w, h }, h / 2, COL_ACCENT, NULL);
    else
        canvas_round_border(c, (Rect){ x, y, w, h }, h / 2, 2, COL_COMPBORDER, COL_PANEL, NULL);
    draw_button(c, button, x + 20, y + 16, 44);
    text(c, &g_body, x + 80, y + (h - g_body.line_h) / 2, label, primary ? 0x1F1E1D : COL_TEXT);
    return w;
}

static void draw_confirm(Canvas *c)
{
    const char *t = g_chat.confirm_text;
    int *starts, *lens;
    int n = layout_wrap(&g_body, t, (int)strlen(t), 980, &starts, &lens);
    int h = 130 + n * g_body.line_h + 170 + (g_setup.active ? 40 : 0);
    Rect r = card(c, 1120, h);
    text(c, &g_head, r.x + 64, r.y + 44, "Allow this action?", COL_TEXT);
    int y = r.y + 130;
    canvas_round(c, (Rect){ r.x + 48, y - 18, r.w - 96, n * g_body.line_h + 36 }, 18, COL_SIDEBAR, NULL);
    for (int i = 0; i < n; i++, y += g_body.line_h)
        font_draw(&g_body, c, r.x + 72, y, t + starts[i], lens[i], COL_TEXT, NULL);
    free(starts);
    free(lens);
    y += 50;
    int x = r.x + 64;
    x += pill(c, x, y, BTN_CROSS, "Allow", 1) + 24;
    pill(c, x, y, BTN_CIRCLE, "Deny", 0);
    if (g_setup.active)
        text(c, &g_tiny, r.x + 64, y + 96, "You can also answer on your phone.", COL_FAINT);
}

static void draw_setup(Canvas *c)
{
    Rect r = card(c, 1360, 720);
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    int qx = r.x + 70, qy = r.y + 110, qsize = 0;
    if (qrcodegen_encodeText(g_setup.url, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                             qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        int n = qrcodegen_getSize(qr), quiet = 4, mod = 500 / (n + 2 * quiet);
        qsize = mod * (n + 2 * quiet);
        canvas_round(c, (Rect){ qx, qy, qsize, qsize }, 18, 0xFFFFFF, NULL);
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                if (qrcodegen_getModule(qr, x, y))
                    canvas_fill(c, (Rect){ qx + (x + quiet) * mod, qy + (y + quiet) * mod, mod, mod }, 0x141413, NULL);
    }
    int tx = qx + qsize + 70, y = r.y + 110;
    text(c, &g_head, tx, y, "Use your phone", COL_TEXT);
    y += g_head.line_h + 30;
    text(c, &g_body, tx, y, "1. Join the same Wi-Fi as the PS4.", COL_TEXT);
    y += g_body.line_h + 12;
    text(c, &g_body, tx, y, "2. Scan the code, or open:", COL_TEXT);
    y += g_body.line_h + 8;
    text(c, &g_body, tx + 40, y, g_setup.url, COL_ACCENT);
    y += g_body.line_h + 26;
    text(c, &g_body, tx, y, "3. Sign in with this PIN:", COL_TEXT);
    y += g_body.line_h + 14;
    char pin[16];
    snprintf(pin, sizeof(pin), "%.3s %.3s", g_setup.pin, g_setup.pin + 3);
    canvas_round(c, (Rect){ tx + 30, y - 8, text_w(&g_hero, pin) + 60, g_hero.line_h + 16 }, 16, COL_SIDEBAR, NULL);
    text(c, &g_hero, tx + 60, y, pin, COL_TEXT);
    y += g_hero.line_h + 36;
    text(c, &g_small, tx, y,
         g_cfg_ok ? "Then chat from the phone or change settings." : "Then enter your API key or PC bridge details.",
         COL_DIM);
    int fy = r.y + r.h - 80;
    draw_button(c, BTN_CIRCLE, r.x + 70, fy, 40);
    text(c, &g_small, r.x + 124, fy + 4, "Close (the page keeps working)", COL_DIM);
}

static void draw_picker(Canvas *c)
{
    int rows0 = picker_count(0), rows1 = picker_count(1);
    int rh0 = 92, rh1 = 70;
    int h0 = rows0 * rh0, h1 = rows1 * rh1;
    int h = 110 + (h0 > h1 ? h0 : h1) + 90;
    int w = 1200;
    canvas_dim(c, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, 140);
    Rect r = { COMP_X + COMP_W - w, COMP_Y - h - 16, w, h };
    canvas_round_border(c, r, 24, 2, COL_COMPBORDER, COL_PANEL, NULL);
    int colw = (w - 3 * 32) / 2;
    for (int col = 0; col < 2; col++) {
        int x = r.x + 32 + col * (colw + 32), y = r.y + 36;
        text(c, &g_tiny, x + 16, y, col ? "EFFORT" : (g_cfg.bridge ? "MODEL (CLAUDE CODE)" : "MODEL"), COL_FAINT);
        y += 50;
        int n = col ? rows1 : rows0, rh = col ? rh1 : rh0, cur = picker_current(col);
        for (int i = 0; i < n; i++, y += rh) {
            int sel = g_picker.col == col && g_picker.row == i;
            if (sel)
                canvas_round(c, (Rect){ x, y, colw, rh - 8 }, 16, COL_SIDEROW, NULL);
            const char *name, *desc;
            if (col) {
                name = EFFORT_NAMES[i];
                desc = EFFORT_DESC[i];
            } else if (g_cfg.bridge) {
                name = bridge_model_name(CONFIG_BRIDGE_MODELS[i]);
                desc = BRIDGE_DESC[i];
            } else {
                name = CONFIG_MODEL_NAMES[i];
                desc = MODEL_DESC[i];
            }
            text(c, &g_body, x + 20, y + 8, name, COL_TEXT);
            if (col) {
                text(c, &g_tiny, x + 24 + text_w(&g_body, name) + 16, y + 8 + g_body.ascent - g_tiny.ascent, desc,
                     COL_DIM);
            } else {
                text(c, &g_tiny, x + 20, y + 8 + g_body.line_h, desc, COL_DIM);
            }
            if (i == cur) {  // check mark
                int cx = x + colw - 50, cy = y + (rh - 8) / 2;
                line(c, cx, cy, cx + 9, cy + 9, 4, COL_ACCENT);
                line(c, cx + 9, cy + 9, cx + 26, cy - 10, 4, COL_ACCENT);
            }
        }
    }
    int fy = r.y + r.h - 64;
    int x = r.x + 48;
    draw_button(c, BTN_CROSS, x, fy, 38);
    text(c, &g_tiny, x + 50, fy + 6, "Choose", COL_DIM);
    x += 190;
    draw_button(c, BTN_CIRCLE, x, fy, 38);
    text(c, &g_tiny, x + 50, fy + 6, "Close", COL_DIM);
    x += 170;
    text(c, &g_tiny, x, fy + 6, "Left/Right: model or effort", COL_DIM);
    if (g_cfg.bridge)
        text(c, &g_tiny, r.x + w - 48 - text_w(&g_tiny, "Uses your Claude plan's limits"), fy + 6,
             "Uses your Claude plan's limits", COL_FAINT);
}

// ---- main render ------------------------------------------------------------

static const char *greeting(void)
{
    return "What can I help with?";
}

static void draw_empty(Canvas *c)
{
    const char *g = greeting();
    int gw = text_w(&g_hero, g), total = 84 + 24 + gw;
    int x = MAIN_X + (MAIN_W - total) / 2, y = 330;
    draw_mark(c, x, y + (g_hero.line_h - 84) / 2, 84);
    text(c, &g_hero, x + 108, y, g, COL_TEXT);
    y += g_hero.line_h + 60;
    static const char *const chips[] = { "How much space is left?", "Find .pkg files on my USB",
                                         "What is in my latest screenshot?" };
    static const char *const bridge_chips[] = { "Explain a game mechanic", "Plan a weekend gaming session",
                                                "Write a short story" };
    const char *const *list = (g_cfg_ok && g_cfg.bridge) ? bridge_chips : chips;
    int cw[3], sum = 0;
    for (int i = 0; i < 3; i++) {
        cw[i] = text_w(&g_small, list[i]) + 48;
        sum += cw[i];
    }
    sum += 2 * 20;
    x = MAIN_X + (MAIN_W - sum) / 2;
    for (int i = 0; i < 3; i++) {
        canvas_round_border(c, (Rect){ x, y, cw[i], 60 }, 30, 2, COL_COMPBORDER, COL_BG, NULL);
        text(c, &g_small, x + 24, y + (60 - g_small.line_h) / 2, list[i], COL_DIM);
        x += cw[i] + 20;
    }
    if (g_cfg_ok)
        text(c, &g_tiny, MAIN_X + (MAIN_W - text_w(&g_tiny, "Press Cross and ask anything.")) / 2, y + 100,
             "Press Cross and ask anything.", COL_FAINT);
}

static void chat_title(char *out, size_t n)
{
    snprintf(out, n, "New chat");
    for (int i = 0; i < g_chat.count; i++) {
        const Msg *m = &g_chat.msgs[i];
        if (m->role != ROLE_USER || !m->text.data)
            continue;
        size_t k = 0;
        const char *s = m->text.data;
        while (s[k] && s[k] != '\n' && k < 60)
            k++;
        if (k >= 60) {  // cut at a word
            size_t cut = k;
            while (cut > 30 && s[cut] != ' ')
                cut--;
            snprintf(out, n, "%.*s...", (int)cut, s);
        } else {
            snprintf(out, n, "%.*s", (int)k, s);
        }
        return;
    }
}

void render(Canvas *c, int frame)
{
    canvas_fill(c, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, COL_BG, NULL);
    draw_sidebar(c);

    pthread_mutex_lock(&g_chat.lock);
    char title[128];
    chat_title(title, sizeof(title));
    int tw = text_w(&g_small, title);
    if (g_chat.count)
        text(c, &g_small, MAIN_X + (MAIN_W - tw) / 2, 26, title, COL_DIM);

    Rect view = { MAIN_X, TOP_H, MAIN_W, COMP_Y - 24 - TOP_H };
    int total = 0, last_reply = -1;
    for (int i = 0; i < g_chat.count; i++) {
        ensure_layout(&g_chat.msgs[i]);
        total += msg_height(&g_chat.msgs[i]);
        if (g_chat.msgs[i].role == ROLE_ASSISTANT)
            last_reply = i;
    }
    int thinking = g_chat.busy && !g_chat.got_text && g_chat.confirm != CONFIRM_PENDING;
    if (thinking)
        total += g_body.line_h + 20;
    if (!g_chat.busy && last_reply >= 0)
        total += 40;  // room for the mark under the last reply

    if (!g_chat.count)
        draw_empty(c);

    g_max_scroll = total > view.h ? total - view.h : 0;
    if (g_scroll > g_max_scroll)
        g_scroll = g_max_scroll;
    if (g_scroll < 0)
        g_scroll = 0;
    int y = (total > view.h) ? view.y + view.h - total + g_scroll : view.y + 10;

    for (int i = 0; i < g_chat.count; i++) {
        Msg *m = &g_chat.msgs[i];
        int h = msg_height(m);
        if (y + h >= view.y && y <= view.y + view.h)
            draw_msg(c, m, y, &view, i == last_reply && !g_chat.busy);
        y += h;
    }
    if (thinking) {
        for (int k = 0; k < 3; k++) {
            int phase = (frame / 8 + 3 - k) % 3;
            int s = phase == 0 ? 16 : 11;
            canvas_round(c, (Rect){ COL_X + k * 28 + (16 - s) / 2, y + 14 + (16 - s) / 2, s, s }, s / 2,
                         phase == 0 ? COL_ACCENT : 0x7A5A4C, &view);
        }
        font_draw(&g_small, c, COL_X + 100, y + 6, "Thinking", 8, COL_DIM, &view);
    }
    pthread_mutex_unlock(&g_chat.lock);

    // Fade the top of the chat under the title.
    if (g_scroll < g_max_scroll)
        canvas_fill(c, (Rect){ MAIN_X, TOP_H - 2, MAIN_W, 1 }, COL_LINE, NULL);

    draw_composer(c, g_chat.count == 0);

    if (g_chat.confirm == CONFIRM_PENDING)
        draw_confirm(c);
    else if (g_setup.shown)
        draw_setup(c);
    else if (g_picker.open)
        draw_picker(c);
}
