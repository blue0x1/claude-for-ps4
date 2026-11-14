// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Home screen folders. The home screen keeps one table per user in
// /system_data/priv/mms/app.db (tbl_appbrowse_<10-digit user>):
//   folder: folderType=1, titleId = 9-digit number, titleName = folder name,
//           folderInfo = {"titleId":"<cover game>","index":0}
//   game in a folder: parentFolderId = folder titleId, positionInFolder = 1..n
//   game not in a folder: parentFolderId NULL, positionInFolder 0
// tbl_version 'folder_index' holds the last folder number used.
// Rows are written the way the PS4 writes folders it makes itself.
#include "homefolders.h"
#include "plat.h"
#include "sqlite3.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_GAMES   100
#define MAX_BACKUPS 10

static char g_db_path[256] = "/system_data/priv/mms/app.db";
static char g_backup_dir[256] = "/data/claude/backups";

void home_set_paths(const char *db_path, const char *backup_dir)
{
    snprintf(g_db_path, sizeof(g_db_path), "%s", db_path);
    snprintf(g_backup_dir, sizeof(g_backup_dir), "%s", backup_dir);
}

// ---- SQLite on the PS4 ----------------------------------------------------
// The toolchain's fcntl.h has Linux lock numbers and the Linux struct flock
// layout, but the PS4 kernel is FreeBSD. Without this SQLite would believe it
// holds a lock while the home screen writes, so locks are converted here.
#ifdef __ORBIS__
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>

struct bsd_flock {
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int16_t l_type;    // 1 read, 2 unlock, 3 write
    int16_t l_whence;
    int32_t l_sysid;
};

static long sys3(long nr, long a, long b, long c, int *err)
{
    long ret;
    char carry;
    register long r10 __asm__("r10") = c;
    __asm__ volatile("syscall; setc %1"
                     : "=a"(ret), "=r"(carry)
                     : "a"(0L), "D"(nr), "S"(a), "d"(b), "r"(r10)
                     : "rcx", "r11", "memory");
    *err = carry ? (int)ret : 0;
    return carry ? -1 : ret;
}

static int ps4_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    int err = 0;
    long r;
    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
        struct flock *l = (struct flock *)arg;
        struct bsd_flock b;
        memset(&b, 0, sizeof(b));
        b.l_start = l->l_start;
        b.l_len = l->l_len;
        b.l_pid = l->l_pid;
        b.l_whence = l->l_whence;
        b.l_type = l->l_type == F_RDLCK ? 1 : l->l_type == F_WRLCK ? 3 : 2;
        r = sys3(92, fd, cmd, (long)&b, &err);  // FreeBSD fcntl; F_*LK numbers match
        if (r >= 0 && cmd == F_GETLK) {
            l->l_type = b.l_type == 1 ? F_RDLCK : b.l_type == 3 ? F_WRLCK : F_UNLCK;
            l->l_start = b.l_start;
            l->l_len = b.l_len;
            l->l_pid = b.l_pid;
            l->l_whence = b.l_whence;
        }
    } else {
        r = sys3(92, fd, cmd, arg, &err);
    }
    if (r < 0) {
        // FreeBSD EAGAIN (35) is what a busy lock returns; SQLite checks the
        // header's EAGAIN to report "busy" instead of an I/O error.
        errno = err == 35 ? EAGAIN : err;
        return -1;
    }
    return (int)r;
}

// The toolchain libc has these as "not implemented" stubs; SQLite needs
// readlink while resolving the path and fchown on journals when root.
static ssize_t ps4_readlink(const char *path, char *buf, size_t n)
{
    int err;
    long r = sys3(58, (long)path, (long)buf, (long)n, &err);
    if (r < 0)
        errno = err;
    return r;
}

static int ps4_fchown(int fd, uid_t uid, gid_t gid)
{
    int err;
    long r = sys3(123, fd, (long)uid, (long)gid, &err);
    if (r < 0)
        errno = err;
    return (int)r;
}

static void sqlite_setup(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    sqlite3_initialize();
    sqlite3_vfs *v = sqlite3_vfs_find(NULL);
    if (v && v->xSetSystemCall) {
        v->xSetSystemCall(v, "fcntl", (sqlite3_syscall_ptr)ps4_fcntl);
        v->xSetSystemCall(v, "lstat", (sqlite3_syscall_ptr)plat_lstat);
        v->xSetSystemCall(v, "readlink", (sqlite3_syscall_ptr)ps4_readlink);
        v->xSetSystemCall(v, "fchown", (sqlite3_syscall_ptr)ps4_fchown);
    }
}
#else
static void sqlite_setup(void) {}
#endif

