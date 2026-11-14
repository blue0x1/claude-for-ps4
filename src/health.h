// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Temperatures over time, overheat warnings and fan profiles.
#pragma once
#include <time.h>
#include "chat.h"
#include "json.h"
#include "strbuf.h"

#define HEALTH_TOOLS_JSON \
"{\"name\":\"health_report\",\"description\":\"Console health: temperatures now and recently, fan profile, how long it has been on, storage, with tips. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}}," \
"{\"name\":\"temperature_history\",\"description\":\"CPU temperature over the last minutes (5 to 240, default 60) as a small chart. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"minutes\":{\"type\":\"integer\"}}}}," \
"{\"name\":\"set_fan_profile\",\"description\":\"Pick a fan profile: quiet (79 C, default), balanced (70), cool (62), max (50), or auto (70, turns up to 55 while hot). Kept across reboots once the app opens. The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"profile\":{\"type\":\"string\",\"enum\":[\"quiet\",\"balanced\",\"cool\",\"max\",\"auto\"]}},\"required\":[\"profile\"]}}"

// Main loop: takes a sample every 30 s (and applies the saved fan profile once).
void health_tick(void);
// For tests: add a sample at a given time; forget everything.
void health_sample(time_t now, int cpu, int soc);
void health_reset(void);
// -1 if name is not a health tool, else like tools_run.
int health_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out);
