// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Saved chats in /data/claude/chats: a readable .txt and a .json that keeps
// what is needed to continue the chat (API history and PC bridge session key).
#pragma once
#include "chat.h"

#define HISTORY_DIR      "/data/claude/chats"
#define HISTORY_MAX      60
#define HISTORY_ID_MAX   40
#define HISTORY_TITLE_MAX 96

typedef struct {
    char id[HISTORY_ID_MAX];       // file name without extension, e.g. chat-20261114-052310
    char title[HISTORY_TITLE_MAX]; // first line of the first message you sent
} HistoryEntry;

// 1 if id is a safe chat id (letters, digits, '-' and '_', not empty).
int history_id_ok(const char *id);
// Makes a new id from the current time.
void history_new_id(char id[HISTORY_ID_MAX]);
// Writes <dir>/<id>.txt and <dir>/<id>.json. Returns 0 on success.
int history_save(Chat *c, const char *dir, const char *id);
// Lists saved chats, newest first. Returns the count.
int history_list(const char *dir, HistoryEntry *out, int max);
// Replaces the chat (must be idle) with a saved one. Returns 0 on success.
int history_load(Chat *c, const char *dir, const char *id);
// Short title from a message: first non-empty line, cut at a UTF-8 boundary.
void history_title(const char *text, char *out, size_t n);
