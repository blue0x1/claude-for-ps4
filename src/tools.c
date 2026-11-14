// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "tools.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_LIST_ENTRIES 400
#define MAX_READ_BYTES   (64 * 1024)
#define MAX_WRITE_BYTES  (1024 * 1024)

const char TOOLS_JSON[] =
"["
"{\"name\":\"get_system_info\",\"description\":\"Get PS4 status: firmware version, CPU and SoC temperature, free space on internal storage and USB drives, IP address, and whether the app has full file access.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}},"
"{\"name\":\"list_directory\",\"description\":\"List files and folders at an absolute path on the PS4 (for example /data, /user/app, /mnt/usb0). Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
"{\"name\":\"read_text_file\",\"description\":\"Read a text file (logs, configs, ini, json). Returns at most max_bytes (default 16384, max 65536). Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"max_bytes\":{\"type\":\"integer\"}},\"required\":[\"path\"]}},"
"{\"name\":\"write_text_file\",\"description\":\"Create or overwrite a text file. The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},\"append\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"content\"]}},"
"{\"name\":\"make_directory\",\"description\":\"Create a folder (and missing parents). The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
"{\"name\":\"copy_path\",\"description\":\"Copy a file or a whole folder, for example to back up saves to USB. The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"destination\":{\"type\":\"string\"}},\"required\":[\"source\",\"destination\"]}},"
"{\"name\":\"move_path\",\"description\":\"Move or rename a file or folder. The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"destination\":{\"type\":\"string\"}},\"required\":[\"source\",\"destination\"]}},"
"{\"name\":\"delete_path\",\"description\":\"Delete a file, or a folder with everything in it when recursive is true. The user must confirm on screen. System folders are refused.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"recursive\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}},"
"{\"name\":\"find_pkg_files\",\"description\":\"Find .pkg files on USB drives and in /data. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}},"
"{\"name\":\"list_installed_apps\",\"description\":\"List installed games and apps with title IDs and names. Read-only.\",\"input_schema\":{\"type\":\"object\",\"properties\":{}}},"
"{\"name\":\"launch_app\",\"description\":\"Launch an installed game or app by title ID (for example CUSA00001). This closes Claude for PS4. The user must confirm on screen.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"title_id\":{\"type\":\"string\"}},\"required\":[\"title_id\"]}},"
"{\"name\":\"show_notification\",\"description\":\"Show a short system notification pop-up on the PS4.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}}"
"]";

static const char *in_str(const JVal *input, const char *key)
{
    return json_get_str(input, key);
}

static int in_bool(const JVal *input, const char *key)
{
    JVal *v = json_get(input, key);
    return v && v->type == J_BOOL && v->boolean;
}

static int fail(StrBuf *out, const char *fmt, const char *arg)
{
    sb_appendf(out, fmt, arg ? arg : "");
    return 1;
}

static int fail_errno(StrBuf *out, const char *what, const char *path)
{
    sb_appendf(out, "%s failed for %s: %s", what, path, strerror(errno));
    return 1;
}

static int bad_path(const char *p)
{
    return !p || p[0] != '/' || strstr(p, "/../") || strlen(p) >= 1024;
}

// Paths that must never be written or deleted.
static int protected_path(const char *p)
{
    static const char *const roots[] = {
        "/system", "/system_ex", "/preinst", "/preinst2", "/update", "/dev", "/proc",
        "/mnt/sandbox", "/system_data/priv/mms", NULL,
    };
    if (!strcmp(p, "/"))
        return 1;
    for (int i = 0; roots[i]; i++) {
        size_t n = strlen(roots[i]);
        if (!strncmp(p, roots[i], n) && (p[n] == 0 || p[n] == '/'))
            return 1;
    }
    // Top-level folders themselves (/data, /user, /mnt/usb0) are off limits.
    const char *slash = strchr(p + 1, '/');
    if (!slash || slash[1] == 0)
        return 1;
    if (!strncmp(p, "/mnt/", 5) && !strchr(p + 5, '/'))
        return 1;
    if (!strncmp(p, "/mnt/", 5)) {
        const char *s = strchr(p + 5, '/');
        if (s && s[1] == 0)
            return 1;
    }
    return 0;
}

static void human_size(unsigned long long b, char *out, size_t n)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        i++;
    }
    snprintf(out, n, i ? "%.1f %s" : "%.0f %s", v, u[i]);
}

static int confirm(Chat *chat, const char *fmt, const char *a, const char *b)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), fmt, a ? a : "", b ? b : "");
    return chat_ask_confirm(chat, buf);
}

// ---- file helpers -------------------------------------------------------

