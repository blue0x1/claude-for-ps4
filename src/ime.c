// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
#include "ime.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <orbis/CommonDialog.h>
#include <orbis/ImeDialog.h>
#include <orbis/Sysmodule.h>

// The PS4 uses 16-bit wchar_t (UTF-16); clang's is 32-bit, so use uint16_t.
#define IME_MAX_CHARS 2000

static uint16_t g_buf[IME_MAX_CHARS + 1];
static uint16_t g_title[128];
static int g_open;
static int g_none_polls;

static void utf8_to_utf16(const char *s, uint16_t *out, size_t max)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && o + 2 < max) {
        unsigned cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); p += 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            p += 4;
        } else { cp = 0xFFFD; p++; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 + (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out[o++] = (uint16_t)cp;
        }
    }
    out[o] = 0;
}

static void utf16_to_utf8(const uint16_t *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 5 < outlen; i++) {
        unsigned cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
}

int ime_init(void)
{
    if (sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG) < 0)
        return -1;
    sceCommonDialogInitialize();
    return 0;
}

int ime_open(int user_id, const char *title, const char *initial)
{
    if (g_open)
        return -1;
    memset(g_buf, 0, sizeof(g_buf));
    utf8_to_utf16(initial ? initial : "", g_buf, IME_MAX_CHARS);
    utf8_to_utf16(title ? title : "", g_title, 128);

    OrbisImeDialogSetting p;
    memset(&p, 0, sizeof(p));
    p.userId = (uint32_t)user_id;
    p.type = ORBIS_TYPE_DEFAULT;
    p.supportedLanguages = 0;
    p.enterLabel = ORBIS_BUTTON_LABEL_SEND;
    p.maxTextLength = IME_MAX_CHARS;
    p.inputTextBuffer = (wchar_t *)g_buf;
    p.title = (const wchar_t *)g_title;
    p.posx = 1920 / 2;
    p.posy = 1080 / 2;
    p.horizontalAlignment = ORBIS_H_CENTER;
    p.verticalAlignment = ORBIS_V_CENTER;
    p.option = 0;
    int r = sceImeDialogInit(&p, NULL);
    if (r < 0)
        return r;
    g_open = 1;
    g_none_polls = 0;
    return 0;
}

int ime_poll(char *out, size_t outlen)
{
    if (!g_open)
        return 2;
    OrbisDialogStatus st = sceImeDialogGetStatus();
    if (st == ORBIS_DIALOG_STATUS_RUNNING)
        return -1;
    // Right after Init the status can still read NONE for a moment.
    if (st == ORBIS_DIALOG_STATUS_NONE && ++g_none_polls < 120)
        return -1;
    int ret = 2;
    if (st == ORBIS_DIALOG_STATUS_STOPPED) {
        OrbisDialogResult res;
        memset(&res, 0, sizeof(res));
        sceImeDialogGetResult(&res);
        if (res.endstatus == ORBIS_DIALOG_OK) {
            utf16_to_utf8(g_buf, out, outlen);
            ret = 0;
        } else {
            ret = 1;
        }
    }
    sceImeDialogTerm();
    g_open = 0;
    return ret;
}

int ime_is_open(void)
{
    return g_open;
}
