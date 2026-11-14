// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Save data backups with versions. Saves live in
//   /user/home/<user id hex>/savedata/<title id>/   (sdimg_* images + *.bin keys)
// A backup is <root>/<title id>/<stamp>/<user id>/... plus <stamp>/info.txt,
// where <root> is <dir>/save-backups or /mnt/usbN/claude-save-backups.
// Saves are encrypted for this console and user, so a backup restores here.
#include "saves.h"
#include "actions.h"
#include "plat.h"
#include "tools.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KEEP_VERSIONS 20

static char g_home[128] = "/user/home";
static char g_usb[64] = "/mnt/usb";

void saves_set_paths(const char *home, const char *usb_prefix)
{
    snprintf(g_home, sizeof(g_home), "%s", home);
    snprintf(g_usb, sizeof(g_usb), "%s", usb_prefix);
}

static int fail(StrBuf *out, const char *fmt, const char *arg)
{
    sb_appendf(out, fmt, arg ? arg : "");
    return 1;
}

static int title_ok(const char *t)
{
    if (!t || strlen(t) != 9)
        return 0;
    for (int i = 0; i < 4; i++)
        if (t[i] < 'A' || t[i] > 'Z')
            return 0;
    for (int i = 4; i < 9; i++)
        if (t[i] < '0' || t[i] > '9')
            return 0;
    return 1;
}

static int hex_user(const char *n)
{
    if (strlen(n) != 8)
        return 0;
    for (int i = 0; i < 8; i++)
        if (!strchr("0123456789abcdef", n[i]))
            return 0;
    return 1;
}

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    mkdir(tmp, 0777);
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// The PSN/local user name from <home>/<uid>/username.dat.
static void user_name(const char *uid, char *out, size_t n)
{
    char p[300];
    snprintf(p, sizeof(p), "%s/%s/username.dat", g_home, uid);
    out[0] = 0;
    FILE *f = fopen(p, "rb");
    if (!f)
        return;
    size_t r = fread(out, 1, n - 1, f);
    out[r] = 0;
    fclose(f);
    for (size_t i = 0; out[i]; i++)
        if ((unsigned char)out[i] < 0x20)
            out[i] = 0;
}

static void me(char *out, size_t n) { snprintf(out, n, "%08x", (unsigned)plat_user_id()); }

// Where backups go: "internal", "usb0", "usb1".
static int backup_root(const char *where, char *out, size_t n)
{
    if (!where || !where[0] || !strcmp(where, "internal")) {
        snprintf(out, n, "%s/save-backups", actions_dir());
        return 0;
    }
    if (!strncmp(where, "usb", 3) && (where[3] == '0' || where[3] == '1') && !where[4]) {
        char drive[80];
        snprintf(drive, sizeof(drive), "%s%c", g_usb, where[3]);
        if (!is_dir(drive))
            return -1;
        snprintf(out, n, "%s/claude-save-backups", drive);
        return 0;
    }
    return -1;
}

static int stamp_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// Sorted list of version folders (oldest first). Caller frees.
static int versions(const char *dir, char ***list)
{
    *list = NULL;
    int n = 0, cap = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            *list = realloc(*list, (size_t)cap * sizeof(char *));
        }
        (*list)[n++] = strdup(e->d_name);
    }
    if (d)
        closedir(d);
    if (n)
        qsort(*list, (size_t)n, sizeof(char *), stamp_cmp);
    return n;
}

static void free_list(char **l, int n)
{
    for (int i = 0; i < n; i++)
        free(l[i]);
    free(l);
}

// ---- list_game_saves ---------------------------------------------------------