// ---- helpers --------------------------------------------------------------

static int fail(StrBuf *out, const char *msg)
{
    sb_append(out, msg);
    return 1;
}

static int db_fail(StrBuf *out, sqlite3 *db, const char *what)
{
    sb_appendf(out, "%s failed: %s", what, db ? sqlite3_errmsg(db) : "no database");
    return 1;
}

static sqlite3 *db_open(StrBuf *out)
{
    sqlite_setup();
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(g_db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        sb_appendf(out, "Could not open the home screen database %s: %s", g_db_path,
                   db ? sqlite3_errmsg(db) : "out of memory");
        if (!plat_jailbroken())
            sb_appendf(out, " (no full file access: %s)", plat_jailbreak_status());
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

static int all_digits(const char *s, size_t want)
{
    size_t n = 0;
    for (; s[n]; n++)
        if (s[n] < '0' || s[n] > '9')
            return 0;
    return want ? n == want : n > 0;
}

static int table_exists(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        ok = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    return ok;
}

// The home screen table of the signed-in user (or the user the input names).
static int pick_table(sqlite3 *db, const JVal *in, char tbl[64], StrBuf *out)
{
    const char *u = json_get_str(in, "user");
    char suffix[24];
    if (u && *u)
        snprintf(suffix, sizeof(suffix), "%s", u);
    else
        snprintf(suffix, sizeof(suffix), "%010d", plat_user_id());
    if (all_digits(suffix, 10)) {
        snprintf(tbl, 64, "tbl_appbrowse_%s", suffix);
        if (table_exists(db, tbl))
            return 0;
    }
    sb_appendf(out, "No home screen table for user %s. Users in the database:", suffix);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT substr(name,15) FROM sqlite_master WHERE type='table' AND name LIKE "
                               "'tbl_appbrowse_%' ORDER BY name", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            sb_appendf(out, " %s", (const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    sb_append(out, ". Ask again with user set to one of these.");
    return 1;
}

static int exec(sqlite3 *db, const char *sql, StrBuf *out)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        sb_appendf(out, "Database error: %s", err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return 1;
    }
    return 0;
}

// Single-row integer from sql with one optional text parameter; def if none.
static long long query_int(sqlite3 *db, const char *sql, const char *p1, long long def)
{
    sqlite3_stmt *st;
    long long v = def;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return def;
    if (p1)
        sqlite3_bind_text(st, 1, p1, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void mkdirs(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    mkdir(tmp, 0777);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// Backup file names, oldest first. Caller frees each and the array.
static int list_backups(char ***names)
{
    *names = NULL;
    DIR *d = opendir(g_backup_dir);
    if (!d)
        return 0;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (strncmp(e->d_name, "app-", 4) || l < 8 || strcmp(e->d_name + l - 3, ".db"))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            *names = realloc(*names, (size_t)cap * sizeof(char *));
        }
        (*names)[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n)
        qsort(*names, (size_t)n, sizeof(char *), name_cmp);
    return n;
}

static void free_names(char **names, int n)
{
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

// Copies the live database (consistently, through SQLite's locks) to a new
// backup file and keeps the newest MAX_BACKUPS.
static int backup_db(sqlite3 *db, char *path, size_t pathlen, StrBuf *out)
{
    mkdirs(g_backup_dir);
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm);
    struct stat st;
    // Always numbered, so names sort in the order they were made.
    snprintf(path, pathlen, "%s/app-%s-01.db", g_backup_dir, stamp);
    for (int i = 2; stat(path, &st) == 0 && i < 100; i++)
        snprintf(path, pathlen, "%s/app-%s-%02d.db", g_backup_dir, stamp, i);
    sqlite3 *b = NULL;
    int rc = sqlite3_open_v2(path, &b, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_backup *bk = sqlite3_backup_init(b, "main", db, "main");
        if (bk) {
            rc = sqlite3_backup_step(bk, -1);
            sqlite3_backup_finish(bk);
            rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
        } else {
            rc = sqlite3_errcode(b);
        }
    }
    if (rc != SQLITE_OK) {
        sb_appendf(out, "Could not back up the home screen database to %s (%s). Nothing was changed.", path,
                   b ? sqlite3_errmsg(b) : "out of memory");
        sqlite3_close(b);
        unlink(path);
        return 1;
    }
    sqlite3_close(b);
    char **names;
    int n = list_backups(&names);
    for (int i = 0; i + MAX_BACKUPS < n; i++) {
        char old[512];
        snprintf(old, sizeof(old), "%s/%s", g_backup_dir, names[i]);
        unlink(old);
    }
    free_names(names, n);
    return 0;
}

// Copies a backup file over the live database through SQLite's locks.
static int restore_from(sqlite3 *db, const char *path, StrBuf *out)
{
    sqlite3 *src = NULL;
    int rc = sqlite3_open_v2(path, &src, SQLITE_OPEN_READONLY, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_backup *bk = sqlite3_backup_init(db, "main", src, "main");
        if (bk) {
            rc = sqlite3_backup_step(bk, -1);
            sqlite3_backup_finish(bk);
            rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
        } else {
            rc = sqlite3_errcode(db);
        }
    }
    if (rc != SQLITE_OK)
        sb_appendf(out, "Restoring %s failed: %s", path, sqlite3_errmsg(rc == SQLITE_OK || !src ? db : src));
    sqlite3_close(src);
    return rc != SQLITE_OK;
}

// After a write: if the database does not check out, put the backup back.
static int verify_or_restore(sqlite3 *db, const char *backup, StrBuf *out)
{
    sqlite3_stmt *st;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &st, NULL) == SQLITE_OK) {
        ok = sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "ok");
        sqlite3_finalize(st);
    }
    if (ok)
        return 0;
    sb_append(out, "The home screen database failed its check after the change; restoring the backup. ");
    if (restore_from(db, backup, out))
        sb_appendf(out, " Restore it with restore_home_backup or rebuild the database in Safe Mode. Backup: %s", backup);
    else
        sb_append(out, "Restored, nothing changed.");
    return 1;
}

static const char RESTART_NOTE[] =
    "\nThe home screen reads this at start: if it does not show the change yet, restart the PS4. "
    "Until then, do not create or edit folders on the PS4 itself.";

// ---- folders and games ----------------------------------------------------

typedef struct {
    char id[16];
    char name[256];
} Folder;

// Finds a folder by its 9-digit ID or its name. 0 ok, 1 error written.
static int find_folder(sqlite3 *db, const char *tbl, const char *key, Folder *f, StrBuf *out)
{
    char sql[256];
    int by_id = all_digits(key, 9);
    snprintf(sql, sizeof(sql), "SELECT titleId, titleName FROM %s WHERE folderType=1 AND %s", tbl,
             by_id ? "titleId=?" : "titleName=? COLLATE NOCASE");
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return db_fail(out, db, "Folder lookup");
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!n) {
            snprintf(f->id, sizeof(f->id), "%s", (const char *)sqlite3_column_text(st, 0));
            const unsigned char *nm = sqlite3_column_text(st, 1);
            snprintf(f->name, sizeof(f->name), "%s", nm ? (const char *)nm : "");
        }
        n++;
    }
    sqlite3_finalize(st);
    if (n == 1)
        return 0;
    sb_appendf(out, n ? "Several folders are called \"%s\"; use the folder ID from list_home_folders."
                      : "No home screen folder \"%s\". Use list_home_folders to see them.", key);
    return 1;
}

