// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Trash (deletes can be undone) and the action history (every change Claude
// made, when, and whether it was approved on the TV or the phone).
#pragma once
#include <stddef.h>
#include "chat.h"
#include "json.h"
#include "strbuf.h"

#define ACTIONS_TOOLS_JSON \
"{\"name\":\"list_trash\",\"description\":\"List what is in the trash (files and folders removed with delete_path), with IDs, original paths, sizes and dates. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}}," \
"{\"name\":\"restore_from_trash\",\"description\":\"Put an item from the trash back where it was (ID from list_trash).\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}}," \
"{\"name\":\"empty_trash\",\"description\":\"Permanently delete trash items to free space: one ID, or all items when id is omitted (optionally only those older than older_than_days). The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"older_than_days\":{\"type\":\"integer\"}}}}," \
"{\"name\":\"show_history\",\"description\":\"Show the latest actions Claude took on the PS4 (newest last): time, tool, target, result and whether it was approved on the TV or the phone. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\"}}}}"

// Base folder for the trash, history and backups (default /data/claude).
void actions_set_dir(const char *dir);
const char *actions_dir(void);

// Moves path into the trash of its drive. 0 ok (id written), 1 error text in out.
int trash_put(const char *path, char *id, size_t idlen, StrBuf *out);
// 1 if path is inside a trash folder (deleting there is permanent).
int in_trash(const char *path);

// Total size in bytes of a file or folder.
unsigned long long tree_size(const char *path);

// Runs a trash or history tool. -1 if name is not one, else like tools_run.
int actions_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out);

// Records a finished tool call that changed something.
void history_add(Chat *chat, const char *tool, const char *what, int is_error, const char *result);
// 1 for tools that only read.
int tool_is_read_only(const char *name);
