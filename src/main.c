// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Claude for PS4: a chat client for the Claude API that can also use console
// tools (files, apps, system info) after on-screen confirmation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#include "chat.h"
#include "config.h"
#include "font.h"
#include "ime.h"
#include "layout.h"
#include "net.h"
#include "plat.h"
#include "ui.h"



Font g_body, g_small, g_title;
Chat g_chat;
Config g_cfg;
int  g_cfg_ok;
int  g_scroll;          // pixels scrolled up from the bottom
int  g_max_scroll;
static int  g_user_id = -1;
static char g_history_path[256];

int plat_user_id(void)
{
    return g_user_id;
}

static unsigned char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

// ---- history ------------------------------------------------------------

static void save_history(void)
{
    if (!g_history_path[0]) {
        mkdir("/data/claude", 0777);
        mkdir("/data/claude/chats", 0777);
        time_t t = time(NULL);
        struct tm *tm = gmtime(&t);
        snprintf(g_history_path, sizeof(g_history_path),
                 "/data/claude/chats/chat-%04d%02d%02d-%02d%02d%02d.txt", tm->tm_year + 1900,
                 tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    FILE *f = fopen(g_history_path, "wb");
    if (!f)
        return;
    pthread_mutex_lock(&g_chat.lock);
    for (int i = 0; i < g_chat.count; i++) {
        const Msg *m = &g_chat.msgs[i];
        const char *who = m->role == ROLE_USER ? "You" : m->role == ROLE_ASSISTANT ? "Claude"
                        : m->role == ROLE_TOOL ? "Tool" : "Note";
        fprintf(f, "%s:\n%s\n\n", who, m->text.data ? m->text.data : "");
    }
    pthread_mutex_unlock(&g_chat.lock);
    fclose(f);
}

// ---- setup --------------------------------------------------------------

static void load_config(void)
{
    char err[300];
    g_cfg_ok = config_load(&g_cfg, err, sizeof(err));
    if (!g_cfg_ok) {
        chat_add(&g_chat, ROLE_NOTICE, err);
        chat_add(&g_chat, ROLE_NOTICE, "Then press Triangle to load it.");
    }
}

static void fatal_screen(Canvas *c, SDL_Window *win, const char *msg)
{
    for (;;) {
        canvas_fill(c, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, COL_BG, NULL);
        if (g_body.cache_size)
            draw_center(c, &g_body, 500, msg, COL_NOTICE);
        SDL_UpdateWindowSurface(win);
        SDL_Delay(500);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    OrbisUserServiceInitializeParams up = { ORBIS_KERNEL_PRIO_FIFO_LOWEST };
    sceUserServiceInitialize(&up);
    sceUserServiceGetInitialUser(&g_user_id);

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return 1;
    SDL_Window *win = SDL_CreateWindow("Claude", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       SCREEN_W, SCREEN_H, 0);
    if (!win)
        return 1;
    SDL_Surface *surf = SDL_GetWindowSurface(win);
    if (!surf || surf->format->BytesPerPixel != 4)
        return 1;
    Canvas canvas = { (uint32_t *)surf->pixels, surf->w, surf->h, surf->pitch / 4,
                      surf->format->Rshift, surf->format->Gshift, surf->format->Bshift,
                      surf->format->Amask };

    unsigned char *font_data = read_file("/app0/assets/fonts/DejaVuSans.ttf");
    if (!font_data || !font_init(&g_body, font_data, 34) || !font_init(&g_small, font_data, 28) ||
        !font_init(&g_title, font_data, 44))
        fatal_screen(&canvas, win, "Font missing");

    chat_init(&g_chat);
    plat_jailbreak();

    char err[200];
    if (net_init(err, sizeof(err)) < 0)
        fatal_screen(&canvas, win, err);
    if (ime_init() < 0)
        chat_add(&g_chat, ROLE_NOTICE, "Could not load the on-screen keyboard module.");

    scePadInit();
    int pad = scePadOpen(g_user_id, 0, 0, NULL);

    load_config();

    unsigned prev_buttons = 0;
    int was_busy = 0, frame = 0;
    char input[8192];
    g_chat.dirty = 1;

    for (;; frame++) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }

        // Keyboard dialog
        if (ime_is_open()) {
            int r = ime_poll(input, sizeof(input));
            if (r == 0 && input[0]) {
                g_scroll = 0;
                net_send(&g_chat, &g_cfg, input);
            }
        }

        // Controller
        OrbisPadData pd;
        memset(&pd, 0, sizeof(pd));
        if (pad >= 0 && scePadReadState(pad, &pd) == 0 && pd.connected && !ime_is_open()) {
            unsigned b = pd.buttons, pressed = b & ~prev_buttons;
            prev_buttons = b;

            if (g_chat.confirm == CONFIRM_PENDING) {
                if (pressed & ORBIS_PAD_BUTTON_CROSS)
                    g_chat.confirm = CONFIRM_YES;
                else if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
                    g_chat.confirm = CONFIRM_NO;
            } else if (g_chat.busy) {
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
                    net_cancel(&g_chat);
            } else {
                if ((pressed & ORBIS_PAD_BUTTON_CROSS) && g_cfg_ok)
                    ime_open(g_user_id, "Message Claude", "");
                if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
                    chat_clear(&g_chat);
                    g_history_path[0] = 0;
                    g_scroll = 0;
                }
                if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
                    chat_clear(&g_chat);
                    load_config();
                    if (g_cfg_ok) {
                        char msg[320];
                        snprintf(msg, sizeof(msg), "Loaded %s (model %s).", g_cfg.loaded_from, g_cfg.model);
                        chat_add(&g_chat, ROLE_NOTICE, msg);
                    }
                }
            }

            int old = g_scroll;
            if (b & ORBIS_PAD_BUTTON_UP)
                g_scroll += 40;
            if (b & ORBIS_PAD_BUTTON_DOWN)
                g_scroll -= 40;
            if (pressed & ORBIS_PAD_BUTTON_L1)
                g_scroll += 700;
            if (pressed & ORBIS_PAD_BUTTON_R1)
                g_scroll -= 700;
            int ry = (int)pd.rightStick.y - 128;
            if (ry < -30 || ry > 30)
                g_scroll -= ry / 3;
            if (g_scroll > g_max_scroll)
                g_scroll = g_max_scroll;
            if (g_scroll < 0)
                g_scroll = 0;
            if (g_scroll != old)
                g_chat.dirty = 1;
        }

        if (was_busy && !g_chat.busy)
            save_history();
        was_busy = g_chat.busy;

        int animate = g_chat.busy && !g_chat.got_text && frame % 20 == 0;
        if (g_chat.dirty || animate) {
            g_chat.dirty = 0;
            render(&canvas, frame);
            SDL_UpdateWindowSurface(win);
        }
        SDL_Delay(16);
    }
    return 0;
}