typedef struct {
    char  id[16];
    char  name[256];
    char  folder[16];   // current folder, "" if none
} Game;

// Resolves the "games" array (title IDs or exact names) to games of the table.
static int find_games(sqlite3 *db, const char *tbl, const JVal *in, Game *g, int *count, StrBuf *out)
{
    JVal *arr = json_get(in, "games");
    *count = 0;
    if (!arr || arr->type != J_ARR || !arr->count)
        return fail(out, "games must be a list of title IDs or names.");
    if (arr->count > MAX_GAMES)
        return fail(out, "Too many games at once (at most 100).");
    char sql[320];
    snprintf(sql, sizeof(sql),
             "SELECT titleId, titleName, ifnull(parentFolderId,'') FROM %s WHERE folderType=0 AND "
             "(titleId=upper(?1) OR titleName=?1 COLLATE NOCASE) ORDER BY titleId=upper(?1) DESC LIMIT 2", tbl);
    StrBuf missing;
    sb_init(&missing);
    for (int i = 0; i < arr->count; i++) {
        JVal *v = arr->items[i];
        if (v->type != J_STR || !v->str[0])
            continue;
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
            sb_free(&missing);
            return db_fail(out, db, "Game lookup");
        }
        sqlite3_bind_text(st, 1, v->str, -1, SQLITE_TRANSIENT);
        int rows = 0;
        Game cur;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (!rows) {
                snprintf(cur.id, sizeof(cur.id), "%s", (const char *)sqlite3_column_text(st, 0));
                const unsigned char *nm = sqlite3_column_text(st, 1);
                snprintf(cur.name, sizeof(cur.name), "%s", nm ? (const char *)nm : "");
                snprintf(cur.folder, sizeof(cur.folder), "%s", (const char *)sqlite3_column_text(st, 2));
            }
            rows++;
        }
        sqlite3_finalize(st);
        if (!rows) {
            sb_appendf(&missing, "%s\"%s\"", missing.len ? ", " : "", v->str);
            continue;
        }
        int dup = 0;
        for (int k = 0; k < *count; k++)
            dup |= !strcmp(g[k].id, cur.id);
        if (!dup)
            g[(*count)++] = cur;
    }
    if (missing.len) {
        sb_appendf(out, "Not found on the home screen: %s. Use list_home_folders for title IDs.", missing.data);
        sb_free(&missing);
        return 1;
    }
    sb_free(&missing);
    if (!*count)
        return fail(out, "No games given.");
    return 0;
}

