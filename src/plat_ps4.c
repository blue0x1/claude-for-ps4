// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
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
#include <time.h>
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
    sb_appendf(out, "Full file access (GoldHEN jailbreak): %s\n", plat_jailbreak_status());
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

// The SDK makes the call indirectly (syscall 0 with 500 as the first
// argument), as libkernel does; the direct form is kept as a fallback.
// Returns rax as is: GoldHEN answers some commands with the carry flag set
// (the version call gives 0x100 that way), so the flag is not an error sign.
static long goldhen_cmd(int indirect, uint64_t cmd, void *data)
{
    long ret;
    register long r10 __asm__("r10") = 0;
    if (indirect)
        __asm__ volatile("syscall"
                         : "=a"(ret)
                         : "a"(0L), "D"(500L), "S"(cmd), "d"(data), "r"(r10)
                         : "rcx", "r11", "memory");
    else
        __asm__ volatile("syscall"
                         : "=a"(ret)
                         : "a"(500L), "D"(cmd), "S"(data), "r"(r10)
                         : "rcx", "r11", "rdx", "memory");
    return ret;
}

static int g_jailbroken;
static char g_jb_status[200] = "not tried";

// The sandbox hides /user/app; seeing it means full file access.
static int outside_sandbox(void)
{
    return access("/user/app", F_OK) == 0 || access("/system_data", F_OK) == 0;
}

int plat_jailbreak(void)
{
    static struct jailbreak_backup jb;
    if (g_jailbroken)
        return 1;
    if (outside_sandbox()) {
        g_jailbroken = 1;
        snprintf(g_jb_status, sizeof(g_jb_status), "yes (already outside the sandbox)");
        return 1;
    }
    // Without GoldHEN, syscall 500 does not exist; don't let SIGSYS kill us.
    signal(SIGSYS, SIG_IGN);
    long ver[2], ret[2];
    for (int ind = 1; ind >= 0; ind--) {
        int i = 1 - ind;
        ver[i] = goldhen_cmd(ind, 0, NULL);       // SDK version, 0x100 = 1.00
        memset(&jb, 0, sizeof(jb));
        ret[i] = goldhen_cmd(ind, 2, &jb);        // jailbreak; checked by its effect
        if (outside_sandbox()) {
            g_jailbroken = 1;
            snprintf(g_jb_status, sizeof(g_jb_status), "yes (GoldHEN SDK 0x%lx, %s call)", ver[i],
                     ind ? "indirect" : "direct");
            return 1;
        }
    }
    snprintf(g_jb_status, sizeof(g_jb_status),
             "no: GoldHEN did not lift the sandbox (version call %ld / %ld, jailbreak call %ld / %ld; "
             "78 means GoldHEN is not running).", ver[0], ver[1], ret[0], ret[1]);
    return 0;
}

int plat_jailbroken(void) { return g_jailbroken; }
const char *plat_jailbreak_status(void) { return g_jb_status; }

// ---- file system calls the libc gets wrong ---------------------------------

#include <errno.h>

int plat_lstat(const char *path, struct stat *st)
{
    // FreeBSD 9 lstat is syscall 190; its struct stat is the toolchain's.
    long ret;
    char carry;
    register long r10 __asm__("r10") = 0;
    __asm__ volatile("syscall; setc %1"
                     : "=a"(ret), "=r"(carry)
                     : "a"(0L), "D"(190L), "S"(path), "d"(st), "r"(r10)
                     : "rcx", "r11", "memory");
    if (carry) {
        errno = (int)ret;
        return -1;
    }
    return 0;
}

const char *plat_strerror(int err)
{
    switch (err) {  // FreeBSD numbers, as the PS4 kernel sets them
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 5:  return "Input/output error";
    case 9:  return "Bad file descriptor";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 16: return "Device busy";
    case 17: return "File exists";
    case 18: return "Cross-device link";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 22: return "Invalid argument";
    case 24: return "Too many open files";
    case 27: return "File too large";
    case 28: return "No space left on device";
    case 30: return "Read-only file system";
    case 35: return "Resource temporarily unavailable";
    case 62: return "Too many levels of symbolic links";
    case 63: return "File name too long";
    case 66: return "Directory not empty";
    case 78: return "Function not implemented";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "error %d", err);
        return buf;
    }
    }
}

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

