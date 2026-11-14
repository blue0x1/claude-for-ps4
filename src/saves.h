// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Save data: list, back up with versions, restore.
#pragma once
#include "chat.h"
#include "json.h"
#include "strbuf.h"

#define SAVES_TOOLS_JSON \
"{\"name\":\"list_game_saves\",\"description\":\"List the save data on the PS4 per user and game: number of saves, size and when it last changed. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"title_id\":{\"type\":\"string\"}}}}," \
"{\"name\":\"backup_saves\",\"description\":\"Back up save data of one game (title_id) or every game (title_id all) for the signed-in user (or all_users). Each backup is a new version; the newest 20 per game are kept. destination: internal (default), usb0 or usb1. note is a short label. Saves only restore on this PS4.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"title_id\":{\"type\":\"string\"},\"destination\":{\"type\":\"string\",\"enum\":[\"internal\",\"usb0\",\"usb1\"]},\"note\":{\"type\":\"string\"},\"all_users\":{\"type\":\"boolean\"}},\"required\":[\"title_id\"]}}," \
"{\"name\":\"list_save_backups\",\"description\":\"List save backups (internal and USB), newest first per game, with their folder paths. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"title_id\":{\"type\":\"string\"}}}}," \
"{\"name\":\"restore_saves\",\"description\":\"Put a save backup back (backup = folder path from list_save_backups). The current saves are backed up automatically first. The game must be closed. The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"backup\":{\"type\":\"string\"}},\"required\":[\"backup\"]}}"

// For tests: the users folder (/user/home) and USB prefix (/mnt/usb).
void saves_set_paths(const char *home, const char *usb_prefix);
// -1 if name is not a save tool, else like tools_run.
int saves_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out);
