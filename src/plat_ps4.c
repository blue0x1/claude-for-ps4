// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// PS4 implementations of the platform hooks used by the tools.
#ifdef __ORBIS__
#include "plat.h"
#include "tools.h"

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <orbis/Sysmodule.h>

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

// ---- random -------------------------------------------------------------

// System libraries that apps do not get at start are loaded on first use and
// looked up by name, so a missing one only disables that feature instead of
// stopping the app from starting.
static void *sys_sym(const char *lib, const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), "/system/common/lib/%s", lib);
    int h = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    if (h < 0) {
        snprintf(path, sizeof(path), "/system/priv/lib/%s", lib);
        h = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    }
    if (h < 0)
        return NULL;
    void *addr = NULL;
    if (sceKernelDlsym(h, name, &addr) < 0)
        return NULL;
    return addr;
}

int plat_random(void *buf, size_t n)
{
    static int (*get_random)(void *, size_t);
    static int tried;
    if (!tried) {
        tried = 1;
        get_random = (int (*)(void *, size_t))sys_sym("libSceRandom.sprx", "sceRandomGetRandomNumber");
    }
    if (!get_random)
        return -1;
    // The system call accepts at most 64 bytes at a time.
    unsigned char *p = buf;
    while (n) {
        size_t k = n > 64 ? 64 : n;
        if (get_random(p, k) < 0)
            return -1;
        p += k;
        n -= k;
    }
    return 0;
}

// ---- fan ----------------------------------------------------------------

int ioctl(int fd, unsigned long request, ...);

int plat_set_fan_threshold(int celsius, StrBuf *out)
{
    int fd = open("/dev/icc_fan", O_RDONLY, 0);
    if (fd < 0) {
        sb_append(out, "Could not open /dev/icc_fan. This needs GoldHEN full access (header must not say sandboxed).");
        return 1;
    }
    unsigned char data[10] = { 0 };
    data[5] = (unsigned char)celsius;
    int r = ioctl(fd, 0xC01C8F07, data);
    close(fd);
    if (r < 0) {
        sb_append(out, "The fan controller refused the change.");
        return 1;
    }
    sb_appendf(out, "Fan threshold set to %d C until the next reboot.", celsius);
    return 0;
}

// ---- pkg install / uninstall ---------------------------------------------

#include <orbis/Bgft.h>

static int bgft_ready, appinst_ready;
static int (*p_bgft_init)(OrbisBgftInitParams *);
static int (*p_bgft_register)(OrbisBgftDownloadParamEx *, OrbisBgftTaskId *);
static int (*p_bgft_start)(OrbisBgftTaskId);
static int (*p_appinst_init)(void);
static int (*p_appinst_uninstall)(const char *);

static int bgft_init(void)
{
    if (bgft_ready)
        return 0;
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_BGFT);
    p_bgft_init = (int (*)(OrbisBgftInitParams *))sys_sym("libSceBgft.sprx", "sceBgftServiceIntInit");
    p_bgft_register = (int (*)(OrbisBgftDownloadParamEx *, OrbisBgftTaskId *))sys_sym(
        "libSceBgft.sprx", "sceBgftServiceIntDownloadRegisterTaskByStorageEx");
    p_bgft_start = (int (*)(OrbisBgftTaskId))sys_sym("libSceBgft.sprx", "sceBgftServiceDownloadStartTask");
    if (!p_bgft_init || !p_bgft_register || !p_bgft_start)
        return -1;
    static OrbisBgftInitParams ip;
    ip.heapSize = 0x100000;
    ip.heap = malloc(ip.heapSize);
    if (!ip.heap || p_bgft_init(&ip) < 0)
        return -1;
    bgft_ready = 1;
    return 0;
}

