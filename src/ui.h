// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#pragma once
#include "chat.h"
#include "config.h"
#include "font.h"
#include "setup.h"

#define SCREEN_W 1920
#define SCREEN_H 1080

#define COL_BG      0x1F1E1D
#define COL_HEADER  0x262624
#define COL_ACCENT  0xD97757
#define COL_TEXT    0xECEBE6
#define COL_DIM     0x9C9A92
#define COL_USERBG  0x31302D
#define COL_NOTICE  0xE5A15B
#define COL_TOOL    0x8FB3D9
#define COL_PANEL   0x2B2A27

#define HEADER_H    96
#define FOOTER_H    84
#define MARGIN_X    170
#define TEXT_W      (SCREEN_W - 2 * MARGIN_X)
#define PAD_X       24

extern Font   g_body, g_small, g_title;
extern Chat   g_chat;
extern Config g_cfg;
extern int    g_cfg_ok;
extern int    g_scroll, g_max_scroll;
extern Setup  g_setup;   // phone setup screen while g_setup.active

void render(Canvas *c, int frame);
void draw_center(Canvas *c, Font *f, int y, const char *s, uint32_t color);
