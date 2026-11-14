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
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

#include "chat.h"
#include "config.h"
#include "font.h"
#include "ime.h"
#include "layout.h"
#include "net.h"
#include "plat.h"
#include "ui.h"
#include "setup.h"
#include "version.h"

#include <orbis/NetCtl.h>



Font g_body, g_small, g_tiny, g_title, g_serif, g_serif_b, g_head, g_hero, g_mono;
Chat g_chat;
Config g_cfg;
int  g_cfg_ok;
int  g_scroll;          // pixels scrolled up from the bottom
int  g_max_scroll;
Setup g_setup;
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
    setup_set_config(&g_setup, &g_cfg, g_cfg_ok);
}

// Starts the phone page server if the network is up. Quiet unless asked.
static int start_phone_server(int verbose)
{
    if (g_setup.active)
        return 1;
    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) < 0 || !info.ip_address[0]) {
        if (verbose)
            chat_add(&g_chat, ROLE_NOTICE, "The phone page needs the PS4 to be connected to your network.");
        return 0;
    }
    if (setup_start(&g_setup, info.ip_address) < 0) {
        if (verbose)
            chat_add(&g_chat, ROLE_NOTICE, "Could not start the phone page (port 8765 busy?).");
        return 0;
    }
    return 1;
}

static void open_phone_setup(void)
{
    if (start_phone_server(1))
        g_setup.shown = 1;
    g_chat.dirty = 1;
}

static void check_clock(void)
{
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    if (tm && tm->tm_year + 1900 < 2025)
        chat_add(&g_chat, ROLE_NOTICE, "The PS4 clock looks wrong. HTTPS needs the right date: "
                                       "set it in Settings > Date and Time.");
}

// ---- model and effort picker ----------------------------------------------

static void open_picker(void)
{
    g_picker.open = 1;
    g_picker.col = 0;
    int cur = picker_current(0);
    g_picker.row = cur < 0 ? 0 : cur;
    g_chat.dirty = 1;
}

static void apply_choice(int col, const char *value)
{
    if (col == 1)
        snprintf(g_cfg.effort, sizeof(g_cfg.effort), "%s", value);
    else if (g_cfg.bridge)
        snprintf(g_cfg.bridge_model, sizeof(g_cfg.bridge_model), "%s", value);
    else
        snprintf(g_cfg.model, sizeof(g_cfg.model), "%s", value);
    if (config_save(&g_cfg) != 0)
        chat_add(&g_chat, ROLE_NOTICE, "Could not save the choice to /data/claude/claude.ini. It is used until the app closes.");
    setup_set_config(&g_setup, &g_cfg, g_cfg_ok);
}

static void handle_picker(unsigned pressed)
{
    int n = picker_count(g_picker.col);
    if (pressed & ORBIS_PAD_BUTTON_UP)
        g_picker.row = g_picker.row > 0 ? g_picker.row - 1 : n - 1;
    if (pressed & ORBIS_PAD_BUTTON_DOWN)
        g_picker.row = g_picker.row < n - 1 ? g_picker.row + 1 : 0;
    if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
        g_picker.col = !g_picker.col;
        int cur = picker_current(g_picker.col);
        g_picker.row = cur < 0 ? 0 : cur;
    }
    if (pressed & ORBIS_PAD_BUTTON_CROSS)
        apply_choice(g_picker.col, picker_value(g_picker.col, g_picker.row));
    if (pressed & (ORBIS_PAD_BUTTON_CIRCLE | ORBIS_PAD_BUTTON_R2))
        g_picker.open = 0;
    if (pressed)
        g_chat.dirty = 1;
}

// ---- startup log ----------------------------------------------------------
// Each startup step is written to /data/claude/boot.log (and shown on screen)
// so a hang on a real console can be traced over FTP.

static FILE *g_boot_log;
static Canvas *g_boot_canvas;
static SDL_Window *g_boot_win;

static void boot_step(const char *step)
{
    if (!g_boot_log) {
        mkdir("/data/claude", 0777);
        g_boot_log = fopen("/data/claude/boot.log", "w");
        if (g_boot_log)
            fprintf(g_boot_log, "Claude for PS4 %s\n", APP_VERSION);
    }
    if (g_boot_log) {
        fprintf(g_boot_log, "%s\n", step);
        fflush(g_boot_log);
    }
    if (g_boot_canvas && g_body.cache_size) {
        canvas_fill(g_boot_canvas, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, COL_BG, NULL);
        draw_center(g_boot_canvas, &g_body, 500, "Starting Claude...", COL_TEXT);
        draw_center(g_boot_canvas, &g_small, 560, step, COL_DIM);
        SDL_UpdateWindowSurface(g_boot_win);
    }
}