static int set_game_folder(sqlite3 *db, const char *tbl, const char *game, const char *folder, StrBuf *out)
{
    char sql[256];
    sqlite3_stmt *st;
    long long pos = 0;
    if (folder) {
        snprintf(sql, sizeof(sql), "SELECT max(positionInFolder) FROM %s WHERE parentFolderId=?", tbl);
        pos = query_int(db, sql, folder, 0) + 1;
        snprintf(sql, sizeof(sql), "UPDATE %s SET parentFolderId=?, positionInFolder=? WHERE titleId=?", tbl);
    } else {
        snprintf(sql, sizeof(sql), "UPDATE %s SET parentFolderId=NULL, positionInFolder=0 WHERE titleId=?", tbl);
    }
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return db_fail(out, db, "Update");
    if (folder) {
        sqlite3_bind_text(st, 1, folder, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, pos);
        sqlite3_bind_text(st, 3, game, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_text(st, 1, game, -1, SQLITE_TRANSIENT);
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : db_fail(out, db, "Update");
}

// After games left a folder: drop it if empty, else keep the cover valid.
static int tidy_folder(sqlite3 *db, const char *tbl, const char *folder, StrBuf *out, StrBuf *note)
{
    char sql[320];
    snprintf(sql, sizeof(sql), "SELECT titleId FROM %s WHERE parentFolderId=? ORDER BY positionInFolder LIMIT 1", tbl);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return db_fail(out, db, "Folder check");
    sqlite3_bind_text(st, 1, folder, -1, SQLITE_TRANSIENT);
    char first[16] = "";
    if (sqlite3_step(st) == SQLITE_ROW)
        snprintf(first, sizeof(first), "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    if (first[0]) {
        // Keep the cover if it is still in the folder, else use the first game.
        char cover[16] = "";
        snprintf(sql, sizeof(sql), "SELECT folderInfo FROM %s WHERE titleId=? AND folderType=1", tbl);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
            return db_fail(out, db, "Folder check");
        sqlite3_bind_text(st, 1, folder, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) {
            const char *info = (const char *)sqlite3_column_text(st, 0), *p = strstr(info, "\"titleId\":\"");
            if (p) {
                p += 11;
                size_t k = 0;
                while (p[k] && p[k] != '"' && k + 1 < sizeof(cover))
                    k++;
                memcpy(cover, p, k);
                cover[k] = 0;
            }
        }
        sqlite3_finalize(st);
        snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s WHERE parentFolderId=?1 AND titleId=?2", tbl);
        int still = 0;
        if (cover[0] && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, folder, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, cover, -1, SQLITE_TRANSIENT);
            still = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0;
            sqlite3_finalize(st);
        }
        if (still)
            return 0;
        snprintf(sql, sizeof(sql),
                 "UPDATE %s SET folderInfo='{\"titleId\":\"'||?2||'\",\"index\":0}' WHERE titleId=?1 AND folderType=1", tbl);
    } else {
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE titleId=?1 AND folderType=1", tbl);
    }
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return db_fail(out, db, "Folder update");
    sqlite3_bind_text(st, 1, folder, -1, SQLITE_TRANSIENT);
    if (first[0])
        sqlite3_bind_text(st, 2, first, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return db_fail(out, db, "Folder update");
    if (!first[0] && sqlite3_changes(db))
        sb_appendf(note, "\nFolder %s was left empty and removed.", folder);
    return 0;
}

static int confirm_text(Chat *chat, const char *head, const Game *g, int n, const char *tail)
{
    StrBuf q;
    sb_init(&q);
    sb_append(&q, head);
    for (int i = 0; i < n && i < 8; i++)
        sb_appendf(&q, "\n- %s", g[i].name[0] ? g[i].name : g[i].id);
    if (n > 8)
        sb_appendf(&q, "\n- and %d more", n - 8);
    if (tail)
        sb_append(&q, tail);
    int yes = chat_ask_confirm(chat, q.data);
    sb_free(&q);
    return yes;
}

// ---- tools ----------------------------------------------------------------

static int t_list(sqlite3 *db, const char *tbl, StrBuf *out)
{
    char sql[400];
    sb_appendf(out, "Home screen folders (user %s):\n", tbl + 14);
    snprintf(sql, sizeof(sql), "SELECT titleId, titleName FROM %s WHERE folderType=1 ORDER BY titleName COLLATE NOCASE", tbl);
    sqlite3_stmt *st, *m;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return db_fail(out, db, "Reading folders");
    snprintf(sql, sizeof(sql), "SELECT titleId, titleName FROM %s WHERE parentFolderId=? ORDER BY positionInFolder", tbl);
    if (sqlite3_prepare_v2(db, sql, -1, &m, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return db_fail(out, db, "Reading folders");
    }
    int folders = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *fid = (const char *)sqlite3_column_text(st, 0);
        const unsigned char *fname = sqlite3_column_text(st, 1);
        sb_appendf(out, "\n[%s] %s\n", fid, fname ? (const char *)fname : "");
        sqlite3_reset(m);
        sqlite3_bind_text(m, 1, fid, -1, SQLITE_TRANSIENT);
        int k = 0;
        while (sqlite3_step(m) == SQLITE_ROW) {
            const unsigned char *gn = sqlite3_column_text(m, 1);
            sb_appendf(out, "  %s  %s\n", (const char *)sqlite3_column_text(m, 0), gn ? (const char *)gn : "");
            k++;
        }
        if (!k)
            sb_append(out, "  (empty)\n");
        folders++;
    }
    sqlite3_finalize(m);
    sqlite3_finalize(st);
    if (!folders)
        sb_append(out, "(no folders)\n");
    snprintf(sql, sizeof(sql),
             "SELECT titleId, titleName FROM %s WHERE folderType=0 AND visible=1 AND "
             "(parentFolderId IS NULL OR parentFolderId='') ORDER BY titleName COLLATE NOCASE", tbl);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return db_fail(out, db, "Reading games");
    sb_append(out, "\nNot in a folder:\n");
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *gn = sqlite3_column_text(st, 1);
        sb_appendf(out, "  %s  %s\n", (const char *)sqlite3_column_text(st, 0), gn ? (const char *)gn : "");
    }
    sqlite3_finalize(st);
    return 0;
}

static int valid_name(const char *name, StrBuf *out)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n > 64)
        return fail(out, "The folder name must be 1 to 64 characters."), 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)name[i] < 0x20)
            return fail(out, "The folder name cannot contain control characters."), 0;
    return 1;
}

