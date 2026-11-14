// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Permission modes and things Claude remembers about the user.
//   full      every change asks on screen (default)
//   trusted   safe, undoable changes run without asking; deletes and
//             other risky actions still ask
//   read-only Claude can look but not change anything
//   kid       read-only, plus a kid-friendly tone; no payloads, launches...
// Claude can make the mode stricter itself. Only the user can loosen it,
// on the phone settings page (behind the PIN login).
#include "prefs.h"
#include "actions.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MEM 50
#define MEM_LEN 240

static const char *const MODES[] = { "kid", "read-only", "full", "trusted", NULL };  // strictest first

static char g_mode[16] = "";
static char g_mem[MAX_MEM][MEM_LEN];
static int g_nmem = -1;   // -1 = not loaded yet
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Changes that are safe and can be undone (trusted mode skips their confirm).
static const char *const SAFE[] = {
    "make_directory", "copy_path", "install_pkg", "install_from_pc", "create_home_folder",
    "move_to_home_folder", "rename_home_folder", "restore_home_backup", "set_plugin", "set_fan_profile",
    "set_fan_threshold", "restore_from_trash", "backup_saves", "remember", "forget",
    "delete_path", "delete_paths",   // only when they go to the trash (tools.c checks)
    NULL,
};

// Tools blocked even with a confirm in kid mode (all changes are, too).
static const char *const KID_BLOCKED[] = { "launch_app", "view_image", NULL };

static void path_of(const char *name, char *p, size_t n) { snprintf(p, n, "%s/%s", actions_dir(), name); }

static int mode_rank(const char *m)
{
    for (int i = 0; MODES[i]; i++)
        if (!strcmp(MODES[i], m))
            return i;
    return -1;
}

// Lock held.
static void load(void)
{
    if (g_nmem >= 0)
        return;
    g_nmem = 0;
    char p[300], line[MEM_LEN + 8];
    path_of("permissions.conf", p, sizeof(p));
    FILE *f = fopen(p, "rb");
    snprintf(g_mode, sizeof(g_mode), "full");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (mode_rank(line) >= 0)
                snprintf(g_mode, sizeof(g_mode), "%s", line);
        }
        fclose(f);
    }
    path_of("memory.txt", p, sizeof(p));
    f = fopen(p, "rb");
    while (f && g_nmem < MAX_MEM && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0])
            snprintf(g_mem[g_nmem++], MEM_LEN, "%s", line);
    }
    if (f)
        fclose(f);
}

