// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Trash and action history. A trashed item lives in
//   <trash>/<id>/<name>      with <trash>/<id>/.origin holding the old path
// where <trash> is /mnt/usbN/.claude-trash for USB drives (so a move is a
// rename on the same drive) and <dir>/trash for everything else.
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

#define HISTORY_MAX_BYTES (256 * 1024)

static char g_dir[256] = "/data/claude";

void actions_set_dir(const char *dir) { snprintf(g_dir, sizeof(g_dir), "%s", dir); }
const char *actions_dir(void) { return g_dir; }

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

// The trash folder for a path: the USB drive's own, or the main one.
static void trash_root_for(const char *path, char *root, size_t n)
{
    if (!strncmp(path, "/mnt/usb", 8) && path[8] >= '0' && path[8] <= '9' && path[9] == '/')
        snprintf(root, n, "%.9s/.claude-trash", path);
    else
        snprintf(root, n, "%s/trash", g_dir);
}

int in_trash(const char *path)
{
    char root[300];
    trash_root_for(path, root, sizeof(root));
    size_t l = strlen(root);
    return !strncmp(path, root, l) && (path[l] == '/' || path[l] == 0);
}

unsigned long long tree_size(const char *path)
{
    struct stat st;
    if (plat_lstat(path, &st) < 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return (unsigned long long)st.st_size;
    unsigned long long total = 0;
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        total += tree_size(p);
    }
    closedir(d);
    return total;
}

static void stamp_id(char *id, size_t n, const char *root)
{
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", gmtime(&now));
    struct stat st;
    for (int i = 1; i < 100; i++) {
        char p[600];
        snprintf(id, n, "%s-%02d", stamp, i);
        snprintf(p, sizeof(p), "%s/%s", root, id);
        if (stat(p, &st) < 0)
            return;
    }
}

int trash_put(const char *path, char *id, size_t idlen, StrBuf *out)
{
    char root[300];
    trash_root_for(path, root, sizeof(root));
    mkdirs(root);
    stamp_id(id, idlen, root);
    char dir[600], dst[1200], origin[700];
    snprintf(dir, sizeof(dir), "%s/%s", root, id);
    if (mkdir(dir, 0777) < 0) {
        sb_appendf(out, "Could not make the trash folder %s: %s", dir, plat_strerror(errno));
        return 1;
    }
    const char *base = strrchr(path, '/');
    base = base && base[1] ? base + 1 : path;
    snprintf(dst, sizeof(dst), "%s/%s", dir, base);
    if (rename(path, dst) < 0) {
        int err = errno;
        rmdir(dir);
        if (err == EXDEV)
            sb_appendf(out, "%s is on another drive than its trash, so it can only be deleted permanently "
                            "(delete_path with permanent true).", path);
        else
            sb_appendf(out, "Could not move %s to the trash: %s", path, plat_strerror(err));
        return 1;
    }
    snprintf(origin, sizeof(origin), "%s/.origin", dir);
    FILE *f = fopen(origin, "wb");
    if (f) {
        fputs(path, f);
        fclose(f);
    }
    return 0;
}

// ---- trash listing ----------------------------------------------------------

typedef struct {
    char root[300];
    char id[40];
    char origin[1024];
    char item[1200];   // full path of the trashed file or folder
} TrashItem;

static int read_item(const char *root, const char *id, TrashItem *t)
{
    snprintf(t->root, sizeof(t->root), "%s", root);
    snprintf(t->id, sizeof(t->id), "%.39s", id);
    char p[700];
    snprintf(p, sizeof(p), "%s/%s/.origin", root, id);
    FILE *f = fopen(p, "rb");
    t->origin[0] = 0;
    if (f) {
        size_t n = fread(t->origin, 1, sizeof(t->origin) - 1, f);
        t->origin[n] = 0;
        fclose(f);
    }
    t->item[0] = 0;
    snprintf(p, sizeof(p), "%s/%s", root, id);
    DIR *d = opendir(p);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, ".origin")) {
            snprintf(t->item, sizeof(t->item), "%s/%s", p, e->d_name);
            break;
        }
    closedir(d);
    return t->item[0] != 0;
}

static int id_cmp(const void *a, const void *b)
{
    return strcmp(((const TrashItem *)a)->id, ((const TrashItem *)b)->id);
}

