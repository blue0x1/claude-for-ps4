// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Phone setup: a small web page served by the PS4 on the local network.
// The user opens it on a phone (QR code on screen), types the PIN shown on
// the TV and enters an API key or PC bridge details. Saved to claude.ini.
#pragma once
#include <pthread.h>

#include "config.h"
#include "strbuf.h"

#define SETUP_PORT 8765

typedef struct {
    volatile int    active;     // server thread running
    volatile int    stop;
    volatile int    saved;      // new settings saved; UI should reload
    char            url[64];    // http://<ip>:<port>/
    char            pin[8];
    int             bad_pins;
    Config          cfg;        // current settings (prefill), updated on save
    pthread_mutex_t lock;
    int             listen_fd;
} Setup;

// Starts the server thread. ip is the PS4's LAN address.
int  setup_start(Setup *s, const Config *current, const char *ip);
void setup_stop(Setup *s);

// Handles one raw HTTP request (portable, used by tests). Fills resp with a
// full HTTP response. Returns 1 if settings were saved.
int  setup_handle_request(Setup *s, const char *req, size_t len, StrBuf *resp);
// Makes a fresh 6-digit PIN.
void setup_new_pin(Setup *s);