static void save_mode(void)
{
    char p[300];
    path_of("permissions.conf", p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (f) {
        fprintf(f, "%s\n", g_mode);
        fclose(f);
    }
}

static void save_mem(void)
{
    char p[300], tmp[310];
    path_of("memory.txt", p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    for (int i = 0; i < g_nmem; i++)
        fprintf(f, "%s\n", g_mem[i]);
    fclose(f);
    rename(tmp, p);
}

void prefs_reset(void)
{
    pthread_mutex_lock(&g_lock);
    g_nmem = -1;
    pthread_mutex_unlock(&g_lock);
}

const char *prefs_mode(void)
{
    pthread_mutex_lock(&g_lock);
    load();
    pthread_mutex_unlock(&g_lock);
    return g_mode;
}

int prefs_set_mode(const char *mode)
{
    if (!mode || mode_rank(mode) < 0)
        return -1;
    pthread_mutex_lock(&g_lock);
    load();
    snprintf(g_mode, sizeof(g_mode), "%s", mode);
    save_mode();
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int in_list(const char *const *l, const char *name)
{
    for (int i = 0; l[i]; i++)
        if (!strcmp(l[i], name))
            return 1;
    return 0;
}

int prefs_check(const char *tool, int read_only, int *auto_ok, StrBuf *out)
{
    const char *m = prefs_mode();
    *auto_ok = 0;
    // Tools that only make the rules stricter or read them are always fine.
    if (!strcmp(tool, "set_permission_mode") || !strcmp(tool, "get_preferences"))
        return 0;
    if (!strcmp(m, "kid") && in_list(KID_BLOCKED, tool)) {
        sb_appendf(out, "Kid mode is on: %s is not allowed. A parent can change this on the phone settings page.", tool);
        return 1;
    }
    if (!read_only && (!strcmp(m, "kid") || !strcmp(m, "read-only"))) {
        if (!strcmp(tool, "remember") || !strcmp(tool, "forget") || !strcmp(tool, "show_notification"))
            return 0;
        sb_appendf(out, "%s mode is on, so Claude can look but not change anything (%s was blocked). "
                        "The user can change the mode on the phone settings page.",
                   !strcmp(m, "kid") ? "Kid" : "Read-only", tool);
        return 1;
    }
    if (!strcmp(m, "trusted") && in_list(SAFE, tool))
        *auto_ok = 1;
    return 0;
}

void prefs_system_extra(StrBuf *out)
{
    pthread_mutex_lock(&g_lock);
    load();
    if (!strcmp(g_mode, "kid"))
        sb_append(out, "\n\nKid mode is on: a child may be using this. Keep answers friendly, simple and "
                       "age-appropriate, and do not help with anything unsafe.");
    else if (!strcmp(g_mode, "read-only"))
        sb_append(out, "\n\nRead-only mode is on: console tools may look but every change is blocked. "
                       "Explain what you would do instead.");
    if (g_nmem > 0) {
        sb_append(out, "\n\nWhat the user asked you to remember (use it; the remember and forget tools change it):");
        for (int i = 0; i < g_nmem; i++)
            sb_appendf(out, "\n%d. %s", i + 1, g_mem[i]);
    }
    pthread_mutex_unlock(&g_lock);
}

static int t_remember(const JVal *in, StrBuf *out)
{
    const char *t = json_get_str(in, "text");
    if (!t || !t[0])
        return sb_append(out, "text is required"), 1;
    char clean[MEM_LEN];
    size_t j = 0;
    for (size_t i = 0; t[i] && j < MEM_LEN - 1; i++)
        clean[j++] = (t[i] == '\n' || t[i] == '\r' || t[i] == '\t') ? ' ' : t[i];
    clean[j] = 0;
    pthread_mutex_lock(&g_lock);
    load();
    for (int i = 0; i < g_nmem; i++)
        if (!strcmp(g_mem[i], clean)) {
            pthread_mutex_unlock(&g_lock);
            sb_append(out, "Already remembered.");
            return 0;
        }
    if (g_nmem == MAX_MEM) {
        memmove(g_mem[0], g_mem[1], (size_t)(MAX_MEM - 1) * MEM_LEN);
        g_nmem--;
    }
    snprintf(g_mem[g_nmem++], MEM_LEN, "%s", clean);
    save_mem();
    int n = g_nmem;
    pthread_mutex_unlock(&g_lock);
    sb_appendf(out, "Remembered (%d things saved). It is used in every chat from now on.", n);
    return 0;
}

static int t_forget(const JVal *in, StrBuf *out)
{
    JVal *num = json_get(in, "number");
    const char *t = json_get_str(in, "text");
    pthread_mutex_lock(&g_lock);
    load();
    int idx = -1;
    if (num && num->type == J_NUM)
        idx = (int)num->num - 1;
    else if (t && !strcmp(t, "all"))
        idx = -2;
    else if (t)
        for (int i = 0; i < g_nmem && idx < 0; i++)
            if (strstr(g_mem[i], t))
                idx = i;
    if (idx == -2) {
        g_nmem = 0;
        save_mem();
        pthread_mutex_unlock(&g_lock);
        sb_append(out, "Forgot everything.");
        return 0;
    }
    if (idx < 0 || idx >= g_nmem) {
        pthread_mutex_unlock(&g_lock);
        sb_append(out, "Nothing like that is remembered (see get_preferences).");
        return 1;
    }
    char was[MEM_LEN];
    snprintf(was, sizeof(was), "%s", g_mem[idx]);
    memmove(g_mem[idx], g_mem[idx + 1], (size_t)(g_nmem - idx - 1) * MEM_LEN);
    g_nmem--;
    save_mem();
    pthread_mutex_unlock(&g_lock);
    sb_appendf(out, "Forgot: %s", was);
    return 0;
}

static int t_get_preferences(StrBuf *out)
{
    pthread_mutex_lock(&g_lock);
    load();
    sb_appendf(out, "Permission mode: %s\n", g_mode);
    if (!g_nmem)
        sb_append(out, "Nothing remembered yet.\n");
    for (int i = 0; i < g_nmem; i++)
        sb_appendf(out, "%d. %s\n", i + 1, g_mem[i]);
    pthread_mutex_unlock(&g_lock);
    sb_append(out, "Modes: full (asks before every change), trusted (safe changes without asking), "
                   "read-only, kid. Claude can only make it stricter; the user loosens it on the phone settings page.");
    return 0;
}

static int t_set_permission_mode(const JVal *in, StrBuf *out)
{
    const char *m = json_get_str(in, "mode");
    if (!m || mode_rank(m) < 0)
        return sb_append(out, "mode must be kid, read-only, full or trusted"), 1;
    const char *now = prefs_mode();
    if (mode_rank(m) > mode_rank(now)) {
        sb_appendf(out, "Claude cannot loosen the permission mode (now %s). The user can change it on the phone "
                        "settings page (Options on the PS4 shows the address).",
                   now);
        return 1;
    }
    prefs_set_mode(m);
    sb_appendf(out, "Permission mode is now %s.", m);
    return 0;
}

int prefs_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out)
{
    (void)chat;
    if (!strcmp(name, "remember"))
        return t_remember(in, out);
    if (!strcmp(name, "forget"))
        return t_forget(in, out);
    if (!strcmp(name, "get_preferences"))
        return t_get_preferences(out);
    if (!strcmp(name, "set_permission_mode"))
        return t_set_permission_mode(in, out);
    return -1;
}