static int t_list_game_saves(const JVal *in, StrBuf *out)
{
    const char *want = json_get_str(in, "title_id");
    char mine[16];
    me(mine, sizeof(mine));
    DIR *hd = opendir(g_home);
    if (!hd)
        return fail(out, "Cannot open %s (no full file access?)", g_home);
    struct dirent *u;
    int titles = 0;
    while ((u = readdir(hd))) {
        if (!hex_user(u->d_name))
            continue;
        char sd[300];
        snprintf(sd, sizeof(sd), "%s/%s/savedata", g_home, u->d_name);
        DIR *d = opendir(sd);
        if (!d)
            continue;
        int header = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!title_ok(e->d_name) || (want && strcmp(want, e->d_name)))
                continue;
            char p[600], name[128] = "", hs[32], when[32] = "";
            snprintf(p, sizeof(p), "%s/%s", sd, e->d_name);
            int count = 0;
            time_t newest = 0;
            DIR *sdir = opendir(p);
            struct dirent *f;
            while (sdir && (f = readdir(sdir))) {
                if (!strncmp(f->d_name, "sdimg_", 6))
                    count++;
                char fp[900];
                struct stat st;
                snprintf(fp, sizeof(fp), "%s/%s", p, f->d_name);
                if (f->d_name[0] != '.' && stat(fp, &st) == 0 && st.st_mtime > newest)
                    newest = st.st_mtime;
            }
            if (sdir)
                closedir(sdir);
            if (!header++) {
                char un[64];
                user_name(u->d_name, un, sizeof(un));
                sb_appendf(out, "User %s%s%s%s:\n", u->d_name, un[0] ? " " : "", un,
                           strcmp(u->d_name, mine) ? "" : " (signed in now)");
            }
            tools_app_title(e->d_name, name, sizeof(name));
            tools_human_size(tree_size(p), hs, sizeof(hs));
            if (newest)
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", gmtime(&newest));
            sb_appendf(out, "  %s  %-36.36s %d save%s, %s, changed %s\n", e->d_name, name[0] ? name : "?", count,
                       count == 1 ? "" : "s", hs, when);
            titles++;
        }
        closedir(d);
    }
    closedir(hd);
    if (!titles)
        sb_append(out, want ? "No save data for that game.\n" : "No save data found.\n");
    return 0;
}

// ---- backup_saves ------------------------------------------------------------

// Backs up one title for the given users (NULL = all) into root. 0 ok.
static int backup_title(const char *root, const char *tid, const char *only_user, const char *note, StrBuf *out,
                        char *stamp_out, size_t stamp_len)
{
    char tdir[400], vdir[500], stamp[40];
    snprintf(tdir, sizeof(tdir), "%s/%s", root, tid);
    mkdirs(tdir);
    time_t now = time(NULL);
    char base[32];
    strftime(base, sizeof(base), "%Y%m%d-%H%M%S", gmtime(&now));
    for (int i = 1; i < 100; i++) {
        snprintf(stamp, sizeof(stamp), "%s-%02d", base, i);
        snprintf(vdir, sizeof(vdir), "%s/%s", tdir, stamp);
        if (mkdir(vdir, 0777) == 0)
            break;
        if (errno != EEXIST)
            return fail(out, "Cannot create the backup folder: %s", plat_strerror(errno));
    }
    int users = 0, files = 0;
    unsigned long long bytes = 0;
    DIR *hd = opendir(g_home);
    struct dirent *u;
    while (hd && (u = readdir(hd))) {
        if (!hex_user(u->d_name) || (only_user && strcmp(only_user, u->d_name)))
            continue;
        char src[400], dst[700];
        snprintf(src, sizeof(src), "%s/%s/savedata/%s", g_home, u->d_name, tid);
        if (!is_dir(src))
            continue;
        snprintf(dst, sizeof(dst), "%s/%s", vdir, u->d_name);
        if (tools_copy_tree(src, dst, out, &files)) {
            closedir(hd);
            int c;
            StrBuf junk;
            sb_init(&junk);
            tools_delete_tree(vdir, &junk, &c);
            sb_free(&junk);
            return 1;
        }
        // Save names and icons shown in the system menu.
        char msrc[400], mdst[720];
        snprintf(msrc, sizeof(msrc), "%s/%s/savedata_meta/user/%s", g_home, u->d_name, tid);
        snprintf(mdst, sizeof(mdst), "%s/%s.meta", vdir, u->d_name);
        if (is_dir(msrc)) {
            int mf = 0;
            StrBuf junk;
            sb_init(&junk);
            tools_copy_tree(msrc, mdst, &junk, &mf);
            sb_free(&junk);
            bytes += tree_size(mdst);
        }
        bytes += tree_size(dst);
        users++;
    }
    if (hd)
        closedir(hd);
    if (!users) {
        rmdir(vdir);
        sb_appendf(out, "%s: no save data to back up\n", tid);
        return 2;
    }
    char ip[600], name[128] = "";
    snprintf(ip, sizeof(ip), "%s/info.txt", vdir);
    tools_app_title(tid, name, sizeof(name));
    FILE *f = fopen(ip, "wb");
    if (f) {
        fprintf(f, "%s\n%s\n", name, note ? note : "");
        fclose(f);
    }
    // Keep the newest versions only.
    char **l;
    int n = versions(tdir, &l);
    for (int i = 0; i + KEEP_VERSIONS < n; i++) {
        char old[700];
        int c;
        StrBuf junk;
        sb_init(&junk);
        snprintf(old, sizeof(old), "%s/%s", tdir, l[i]);
        tools_delete_tree(old, &junk, &c);
        sb_free(&junk);
    }
    free_list(l, n);
    char hs[32];
    tools_human_size(bytes, hs, sizeof(hs));
    sb_appendf(out, "%s %s: backed up %d file%s (%s) as %s/%s\n", tid, name, files, files == 1 ? "" : "s", hs, tid, stamp);
    if (stamp_out)
        snprintf(stamp_out, stamp_len, "%s", stamp);
    return 0;
}