static void boot_done(void)
{
    if (g_boot_log) {
        fprintf(g_boot_log, "ready\n");
        fclose(g_boot_log);
        g_boot_log = NULL;
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
    boot_step("user service");

    OrbisUserServiceInitializeParams up = { ORBIS_KERNEL_PRIO_FIFO_LOWEST };
    sceUserServiceInitialize(&up);
    sceUserServiceGetInitialUser(&g_user_id);

    boot_step("video");
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
    g_boot_canvas = &canvas;
    g_boot_win = win;
    // The system start screen stays on top of the app until it is hidden.
    sceSystemServiceHideSplashScreen();
    boot_step("fonts");

    unsigned char *sans = read_file("/app0/assets/fonts/DejaVuSans.ttf");
    unsigned char *serif = read_file("/app0/assets/fonts/DejaVuSerif.ttf");
    unsigned char *serif_b = read_file("/app0/assets/fonts/DejaVuSerif-Bold.ttf");
    unsigned char *mono = read_file("/app0/assets/fonts/DejaVuSansMono.ttf");
    if (!sans || !font_init(&g_body, sans, 32) || !font_init(&g_small, sans, 27) || !font_init(&g_tiny, sans, 23) ||
        !font_init(&g_title, sans, 44))
        fatal_screen(&canvas, win, "Font missing");
    // Fall back to the sans font if an extra font is missing.
    if (!serif || !font_init(&g_serif, serif, 34) || !font_init(&g_hero, serif, 64))
        font_init(&g_serif, sans, 33), font_init(&g_hero, sans, 60);
    if (!serif_b || !font_init(&g_serif_b, serif_b, 34) || !font_init(&g_head, serif_b, 42))
        font_init(&g_serif_b, sans, 33), font_init(&g_head, sans, 42);
    if (!mono || !font_init(&g_mono, mono, 27))
        font_init(&g_mono, sans, 27);
    ui_init();
    boot_step("chat state");

    chat_init(&g_chat);
    setup_init(&g_setup, &g_chat);
    boot_step("GoldHEN jailbreak");
    plat_jailbreak();
    boot_step("network");

    char err[200];
    if (net_init(err, sizeof(err)) < 0)
        fatal_screen(&canvas, win, err);
    boot_step("keyboard");
    if (ime_init() < 0)
        chat_add(&g_chat, ROLE_NOTICE, "Could not load the on-screen keyboard module.");

    boot_step("controller");
    scePadInit();
    int pad = scePadOpen(g_user_id, 0, 0, NULL);

    boot_step("settings");
    sceNetCtlInit();
    load_config();
    check_clock();
    boot_step("phone page");
    if (!g_cfg_ok)
        open_phone_setup();
    else
        start_phone_server(0);

    unsigned prev_buttons = 0;
    int was_busy = 0, frame = 0;
    char input[8192];
    boot_done();
    sceSystemServiceHideSplashScreen();
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

            if (g_picker.open && g_chat.confirm != CONFIRM_PENDING && !g_setup.shown) {
                handle_picker(pressed);
                pressed = 0;  // no scrolling or other actions while the picker is open
                b = 0;
            } else if (g_setup.shown && g_chat.confirm != CONFIRM_PENDING) {
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                    g_setup.shown = 0;  // the server keeps running for the phone remote
                    g_chat.dirty = 1;
                }
            } else if (g_chat.confirm == CONFIRM_PENDING) {
                if (pressed & ORBIS_PAD_BUTTON_CROSS)
                    g_chat.confirm = CONFIRM_YES;
                else if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
                    g_chat.confirm = CONFIRM_NO;
            } else if (g_chat.busy) {
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
                    net_cancel(&g_chat);
                if ((pressed & ORBIS_PAD_BUTTON_R2) && g_cfg_ok)
                    open_picker();
            } else {
                if ((pressed & ORBIS_PAD_BUTTON_R2) && g_cfg_ok)
                    open_picker();
                if ((pressed & ORBIS_PAD_BUTTON_CROSS) && g_cfg_ok)
                    ime_open(g_user_id, "Message Claude", "");
                if (pressed & ORBIS_PAD_BUTTON_OPTIONS)
                    open_phone_setup();
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

        // Requests from the phone remote
        if (setup_take_config(&g_setup, &g_cfg))
            g_chat.dirty = 1;
        if (g_setup.want_stop) {
            g_setup.want_stop = 0;
            if (g_chat.busy)
                net_cancel(&g_chat);
        }
        if (g_setup.want_new && !g_chat.busy) {
            g_setup.want_new = 0;
            chat_clear(&g_chat);
            g_history_path[0] = 0;
            g_scroll = 0;
        }
        if (!g_chat.busy && g_cfg_ok && !ime_is_open() && setup_take_text(&g_setup, input, sizeof(input))) {
            g_scroll = 0;
            net_send(&g_chat, &g_cfg, input);
        }

        // Settings saved from the phone (applied once idle)
        if (g_setup.saved && !g_chat.busy) {
            g_setup.saved = 0;
            g_setup.shown = 0;
            chat_clear(&g_chat);
            load_config();
            if (g_cfg_ok)
                chat_add(&g_chat, ROLE_NOTICE, g_cfg.bridge
                             ? "Saved from your phone. Using your PC bridge (Claude account). Press Cross to type."
                             : "Saved from your phone. Using your API key. Press Cross to type.");
        }
        if (g_setup.shown && frame % 30 == 0)
            g_chat.dirty = 1;  // the PIN can change after wrong tries
        if (!g_setup.active && frame % 600 == 300)
            start_phone_server(0);  // network came up later

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