static int copy_file(const char *src, const char *dst, StrBuf *out)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return fail_errno(out, "open", src);
    int o = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (o < 0) {
        close(in);
        return fail_errno(out, "create", dst);
    }
    static char buf[256 * 1024];
    ssize_t n;
    int err = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(o, buf, (size_t)n) != n) {
            err = fail_errno(out, "write", dst);
            break;
        }
    }
    if (n < 0 && !err)
        err = fail_errno(out, "read", src);
    close(in);
    close(o);
    return err;
}

static int copy_tree(const char *src, const char *dst, StrBuf *out, int *files)
{
    struct stat st;
    if (stat(src, &st) < 0)
        return fail_errno(out, "stat", src);
    if (!S_ISDIR(st.st_mode)) {
        int r = copy_file(src, dst, out);
        if (!r)
            (*files)++;
        return r;
    }
    if (mkdir(dst, 0777) < 0 && errno != EEXIST)
        return fail_errno(out, "mkdir", dst);
    DIR *d = opendir(src);
    if (!d)
        return fail_errno(out, "opendir", src);
    struct dirent *e;
    int err = 0;
    while (!err && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char s2[1024], d2[1024];
        snprintf(s2, sizeof(s2), "%s/%s", src, e->d_name);
        snprintf(d2, sizeof(d2), "%s/%s", dst, e->d_name);
        err = copy_tree(s2, d2, out, files);
    }
    closedir(d);
    return err;
}

static int delete_tree(const char *path, StrBuf *out, int *count)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return fail_errno(out, "stat", path);
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return fail_errno(out, "opendir", path);
        struct dirent *e;
        int err = 0;
        while (!err && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char p2[1024];
            snprintf(p2, sizeof(p2), "%s/%s", path, e->d_name);
            err = delete_tree(p2, out, count);
        }
        closedir(d);
        if (err)
            return err;
        if (rmdir(path) < 0)
            return fail_errno(out, "rmdir", path);
    } else if (unlink(path) < 0) {
        return fail_errno(out, "delete", path);
    }
    (*count)++;
    return 0;
}

static int mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return (mkdir(tmp, 0777) < 0 && errno != EEXIST) ? -1 : 0;
}

// ---- param.sfo ----------------------------------------------------------

static int sfo_get_string(const char *path, const char *key, char *out, size_t outlen)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    static unsigned char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 20 || memcmp(buf, "\0PSF", 4))
        return 0;
    uint32_t key_tab, data_tab, count;
    memcpy(&key_tab, buf + 8, 4);
    memcpy(&data_tab, buf + 12, 4);
    memcpy(&count, buf + 16, 4);
    for (uint32_t i = 0; i < count && 20 + (i + 1) * 16 <= n; i++) {
        const unsigned char *e = buf + 20 + i * 16;
        uint16_t koff;
        uint32_t dlen, doff;
        memcpy(&koff, e, 2);
        memcpy(&dlen, e + 4, 4);
        memcpy(&doff, e + 12, 4);
        if (key_tab + koff >= n || data_tab + doff + dlen > n)
            continue;
        if (strcmp((const char *)buf + key_tab + koff, key))
            continue;
        size_t len = dlen < outlen ? dlen : outlen - 1;
        memcpy(out, buf + data_tab + doff, len);
        out[len] = 0;
        return 1;
    }
    return 0;
}

// ---- tools --------------------------------------------------------------

static int t_list_directory(const JVal *in, StrBuf *out)
{
    const char *path = in_str(in, "path");
    if (!path || path[0] != '/')
        return fail(out, "path must be absolute", NULL);
    DIR *d = opendir(path);
    if (!d)
        return fail_errno(out, "list", path);
    int n = 0, more = 0;
    struct dirent *e;
    sb_appendf(out, "%s:\n", path);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (n >= MAX_LIST_ENTRIES) {
            more++;
            continue;
        }
        char full[1024];
        snprintf(full, sizeof(full), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            sb_appendf(out, "[dir]  %s\n", e->d_name);
        } else if (stat(full, &st) == 0) {
            char sz[32];
            human_size((unsigned long long)st.st_size, sz, sizeof(sz));
            sb_appendf(out, "[file] %s (%s)\n", e->d_name, sz);
        } else {
            sb_appendf(out, "[?]    %s\n", e->d_name);
        }
        n++;
    }
    closedir(d);
    if (!n)
        sb_append(out, "(empty)\n");
    if (more)
        sb_appendf(out, "... and %d more entries\n", more);
    return 0;
}