// Highest folder number used in any user's table or in tbl_version.
static long long next_folder_number(sqlite3 *db)
{
    long long top = query_int(db, "SELECT status FROM tbl_version WHERE category='folder_index'", NULL, 0);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_appbrowse_%'",
                           -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(st, 0);
            if (!all_digits(t + 14, 10))
                continue;
            char sql[200];
            snprintf(sql, sizeof(sql), "SELECT max(CAST(titleId AS INTEGER)) FROM %s WHERE folderType=1", t);
            long long m = query_int(db, sql, NULL, 0);
            if (m > top)
                top = m;
        }
        sqlite3_finalize(st);
    }
    return top + 1;
}

static int t_create(Chat *chat, sqlite3 *db, const char *tbl, const JVal *in, StrBuf *out)
{
    const char *name = json_get_str(in, "name");
    if (!valid_name(name, out))
        return 1;
    Folder existing;
    StrBuf tmp;
    sb_init(&tmp);
    int exists = !find_folder(db, tbl, name, &existing, &tmp);
    sb_free(&tmp);
    if (exists) {
        sb_appendf(out, "A folder called \"%s\" already exists (%s). Use move_to_home_folder to add games.",
                   existing.name, existing.id);
        return 1;
    }
    static Game g[MAX_GAMES];
    int n;
    if (find_games(db, tbl, in, g, &n, out))
        return 1;
    char head[200];
    snprintf(head, sizeof(head), "Create home screen folder \"%.64s\" with %d game%s?", name, n, n == 1 ? "" : "s");
    if (!confirm_text(chat, head, g, n, "\nA backup is made first."))
        return fail(out, "The user declined.");
    char backup[512];
    if (backup_db(db, backup, sizeof(backup), out))
        return 1;
    if (exec(db, "BEGIN IMMEDIATE", out))
        return 1;
    long long num = next_folder_number(db);
    char fid[16];
    snprintf(fid, sizeof(fid), "%09lld", num);
    long long access = query_int(db, "SELECT status FROM tbl_version WHERE category='access_index'", NULL, 0);
    char sql[900], info[64];
    snprintf(info, sizeof(info), "{\"titleId\":\"%s\",\"index\":0}", g[0].id);
    snprintf(sql, sizeof(sql),
             "INSERT INTO %s (titleId,titleName,lastAccessTime,contentStatus,onDisc,parentalLevel,visible,"
             "sortPriority,pathInfo,lastAccessIndex,dispLocation,canRemove,contentType,pathInfo2,presentBoxStatus,"
             "entitlement,contentSize,installDate,platform,disableLiveDetail,linkType,folderType,folderInfo) "
             "VALUES (?1,?2,strftime('%%Y-%%m-%%d %%H:%%M:%%f','now'),0,0,0,1,100,0,?3,5,1,0,0,0,0,0,"
             "strftime('%%Y-%%m-%%d %%H:%%M:%%f','now'),0,0,0,1,?4)", tbl);
    sqlite3_stmt *st;
    int bad = sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK;
    if (!bad) {
        sqlite3_bind_text(st, 1, fid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, access);
        sqlite3_bind_text(st, 4, info, -1, SQLITE_TRANSIENT);
        bad = sqlite3_step(st) != SQLITE_DONE;
        sqlite3_finalize(st);
    }
    if (bad)
        db_fail(out, db, "Creating the folder");
    if (!bad) {
        sqlite3_stmt *v;
        bad = sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO tbl_version (category,status) VALUES ('folder_index',?)",
                                 -1, &v, NULL) != SQLITE_OK;
        if (!bad) {
            sqlite3_bind_int64(v, 1, num);
            bad = sqlite3_step(v) != SQLITE_DONE;
            sqlite3_finalize(v);
        }
        if (bad)
            db_fail(out, db, "Updating folder_index");
    }
    StrBuf note;
    sb_init(&note);
    for (int i = 0; !bad && i < n; i++)
        bad = set_game_folder(db, tbl, g[i].id, fid, out);
    for (int i = 0; !bad && i < n; i++)
        if (g[i].folder[0])
            bad = tidy_folder(db, tbl, g[i].folder, out, &note);
    if (bad) {
        exec(db, "ROLLBACK", out);
        sb_free(&note);
        return 1;
    }
    if (exec(db, "COMMIT", out)) {
        exec(db, "ROLLBACK", out);
        sb_free(&note);
        return 1;
    }
    if (verify_or_restore(db, backup, out)) {
        sb_free(&note);
        return 1;
    }
    sb_appendf(out, "Created folder \"%s\" (%s) with %d game%s.%s\nBackup: %s%s", name, fid, n, n == 1 ? "" : "s",
               note.data ? note.data : "", backup, RESTART_NOTE);
    sb_free(&note);
    return 0;
}

