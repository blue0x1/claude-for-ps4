// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Install from PC: the bridge lists the .pkg files in its folder and serves
// them over HTTP; the PS4's own installer downloads them from there. Started
// installs are tracked so their progress can be shown and a notification
// pops up when each one finishes.
#include "pcinstall.h"
#include "plat.h"
#include "tools.h"
#include "version.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_INSTALLS 64

typedef struct {
    int task;
    char name[160];
    char title[128];
    unsigned long long size;
    time_t started;
    int state;      // 0 running, 1 done, -1 failed
    int notified;
} Install;

static Install g_inst[MAX_INSTALLS];
static int g_ninst;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int fail(StrBuf *out, const char *fmt, const char *arg)
{
    sb_appendf(out, fmt, arg ? arg : "");
    return 1;
}

static int installed_state(const char *cid, const char *type)
{
    if (!cid || strlen(cid) < 36)
        return 0;
    char tid[10], p[300];
    memcpy(tid, cid + 7, 9);
    tid[9] = 0;
    struct stat st;
    if (!strcmp(type, "PS4PATCH"))
        snprintf(p, sizeof(p), "/user/patch/%s", tid);
    else if (!strcmp(type, "PS4AC") || !strcmp(type, "PS4AL"))
        snprintf(p, sizeof(p), "/user/addcont/%s/%.16s", tid, cid + 20);
    else
        snprintf(p, sizeof(p), "/user/app/%s", tid);
    return stat(p, &st) == 0;
}

static JVal *fetch_list(StrBuf *out)
{
    if (!plat_bridge_url()[0]) {
        fail(out, "Install from PC needs PC bridge mode (claude-bridge.py running on the PC).", NULL);
        return NULL;
    }
    StrBuf body;
    sb_init(&body);
    if (plat_bridge_get("/v1/packages", &body) || !body.data) {
        sb_free(&body);
        fail(out, "Could not get the package list from the PC bridge. Is it running and up to date?", NULL);
        return NULL;
    }
    JVal *v = json_parse(body.data);
    sb_free(&body);
    JVal *pk = v ? json_get(v, "packages") : NULL;
    if (!pk || pk->type != J_ARR) {
        json_free(v);
        fail(out, "The PC bridge sent a bad package list.", NULL);
        return NULL;
    }
    return v;
}

static const char *kind_word(const char *type)
{
    if (!strcmp(type, "PS4PATCH"))
        return "update";
    if (!strcmp(type, "PS4AC") || !strcmp(type, "PS4AL"))
        return "DLC";
    return "game";
}

static double num(const JVal *o, const char *k)
{
    JVal *v = json_get(o, k);
    return v && v->type == J_NUM ? v->num : 0;
}

static int t_list_pc_packages(StrBuf *out)
{
    JVal *v = fetch_list(out);
    if (!v)
        return 1;
    JVal *pk = json_get(v, "packages");
    const char *dir = json_get_str(v, "dir");
    if (pk->count == 0) {
        sb_appendf(out, "No .pkg files in the bridge folder on the PC (%s). Put them there and ask again.", dir ? dir : "?");
        json_free(v);
        return 0;
    }
    sb_appendf(out, "Packages on the PC (%s):\n", dir ? dir : "?");
    for (int i = 0; i < pk->count; i++) {
        const JVal *p = pk->items[i];
        const char *name = json_get_str(p, "name"), *title = json_get_str(p, "title");
        const char *cid = json_get_str(p, "content_id"), *type = json_get_str(p, "type"), *ver = json_get_str(p, "version");
        char hs[32];
        tools_human_size((unsigned long long)num(p, "size"), hs, sizeof(hs));
        if (!type)
            type = "PS4GD";
        sb_appendf(out, "  %s  (%s)  %s %s%s%s  %s%s\n", name ? name : "?", hs, kind_word(type),
                   title && title[0] ? title : "", ver && ver[0] ? " v" : "", ver ? ver : "", cid ? cid : "",
                   installed_state(cid, type) ? "  [already installed]" : "");
    }
    json_free(v);
    sb_append(out, "Install with install_from_pc (names as listed).");
    return 0;
}

