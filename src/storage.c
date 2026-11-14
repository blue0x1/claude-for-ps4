// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Storage report: what uses the space (games with their updates and DLC,
// captures, big files), installers for things already installed, and
// updates or DLC left behind by games that are gone.
#include "storage.h"
#include "actions.h"
#include "tools.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char g_user[64] = "/user";

void storage_set_root(const char *user_root) { snprintf(g_user, sizeof(g_user), "%s", user_root); }

typedef struct {
    char id[16];
    char name[128];
    unsigned long long app, patch, dlc;
} GameUse;

static int title_dir(const char *n)
{
    if (strlen(n) != 9)
        return 0;
    for (int i = 0; i < 4; i++)
        if (n[i] < 'A' || n[i] > 'Z')
            return 0;
    for (int i = 4; i < 9; i++)
        if (n[i] < '0' || n[i] > '9')
            return 0;
    return 1;
}

static int use_cmp(const void *a, const void *b)
{
    const GameUse *x = a, *y = b;
    unsigned long long tx = x->app + x->patch + x->dlc, ty = y->app + y->patch + y->dlc;
    return tx < ty ? 1 : tx > ty ? -1 : 0;
}

static void size_str(unsigned long long b, char *out, size_t n) { tools_human_size(b, out, n); }

static int exists(const char *fmt, const char *a, const char *b)
{
    char p[600];
    snprintf(p, sizeof(p), fmt, g_user, a, b ? b : "");
    struct stat st;
    return stat(p, &st) == 0;
}

// .pkg files: what they are and whether that content is already installed.
static void report_pkgs(const char *dir, int depth, StrBuf *out, int *n, unsigned long long *redundant)
{
    if (depth < 0 || *n >= 300)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && *n < 300) {
        if (e->d_name[0] == '.')
            continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (!in_trash(p))
                report_pkgs(p, depth - 1, out, n, redundant);
            continue;
        }
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".pkg"))
            continue;
        char cid[37], hs[32], name[128] = "";
        const char *type = "";
        unsigned long long size = (unsigned long long)st.st_size;
        size_str(size, hs, sizeof(hs));
        (*n)++;
        if (!pkg_read_info(p, cid, &type, &size)) {
            sb_appendf(out, "  %s (%s): not a readable PS4 package\n", p, hs);
            continue;
        }
        char tid[10];
        memcpy(tid, cid + 7, 9);
        tid[9] = 0;
        tools_app_title(tid, name, sizeof(name));
        const char *kind = "game", *state;
        int installed;
        if (!strcmp(type, "PS4PATCH")) {
            kind = "update";
            installed = exists("%s/patch/%s%s", tid, NULL);
            state = installed ? "an update for this game is installed (check it is this version or newer)"
                              : exists("%s/app/%s%s", tid, NULL) ? "not installed yet" : "game not installed";
        } else if (!strcmp(type, "PS4AC") || !strcmp(type, "PS4AL")) {
            kind = "DLC";
            char label[17];
            memcpy(label, cid + 20, 16);
            label[16] = 0;
            installed = exists("%s/addcont/%s/%s", tid, label);
            state = installed ? "already installed" : "not installed";
        } else {
            installed = exists("%s/app/%s%s", tid, NULL);
            state = installed ? "already installed" : "not installed";
        }
        if (installed)
            *redundant += size;
        sb_appendf(out, "  %s (%s): %s %s%s%s, %s%s\n", p, hs, kind, tid, name[0] ? " " : "", name, state,
                   installed ? " -> the installer can be removed" : "");
    }
    closedir(d);
}

// Folders or files in dir bigger than min bytes (skipping the given names).
static void big_items(const char *dir, unsigned long long min, const char *const *skip, StrBuf *out, int *found)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        int skipped = 0;
        for (int i = 0; skip && skip[i]; i++)
            skipped |= !strcmp(e->d_name, skip[i]);
        if (skipped)
            continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unsigned long long sz = tree_size(p);
        if (sz < min)
            continue;
        char hs[32];
        size_str(sz, hs, sizeof(hs));
        sb_appendf(out, "  %s  %s\n", p, hs);
        (*found)++;
    }
    closedir(d);
}

