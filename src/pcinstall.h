// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Install .pkg files straight from the PC running the bridge.
#pragma once
#include "chat.h"
#include "json.h"
#include "strbuf.h"

#define PCINSTALL_TOOLS_JSON \
"{\"name\":\"list_pc_packages\",\"description\":\"List the .pkg files in the PC bridge's package folder (game, update or DLC, title, version, size, and whether it is already installed on the PS4). Read-only. Needs PC bridge mode.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}}," \
"{\"name\":\"install_from_pc\",\"description\":\"Install one or more packages straight from the PC (names from list_pc_packages). They download over the network into the PS4's install queue, games before their updates if both are listed. The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"names\"]}}," \
"{\"name\":\"check_app_update\",\"description\":\"Check whether the PC bridge has a newer Claude for PS4 version and say how to update. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}}," \
"{\"name\":\"install_status\",\"description\":\"Progress of the installs started with install_from_pc: percent, speed, time left, done or failed. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}}"

// -1 if name is not one of these tools, else like tools_run.
int pcinstall_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out);
// Called from the main loop: pops up a notification when an install ends.
void pcinstall_tick(void);
