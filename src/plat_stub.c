// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Host stubs so the portable code can be tested on a PC.
#ifndef __ORBIS__
#include "plat.h"
#include "tools.h"

#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

int plat_free_space(const char *path, unsigned long long *f, unsigned long long *t)
{
    struct statvfs s;
    if (statvfs(path, &s) < 0)
        return -1;
    *f = (unsigned long long)s.f_bavail * s.f_frsize;
    *t = (unsigned long long)s.f_blocks * s.f_frsize;
    return 0;
}
void plat_system_info(StrBuf *out) { sb_append(out, "Firmware: host build\n"); }
int plat_launch_app(const char *tid, StrBuf *out) { sb_appendf(out, "Launching %s.", tid); return 0; }
int plat_notify(const char *text) { printf("[notify] %s\n", text); return 0; }
int plat_jailbreak(void) { return 0; }
int plat_jailbroken(void) { return 0; }
const char *plat_jailbreak_status(void) { return "no (not a PS4)"; }
int plat_user_id(void) { return 0; }
int plat_random(void *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    int ok = f && fread(buf, 1, n, f) == n;
    if (f) fclose(f);
    return ok ? 0 : -1;
}
int plat_stub_cpu = 60, plat_stub_fan = 0;
int plat_temperatures(int *cpu, int *soc) { *cpu = plat_stub_cpu; *soc = plat_stub_cpu - 5; return 0; }
long plat_uptime(void) { return 3725; }
int plat_set_fan_threshold(int c, StrBuf *out) { plat_stub_fan = c; sb_appendf(out, "Fan threshold set to %d C.", c); return 0; }
int plat_install_pkg(const char *p, const char *cid, const char *type, unsigned long long size, const char *title, StrBuf *out)
{ (void)size; (void)title; sb_appendf(out, "Install started for %s (%s, %s).", p, cid, type); return 0; }
int plat_last_install_task(void) { static int n = 100; return n++; }
const char *plat_stub_bridge_reply;   // tests set this
int plat_stub_progress_state;
int plat_install_progress(int task, unsigned long long *done, unsigned long long *total, int *state, int *err)
{
    (void)task;
    *total = 1000;
    *done = plat_stub_progress_state ? 1000 : 400;
    *state = plat_stub_progress_state;
    *err = 0;
    return 0;
}
int plat_bridge_get(const char *path, StrBuf *body)
{
    (void)path;
    if (!plat_stub_bridge_reply)
        return -1;
    sb_append(body, plat_stub_bridge_reply);
    return 0;
}
const char *plat_bridge_url(void) { return plat_stub_bridge_reply ? "http://pc:8787" : ""; }
int plat_uninstall_app(const char *t, StrBuf *out) { sb_appendf(out, "Uninstalled %s.", t); return 0; }
int plat_send_payload(const char *p, StrBuf *out) { sb_appendf(out, "Sent %s.", p); return 0; }
int plat_lstat(const char *path, struct stat *st) { return lstat(path, st); }
const char *plat_strerror(int err) { return strerror(err); }
#endif