static int t_read_text_file(const JVal *in, StrBuf *out)
{
    const char *path = in_str(in, "path");
    if (!path || path[0] != '/')
        return fail(out, "path must be absolute", NULL);
    JVal *mb = json_get(in, "max_bytes");
    long max = (mb && mb->type == J_NUM && mb->num > 0) ? (long)mb->num : 16384;
    if (max > MAX_READ_BYTES)
        max = MAX_READ_BYTES;
    FILE *f = fopen(path, "rb");
    if (!f)
        return fail_errno(out, "open", path);
    char *buf = malloc((size_t)max + 1);
    size_t n = buf ? fread(buf, 1, (size_t)max, f) : 0;
    int truncated = n == (size_t)max && fgetc(f) != EOF;
    fclose(f);
    if (!buf)
        return fail(out, "out of memory", NULL);
    // Replace NULs and other binary bytes so the result stays valid text.
    size_t bin = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) {
            buf[i] = '.';
            bin++;
        }
    }
    buf[n] = 0;
    if (bin > n / 10 && n > 64)
        sb_appendf(out, "(This looks like a binary file; %zu of %zu bytes are not text.)\n", bin, n);
    sb_append(out, buf);
    if (truncated)
        sb_appendf(out, "\n... (truncated after %ld bytes)", max);
    free(buf);
    return 0;
}

static int t_write_text_file(Chat *chat, const JVal *in, StrBuf *out)
{
    const char *path = in_str(in, "path");
    const char *content = in_str(in, "content");
    int append = in_bool(in, "append");
    if (bad_path(path) || !content)
        return fail(out, "need an absolute path and content", NULL);
    if (protected_path(path))
        return fail(out, "refused: %s is a protected location", path);
    if (strlen(content) > MAX_WRITE_BYTES)
        return fail(out, "content too large", NULL);
    char sz[32];
    human_size(strlen(content), sz, sizeof(sz));
    if (!confirm(chat, append ? "Append %s of text to\n%s ?" : "Write %s of text to\n%s ?", sz, path))
        return fail(out, "The user declined.", NULL);
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (!f)
        return fail_errno(out, "open", path);
    size_t len = strlen(content);
    size_t w = fwrite(content, 1, len, f);
    fclose(f);
    if (w != len)
        return fail_errno(out, "write", path);
    sb_appendf(out, "Wrote %zu bytes to %s", len, path);
    return 0;
}

static int t_make_directory(Chat *chat, const JVal *in, StrBuf *out)
{
    const char *path = in_str(in, "path");
    if (bad_path(path))
        return fail(out, "need an absolute path", NULL);
    if (protected_path(path))
        return fail(out, "refused: %s is a protected location", path);
    if (!confirm(chat, "Create folder\n%s ?%s", path, ""))
        return fail(out, "The user declined.", NULL);
    if (mkdirs(path) < 0)
        return fail_errno(out, "mkdir", path);
    sb_appendf(out, "Created %s", path);
    return 0;
}

static int t_copy_move(Chat *chat, const JVal *in, StrBuf *out, int move)
{
    const char *src = in_str(in, "source");
    const char *dst = in_str(in, "destination");
    if (bad_path(src) || bad_path(dst))
        return fail(out, "need absolute source and destination paths", NULL);
    if (protected_path(dst) || (move && protected_path(src)))
        return fail(out, "refused: protected location", NULL);
    size_t sl = strlen(src);
    if (!strncmp(dst, src, sl) && (dst[sl] == '/' || dst[sl] == 0))
        return fail(out, "destination is inside the source", NULL);
    struct stat st;
    if (stat(src, &st) < 0)
        return fail_errno(out, "stat", src);
    char q[2200];
    snprintf(q, sizeof(q), "%s %s\n%s\nto\n%s ?", move ? "Move" : "Copy",
             S_ISDIR(st.st_mode) ? "folder" : "file", src, dst);
    if (!chat_ask_confirm(chat, q))
        return fail(out, "The user declined.", NULL);
    if (move && rename(src, dst) == 0) {
        sb_appendf(out, "Moved %s to %s", src, dst);
        return 0;
    }
    int files = 0;
    if (copy_tree(src, dst, out, &files))
        return 1;
    if (move) {
        int count = 0;
        if (delete_tree(src, out, &count))
            return 1;
        sb_appendf(out, "Moved %s to %s (%d files copied, source removed)", src, dst, files);
    } else {
        sb_appendf(out, "Copied %s to %s (%d files)", src, dst, files);
    }
    return 0;
}

