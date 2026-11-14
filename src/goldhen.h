// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// GoldHEN cheats, game patches, plugins and settings.
#pragma once
#include "chat.h"
#include "json.h"
#include "strbuf.h"

#define GOLDHEN_TOOLS_JSON \
"{\"name\":\"list_cheats\",\"description\":\"List the GoldHEN cheats available for a game (by title ID) and which file matches the installed game version. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"title_id\":{\"type\":\"string\"}},\"required\":[\"title_id\"]}}," \
"{\"name\":\"list_game_patches\",\"description\":\"List GoldHEN game patches (60 FPS, resolution, fixes) for a game and which match the installed version. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"title_id\":{\"type\":\"string\"}},\"required\":[\"title_id\"]}}," \
"{\"name\":\"list_plugins\",\"description\":\"List GoldHEN plugins (.prx) and for which games each is on in plugins.ini. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}}," \
"{\"name\":\"set_plugin\",\"description\":\"Turn a GoldHEN plugin on or off for one game (title_id) or all games (title_id all). The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"plugin\":{\"type\":\"string\"},\"title_id\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"plugin\",\"enabled\"]}}," \
"{\"name\":\"goldhen_settings\",\"description\":\"Show GoldHEN's settings (config.ini): FTP, BinLoader, cheat menu, FPS overlay, plugins, debug settings. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}}," \
"{\"name\":\"set_goldhen_setting\",\"description\":\"Change one existing GoldHEN setting in config.ini (section and key from goldhen_settings, value usually 0 or 1). The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"section\",\"key\",\"value\"]}}"

// For tests: the GoldHEN folder (default /data/GoldHEN).
void goldhen_set_dir(const char *dir);
// -1 if name is not a GoldHEN tool, else like tools_run.
int goldhen_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out);
