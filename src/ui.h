// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
#pragma once
#include "chat.h"
#include "config.h"
#include "font.h"
#include "history.h"
#include "setup.h"

#define SCREEN_W 1920
#define SCREEN_H 1080

// Warm dark theme.
#define COL_BG         0x262624
#define COL_SIDEBAR    0x1F1E1D
#define COL_SIDEROW    0x2C2B28
#define COL_LINE       0x3A3935
#define COL_TEXT       0xF2F0E8
#define COL_DIM        0xA6A49B
#define COL_FAINT      0x75736C
#define COL_ACCENT     0xD97757
#define COL_USERBG     0x141413
#define COL_COMPOSER   0x30302E
#define COL_COMPBORDER 0x4A4843
#define COL_PANEL      0x30302E
#define COL_CODE       0xEBCDB4
#define COL_CODEBG     0x1A1A18
#define COL_INLINEBG   0x3B3A36
#define COL_NOTICE     0xE5A15B
#define COL_TOOL       0xB8B5AC
#define COL_TOOLBG     0x2E2D2A

#define SIDEBAR_W  420
#define MAIN_X     SIDEBAR_W
#define MAIN_W     (SCREEN_W - SIDEBAR_W)
#define COL_W      1160
#define COL_X      (MAIN_X + (MAIN_W - COL_W) / 2)
#define TOP_H      84
#define COMP_H     140
#define COMP_X     (COL_X - 24)
#define COMP_W     (COL_W + 48)
#define COMP_Y     (SCREEN_H - 40 - COMP_H)

// Fonts (main.c): sans for the interface and your messages, serif for
// Claude's replies, mono for code.
extern Font g_body, g_small, g_tiny, g_title, g_serif, g_serif_b, g_head, g_hero, g_mono;
extern Chat   g_chat;
extern Config g_cfg;
extern int    g_cfg_ok;
extern int    g_scroll, g_max_scroll;
extern Setup  g_setup;   // QR/PIN panel while g_setup.shown

// Model and effort picker: two columns (0 model, 1 effort).
typedef struct {
    int open, col, row;
} Picker;
extern Picker g_picker;
int         picker_count(int col);
const char *picker_value(int col, int row);
int         picker_current(int col);   // -1 if the saved value is not in the list
const char *effort_name(const char *effort);

// Sidebar: saved chats and the Log out row, chosen with the D-pad.
typedef struct {
    int focus;   // 1 while the sidebar has the controller
    int sel;     // 0..g_recent_count-1 chats, g_recent_count = Log out
} SideNav;
extern SideNav      g_side;
extern HistoryEntry g_recent[HISTORY_MAX];
extern int          g_recent_count;
extern char         g_chat_id[HISTORY_ID_MAX];  // saved chat on screen ("" = not saved yet)
extern char         g_profile[96];              // name shown at the bottom left
extern char         g_profile_sub[96];
int side_count(void);
extern int          g_about;   // 1 while the About popup is showing

void ui_init(void);   // after the fonts are loaded
void render(Canvas *c, int frame);
void draw_center(Canvas *c, Font *f, int y, const char *s, uint32_t color);