static int t_delete_path(Chat *chat, const JVal *in, StrBuf *out)
{
    const char *path = in_str(in, "path");
    int recursive = in_bool(in, "recursive");
    if (bad_path(path))
        return fail(out, "need an absolute path", NULL);
    if (protected_path(path))
        return fail(out, "refused: %s is a protected location", path);
    struct stat st;
    if (lstat(path, &st) < 0)
        return fail_errno(out, "stat", path);
    int dir = S_ISDIR(st.st_mode);
    if (dir && !recursive) {
        if (!confirm(chat, "Delete empty folder\n%s ?%s", path, ""))
            return fail(out, "The user declined.", NULL);
        if (rmdir(path) < 0)
            return fail_errno(out, "rmdir", path);
        sb_appendf(out, "Deleted %s", path);
        return 0;
    }
    if (!confirm(chat, dir ? "Delete folder and EVERYTHING in it\n%s ?%s" : "Delete file\n%s ?%s", path, ""))
        return fail(out, "The user declined.", NULL);
    int count = 0;
    if (delete_tree(path, out, &count))
        return 1;
    sb_appendf(out, "Deleted %s (%d items)", path, count);
    return 0;
}

static void find_pkgs(const char *dir, int depth, StrBuf *out, int *found)
{
    if (depth < 0 || *found >= 200)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && *found < 200) {
        if (e->d_name[0] == '.')
            continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            find_pkgs(p, depth - 1, out, found);
        } else {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcasecmp(e->d_name + n - 4, ".pkg")) {
                char sz[32];
                human_size((unsigned long long)st.st_size, sz, sizeof(sz));
                sb_appendf(out, "%s (%s)\n", p, sz);
                (*found)++;
            }
        }
    }
    closedir(d);
}

static int t_find_pkg_files(StrBuf *out)
{
    static const char *const roots[] = { "/mnt/usb0", "/mnt/usb1", "/data", NULL };
    int found = 0;
    for (int i = 0; roots[i]; i++)
        find_pkgs(roots[i], 3, out, &found);
    if (!found)
        sb_append(out, "No .pkg files found on /mnt/usb0, /mnt/usb1 or /data (searched 3 folders deep).");
    return 0;
}

static int t_list_installed_apps(StrBuf *out)
{
    DIR *d = opendir("/user/app");
    if (!d)
        return fail_errno(out, "list", "/user/app");
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 9)
            continue;
        char title[256] = "", sfo[256];
        static const char *const metas[] = { "/system_data/priv/appmeta/%s/param.sfo",
                                             "/user/appmeta/%s/param.sfo",
                                             "/user/app/%s/sce_sys/param.sfo", NULL };
        for (int i = 0; metas[i] && !title[0]; i++) {
            snprintf(sfo, sizeof(sfo), metas[i], e->d_name);
            sfo_get_string(sfo, "TITLE", title, sizeof(title));
        }
        sb_appendf(out, "%s  %s\n", e->d_name, title[0] ? title : "(name not readable)");
        n++;
    }
    closedir(d);
    if (!n)
        sb_append(out, "No apps found in /user/app.");
    return 0;
}

int tools_run(Chat *chat, const char *name, const JVal *in, StrBuf *out)
{
    if (!strcmp(name, "get_system_info")) {
        plat_system_info(out);
        return 0;
    }
    if (!strcmp(name, "list_directory"))
        return t_list_directory(in, out);
    if (!strcmp(name, "read_text_file"))
        return t_read_text_file(in, out);
    if (!strcmp(name, "write_text_file"))
        return t_write_text_file(chat, in, out);
    if (!strcmp(name, "make_directory"))
        return t_make_directory(chat, in, out);
    if (!strcmp(name, "copy_path"))
        return t_copy_move(chat, in, out, 0);
    if (!strcmp(name, "move_path"))
        return t_copy_move(chat, in, out, 1);
    if (!strcmp(name, "delete_path"))
        return t_delete_path(chat, in, out);
    if (!strcmp(name, "find_pkg_files"))
        return t_find_pkg_files(out);
    if (!strcmp(name, "list_installed_apps"))
        return t_list_installed_apps(out);
    if (!strcmp(name, "launch_app")) {
        const char *tid = in_str(in, "title_id");
        if (!tid || strlen(tid) != 9)
            return fail(out, "title_id must look like CUSA00001", NULL);
        if (!confirm(chat, "Launch %s ?\nClaude for PS4 will close.%s", tid, ""))
            return fail(out, "The user declined.", NULL);
        return plat_launch_app(tid, out);
    }
    if (!strcmp(name, "show_notification")) {
        const char *t = in_str(in, "text");
        if (!t)
            return fail(out, "text is required", NULL);
        if (plat_notify(t) < 0)
            return fail(out, "notification failed", NULL);
        sb_append(out, "Shown.");
        return 0;
    }
    return fail(out, "Unknown tool %s", name);
}

void tools_describe(const char *name, const JVal *in, char *out, int outlen)
{
    const char *a = in_str(in, "path");
    if (!a) a = in_str(in, "source");
    if (!a) a = in_str(in, "title_id");
    snprintf(out, (size_t)outlen, "Tool: %s%s%s", name, a ? "  " : "", a ? a : "");
}
