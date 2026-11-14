// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// HTTPS transport (PS4 libSceHttp) and the request worker thread.
#pragma once
#include "chat.h"
#include "config.h"

#define API_URL          "https://api.anthropic.com/v1/messages"
#define API_VERSION      "2026-10-21"
#define API_BETA_FALLBACK "server-side-fallback-2026-07-01"

// Loads the network modules and creates the libnet/libssl/libhttp contexts.
int net_init(char *err, int errlen);
// Adds the user's message and runs the reply (with tool calls) on a worker
// thread. Returns 0 if the thread started.
int net_send(Chat *chat, const Config *cfg, const char *user_text);
// Asks the running request to stop.
void net_cancel(Chat *chat);
// Asks the PC bridge which Claude account it uses (blocking). Returns 0 if
// a name or email came back.
#include <stddef.h>
int net_whoami(const Config *cfg, char *name, size_t name_len, char *email, size_t email_len);
