// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// claude.ini loader. The API key is only ever read from this file.
#pragma once

typedef struct {
    char api_key[256];
    char model[96];       // API key mode
    char bridge_model[64]; // PC bridge mode: "default" or a Claude Code alias/id
    int  max_tokens;
    char effort[16];      // "" = API default
    char system[4096];
    int  tls_verify;      // 1 = verify api.anthropic.com certificate (default)
    int  fallbacks;       // -1 auto, 0 off, 1 on
    int  bridge;          // 1 = talk to the PC bridge instead of the API
    char bridge_url[160]; // e.g. http://192.168.1.50:8787
    char bridge_token[96];
    char loaded_from[256];
} Config;

#define CONFIG_DEFAULT_MODEL  "claude-opus-5-5"
#define CONFIG_DEFAULT_SYSTEM \
    "You are Claude, chatting with the user through an app on a PlayStation 4. " \
    "The user may also be on a phone. Replies render light Markdown: headings, **bold**, " \
    "`inline code`, code blocks, bullet and numbered lists, and quotes. Tables, links and " \
    "images are not rendered, so avoid them. Keep replies easy to read on a TV."

// Choices offered by the model/effort pickers (NULL terminated).
extern const char *const CONFIG_MODELS[];         // API key mode
extern const char *const CONFIG_MODEL_NAMES[];    // display names, same order
extern const char *const CONFIG_BRIDGE_MODELS[];  // PC bridge mode
extern const char *const CONFIG_EFFORTS[];        // "" = model default
// The model in use for the current mode.
const char *config_active_model(const Config *cfg);
// Short display name for a model id ("Opus 5.5"); returns id if unknown.
const char *config_model_name(const char *id);
// 1 if s is a safe model id/alias (letters, digits, dot, dash; not starting with -).
int config_model_ok(const char *s);
int config_effort_ok(const char *s);

// Search paths, first match wins.
extern const char *const CONFIG_PATHS[];

void config_defaults(Config *cfg);
// Parses ini text into cfg (keys: api_key, model, max_tokens, effort, system,
// tls_verify, fallbacks). Unknown keys are ignored.
void config_parse(Config *cfg, const char *text);
// Loads the first config file found. Returns 1 on success; on failure writes
// a reason to err.
int config_load(Config *cfg, char *err, int errlen);
// Serializes cfg as ini text (round-trips through config_parse).
#include "strbuf.h"
void config_write(const Config *cfg, StrBuf *out);
// Saves cfg to CONFIG_SAVE_PATH. Returns 0 on success.
#ifndef CONFIG_SAVE_DIR
#define CONFIG_SAVE_DIR  "/data/claude"
#endif
#define CONFIG_SAVE_PATH CONFIG_SAVE_DIR "/claude.ini"
int config_save(const Config *cfg);

// Whether the request should carry the server-side fallback parameter.
int config_use_fallbacks(const Config *cfg);