static int t_move(Chat *chat, sqlite3 *db, const char *tbl, const JVal *in, StrBuf *out)
{
    const char *fkey = json_get_str(in, "folder");
    Folder f = { "", "" };
    int out_of_folder = !fkey || !fkey[0] || !strcasecmp(fkey, "none") || !strcasecmp(fkey, "home");
    if (!out_of_folder && find_folder(db, tbl, fkey, &f, out))
        return 1;
    static Game g[MAX_GAMES];
    int n;
    if (find_games(db, tbl, in, g, &n, out))
        return 1;
    char head[340];
    if (out_of_folder)
        snprintf(head, sizeof(head), "Take %d game%s out of their folders?", n, n == 1 ? "" : "s");
    else
        snprintf(head, sizeof(head), "Move %d game%s into the folder \"%.200s\"?", n, n == 1 ? "" : "s", f.name);
    if (!confirm_text(chat, head, g, n, "\nA backup is made first."))
        return fail(out, "The user declined.");
    char backup[512];
    if (backup_db(db, backup, sizeof(backup), out))
        return 1;
    if (exec(db, "BEGIN IMMEDIATE", out))
        return 1;
    int bad = 0, moved = 0;
    StrBuf note;
    sb_init(&note);
    for (int i = 0; !bad && i < n; i++) {
        if (!strcmp(g[i].folder, f.id))
            continue;  // already there
        bad = set_game_folder(db, tbl, g[i].id, out_of_folder ? NULL : f.id, out);
        moved++;
    }
    for (int i = 0; !bad && i < n; i++)
        if (g[i].folder[0] && strcmp(g[i].folder, f.id))
            bad = tidy_folder(db, tbl, g[i].folder, out, &note);
    if (!bad && !out_of_folder)
        bad = tidy_folder(db, tbl, f.id, out, &note);
    if (bad || exec(db, "COMMIT", out)) {
        exec(db, "ROLLBACK", out);
        sb_free(&note);
        return 1;
    }
    if (verify_or_restore(db, backup, out)) {
        sb_free(&note);
        return 1;
    }
    if (out_of_folder)
        sb_appendf(out, "Took %d game%s out of their folders.", moved, moved == 1 ? "" : "s");
    else
        sb_appendf(out, "Moved %d game%s into \"%s\" (%d already there).", moved, moved == 1 ? "" : "s", f.name,
                   n - moved);
    sb_appendf(out, "%s\nBackup: %s%s", note.data ? note.data : "", backup, RESTART_NOTE);
    sb_free(&note);
    return 0;
}

