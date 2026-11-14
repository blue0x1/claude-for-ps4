// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Conversation state shared by the UI thread and the network thread.
#pragma once
#include <pthread.h>
#include <stddef.h>

#include "config.h"
#include "strbuf.h"

typedef enum { ROLE_USER, ROLE_ASSISTANT, ROLE_NOTICE, ROLE_TOOL } Role;

// One entry on screen.
typedef struct {
    Role   role;
    StrBuf text;
    // Layout cache, owned by the UI.
    size_t layout_len;
    int    layout_width;
    int    nlines;
    int   *line_start;
    int   *line_len;
    void  *rich;          // Markdown layout for replies (Rich *), owned by the UI
    size_t rich_len;
    int    rich_width;
} Msg;

// Frees the UI layout cache of a message (set by the UI).
extern void (*chat_free_layout)(Msg *m);

enum { CONFIRM_NONE, CONFIRM_PENDING, CONFIRM_YES, CONFIRM_NO };

typedef struct {
    // What is shown.
    Msg            *msgs;
    int             count;
    int             cap;
    // What is sent: Messages API message objects as JSON, append-only
    // except for dropping an unanswered tail.
    char          **api;
    int             api_count;
    int             api_cap;

    pthread_mutex_t lock;
    volatile int    busy;          // a request is in flight
    volatile int    cancel;        // user asked to stop
    volatile int    got_text;      // first text arrived for the current reply
    volatile int    req_id;        // sceHttp request id, for abort
    volatile int    dirty;         // UI should redraw

    int             gen;           // bumped when the chat is cleared
    char            conv_id[40];   // new id per chat (PC bridge session key)

    // Tool confirmation, asked by the network thread, answered by the UI.
    volatile int    confirm;
    char            confirm_text[1024];
    char            confirm_title[64];   // "" = "Allow this action?"
    char            confirm_yes[24];     // "" = "Allow"
    char            confirm_no[24];      // "" = "Deny"
} Chat;

void chat_init(Chat *c);
void chat_clear(Chat *c);   // only when idle
int  chat_add(Chat *c, Role role, const char *text);   // returns index
void chat_append(Chat *c, int idx, const char *text, size_t n);

void chat_api_push(Chat *c, const char *message_json);
void chat_api_truncate(Chat *c, int count);

// Builds the request body from the API history.
void chat_build_request(Chat *c, const Config *cfg, const char *tools_json, StrBuf *out);

// Blocks the calling (network) thread until the user answers on screen.
// Returns 1 for yes.
int chat_ask_confirm(Chat *c, const char *text);