static int t_install_from_pc(Chat *chat, const JVal *in, StrBuf *out)
{
    JVal *names = json_get(in, "names");
    if (!names || names->type != J_ARR || names->count == 0)
        return fail(out, "names must list package names from list_pc_packages", NULL);
    JVal *v = fetch_list(out);
    if (!v)
        return 1;
    JVal *pk = json_get(v, "packages");
    const JVal *pick[32];
    int n = 0;
    unsigned long long total = 0;
    StrBuf q;
    sb_init(&q);
    for (int i = 0; i < names->count && n < 32; i++) {
        const char *want = names->items[i]->type == J_STR ? names->items[i]->str : "";
        const JVal *hit = NULL;
        for (int k = 0; k < pk->count && !hit; k++) {
            const char *nm = json_get_str(pk->items[k], "name");
            const char *base = nm ? strrchr(nm, '/') : NULL;
            if (nm && (!strcmp(nm, want) || (base && !strcmp(base + 1, want))))
                hit = pk->items[k];
        }
        if (!hit) {
            sb_free(&q);
            json_free(v);
            return fail(out, "No package named %s on the PC (see list_pc_packages).", want);
        }
        pick[n++] = hit;
        total += (unsigned long long)num(hit, "size");
        const char *title = json_get_str(hit, "title");
        if (n <= 8)
            sb_appendf(&q, "%s\n", title && title[0] ? title : json_get_str(hit, "name"));
    }
    char hs[32], head[160];
    tools_human_size(total, hs, sizeof(hs));
    snprintf(head, sizeof(head), "Install %d package%s from the PC (%s)?\n", n, n == 1 ? "" : "s", hs);
    StrBuf full;
    sb_init(&full);
    sb_append(&full, head);
    sb_append(&full, q.data ? q.data : "");
    sb_free(&q);
    int ok = chat_ask_confirm(chat, full.data);
    sb_free(&full);
    if (!ok) {
        json_free(v);
        return fail(out, "The user declined.", NULL);
    }
    int started = 0;
    for (int i = 0; i < n; i++) {
        const JVal *p = pick[i];
        const char *cid = json_get_str(p, "content_id"), *type = json_get_str(p, "type");
        const char *url = json_get_str(p, "url"), *name = json_get_str(p, "name"), *title = json_get_str(p, "title");
        if (!cid || !type || !url || strlen(cid) != 36) {
            sb_appendf(out, "%s: bad package info, skipped\n", name ? name : "?");
            continue;
        }
        char full_url[700];
        if (!strncmp(url, "http://", 7))
            snprintf(full_url, sizeof(full_url), "%s", url);   // bridge with --https: plain package port
        else
            snprintf(full_url, sizeof(full_url), "%s%s", plat_bridge_url(), url);
        StrBuf r;
        sb_init(&r);
        unsigned long long size = (unsigned long long)num(p, "size");
        const char *shown = title && title[0] ? title : name;
        int e = plat_install_pkg(full_url, cid, type, size, shown, &r);
        sb_appendf(out, "%s: %s\n", shown, r.data ? r.data : "");
        sb_free(&r);
        if (e)
            continue;
        started++;
        pthread_mutex_lock(&g_lock);
        if (g_ninst == MAX_INSTALLS) {
            memmove(g_inst, g_inst + 1, sizeof(Install) * (MAX_INSTALLS - 1));
            g_ninst--;
        }
        Install *it = &g_inst[g_ninst++];
        memset(it, 0, sizeof(*it));
        it->task = plat_last_install_task();
        snprintf(it->name, sizeof(it->name), "%s", name);
        snprintf(it->title, sizeof(it->title), "%s", shown);
        it->size = size;
        it->started = time(NULL);
        pthread_mutex_unlock(&g_lock);
    }
    json_free(v);
    if (!started)
        return 1;
    sb_appendf(out, "%d install%s queued. install_status shows progress; a notification pops up when each finishes.",
               started, started == 1 ? "" : "s");
    return 0;
}

// ---- app update check ----------------------------------------------------------
// Only checks: the PS4 installer will not install over the installed app, and
// replacing the installed package by hand is not safe, so the user updates by
// uninstalling and installing the new package.

