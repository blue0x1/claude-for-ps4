// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(StrBuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb_init(sb);
}

void sb_reserve(StrBuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap)
        return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need)
        cap *= 2;
    char *p = realloc(sb->data, cap);
    if (!p)
        abort();
    sb->data = p;
    sb->cap = cap;
    sb->data[sb->len] = 0;
}

void sb_appendn(StrBuf *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = 0;
}

void sb_append(StrBuf *sb, const char *s)
{
    sb_appendn(sb, s, strlen(s));
}

void sb_appendc(StrBuf *sb, char c)
{
    sb_appendn(sb, &c, 1);
}

void sb_appendf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    char probe[1];
    va_start(ap, fmt);
    int n = vsnprintf(probe, sizeof(probe), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    sb_reserve(sb, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

void sb_append_json_string(StrBuf *sb, const char *s)
{
    sb_appendc(sb, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\n': sb_append(sb, "\\n"); break;
        case '\r': sb_append(sb, "\\r"); break;
        case '\t': sb_append(sb, "\\t"); break;
        case '\b': sb_append(sb, "\\b"); break;
        case '\f': sb_append(sb, "\\f"); break;
        default:
            if (*p < 0x20)
                sb_appendf(sb, "\\u%04x", *p);
            else
                sb_appendc(sb, (char)*p);
        }
    }
    sb_appendc(sb, '"');
}
