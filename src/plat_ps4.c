// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// PS4 implementations of the platform hooks used by the tools.
#ifdef __ORBIS__
#include "plat.h"
#include "tools.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <orbis/NetCtl.h>
#include <orbis/libkernel.h>

int sceKernelGetCpuTemperature(int *temp);
int sceKernelGetSocSensorTemperature(int sensor, int *temp);
int _fstatfs(int fd, void *buf);
int sceSystemServiceLaunchApp(const char *title_id, const char *argv[], void *param);

// FreeBSD 9 struct statfs (the PS4 kernel layout).
struct bsd_statfs {
    uint32_t f_version, f_type;
    uint64_t f_flags, f_bsize, f_iosize, f_blocks, f_bfree;
    int64_t  f_bavail;
    uint64_t f_files;
    int64_t  f_ffree;
    uint64_t f_syncwrites, f_asyncwrites, f_syncreads, f_asyncreads, f_spare[10];
    uint32_t f_namemax;
    uint32_t f_owner;
    int32_t  f_fsid[2];
    char     f_charspare[80], f_fstypename[16], f_mntfromname[88], f_mntonname[88];
};

int plat_free_space(const char *path, unsigned long long *free_b, unsigned long long *total_b)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct bsd_statfs st;
    memset(&st, 0, sizeof(st));
    int r = _fstatfs(fd, &st);
    close(fd);
    if (r < 0 || !st.f_bsize)
        return -1;
    *free_b = (unsigned long long)st.f_bavail * st.f_bsize;
    *total_b = (unsigned long long)st.f_blocks * st.f_bsize;
    return 0;
}

static void gb(StrBuf *out, const char *label, const char *path)
{
    unsigned long long f, t;
    if (plat_free_space(path, &f, &t) == 0)
        sb_appendf(out, "%s (%s): %.1f GB free of %.1f GB\n", label, path,
                   f / 1073741824.0, t / 1073741824.0);
}

void plat_system_info(StrBuf *out)
{
    OrbisKernelSwVersion v;
    memset(&v, 0, sizeof(v));
    if (sceKernelGetSystemSwVersion(&v) == 0)
        sb_appendf(out, "Firmware: %.10s (0x%08X)\n", v.s_version, v.i_version);
    int t = 0;
    if (sceKernelGetCpuTemperature(&t) == 0)
        sb_appendf(out, "CPU temperature: %d C\n", t);
    if (sceKernelGetSocSensorTemperature(0, &t) == 0)
        sb_appendf(out, "SoC temperature: %d C\n", t);
    gb(out, "Internal storage", "/user");
    gb(out, "USB 0", "/mnt/usb0");
    gb(out, "USB 1", "/mnt/usb1");
    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) == 0)
        sb_appendf(out, "IP address: %s\n", info.ip_address);
    sb_appendf(out, "Full file access (GoldHEN jailbreak): %s\n", plat_jailbroken() ? "yes" : "no");
    sb_append(out, "Fan threshold control: not available yet in this version.\n");
}

typedef struct {
    uint32_t size;
    int32_t  user_id;
    int32_t  app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} LaunchAppParam;

int plat_launch_app(const char *title_id, StrBuf *out)
{
    LaunchAppParam p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.user_id = plat_user_id();
    p.check_flag = 1;  // skip the system update check
    int r = sceSystemServiceLaunchApp(title_id, NULL, &p);
    if (r < 0) {
        sb_appendf(out, "Launch failed (0x%08X). Check the title ID with list_installed_apps.", (unsigned)r);
        return 1;
    }
    sb_appendf(out, "Launching %s.", title_id);
    return 0;
}

int plat_notify(const char *text)
{
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    snprintf(req.message, sizeof(req.message), "%s", text);
    return sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// ---- GoldHEN jailbreak (syscall 500, GoldHEN SDK command 2) -------------
// Based on the GoldHEN Plugins SDK (MIT).

struct jailbreak_backup {
    uint32_t cr_uid, cr_ruid, cr_rgid, cr_groups;
    uint64_t cr_paid;
    uint64_t cr_caps[2];
    void *cr_prison, *fd_cdir, *fd_jdir, *fd_rdir;
};

static long goldhen_cmd(uint64_t cmd, void *data)
{
    long ret;
    char carry;
    register long r10 __asm__("r10") = 0;
    __asm__ volatile("syscall; setc %1"
                     : "=a"(ret), "=r"(carry)
                     : "a"(500L), "D"(cmd), "S"(data), "r"(r10)
                     : "rcx", "r11", "rdx", "memory");
    return carry ? -1 : ret;
}

static int g_jailbroken;

int plat_jailbreak(void)
{
    static struct jailbreak_backup jb;
    // Without GoldHEN, syscall 500 does not exist; don't let SIGSYS kill us.
    signal(SIGSYS, SIG_IGN);
    // Command 0 returns the SDK version when GoldHEN is running.
    if (goldhen_cmd(0, NULL) <= 0)
        return 0;
    g_jailbroken = goldhen_cmd(2, &jb) == 0;
    return g_jailbroken;
}

int plat_jailbroken(void) { return g_jailbroken; }
#endif