static int t_backup_saves(const JVal *in, StrBuf *out)
{
    const char *tid = json_get_str(in, "title_id");
    const char *where = json_get_str(in, "destination");
    const char *note = json_get_str(in, "note");
    JVal *all = json_get(in, "all_users");
    char root[300], mine[16];
    if (backup_root(where, root, sizeof(root)))
        return fail(out, "destination must be internal, usb0 or usb1 (and the drive must be plugged in)", NULL);
    me(mine, sizeof(mine));
    const char *only = all && all->type == J_BOOL && all->boolean ? NULL : mine;
    if (tid && strcmp(tid, "all")) {
        if (!title_ok(tid))
            return fail(out, "title_id must look like CUSA00001 (or all)", NULL);
        int r = backup_title(root, tid, only, note, out, NULL, 0);
        return r == 1 ? 1 : r == 2 ? 1 : 0;
    }
    // Every game with saves for this user (or all users).
    int done = 0, failed = 0;
    DIR *hd = opendir(g_home);
    struct dirent *u;
    char seen[512][10];
    int nseen = 0;
    while (hd && (u = readdir(hd))) {
        if (!hex_user(u->d_name) || (only && strcmp(only, u->d_name)))
            continue;
        char sd[300];
        snprintf(sd, sizeof(sd), "%s/%s/savedata", g_home, u->d_name);
        DIR *d = opendir(sd);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!title_ok(e->d_name))
                continue;
            int dup = 0;
            for (int i = 0; i < nseen; i++)
                dup |= !strcmp(seen[i], e->d_name);
            if (dup || nseen >= 512)
                continue;
            snprintf(seen[nseen++], 10, "%s", e->d_name);
        }
        if (d)
            closedir(d);
    }
    if (hd)
        closedir(hd);
    for (int i = 0; i < nseen; i++) {
        int r = backup_title(root, seen[i], only, note, out, NULL, 0);
        if (r == 0)
            done++;
        else if (r == 1)
            failed++;
    }
    sb_appendf(out, "%d game%s backed up to %s%s.", done, done == 1 ? "" : "s", root, failed ? ", some failed" : "");
    return done == 0;
}

// ---- list_save_backups -------------------------------------------------------

static void list_root(const char *root, const char *want, StrBuf *out, int *count)
{
    DIR *d = opendir(root);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!title_ok(e->d_name) || (want && strcmp(want, e->d_name)))
            continue;
        char tdir[600];
        snprintf(tdir, sizeof(tdir), "%s/%s", root, e->d_name);
        char **l;
        int n = versions(tdir, &l);
        for (int i = n - 1; i >= 0; i--) {
            char v[800], info[900], name[128] = "", note[200] = "", hs[32];
            snprintf(v, sizeof(v), "%s/%s", tdir, l[i]);
            snprintf(info, sizeof(info), "%s/info.txt", v);
            FILE *f = fopen(info, "rb");
            if (f) {
                if (fgets(name, sizeof(name), f))
                    name[strcspn(name, "\n")] = 0;
                if (fgets(note, sizeof(note), f))
                    note[strcspn(note, "\n")] = 0;
                fclose(f);
            }
            tools_human_size(tree_size(v), hs, sizeof(hs));
            sb_appendf(out, "  %s  %s  %s%s%s%s\n", v, hs, name, note[0] ? " (" : "", note, note[0] ? ")" : "");
            (*count)++;
        }
        free_list(l, n);
    }
    if (d)
        closedir(d);
}

static int t_list_save_backups(const JVal *in, StrBuf *out)
{
    const char *want = json_get_str(in, "title_id");
    int count = 0;
    char root[300];
    static const char *const where[] = { "internal", "usb0", "usb1", NULL };
    for (int i = 0; where[i]; i++)
        if (!backup_root(where[i], root, sizeof(root)))
            list_root(root, want, out, &count);
    if (!count)
        sb_append(out, "No save backups yet. backup_saves makes one.");
    else
        sb_appendf(out, "%d backup%s, newest first per game. Restore one with restore_saves.", count, count == 1 ? "" : "s");
    return 0;
}