int plat_temperatures(int *cpu, int *soc)
{
    *cpu = *soc = 0;
    int r = sceKernelGetCpuTemperature(cpu);
    sceKernelGetSocSensorTemperature(0, soc);
    return r == 0 ? 0 : -1;
}

long plat_uptime(void)
{
    // kern.boottime through FreeBSD __sysctl (syscall 202). The toolchain's
    // CLOCK_MONOTONIC counts from when the app started, not the console.
    int mib[2] = { 1, 21 };   // CTL_KERN, KERN_BOOTTIME
    struct { long sec, usec; } boot = { 0, 0 };
    unsigned long len = sizeof(boot);
    long ret;
    char carry;
    register long r10 __asm__("r10") = (long)&len;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile("syscall; setc %1"
                     : "=a"(ret), "=r"(carry)
                     : "a"(202L), "D"(mib), "S"(2L), "d"(&boot), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    time_t now = time(NULL);
    if (!carry && boot.sec > 0 && boot.sec <= now)
        return (long)(now - boot.sec);
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long)ts.tv_sec;
}

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
static int (*p_bgft_progress)(OrbisBgftTaskId, OrbisBgftTaskProgress *);
static int (*p_bgft_register_url)(OrbisBgftDownloadParam *, OrbisBgftTaskId *);
static int (*p_bgft_register_task)(OrbisBgftDownloadParam *, OrbisBgftTaskId *);
static int g_last_task = -1;
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
    p_bgft_register_url = (int (*)(OrbisBgftDownloadParam *, OrbisBgftTaskId *))sys_sym(
        "libSceBgft.sprx", "sceBgftServiceIntDebugDownloadRegisterPkg");
    p_bgft_register_task = (int (*)(OrbisBgftDownloadParam *, OrbisBgftTaskId *))sys_sym(
        "libSceBgft.sprx", "sceBgftServiceIntDownloadRegisterTask");
    p_bgft_progress = (int (*)(OrbisBgftTaskId, OrbisBgftTaskProgress *))sys_sym(
        "libSceBgft.sprx", "sceBgftServiceIntDownloadGetProgress");
    if (!p_bgft_progress)
        p_bgft_progress = (int (*)(OrbisBgftTaskId, OrbisBgftTaskProgress *))sys_sym(
            "libSceBgft.sprx", "sceBgftServiceDownloadGetProgress");
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
    g_last_task = -1;
    int r;
    if (!strncmp(path, "http://", 7) || !strncmp(path, "https://", 8)) {
        // Network installs (install from PC) go through the debug package
        // registration, which downloads from a URL, for the signed-in user.
        p.params.userId = plat_user_id();
        r = (int)0x80990000;
        if (p_bgft_register_url)
            r = p_bgft_register_url(&p.params, &task);
        if (r < 0 && r != (int)0x80990088 && p_bgft_register_task) {
            int r2 = p_bgft_register_task(&p.params, &task);
            if (r2 >= 0 || r2 == (int)0x80990088)
                r = r2;
        }
    } else {
        r = p_bgft_register(&p, &task);
    }
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
    g_last_task = task;
    sb_appendf(out, "Install started for %s. Progress shows in Notifications > Downloads.", content_id);
    return 0;
}

int plat_last_install_task(void) { return g_last_task; }

int plat_install_progress(int task, unsigned long long *done, unsigned long long *total, int *state, int *err)
{
    if (bgft_init() < 0 || !p_bgft_progress || task < 0)
        return -1;
    OrbisBgftTaskProgress pr;
    memset(&pr, 0, sizeof(pr));
    int r = p_bgft_progress(task, &pr);
    if (r < 0)
        return -1;
    *total = pr.length ? pr.length : pr.lengthTotal;
    *done = pr.length ? pr.transferred : pr.transferredTotal;
    *err = pr.errorResult;
    *state = pr.errorResult ? -1 : (*total && *done >= *total) ? 1 : 0;
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
