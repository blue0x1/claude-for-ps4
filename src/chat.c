// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void new_conv_id(Chat *c)
{
    static unsigned n;
    snprintf(c->conv_id, sizeof(c->conv_id), "ps4-%lx-%u", (unsigned long)time(NULL), ++n);
}

void chat_init(Chat *c)
{
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->lock, NULL);
    new_conv_id(c);
}

void (*chat_free_layout)(Msg *m);

void chat_clear(Chat *c)
{
    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < c->count; i++) {
        sb_free(&c->msgs[i].text);
        free(c->msgs[i].line_start);
        free(c->msgs[i].line_len);
        if (chat_free_layout)
            chat_free_layout(&c->msgs[i]);
    }
    c->count = 0;
    c->gen++;
    pthread_mutex_unlock(&c->lock);
    chat_api_truncate(c, 0);
    new_conv_id(c);
    c->dirty = 1;
}

int chat_add(Chat *c, Role role, const char *text)
{
    pthread_mutex_lock(&c->lock);
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 16;
        Msg *m = realloc(c->msgs, sizeof(Msg) * (size_t)cap);
        if (!m)
            abort();
        c->msgs = m;
        c->cap = cap;
    }
    Msg *m = &c->msgs[c->count];
    memset(m, 0, sizeof(*m));
    m->role = role;
    sb_init(&m->text);
    sb_append(&m->text, text ? text : "");
    int idx = c->count++;
    c->dirty = 1;
    pthread_mutex_unlock(&c->lock);
    return idx;
}

void chat_append(Chat *c, int idx, const char *text, size_t n)
{
    pthread_mutex_lock(&c->lock);
    if (idx >= 0 && idx < c->count) {
        sb_appendn(&c->msgs[idx].text, text, n);
        c->dirty = 1;
    }
    pthread_mutex_unlock(&c->lock);
}

void chat_api_push(Chat *c, const char *json)
{
    pthread_mutex_lock(&c->lock);
    if (c->api_count == c->api_cap) {
        int cap = c->api_cap ? c->api_cap * 2 : 16;
        char **a = realloc(c->api, sizeof(char *) * (size_t)cap);
        if (!a)
            abort();
        c->api = a;
        c->api_cap = cap;
    }
    c->api[c->api_count++] = strdup(json);
    pthread_mutex_unlock(&c->lock);
}

void chat_api_truncate(Chat *c, int count)
{
    pthread_mutex_lock(&c->lock);
    while (c->api_count > count)
        free(c->api[--c->api_count]);
    pthread_mutex_unlock(&c->lock);
}

void chat_build_request(Chat *c, const Config *cfg, const char *tools_json, StrBuf *out)
{
    sb_append(out, "{\"model\":");
    sb_append_json_string(out, config_active_model(cfg));
    sb_appendf(out, ",\"max_tokens\":%d,\"stream\":true", cfg->max_tokens);
    // Cache the growing conversation prefix (cheaper tool loops).
    sb_append(out, ",\"cache_control\":{\"type\":\"ephemeral\"}");
    if (cfg->effort[0]) {
        sb_append(out, ",\"output_config\":{\"effort\":");
        sb_append_json_string(out, cfg->effort);
        sb_append(out, "}");
    }
    if (!cfg->bridge && config_use_fallbacks(cfg))
        sb_append(out, ",\"fallbacks\":\"default\"");
    if (cfg->system[0]) {
        sb_append(out, ",\"system\":");
        sb_append_json_string(out, cfg->system);
    }
    if (tools_json && *tools_json) {
        sb_append(out, ",\"tools\":");
        sb_append(out, tools_json);
    }
    sb_append(out, ",\"messages\":[");
    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < c->api_count; i++) {
        if (i)
            sb_appendc(out, ',');
        sb_append(out, c->api[i]);
    }
    pthread_mutex_unlock(&c->lock);
    sb_append(out, "]}");
}

int chat_ask_confirm(Chat *c, const char *text)
{
    snprintf(c->confirm_text, sizeof(c->confirm_text), "%s", text);
    c->confirm_title[0] = c->confirm_yes[0] = c->confirm_no[0] = 0;
    c->confirm = CONFIRM_PENDING;
    c->dirty = 1;
    while (c->confirm == CONFIRM_PENDING && !c->cancel)
        usleep(20 * 1000);
    int yes = c->confirm == CONFIRM_YES && !c->cancel;
    c->confirm = CONFIRM_NONE;
    c->dirty = 1;
    return yes;
}
