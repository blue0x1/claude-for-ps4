// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
#pragma once
#include "strbuf.h"

#define STORAGE_TOOLS_JSON \
"{\"name\":\"storage_report\",\"description\":\"Show what uses the PS4's storage: free space, every game with its update and DLC sizes (largest first), updates/DLC left by games that are gone, .pkg files and whether their content is already installed, captures and other big files. Read-only; can take a minute. Use it before suggesting a cleanup.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}}," \
"{\"name\":\"delete_paths\",\"description\":\"Delete several files or folders with one confirm on screen, for cleanups. They go to the trash unless permanent is true; on the internal drive the space is only freed by empty_trash or permanent. Never use it on /user/app, /user/patch or /user/addcont folders of installed games (use uninstall_app).\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"permanent\":{\"type\":\"boolean\"}},\"required\":[\"paths\"]}}"

// For tests: the folder used as /user.
void storage_set_root(const char *user_root);
int storage_report(StrBuf *out);
