// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Home screen folders: reads and edits the system app database
// (/system_data/priv/mms/app.db) the way the PS4 itself stores folders.
// Every change is backed up first and checked after.
#pragma once
#include "chat.h"
#include "json.h"
#include "strbuf.h"

// Tool definitions (JSON objects, comma separated, no brackets) for TOOLS_JSON.
#define HOME_TOOLS_JSON \
"{\"name\":\"list_home_folders\",\"description\":\"List the folders on the PS4 home screen with the games in each, and the games not in a folder, with title IDs. Read-only. Use this before changing folders.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"user\":{\"type\":\"string\",\"description\":\"Only if asked: the 10-digit user number from a previous error message.\"}}}}," \
"{\"name\":\"create_home_folder\",\"description\":\"Create a folder on the PS4 home screen and put games in it (title IDs or exact names; at least one). Games already in another folder are moved. A backup of the home screen database is made first. The user must confirm on screen. The PS4 may need a restart to show it.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"games\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"user\":{\"type\":\"string\"}},\"required\":[\"name\",\"games\"]}}," \
"{\"name\":\"move_to_home_folder\",\"description\":\"Move games (title IDs or exact names) into an existing home screen folder (folder name or ID), or out of their folder when folder is empty. A folder left empty is removed. Backup first; the user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"games\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"folder\":{\"type\":\"string\"},\"user\":{\"type\":\"string\"}},\"required\":[\"games\",\"folder\"]}}," \
"{\"name\":\"rename_home_folder\",\"description\":\"Rename a home screen folder. Backup first; the user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"folder\":{\"type\":\"string\"},\"new_name\":{\"type\":\"string\"},\"user\":{\"type\":\"string\"}},\"required\":[\"folder\",\"new_name\"]}}," \
"{\"name\":\"delete_home_folder\",\"description\":\"Remove a home screen folder. Its games go back to the home screen; nothing is uninstalled. Backup first; the user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"folder\":{\"type\":\"string\"},\"user\":{\"type\":\"string\"}},\"required\":[\"folder\"]}}," \
"{\"name\":\"restore_home_backup\",\"description\":\"Undo folder changes: put back a backup of the home screen database made before an earlier change (the newest one unless backup names a file). The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"backup\":{\"type\":\"string\"}}}}"

// Runs a home folder tool. Returns -1 if name is not one, else like tools_run.
int home_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out);

// For tests: where the database and the backups are.
void home_set_paths(const char *db_path, const char *backup_dir);
