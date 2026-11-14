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
#endif
