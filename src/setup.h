// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Phone page: a small web server on the PS4 for the local network.
// The user opens it on a phone (QR code on screen) and signs in with the PIN
// shown on the TV. Then the phone can chat (phone remote), answer confirm
// boxes, and change settings (API key or PC bridge, saved to claude.ini).
#pragma once
#include <pthread.h>

#include "chat.h"
#include "config.h"
#include "strbuf.h"

#define SETUP_PORT 8765

#define SETUP_SESSIONS 4
#define REMOTE_TEXT_MAX 8192

typedef struct {
    volatile int    active;     // server thread running
    volatile int    stop;
    volatile int    shown;      // QR/PIN panel on the TV
    volatile int    saved;      // new settings saved; UI should reload
    volatile int    cfg_changed; // model or effort changed from the phone
    char            url[64];    // http://<ip>:<port>/
    char            pin[8];
    int             bad_pins;
    char            sessions[SETUP_SESSIONS][33];  // signed-in phones
    int             next_session;
    Config          cfg;        // current settings (prefill), updated on save
    int             cfg_ok;
    Chat           *chat;       // for the phone remote (NULL: settings only)
    // Requests from the phone, done by the UI thread.
    volatile int    want_send, want_stop, want_new;
    char            send_text[REMOTE_TEXT_MAX];
    pthread_mutex_t lock;
    int             listen_fd;
} Setup;

// Prepares the struct (lock, PIN). Call once before anything else.
void setup_init(Setup *s, Chat *chat);
// Starts the server thread. ip is the PS4's LAN address.
int  setup_start(Setup *s, const char *ip);
void setup_stop(Setup *s);
// Tells the page about the settings now in use.
void setup_set_config(Setup *s, const Config *c, int ok);
// Copies a model/effort change made on the phone into cfg. Returns 1 if there was one.
int  setup_take_config(Setup *s, Config *cfg);
// Takes a message typed on the phone. Returns 1 and fills text if there is one.
int  setup_take_text(Setup *s, char *text, size_t len);

// Handles one raw HTTP request (portable, used by tests). Fills resp with a
// full HTTP response. Returns 1 if settings were saved.
int  setup_handle_request(Setup *s, const char *req, size_t len, StrBuf *resp);
// Makes a fresh 6-digit PIN (also signs out nobody; sessions stay).
void setup_new_pin(Setup *s);
