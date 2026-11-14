// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void stream_init(Stream *s, Chat *chat)
{
    memset(s, 0, sizeof(*s));
    s->chat = chat;
    s->display_idx = -1;
    s->content = json_new_arr();
    sb_init(&s->line);
}

void stream_free(Stream *s)
{
    json_free(s->content);
    for (int i = 0; i < s->npartial; i++)
        sb_free(&s->partial[i]);
    free(s->partial);
    sb_free(&s->line);
    memset(s, 0, sizeof(*s));
}

static StrBuf *partial_for(Stream *s, int idx)
{
    if (idx < 0 || idx > 1024)
        return NULL;
    if (idx >= s->npartial) {
        StrBuf *p = realloc(s->partial, sizeof(StrBuf) * (size_t)(idx + 1));
        if (!p)
            return NULL;
        for (int i = s->npartial; i <= idx; i++)
            sb_init(&p[i]);
        s->partial = p;
        s->npartial = idx + 1;
    }
    return &s->partial[idx];
}

static JVal *block_at(Stream *s, int idx)
{
    return (idx >= 0 && idx < s->content->count) ? s->content->items[idx] : NULL;
}

static void append_field(JVal *block, const char *key, const char *add)
{
    JVal *cur = json_get(block, key);
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, (cur && cur->type == J_STR) ? cur->str : "");
    sb_append(&sb, add);
    json_obj_set(block, key, json_new_str(sb.data));
    sb_free(&sb);
}

static void show_text(Stream *s, const char *text)
{
    if (!*text)
        return;
    if (s->display_idx < 0)
        s->display_idx = chat_add(s->chat, ROLE_ASSISTANT, "");
    chat_append(s->chat, s->display_idx, text, strlen(text));
    s->chat->got_text = 1;
}

void stream_handle_event(Stream *s, const char *json)
{
    JVal *ev = json_parse(json);
    const char *type = json_get_str(ev, "type");
    if (!type) {
        json_free(ev);
        return;
    }
    if (!strcmp(type, "content_block_start")) {
        JVal *idxv = json_get(ev, "index");
        JVal *block = json_get(ev, "content_block");
        int idx = idxv ? (int)idxv->num : s->content->count;
        if (block && block->type == J_OBJ && idx == s->content->count) {
            // Detach the block from the event so it survives json_free(ev).
            for (int i = 0; i < ev->count; i++)
                if (ev->items[i] == block)
                    ev->items[i] = json_new_obj();
            json_arr_push(s->content, block);
            const char *bt = json_get_str(block, "type");
            if (bt && !strcmp(bt, "text")) {
                const char *t = json_get_str(block, "text");
                s->display_idx = -1;  // a new text block starts a new screen message
                if (t)
                    show_text(s, t);
            }
        }
    } else if (!strcmp(type, "content_block_delta")) {
        JVal *idxv = json_get(ev, "index");
        int idx = idxv ? (int)idxv->num : -1;
        JVal *block = block_at(s, idx);
        JVal *delta = json_get(ev, "delta");
        const char *dt = json_get_str(delta, "type");
        if (block && dt) {
            if (!strcmp(dt, "text_delta")) {
                const char *t = json_get_str(delta, "text");
                if (t) {
                    append_field(block, "text", t);
                    show_text(s, t);
                }
            } else if (!strcmp(dt, "thinking_delta")) {
                const char *t = json_get_str(delta, "thinking");
                if (t)
                    append_field(block, "thinking", t);
            } else if (!strcmp(dt, "signature_delta")) {
                const char *t = json_get_str(delta, "signature");
                if (t)
                    json_obj_set(block, "signature", json_new_str(t));
            } else if (!strcmp(dt, "input_json_delta")) {
                const char *t = json_get_str(delta, "partial_json");
                StrBuf *p = partial_for(s, idx);
                if (t && p)
                    sb_append(p, t);
            } else if (!strcmp(dt, "citations_delta")) {
                // Not used by this app; the text itself still arrives.
            }
        }
    } else if (!strcmp(type, "message_delta")) {
        const char *sr = json_get_str(json_get(ev, "delta"), "stop_reason");
        if (sr)
            snprintf(s->stop_reason, sizeof(s->stop_reason), "%s", sr);
    } else if (!strcmp(type, "message_stop")) {
        s->done = 1;
    } else if (!strcmp(type, "error")) {
        JVal *err = json_get(ev, "error");
        const char *et = json_get_str(err, "type");
        const char *msg = json_get_str(err, "message");
        snprintf(s->error, sizeof(s->error), "%s%s%s", et ? et : "error", msg ? ": " : "", msg ? msg : "");
    }
    json_free(ev);
}

static void handle_line(Stream *s, char *line)
{
    size_t n = strlen(line);
    if (n && line[n - 1] == '\r')
        line[--n] = 0;
    if (strncmp(line, "data:", 5))
        return;
    char *payload = line + 5;
    if (*payload == ' ')
        payload++;
    stream_handle_event(s, payload);
}

void stream_feed(Stream *s, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            sb_reserve(&s->line, 0);
            s->line.data[s->line.len] = 0;
            handle_line(s, s->line.data);
            s->line.len = 0;
        } else {
            sb_appendc(&s->line, data[i]);
        }
    }
}

char *stream_message_json(Stream *s)
{
    if (!s->content->count)
        return NULL;
    for (int i = 0; i < s->content->count; i++) {
        JVal *b = s->content->items[i];
        const char *bt = json_get_str(b, "type");
        if (!bt || (strcmp(bt, "tool_use") && strcmp(bt, "server_tool_use")))
            continue;
        StrBuf *p = i < s->npartial ? &s->partial[i] : NULL;
        if (p && p->len) {
            JVal *input = json_parse(p->data);
            json_obj_set(b, "input", input ? input : json_new_obj());
            p->len = 0;
        } else if (!json_get(b, "input")) {
            json_obj_set(b, "input", json_new_obj());
        }
    }
    StrBuf out;
    sb_init(&out);
    sb_append(&out, "{\"role\":\"assistant\",\"content\":");
    json_write(s->content, &out);
    sb_append(&out, "}");
    return out.data;
}

void api_error_message(const char *body, int status, char *out, size_t outlen)
{
    JVal *v = json_parse(body);
    JVal *err = json_get(v, "error");
    const char *msg = json_get_str(err, "message");
    const char *type = json_get_str(err, "type");
    if (msg)
        snprintf(out, outlen, "API error %d (%s): %s", status, type ? type : "error", msg);
    else
        snprintf(out, outlen, "API error %d", status);
    json_free(v);
}