int storage_report(StrBuf *out)
{
    char hs[32], hs2[32], p[600];
    unsigned long long f, t;
    if (plat_free_space(g_user, &f, &t) == 0) {
        size_str(f, hs, sizeof(hs));
        size_str(t, hs2, sizeof(hs2));
        sb_appendf(out, "Internal storage: %s free of %s (%.0f%% used)\n", hs, hs2, t ? 100.0 * (double)(t - f) / (double)t : 0);
    }
    for (int u = 0; u < 2; u++) {
        snprintf(p, sizeof(p), "/mnt/usb%d", u);
        if (plat_free_space(p, &f, &t) == 0 && t) {
            size_str(f, hs, sizeof(hs));
            size_str(t, hs2, sizeof(hs2));
            sb_appendf(out, "USB %d: %s free of %s\n", u, hs, hs2);
        }
    }

    // Games with their updates and DLC.
    GameUse *g = NULL;
    int n = 0, cap = 0;
    snprintf(p, sizeof(p), "%s/app", g_user);
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!title_dir(e->d_name))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            g = realloc(g, (size_t)cap * sizeof(GameUse));
        }
        GameUse *u = &g[n++];
        memset(u, 0, sizeof(*u));
        snprintf(u->id, sizeof(u->id), "%s", e->d_name);
        tools_app_title(u->id, u->name, sizeof(u->name));
        snprintf(p, sizeof(p), "%s/app/%s", g_user, u->id);
        u->app = tree_size(p);
        snprintf(p, sizeof(p), "%s/patch/%s", g_user, u->id);
        u->patch = tree_size(p);
        snprintf(p, sizeof(p), "%s/addcont/%s", g_user, u->id);
        u->dlc = tree_size(p);
    }
    if (d)
        closedir(d);
    if (n)
        qsort(g, (size_t)n, sizeof(GameUse), use_cmp);
    unsigned long long games_total = 0;
    for (int i = 0; i < n; i++)
        games_total += g[i].app + g[i].patch + g[i].dlc;
    size_str(games_total, hs, sizeof(hs));
    sb_appendf(out, "\nGames and apps: %d installed, %s with updates and DLC. Largest first:\n", n, hs);
    for (int i = 0; i < n && i < 25; i++) {
        char a[32], pa[32], dl[32], tot[32];
        size_str(g[i].app + g[i].patch + g[i].dlc, tot, sizeof(tot));
        size_str(g[i].app, a, sizeof(a));
        size_str(g[i].patch, pa, sizeof(pa));
        size_str(g[i].dlc, dl, sizeof(dl));
        sb_appendf(out, "  %s  %-40.40s %9s  (game %s", g[i].id, g[i].name[0] ? g[i].name : "?", tot, a);
        if (g[i].patch)
            sb_appendf(out, ", update %s", pa);
        if (g[i].dlc)
            sb_appendf(out, ", DLC %s", dl);
        sb_append(out, ")\n");
    }
    if (n > 25)
        sb_appendf(out, "  ... and %d smaller ones\n", n - 25);

    // Updates and DLC whose game is not installed.
    int orphans = 0;
    static const char *const kinds[2] = { "patch", "addcont" };
    for (int k = 0; k < 2; k++) {
        snprintf(p, sizeof(p), "%s/%s", g_user, kinds[k]);
        d = opendir(p);
        while (d && (e = readdir(d))) {
            if (!title_dir(e->d_name) || exists("%s/app/%s%s", e->d_name, NULL))
                continue;
            char q[600], name[128] = "";
            snprintf(q, sizeof(q), "%s/%s/%s", g_user, kinds[k], e->d_name);
            unsigned long long sz = tree_size(q);
            if (!sz)
                continue;
            if (!orphans++)
                sb_append(out, "\nUpdates and DLC left without their game (or for a game played from disc):\n");
            tools_app_title(e->d_name, name, sizeof(name));
            size_str(sz, hs, sizeof(hs));
            sb_appendf(out, "  %s  %s %s%s%s\n", q, hs, k ? "DLC for" : "update for", name[0] ? " " : " ",
                       name[0] ? name : e->d_name);
        }
        if (d)
            closedir(d);
    }

    // Package files.
    sb_append(out, "\nPackage files (.pkg):\n");
    int pk = 0;
    unsigned long long redundant = 0;
    char dataroot[300];
    snprintf(dataroot, sizeof(dataroot), "%s/data", g_user);
    report_pkgs(dataroot, 3, out, &pk, &redundant);
    for (int u = 0; u < 2; u++) {
        snprintf(p, sizeof(p), "/mnt/usb%d", u);
        report_pkgs(p, 3, out, &pk, &redundant);
    }
    if (!pk)
        sb_append(out, "  none found\n");
    else if (redundant) {
        size_str(redundant, hs, sizeof(hs));
        sb_appendf(out, "  Installers for content that is already installed: %s.\n", hs);
    }

    // Captures and other big things.
    snprintf(p, sizeof(p), "%s/av_contents", g_user);
    size_str(tree_size(p), hs, sizeof(hs));
    sb_appendf(out, "\nScreenshots and videos (%s): %s\n", p, hs);
    sb_append(out, "Other items over 500 MB:\n");
    int big = 0;
    static const char *const data_skip[] = { "pkg", NULL };
    snprintf(p, sizeof(p), "%s/home", g_user);
    big_items(p, 500ull << 20, NULL, out, &big);
    big_items(dataroot, 500ull << 20, data_skip, out, &big);
    if (!big)
        sb_append(out, "  none\n");
    snprintf(p, sizeof(p), "%s/trash", actions_dir());
    unsigned long long tr = tree_size(p);
    if (tr) {
        size_str(tr, hs, sizeof(hs));
        sb_appendf(out, "Trash: %s (empty_trash frees it).\n", hs);
    }
    sb_append(out, "\nTo free space, remove items with delete_paths (one confirm for all). Uninstall games with "
                   "uninstall_app; deleting game folders directly is not safe.");
    free(g);
    return 0;
}
