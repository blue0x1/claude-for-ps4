// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Host stubs so the portable code can be tested on a PC.
#ifndef __ORBIS__
#include "plat.h"
#include "tools.h"

#include <stdio.h>
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
int plat_user_id(void) { return 0; }
int plat_random(void *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    int ok = f && fread(buf, 1, n, f) == n;
    if (f) fclose(f);
    return ok ? 0 : -1;
}
int plat_set_fan_threshold(int c, StrBuf *out) { sb_appendf(out, "Fan threshold set to %d C.", c); return 0; }
int plat_install_pkg(const char *p, const char *cid, const char *type, unsigned long long size, const char *title, StrBuf *out)
{ (void)size; (void)title; sb_appendf(out, "Install started for %s (%s, %s).", p, cid, type); return 0; }
int plat_uninstall_app(const char *t, StrBuf *out) { sb_appendf(out, "Uninstalled %s.", t); return 0; }
int plat_send_payload(const char *p, StrBuf *out) { sb_appendf(out, "Sent %s.", p); return 0; }
#endif
