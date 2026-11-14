// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// claude.ini loader. The API key is only ever read from this file.
#pragma once

typedef struct {
    char api_key[256];
    char model[96];
    int  max_tokens;
    char effort[16];      // "" = API default
    char system[4096];
    int  tls_verify;      // 1 = verify api.anthropic.com certificate (default)
    int  fallbacks;       // -1 auto, 0 off, 1 on
    char loaded_from[256];
} Config;

#define CONFIG_DEFAULT_MODEL  "claude-opus-5-5"
#define CONFIG_DEFAULT_SYSTEM \
    "You are Claude, chatting with the user through an app on a PlayStation 4. " \
    "Your replies are shown as plain text on a TV screen, so do not use Markdown " \
    "tables, headings or bold markers. Short paragraphs and simple lists are fine."

// Search paths, first match wins.
extern const char *const CONFIG_PATHS[];

void config_defaults(Config *cfg);
// Parses ini text into cfg (keys: api_key, model, max_tokens, effort, system,
// tls_verify, fallbacks). Unknown keys are ignored.
void config_parse(Config *cfg, const char *text);
// Loads the first config file found. Returns 1 on success; on failure writes
// a reason to err.
int config_load(Config *cfg, char *err, int errlen);
// Whether the request should carry the server-side fallback parameter.
int config_use_fallbacks(const Config *cfg);
