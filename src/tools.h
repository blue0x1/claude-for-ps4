// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Console tools Claude can call. Anything that writes, deletes, installs or
// runs asks the user on screen first.
#pragma once
#include "chat.h"
#include "json.h"
#include "strbuf.h"
#include "plat.h"

// JSON array for the "tools" request field.
extern const char TOOLS_JSON[];

// Runs one tool. Writes the result text to out; returns 1 if it is an error.
// Tools that return pictures append image content blocks (JSON objects,
// comma separated) to images.
int tools_run(Chat *chat, const char *name, const JVal *input, StrBuf *out, StrBuf *images);

// Reads a .pkg header. Returns 1 for a valid PS4 package.
int pkg_read_info(const char *path, char content_id[37], const char **pkg_type, unsigned long long *size);
// Appends standard base64 of data.
void base64_append(StrBuf *out, const unsigned char *data, size_t n);

// Short one-line description shown on screen while a tool runs.
void tools_describe(const char *name, const JVal *input, char *out, int outlen);

// Platform hooks (tools_ps4.c on the console, stubs elsewhere).
void plat_system_info(StrBuf *out);
int  plat_launch_app(const char *title_id, StrBuf *out);
int  plat_notify(const char *text);
int  plat_free_space(const char *path, unsigned long long *free_b, unsigned long long *total_b);
