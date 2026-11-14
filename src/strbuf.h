// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Growable string buffer.
#pragma once
#include <stddef.h>

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_reserve(StrBuf *sb, size_t extra);
void sb_append(StrBuf *sb, const char *s);
void sb_appendn(StrBuf *sb, const char *s, size_t n);
void sb_appendc(StrBuf *sb, char c);
void sb_appendf(StrBuf *sb, const char *fmt, ...);
// Appends s as a quoted JSON string literal.
void sb_append_json_string(StrBuf *sb, const char *s);