// ---- restore_saves -----------------------------------------------------------

static int t_restore_saves(Chat *chat, const JVal *in, StrBuf *out)
{
    const char *b = json_get_str(in, "backup");
    if (!b || b[0] != '/' || strstr(b, ".."))
        return fail(out, "backup must be a folder path from list_save_backups", NULL);
    // .../<root>/<TID>/<stamp>
    char path[600];
    snprintf(path, sizeof(path), "%s", b);
    size_t l = strlen(path);
    while (l > 1 && path[l - 1] == '/')
        path[--l] = 0;
    char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return fail(out, "not a save backup: %s", b);
    const char *stamp = slash + 1;
    *slash = 0;
    char *slash2 = strrchr(path, '/');
    const char *tid = slash2 ? slash2 + 1 : "";
    char tid_copy[16], stamp_copy[40];
    snprintf(tid_copy, sizeof(tid_copy), "%s", tid);
    snprintf(stamp_copy, sizeof(stamp_copy), "%s", stamp);
    *slash = '/';
    if (!title_ok(tid_copy) || stamp_copy[0] < '0' || stamp_copy[0] > '9' ||
        !(strstr(b, "/save-backups/") || strstr(b, "/claude-save-backups/")) || !is_dir(path))
        return fail(out, "not a save backup: %s", b);
    char name[128] = "";
    tools_app_title(tid_copy, name, sizeof(name));
    char q[1200];
    snprintf(q, sizeof(q),
             "Restore the saves of %s %s\nfrom the backup of %.8s %.2s:%.2s UTC?\nClose the game first. The current saves are "
             "backed up before they are replaced.",
             tid_copy, name, stamp_copy, stamp_copy + 9, stamp_copy + 11);
    if (!chat_ask_confirm(chat, q))
        return fail(out, "The user declined.", NULL);
    // Safety copy of what is there now.
    char root[300], safety[40] = "";
    backup_root("internal", root, sizeof(root));
    int r = backup_title(root, tid_copy, NULL, "automatic, before a restore", out, safety, sizeof(safety));
    if (r == 1)
        return fail(out, "\nThe restore was stopped because the current saves could not be backed up.", NULL);
    DIR *d = opendir(path);
    struct dirent *e;
    int users = 0, files = 0, err = 0;
    while (d && !err && (e = readdir(d))) {
        if (!hex_user(e->d_name))
            continue;
        char src[800], dst[400], userdir[300];
        snprintf(userdir, sizeof(userdir), "%s/%s", g_home, e->d_name);
        if (!is_dir(userdir)) {
            sb_appendf(out, "User %s is not on this PS4; skipped.\n", e->d_name);
            continue;
        }
        snprintf(src, sizeof(src), "%s/%s", path, e->d_name);
        snprintf(dst, sizeof(dst), "%s/savedata/%s", userdir, tid_copy);
        mkdirs(dst);
        err = tools_copy_tree(src, dst, out, &files);
        char msrc[820], mdst[420];
        snprintf(msrc, sizeof(msrc), "%s/%s.meta", path, e->d_name);
        snprintf(mdst, sizeof(mdst), "%s/savedata_meta/user/%s", userdir, tid_copy);
        if (!err && is_dir(msrc)) {
            int mf = 0;
            mkdirs(mdst);
            tools_copy_tree(msrc, mdst, out, &mf);
        }
        users++;
    }
    if (d)
        closedir(d);
    if (err)
        return fail(out, "\nThe restore failed part way. The earlier saves are in the automatic backup above.", NULL);
    if (!users)
        return fail(out, "That backup has no saves for a user on this PS4.", NULL);
    sb_appendf(out, "Restored %d file%s for %s %s. If the game shows the old saves, restart the PS4.", files,
               files == 1 ? "" : "s", tid_copy, name);
    return 0;
}

int saves_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out)
{
    if (!strcmp(name, "list_game_saves"))
        return t_list_game_saves(in, out);
    if (!strcmp(name, "backup_saves"))
        return t_backup_saves(in, out);
    if (!strcmp(name, "list_save_backups"))
        return t_list_save_backups(in, out);
    if (!strcmp(name, "restore_saves"))
        return t_restore_saves(chat, in, out);
    return -1;
}