static int t_check_app_update(StrBuf *out)
{
    if (!plat_bridge_url()[0])
        return fail(out, "Update checks use the PC bridge; this PS4 is not using one.", NULL);
    StrBuf body;
    sb_init(&body);
    if (plat_bridge_get("/v1/app-update", &body) || !body.data) {
        sb_free(&body);
        return fail(out, "The PC bridge did not answer the update check. Update claude-bridge.py on the PC.", NULL);
    }
    JVal *v = json_parse(body.data);
    sb_free(&body);
    const char *ver = v ? json_get_str(v, "version") : NULL;
    const char *file = v ? json_get_str(v, "file") : NULL;
    if (!ver || !ver[0])
        sb_appendf(out, "This is version %s. The PC has no Claude for PS4 package to compare with "
                        "(put the .pkg next to claude-bridge.py).", APP_VERSION);
    else if (strcmp(ver, APP_VERSION) <= 0)
        sb_appendf(out, "Up to date: this is version %s; the PC has %s.", APP_VERSION, ver);
    else
        sb_appendf(out, "Version %s is available on the PC (this is %s). To update: copy %s from the PC to "
                        "/data/pkg on the PS4, uninstall Claude for PS4 (chats and settings in /data/claude stay), "
                        "then install the new package.",
                   ver, APP_VERSION, file && file[0] ? file : "the new package");
    json_free(v);
    return 0;
}

static int t_install_status(StrBuf *out)
{
    pthread_mutex_lock(&g_lock);
    int n = g_ninst;
    Install copy[MAX_INSTALLS];
    memcpy(copy, g_inst, sizeof(Install) * (size_t)n);
    pthread_mutex_unlock(&g_lock);
    if (!n) {
        sb_append(out, "No installs started from this app since it opened. (Installs also show in Notifications > Downloads.)");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        Install *it = &copy[i];
        unsigned long long done = 0, total = 0;
        int state = 0, err = 0;
        char a[32], b[32];
        if (plat_install_progress(it->task, &done, &total, &state, &err) < 0) {
            sb_appendf(out, "%s: progress not available (see Notifications > Downloads)\n", it->title);
            continue;
        }
        if (!total)
            total = it->size;
        tools_human_size(done, a, sizeof(a));
        tools_human_size(total, b, sizeof(b));
        if (state < 0) {
            sb_appendf(out, "%s: FAILED (error 0x%08X)\n", it->title, (unsigned)err);
        } else if (state > 0) {
            sb_appendf(out, "%s: installed (%s)\n", it->title, b);
        } else {
            double pct = total ? 100.0 * (double)done / (double)total : 0;
            long secs = (long)(time(NULL) - it->started);
            sb_appendf(out, "%s: %.0f%% (%s of %s)", it->title, pct, a, b);
            if (done > 0 && secs > 5) {
                double rate = (double)done / (double)secs;
                long left = (long)((double)(total - done) / rate);
                sb_appendf(out, ", %.1f MB/s, about %ld min left", rate / 1048576.0, left / 60 + 1);
            }
            sb_append(out, "\n");
        }
    }
    return 0;
}

void pcinstall_tick(void)
{
    static time_t last;
    time_t now = time(NULL);
    if (now - last < 5)
        return;
    last = now;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ninst; i++) {
        Install *it = &g_inst[i];
        if (it->notified)
            continue;
        unsigned long long d, t;
        int state = 0, err = 0;
        if (plat_install_progress(it->task, &d, &t, &state, &err) < 0 || state == 0)
            continue;
        it->state = state;
        it->notified = 1;
        char msg[200];
        snprintf(msg, sizeof(msg), state > 0 ? "Installed: %s" : "Install failed: %s", it->title);
        plat_notify(msg);
    }
    pthread_mutex_unlock(&g_lock);
}

int pcinstall_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out)
{
    if (!strcmp(name, "list_pc_packages"))
        return t_list_pc_packages(out);
    if (!strcmp(name, "install_from_pc"))
        return t_install_from_pc(chat, in, out);
    if (!strcmp(name, "check_app_update"))
        return t_check_app_update(out);
    if (!strcmp(name, "install_status"))
        return t_install_status(out);
    return -1;
}