// All trash items on every drive, oldest first. Caller frees.
static int collect_trash(TrashItem **items)
{
    char roots[4][300];
    int nr = 0;
    snprintf(roots[nr++], 300, "%s/trash", g_dir);
    for (int u = 0; u < 3; u++)
        snprintf(roots[nr++], 300, "/mnt/usb%d/.claude-trash", u);
    int n = 0, cap = 0;
    *items = NULL;
    for (int r = 0; r < nr; r++) {
        DIR *d = opendir(roots[r]);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                *items = realloc(*items, (size_t)cap * sizeof(TrashItem));
            }
            if (read_item(roots[r], e->d_name, &(*items)[n]))
                n++;
        }
        closedir(d);
    }
    if (n)
        qsort(*items, (size_t)n, sizeof(TrashItem), id_cmp);
    return n;
}

static void id_date(const char *id, char *out, size_t n)
{
    // 20261114-064216-01 -> 2026-11-14 06:42 UTC
    if (strlen(id) >= 15)
        snprintf(out, n, "%.4s-%.2s-%.2s %.2s:%.2s UTC", id, id + 4, id + 6, id + 9, id + 11);
    else
        snprintf(out, n, "%s", id);
}

static int t_list_trash(StrBuf *out)
{
    TrashItem *items;
    int n = collect_trash(&items);
    if (!n) {
        sb_append(out, "The trash is empty.");
        free(items);
        return 0;
    }
    unsigned long long total = 0;
    sb_append(out, "Trash (oldest first):\n");
    for (int i = 0; i < n; i++) {
        unsigned long long sz = tree_size(items[i].item);
        total += sz;
        char hs[32], date[40];
        tools_human_size(sz, hs, sizeof(hs));
        id_date(items[i].id, date, sizeof(date));
        sb_appendf(out, "%s  %s  %s  (%s)\n", items[i].id, items[i].origin[0] ? items[i].origin : items[i].item, hs, date);
    }
    char hs[32];
    tools_human_size(total, hs, sizeof(hs));
    sb_appendf(out, "%d item%s, %s in total. empty_trash frees the space.", n, n == 1 ? "" : "s", hs);
    free(items);
    return 0;
}

static int find_item(const char *id, TrashItem *t, StrBuf *out)
{
    TrashItem *items;
    int n = collect_trash(&items), found = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(items[i].id, id)) {
            *t = items[i];
            found = 1;
        }
    free(items);
    if (!found)
        sb_appendf(out, "No trash item %s. Use list_trash for the IDs.", id ? id : "");
    return !found;
}

static int t_restore(const JVal *in, StrBuf *out)
{
    const char *id = json_get_str(in, "id");
    TrashItem t;
    if (!id || find_item(id, &t, out))
        return 1;
    if (!t.origin[0])
        return sb_appendf(out, "Trash item %s has no record of where it came from. It is at %s.", id, t.item), 1;
    struct stat st;
    if (plat_lstat(t.origin, &st) == 0)
        return sb_appendf(out, "Something already exists at %s, so %s was not put back. Move that away first.", t.origin,
                          id), 1;
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", t.origin);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        mkdirs(parent);
    }
    if (rename(t.item, t.origin) < 0)
        return sb_appendf(out, "Could not put %s back: %s", t.origin, plat_strerror(errno)), 1;
    char p[700];
    snprintf(p, sizeof(p), "%s/%s/.origin", t.root, t.id);
    unlink(p);
    snprintf(p, sizeof(p), "%s/%s", t.root, t.id);
    rmdir(p);
    sb_appendf(out, "Restored %s.", t.origin);
    return 0;
}

static int t_empty(Chat *chat, const JVal *in, StrBuf *out)
{
    const char *id = json_get_str(in, "id");
    JVal *age = json_get(in, "older_than_days");
    double days = age && age->type == J_NUM ? age->num : 0;
    TrashItem *items;
    int n = collect_trash(&items);
    // Pick the items: one ID, or all (older than the given days).
    time_t now = time(NULL);
    int pick = 0;
    unsigned long long total = 0;
    char *sel = calloc((size_t)(n ? n : 1), 1);
    for (int i = 0; i < n; i++) {
        if (id && *id) {
            sel[i] = !strcmp(items[i].id, id);
        } else if (days > 0) {
            struct stat st;
            char p[700];
            snprintf(p, sizeof(p), "%s/%s", items[i].root, items[i].id);
            sel[i] = stat(p, &st) == 0 && difftime(now, st.st_mtime) > days * 86400;
        } else {
            sel[i] = 1;
        }
        if (sel[i]) {
            pick++;
            total += tree_size(items[i].item);
        }
    }
    int r = 0;
    if (!pick) {
        sb_append(out, id && *id ? "No trash item with that ID. Use list_trash." : "Nothing in the trash matches.");
    } else {
        char hs[32], q[300];
        tools_human_size(total, hs, sizeof(hs));
        snprintf(q, sizeof(q), "PERMANENTLY delete %d trash item%s (%s)?\nThis cannot be undone.", pick,
                 pick == 1 ? "" : "s", hs);
        if (!chat_ask_confirm(chat, q)) {
            sb_append(out, "The user declined.");
            r = 1;
        } else {
            int count = 0;
            for (int i = 0; i < n && !r; i++) {
                if (!sel[i])
                    continue;
                char p[700];
                snprintf(p, sizeof(p), "%s/%s", items[i].root, items[i].id);
                r = tools_delete_tree(p, out, &count);
            }
            if (!r)
                sb_appendf(out, "Emptied %d trash item%s, freed %s.", pick, pick == 1 ? "" : "s", hs);
        }
    }
    free(sel);
    free(items);
    return r;
}

