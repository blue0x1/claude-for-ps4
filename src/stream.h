// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Parses a streamed Messages API response (server-sent events), shows text
// as it arrives and rebuilds the full assistant message for the history.
#pragma once
#include <stddef.h>

#include "chat.h"
#include "json.h"
#include "strbuf.h"

typedef struct {
    Chat   *chat;
    int     display_idx;     // screen message receiving text, -1 if none yet
    JVal   *content;         // content blocks, rebuilt from the stream
    StrBuf *partial;         // input_json_delta per block
    int     npartial;
    StrBuf  line;
    char    stop_reason[32];
    char    error[512];
    int     done;
} Stream;

void stream_init(Stream *s, Chat *chat);
void stream_free(Stream *s);
void stream_feed(Stream *s, const char *data, size_t n);
void stream_handle_event(Stream *s, const char *json);
// Finishes tool inputs and returns {"role":"assistant","content":[...]}.
// Caller frees. Returns NULL if no content arrived.
char *stream_message_json(Stream *s);

// Readable message from a non-200 response body.
void api_error_message(const char *body, int status, char *out, size_t outlen);
