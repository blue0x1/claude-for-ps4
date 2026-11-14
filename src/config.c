// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *const CONFIG_PATHS[] = {
    "/data/claude/claude.ini",   // written by phone setup
    "/mnt/usb0/claude.ini",
    "/mnt/usb1/claude.ini",
    "/data/claude.ini",
    NULL,
};

const char *const CONFIG_MODELS[] = {
    "claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5", "claude-fable-5-2", "claude-opus-5", NULL,
};
const char *const CONFIG_MODEL_NAMES[] = {
    "Opus 5.5", "Sonnet 5.5", "Haiku 5.5", "Fable 5.2", "Opus 5", NULL,
};
const char *const CONFIG_BRIDGE_MODELS[] = { "default", "opus", "sonnet", "haiku", "fable", NULL };
const char *const CONFIG_EFFORTS[] = { "", "low", "medium", "high", "xhigh", "max", NULL };

const char *config_active_model(const Config *cfg)
{
    return cfg->bridge ? cfg->bridge_model : cfg->model;
}

const char *config_model_name(const char *id)
{
    for (int i = 0; CONFIG_MODELS[i]; i++)
        if (!strcmp(id, CONFIG_MODELS[i]))
            return CONFIG_MODEL_NAMES[i];
    if (!strcmp(id, "default"))
        return "PC default";
    return id;
}

int config_model_ok(const char *s)
{
    if (!s || !*s || *s == '-' || strlen(s) > 63)
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '.' || *s == '-' || *s == '_' || *s == '[' || *s == ']'))
            return 0;
    return 1;
}

int config_effort_ok(const char *s)
{
    for (int i = 0; CONFIG_EFFORTS[i]; i++)
        if (!strcmp(s, CONFIG_EFFORTS[i]))
            return 1;
    return 0;
}

void config_defaults(Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->model, sizeof(cfg->model), "%s", CONFIG_DEFAULT_MODEL);
    snprintf(cfg->bridge_model, sizeof(cfg->bridge_model), "default");
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
        else if (!strcmp(key, "bridge_model") && *val)
            snprintf(cfg->bridge_model, sizeof(cfg->bridge_model), "%s", val);
        else if (!strcmp(key, "max_tokens") && atoi(val) > 0)
            cfg->max_tokens = atoi(val);
        else if (!strcmp(key, "effort") && config_effort_ok(val))
            snprintf(cfg->effort, sizeof(cfg->effort), "%s", val);
        else if (!strcmp(key, "system")) {
            snprintf(cfg->system, sizeof(cfg->system), "%s", val);
            unescape_newlines(cfg->system);
        } else if (!strcmp(key, "tls_verify"))
            cfg->tls_verify = atoi(val) != 0;
        else if (!strcmp(key, "mode"))
            cfg->bridge = !strcmp(val, "bridge");
        else if (!strcmp(key, "bridge_url"))
            snprintf(cfg->bridge_url, sizeof(cfg->bridge_url), "%s", val);
        else if (!strcmp(key, "bridge_token"))
            snprintf(cfg->bridge_token, sizeof(cfg->bridge_token), "%s", val);
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
        if (cfg->bridge && (!cfg->bridge_url[0] || !cfg->bridge_token[0])) {
            snprintf(err, (size_t)errlen, "%s: mode=bridge needs bridge_url and bridge_token.", CONFIG_PATHS[i]);
            return 0;
        }
        if (!cfg->bridge && !cfg->api_key[0]) {
            snprintf(err, (size_t)errlen, "%s has no api_key line.", CONFIG_PATHS[i]);
            return 0;
        }
        return 1;
    }
    snprintf(err, (size_t)errlen,
             "Not set up yet. Press Options to set up from your phone, or put a "
             "claude.ini on a USB drive (root folder).");
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

static void escape_newlines(const char *in, StrBuf *out)
{
    for (; *in; in++) {
        if (*in == '\n')
            sb_append(out, "\\n");
        else if (*in != '\r')
            sb_appendc(out, *in);
    }
}

void config_write(const Config *cfg, StrBuf *out)
{
    sb_append(out, "# Claude for PS4 settings (written by phone setup)\n");
    sb_appendf(out, "mode=%s\n", cfg->bridge ? "bridge" : "api");
    sb_appendf(out, "api_key=%s\n", cfg->api_key);
    sb_appendf(out, "bridge_url=%s\n", cfg->bridge_url);
    sb_appendf(out, "bridge_token=%s\n", cfg->bridge_token);
    sb_appendf(out, "model=%s\n", cfg->model);
    sb_appendf(out, "bridge_model=%s\n", cfg->bridge_model);
    sb_appendf(out, "max_tokens=%d\n", cfg->max_tokens);
    sb_appendf(out, "effort=%s\n", cfg->effort);
    sb_appendf(out, "tls_verify=%d\n", cfg->tls_verify);
    sb_appendf(out, "fallbacks=%s\n", cfg->fallbacks < 0 ? "auto" : cfg->fallbacks ? "1" : "0");
    if (strcmp(cfg->system, CONFIG_DEFAULT_SYSTEM)) {
        sb_append(out, "system=");
        escape_newlines(cfg->system, out);
        sb_append(out, "\n");
    }
}

int config_save(const Config *cfg)
{
    StrBuf sb;
    sb_init(&sb);
    config_write(cfg, &sb);
    mkdir(CONFIG_SAVE_DIR, 0777);
    FILE *f = fopen(CONFIG_SAVE_PATH, "wb");
    int ok = f && fwrite(sb.data, 1, sb.len, f) == sb.len;
    if (f)
        fclose(f);
    sb_free(&sb);
    return ok ? 0 : -1;
}
