// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const CONFIG_PATHS[] = {
    "/mnt/usb0/claude.ini",
    "/mnt/usb1/claude.ini",
    "/data/claude/claude.ini",
    "/data/claude.ini",
    NULL,
};

void config_defaults(Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->model, sizeof(cfg->model), "%s", CONFIG_DEFAULT_MODEL);
    cfg->max_tokens = 16000;
    snprintf(cfg->system, sizeof(cfg->system), "%s", CONFIG_DEFAULT_SYSTEM);
    cfg->tls_verify = 1;
    cfg->fallbacks = -1;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    // Strip one pair of surrounding quotes.
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = 0;
        s++;
    }
    return s;
}

static void unescape_newlines(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] == 'n') {
            *w++ = '\n';
            r++;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

void config_parse(Config *cfg, const char *text)
{
    char *copy = strdup(text);
    if (!copy)
        return;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';' || *l == '[')
            continue;
        char *eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = trim(l);
        char *val = trim(eq + 1);
        if (!strcmp(key, "api_key"))
            snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", val);
        else if (!strcmp(key, "model") && *val)
            snprintf(cfg->model, sizeof(cfg->model), "%s", val);
        else if (!strcmp(key, "max_tokens") && atoi(val) > 0)
            cfg->max_tokens = atoi(val);
        else if (!strcmp(key, "effort"))
            snprintf(cfg->effort, sizeof(cfg->effort), "%s", val);
        else if (!strcmp(key, "system")) {
            snprintf(cfg->system, sizeof(cfg->system), "%s", val);
            unescape_newlines(cfg->system);
        } else if (!strcmp(key, "tls_verify"))
            cfg->tls_verify = atoi(val) != 0;
        else if (!strcmp(key, "fallbacks"))
            cfg->fallbacks = !strcmp(val, "auto") ? -1 : (atoi(val) != 0);
    }
    free(copy);
}

int config_load(Config *cfg, char *err, int errlen)
{
    config_defaults(cfg);
    for (int i = 0; CONFIG_PATHS[i]; i++) {
        FILE *f = fopen(CONFIG_PATHS[i], "rb");
        if (!f)
            continue;
        char *buf = calloc(1, 65537);
        size_t n = buf ? fread(buf, 1, 65536, f) : 0;
        fclose(f);
        if (!buf)
            break;
        buf[n] = 0;
        config_parse(cfg, buf);
        free(buf);
        snprintf(cfg->loaded_from, sizeof(cfg->loaded_from), "%s", CONFIG_PATHS[i]);
        if (!cfg->api_key[0]) {
            snprintf(err, (size_t)errlen, "%s has no api_key line.", CONFIG_PATHS[i]);
            return 0;
        }
        return 1;
    }
    snprintf(err, (size_t)errlen,
             "No claude.ini found. Put one on a USB drive (root folder) or at "
             "/data/claude/claude.ini with a line like: api_key=sk-ant-...");
    return 0;
}

int config_use_fallbacks(const Config *cfg)
{
    if (cfg->fallbacks >= 0)
        return cfg->fallbacks;
    // Models that accept fallbacks: "default".
    static const char *const ok[] = {
        "claude-opus-5-5", "claude-opus-5", "claude-fable-5-2", "claude-fable-5-1",
        "claude-sonnet-5-5", NULL,
    };
    for (int i = 0; ok[i]; i++)
        if (!strcmp(cfg->model, ok[i]))
            return 1;
    return 0;
}
