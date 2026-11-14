// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Permission modes (full, trusted, read-only, kid) and remembered preferences.
#pragma once
#include "chat.h"
#include "json.h"
#include "strbuf.h"

#define PREFS_TOOLS_JSON \
"{\"name\":\"remember\",\"description\":\"Save something the user wants you to remember in every future chat (a preference, a fact about their setup). One short sentence.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}}," \
"{\"name\":\"forget\",\"description\":\"Forget a remembered item: number from get_preferences, matching text, or text all.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"number\":{\"type\":\"integer\"},\"text\":{\"type\":\"string\"}}}}," \
"{\"name\":\"get_preferences\",\"description\":\"Show the permission mode and everything remembered. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}}," \
"{\"name\":\"set_permission_mode\",\"description\":\"Make the permission mode stricter: full (ask before every change), read-only, or kid (read-only and kid-friendly). Only the user can loosen it, on the phone settings page.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"kid\",\"read-only\",\"full\",\"trusted\"]}},\"required\":[\"mode\"]}}"

const char *prefs_mode(void);
// Sets the mode without checks (phone settings page). 0 ok.
int prefs_set_mode(const char *mode);
// Before a tool runs: 1 = blocked (reason in out). *auto_ok = 1 when its
// confirm may be skipped (trusted mode, safe tool).
int prefs_check(const char *tool, int read_only, int *auto_ok, StrBuf *out);
// Appended to the system prompt: mode notes and remembered items.
void prefs_system_extra(StrBuf *out);
// For tests: reload from disk next time.
void prefs_reset(void);
int prefs_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out);
