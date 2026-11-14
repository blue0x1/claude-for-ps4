// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#pragma once
// Escapes the app sandbox through GoldHEN so the tools can reach /user,
// /system_data and USB drives. Returns 1 on success.
int plat_jailbreak(void);
int plat_jailbroken(void);
int plat_user_id(void);

#include <stddef.h>
#include "strbuf.h"
// Fills buf with random bytes (system RNG). Returns 0 on success.
int plat_random(void *buf, size_t n);
// Sets the fan threshold temperature in Celsius (45 to 80).
int plat_set_fan_threshold(int celsius, StrBuf *out);
// Starts installing a .pkg file in the background (system downloads queue).
int plat_install_pkg(const char *path, const char *content_id, const char *pkg_type,
                     unsigned long long size, const char *title, StrBuf *out);
// Uninstalls an app or game by title ID.
int plat_uninstall_app(const char *title_id, StrBuf *out);
// Sends a payload file to GoldHEN's BinLoader on 127.0.0.1:9090.
int plat_send_payload(const char *path, StrBuf *out);