static int t_rename(Chat *chat, sqlite3 *db, const char *tbl, const JVal *in, StrBuf *out)
{
    const char *fkey = json_get_str(in, "folder"), *to = json_get_str(in, "new_name");
    Folder f;
    if (!fkey || find_folder(db, tbl, fkey, &f, out))
        return fkey ? 1 : fail(out, "folder is required.");
    if (!valid_name(to, out))
        return 1;
    char q[400];
    snprintf(q, sizeof(q), "Rename the home screen folder \"%.150s\" to \"%.64s\"?\nA backup is made first.", f.name, to);
    if (!chat_ask_confirm(chat, q))
        return fail(out, "The user declined.");
    char backup[512];
    if (backup_db(db, backup, sizeof(backup), out))
        return 1;
    char sql[200];
    snprintf(sql, sizeof(sql), "UPDATE %s SET titleName=? WHERE titleId=? AND folderType=1", tbl);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return db_fail(out, db, "Rename");
    sqlite3_bind_text(st, 1, to, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, f.id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return db_fail(out, db, "Rename");
    if (verify_or_restore(db, backup, out))
        return 1;
    sb_appendf(out, "Renamed \"%s\" to \"%s\".\nBackup: %s%s", f.name, to, backup, RESTART_NOTE);
    return 0;
}

static int t_delete(Chat *chat, sqlite3 *db, const char *tbl, const JVal *in, StrBuf *out)
{
    const char *fkey = json_get_str(in, "folder");
    Folder f;
    if (!fkey || find_folder(db, tbl, fkey, &f, out))
        return fkey ? 1 : fail(out, "folder is required.");
    char sql[300];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s WHERE parentFolderId=?", tbl);
    long long n = query_int(db, sql, f.id, 0);
    char q[400];
    snprintf(q, sizeof(q), "Remove the home screen folder \"%.150s\"?\nIts %lld game%s go back to the home screen; "
             "nothing is uninstalled. A backup is made first.", f.name, n, n == 1 ? "" : "s");
    if (!chat_ask_confirm(chat, q))
        return fail(out, "The user declined.");
    char backup[512];
    if (backup_db(db, backup, sizeof(backup), out))
        return 1;
    if (exec(db, "BEGIN IMMEDIATE", out))
        return 1;
    int bad = 0;
    const char *stmts[2] = { "UPDATE %s SET parentFolderId=NULL, positionInFolder=0 WHERE parentFolderId=?",
                             "DELETE FROM %s WHERE titleId=? AND folderType=1" };
    for (int i = 0; i < 2 && !bad; i++) {
        snprintf(sql, sizeof(sql), stmts[i], tbl);
        sqlite3_stmt *st;
        bad = sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK;
        if (!bad) {
            sqlite3_bind_text(st, 1, f.id, -1, SQLITE_TRANSIENT);
            bad = sqlite3_step(st) != SQLITE_DONE;
            sqlite3_finalize(st);
        }
        if (bad)
            db_fail(out, db, "Removing the folder");
    }
    if (bad || exec(db, "COMMIT", out)) {
        exec(db, "ROLLBACK", out);
        return 1;
    }
    if (verify_or_restore(db, backup, out))
        return 1;
    sb_appendf(out, "Removed folder \"%s\"; %lld game%s are back on the home screen.\nBackup: %s%s", f.name, n,
               n == 1 ? "" : "s", backup, RESTART_NOTE);
    return 0;
}

static int t_restore(Chat *chat, sqlite3 *db, const JVal *in, StrBuf *out)
{
    char **names;
    int n = list_backups(&names);
    if (!n)
        return fail(out, "There are no backups of the home screen database yet.");
    const char *want = json_get_str(in, "backup");
    const char *pick = NULL;
    if (want && *want) {
        const char *base = strrchr(want, '/');
        base = base ? base + 1 : want;
        for (int i = 0; i < n; i++)
            if (!strcmp(names[i], base))
                pick = names[i];
        if (!pick) {
            sb_appendf(out, "No backup called %s. Backups (oldest first):", base);
            for (int i = 0; i < n; i++)
                sb_appendf(out, " %s", names[i]);
            free_names(names, n);
            return 1;
        }
    } else {
        pick = names[n - 1];
    }
    char path[512], q[700];
    snprintf(path, sizeof(path), "%s/%s", g_backup_dir, pick);
    snprintf(q, sizeof(q), "Put back the home screen database from the backup\n%s ?\nFolder changes made after it "
             "are undone. The current state is backed up first.", pick);
    if (!chat_ask_confirm(chat, q)) {
        free_names(names, n);
        return fail(out, "The user declined.");
    }
    char now[512];
    int bad = backup_db(db, now, sizeof(now), out) || restore_from(db, path, out);
    if (!bad)
        bad = verify_or_restore(db, now, out);
    if (!bad)
        sb_appendf(out, "Restored the home screen database from %s. The state before the restore is in %s.%s", pick,
                   now, RESTART_NOTE);
    free_names(names, n);
    return bad;
}

int home_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out)
{
    static const char *const tools[] = { "list_home_folders", "create_home_folder", "move_to_home_folder",
                                         "rename_home_folder", "delete_home_folder", "restore_home_backup", NULL };
    int which = -1;
    for (int i = 0; tools[i]; i++)
        if (!strcmp(name, tools[i]))
            which = i;
    if (which < 0)
        return -1;
    sqlite3 *db = db_open(out);
    if (!db)
        return 1;
    int r;
    char tbl[64];
    if (which == 5)
        r = t_restore(chat, db, in, out);
    else if (pick_table(db, in, tbl, out))
        r = 1;
    else if (which == 0)
        r = t_list(db, tbl, out);
    else if (which == 1)
        r = t_create(chat, db, tbl, in, out);
    else if (which == 2)
        r = t_move(chat, db, tbl, in, out);
    else if (which == 3)
        r = t_rename(chat, db, tbl, in, out);
    else
        r = t_delete(chat, db, tbl, in, out);
    sqlite3_close(db);
    return r;
}