// ---- history ----------------------------------------------------------------

static const char *const READ_ONLY[] = {
    "get_system_info", "list_directory", "read_text_file", "find_pkg_files", "list_installed_apps",
    "list_screenshots", "view_image", "list_home_folders", "list_trash", "show_history", "show_notification",
    "storage_report", "list_save_backups", "list_game_saves", "temperature_history", "list_cheats",
    "list_plugins", "get_preferences", "install_status", "list_pc_packages",
    "list_game_patches", "goldhen_settings", "check_app_update", "health_report", "get_permissions", NULL,
};

int tool_is_read_only(const char *name)
{
    for (int i = 0; READ_ONLY[i]; i++)
        if (!strcmp(name, READ_ONLY[i]))
            return 1;
    return 0;
}

static void history_path(char *p, size_t n) { snprintf(p, n, "%s/history.log", g_dir); }

void history_add(Chat *chat, const char *tool, const char *what, int is_error, const char *result)
{
    char path[300];
    history_path(path, sizeof(path));
    mkdirs(g_dir);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > HISTORY_MAX_BYTES) {
        // Keep the newer half.
        FILE *f = fopen(path, "rb");
        if (f) {
            long keep = (long)st.st_size / 2;
            fseek(f, -keep, SEEK_END);
            char *buf = malloc((size_t)keep + 1);
            size_t n = buf ? fread(buf, 1, (size_t)keep, f) : 0;
            fclose(f);
            char *nl = buf ? memchr(buf, '\n', n) : NULL;
            f = fopen(path, "wb");
            if (f && nl)
                fwrite(nl + 1, 1, n - (size_t)(nl + 1 - buf), f);
            if (f)
                fclose(f);
            free(buf);
        }
    }
    FILE *f = fopen(path, "ab");
    if (!f)
        return;
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", gmtime(&now));
    const char *outcome = is_error ? (result && strstr(result, "The user declined") ? "declined" : "failed") : "done";
    const char *by = !chat ? "-" : chat->confirm_by == 1 ? "phone" : chat->confirm_by == 0 ? "TV"
                   : chat->confirm_by == 2 ? "auto" : "-";
    // One line per action; tabs and newlines in the text become spaces.
    char line[700];
    snprintf(line, sizeof(line), "%s UTC\t%s\t%s\t%s\t%s\t%.300s", stamp, by, tool, what ? what : "", outcome,
             result ? result : "");
    for (char *p = line; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    fprintf(f, "%s\n", line);
    fclose(f);
}

static int t_history(const JVal *in, StrBuf *out)
{
    JVal *c = json_get(in, "count");
    int want = c && c->type == J_NUM && c->num > 0 ? (int)c->num : 20;
    if (want > 200)
        want = 200;
    char path[300];
    history_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        sb_append(out, "No actions recorded yet.");
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > 64 * 1024 ? size - 64 * 1024 : 0;
    fseek(f, from, SEEK_SET);
    char *buf = malloc((size_t)(size - from) + 1);
    size_t n = buf ? fread(buf, 1, (size_t)(size - from), f) : 0;
    fclose(f);
    if (!buf)
        return sb_append(out, "out of memory"), 1;
    buf[n] = 0;
    // Start at the want-th line from the end.
    int lines = 0;
    char *start = buf + n;
    while (start > buf) {
        start--;
        if (*start == '\n' && start + 1 < buf + n && ++lines >= want) {
            start++;
            break;
        }
    }
    sb_append(out, "time\tapproved on\ttool\ttarget\tresult\tdetails\n");
    sb_append(out, start);
    free(buf);
    return 0;
}

int actions_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out)
{
    if (!strcmp(name, "list_trash"))
        return t_list_trash(out);
    if (!strcmp(name, "restore_from_trash"))
        return t_restore(in, out);
    if (!strcmp(name, "empty_trash"))
        return t_empty(chat, in, out);
    if (!strcmp(name, "show_history"))
        return t_history(in, out);
    return -1;
}
