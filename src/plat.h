// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
#pragma once
// Escapes the app sandbox through GoldHEN so the tools can reach /user,
// /system_data and USB drives. Returns 1 on success.
int plat_jailbreak(void);
int plat_jailbroken(void);
// Readable result of the last plat_jailbreak() ("yes ..." or "no: why").
const char *plat_jailbreak_status(void);
int plat_user_id(void);

#include <stddef.h>
#include <sys/stat.h>
#include "strbuf.h"
// lstat() that works on the PS4: the toolchain libc routes lstat through
// fstatat, which is a stub there ("Function not implemented").
int plat_lstat(const char *path, struct stat *st);
// Text for an errno value. On the PS4 the kernel sets FreeBSD numbers while
// the toolchain's strerror expects Linux ones.
const char *plat_strerror(int err);
// Fills buf with random bytes (system RNG). Returns 0 on success.
int plat_random(void *buf, size_t n);
// CPU and SoC temperatures in Celsius. 0 on success.
int plat_temperatures(int *cpu, int *soc);
// Seconds since the console started.
long plat_uptime(void);
// Sets the fan threshold temperature in Celsius (45 to 80).
int plat_set_fan_threshold(int celsius, StrBuf *out);
// Starts installing a .pkg file in the background (system downloads queue).
int plat_install_pkg(const char *path, const char *content_id, const char *pkg_type,
                     unsigned long long size, const char *title, StrBuf *out);
// Task ID of the last install started by plat_install_pkg (-1 if none).
int plat_last_install_task(void);
// Progress of an install task: bytes done and total. Returns 0 when known;
// *state is 0 running, 1 finished, -1 failed (error code in *err).
int plat_install_progress(int task, unsigned long long *done, unsigned long long *total, int *state, int *err);
// GET on the PC bridge with the pairing code (blocking). 0 on HTTP 200.
int plat_bridge_get(const char *path, StrBuf *body);
// The bridge address from claude.ini ("" when not in bridge mode).
const char *plat_bridge_url(void);
// Uninstalls an app or game by title ID.
int plat_uninstall_app(const char *title_id, StrBuf *out);
// Sends a payload file to GoldHEN's BinLoader on 127.0.0.1:9090.
int plat_send_payload(const char *path, StrBuf *out);