static int appinst_init(void)
{
    if (appinst_ready)
        return 0;
    p_appinst_init = (int (*)(void))sys_sym("libSceAppInstUtil.sprx", "sceAppInstUtilInitialize");
    p_appinst_uninstall = (int (*)(const char *))sys_sym("libSceAppInstUtil.sprx", "sceAppInstUtilAppUnInstall");
    if (!p_appinst_init || !p_appinst_uninstall || p_appinst_init() < 0)
        return -1;
    appinst_ready = 1;
    return 0;
}

int plat_install_pkg(const char *path, const char *content_id, const char *pkg_type,
                     unsigned long long size, const char *title, StrBuf *out)
{
    if (bgft_init() < 0) {
        sb_append(out, "Could not start the system package installer (BGFT).");
        return 1;
    }
    OrbisBgftDownloadParamEx p;
    memset(&p, 0, sizeof(p));
    p.params.entitlementType = 5;
    p.params.id = content_id;
    p.params.contentUrl = path;
    p.params.contentName = title;
    p.params.iconPath = "";
    p.params.playgoScenarioId = "0";
    p.params.option = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM;
    p.params.packageType = pkg_type;
    p.params.packageSubType = "";
    p.params.packageSize = size;
    p.slot = 0;
    OrbisBgftTaskId task = -1;
    int r = p_bgft_register(&p, &task);
    if (r == (int)0x80990088) {
        sb_append(out, "This package is already installed (or an install of it is queued).");
        return 1;
    }
    if (r < 0) {
        sb_appendf(out, "The installer refused the package (0x%08X).", (unsigned)r);
        return 1;
    }
    r = p_bgft_start(task);
    if (r < 0) {
        sb_appendf(out, "Could not start the install task (0x%08X).", (unsigned)r);
        return 1;
    }
    sb_appendf(out, "Install started for %s. Progress shows in Notifications > Downloads.", content_id);
    return 0;
}

int plat_uninstall_app(const char *title_id, StrBuf *out)
{
    if (appinst_init() < 0) {
        sb_append(out, "Could not start the system app installer service.");
        return 1;
    }
    int r = p_appinst_uninstall(title_id);
    if (r < 0) {
        sb_appendf(out, "Uninstall failed (0x%08X).", (unsigned)r);
        return 1;
    }
    sb_appendf(out, "Uninstalled %s.", title_id);
    return 0;
}

// ---- payloads -----------------------------------------------------------

#include <orbis/Net.h>

struct bsd_sockaddr_in4 {
    uint8_t len, family;
    uint16_t port;
    uint32_t addr;
    char zero[8];
};

int plat_send_payload(const char *path, StrBuf *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        sb_appendf(out, "Could not open %s.", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024 * 1024) {
        fclose(f);
        sb_append(out, "The payload is empty or too large.");
        return 1;
    }
    char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        sb_append(out, "Could not read the payload.");
        return 1;
    }
    fclose(f);
    int s = sceNetSocket("claudePayload", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    struct bsd_sockaddr_in4 a;
    memset(&a, 0, sizeof(a));
    a.len = sizeof(a);
    a.family = ORBIS_NET_AF_INET;
    a.port = sceNetHtons(9090);
    a.addr = 0x0100007F;  // 127.0.0.1 in network order
    if (s < 0 || sceNetConnect(s, (OrbisNetSockaddr *)&a, sizeof(a)) < 0) {
        if (s >= 0)
            sceNetSocketClose(s);
        free(buf);
        sb_append(out, "GoldHEN BinLoader is not listening on port 9090. Turn on BinLoader Server in GoldHEN settings.");
        return 1;
    }
    long off = 0;
    while (off < n) {
        int w = sceNetSend(s, buf + off, (size_t)(n - off), 0);
        if (w <= 0)
            break;
        off += w;
    }
    sceNetSocketClose(s);
    free(buf);
    if (off != n) {
        sb_appendf(out, "Sending stopped after %ld of %ld bytes.", off, n);
        return 1;
    }
    sb_appendf(out, "Sent %ld bytes to GoldHEN BinLoader.", n);
    return 0;
}
#endif
