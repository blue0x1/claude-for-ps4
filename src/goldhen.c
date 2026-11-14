// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// GoldHEN manager: cheats and game patches available per game (and whether
// they match the installed version), plugins and where they are on
// (plugins.ini), and GoldHEN's own settings (config.ini).
#include "goldhen.h"
#include "plat.h"
#include "tools.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_gh[128] = "/data/GoldHEN";

void goldhen_set_dir(const char *dir) { snprintf(g_gh, sizeof(g_gh), "%s", dir); }

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
        if (!isupper((unsigned char)t[i]))
            return 0;
    for (int i = 4; i < 9; i++)
        if (!isdigit((unsigned char)t[i]))
            return 0;
    return 1;
}

static char *read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    if (!buf)
        return NULL;
    buf[n] = 0;
    if (len)
        *len = n;
    return buf;
}

static int write_file(const char *path, const char *data, size_t n, StrBuf *out)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return fail(out, "Cannot write %s", tmp);
    int ok = fwrite(data, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return fail(out, "Cannot save %s", path);
    }
    return 0;
}

static void backup_file(const char *path)
{
    size_t n;
    char *d = read_file(path, 1 << 20, &n);
    if (!d)
        return;
    char b[300];
    snprintf(b, sizeof(b), "%s.claude-bak", path);
    FILE *f = fopen(b, "wb");
    if (f) {
        fwrite(d, 1, n, f);
        fclose(f);
    }
    free(d);
}

// Value of attr="..." inside the tag text starting at p (up to the next '>').
static void xml_attr(const char *p, const char *attr, char *out, size_t n)
{
    out[0] = 0;
    const char *end = strchr(p, '>');
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=\"", attr);
    const char *a = strstr(p, pat);
    if (!a || (end && a > end))
        return;
    a += strlen(pat);
    const char *q = strchr(a, '"');
    if (!q)
        return;
    size_t l = (size_t)(q - a) < n - 1 ? (size_t)(q - a) : n - 1;
    memcpy(out, a, l);
    out[l] = 0;
}

// ---- cheats -----------------------------------------------------------------

static void list_cheat_json(const char *path, const char *fname, const char *ver_now, StrBuf *out)
{
    char *d = read_file(path, 512 * 1024, NULL);
    JVal *v = d ? json_parse(d) : NULL;
    free(d);
    const char *ver = v ? json_get_str(v, "version") : NULL;
    JVal *mods = v ? json_get(v, "mods") : NULL;
    int match = ver && ver_now[0] && !strcmp(ver, ver_now);
    sb_appendf(out, "  %s (cheats for game version %s)%s\n", fname, ver ? ver : "?",
               match ? "  <- matches the installed version" : "");
    for (int i = 0; mods && mods->type == J_ARR && i < mods->count && i < 40; i++) {
        const char *nm = json_get_str(mods->items[i], "name");
        const char *ty = json_get_str(mods->items[i], "type");
        sb_appendf(out, "    - %s%s\n", nm ? nm : "?", ty && !strcmp(ty, "button") ? " (button)" : "");
    }
    if (mods && mods->count > 40)
        sb_appendf(out, "    ... and %d more\n", mods->count - 40);
    json_free(v);
}

static void list_cheat_shn(const char *path, const char *fname, const char *ver_now, StrBuf *out)
{
    char *d = read_file(path, 512 * 1024, NULL);
    if (!d)
        return;
    char ver[32];
    const char *t = strstr(d, "<Trainer");
    xml_attr(t ? t : d, "Version", ver, sizeof(ver));
    int match = ver[0] && ver_now[0] && !strcmp(ver, ver_now);
    sb_appendf(out, "  %s (cheats for game version %s)%s\n", fname, ver[0] ? ver : "?",
               match ? "  <- matches the installed version" : "");
    int n = 0;
    for (const char *p = strstr(d, "<Cheat "); p; p = strstr(p + 1, "<Cheat ")) {
        char txt[128];
        xml_attr(p, "Text", txt, sizeof(txt));
        if (n < 40)
            sb_appendf(out, "    - %s\n", txt);
        n++;
    }
    if (n > 40)
        sb_appendf(out, "    ... and %d more\n", n - 40);
    free(d);
}

static int t_list_cheats(const JVal *in, StrBuf *out)
{
    const char *tid = json_get_str(in, "title_id");
    if (!title_ok(tid))
        return fail(out, "title_id must look like CUSA00001", NULL);
    char name[128] = "", ver_now[16] = "";
    tools_app_title(tid, name, sizeof(name));
    tools_app_sfo(tid, "APP_VER", ver_now, sizeof(ver_now));
    sb_appendf(out, "%s %s, installed version %s\n", tid, name, ver_now[0] ? ver_now : "unknown");
    static const char *const kinds[] = { "json", "shn", "mc4", NULL };
    int found = 0;
    for (int k = 0; kinds[k]; k++) {
        char dir[300];
        snprintf(dir, sizeof(dir), "%s/cheats/%s", g_gh, kinds[k]);
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (strncmp(e->d_name, tid, 9) || e->d_name[9] != '_')
                continue;
            char p[600];
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            if (k == 0)
                list_cheat_json(p, e->d_name, ver_now, out);
            else if (k == 1)
                list_cheat_shn(p, e->d_name, ver_now, out);
            else
                sb_appendf(out, "  %s (mc4 format)\n", e->d_name);
            found++;
        }
        if (d)
            closedir(d);
    }
    if (!found)
        sb_append(out, "No cheat files for this game. GoldHEN can download the cheat collection: "
                       "Settings > GoldHEN > Cheats > Download (or update) cheats.\n");
    else
        sb_append(out, "Turn cheats on in the game with the GoldHEN cheat menu (hold the button combo set in GoldHEN). "
                       "Only cheats for the installed version work.\n");
    return 0;
}

// ---- patches ----------------------------------------------------------------

static int t_list_game_patches(const JVal *in, StrBuf *out)
{
    const char *tid = json_get_str(in, "title_id");
    if (!title_ok(tid))
        return fail(out, "title_id must look like CUSA00001", NULL);
    char p[300], name[128] = "", ver_now[16] = "";
    snprintf(p, sizeof(p), "%s/patches/xml/%s.xml", g_gh, tid);
    tools_app_title(tid, name, sizeof(name));
    tools_app_sfo(tid, "APP_VER", ver_now, sizeof(ver_now));
    char *d = read_file(p, 2 * 1024 * 1024, NULL);
    sb_appendf(out, "%s %s, installed version %s\n", tid, name, ver_now[0] ? ver_now : "unknown");
    if (!d) {
        sb_append(out, "No game patches for this game. GoldHEN can download the patch collection: "
                       "Settings > GoldHEN > Plugins > Game Patch > Download patches.");
        return 0;
    }
    int n = 0, matching = 0;
    for (const char *m = strstr(d, "<Metadata"); m && n < 60; m = strstr(m + 1, "<Metadata")) {
        char nm[128], note[200], author[96], appver[24];
        xml_attr(m, "Name", nm, sizeof(nm));
        xml_attr(m, "Note", note, sizeof(note));
        xml_attr(m, "Author", author, sizeof(author));
        xml_attr(m, "AppVer", appver, sizeof(appver));
        int match = ver_now[0] && (!strcmp(appver, ver_now) || !strcmp(appver, "mask"));
        matching += match;
        sb_appendf(out, "  - %s (game version %s%s)%s%s by %s\n", nm, appver, match ? ", matches" : "",
                   note[0] ? ": " : "", note, author[0] ? author : "?");
        n++;
    }
    free(d);
    sb_appendf(out, "%d patch%s, %d for the installed version. Turn them on in GoldHEN's in-game menu "
                    "(Game Patch must be on in Plugins).",
               n, n == 1 ? "" : "es", matching);
    return 0;
}

// ---- plugins.ini ----------------------------------------------------------------

typedef struct {
    char section[32];
    char line[300];
} IniLine;

// Reads plugins.ini into (section, line) pairs; section headers have line "".
static int read_plugins_ini(IniLine **lines, int *n)
{
    char p[300];
    snprintf(p, sizeof(p), "%s/plugins.ini", g_gh);
    *lines = NULL;
    *n = 0;
    char *d = read_file(p, 256 * 1024, NULL);
    if (!d)
        return -1;
    char *s = d;
    if (!strncmp(s, "\xEF\xBB\xBF", 3))
        s += 3;
    char section[32] = "";
    int cap = 0;
    for (char *tok = s; tok && *tok;) {
        char *nl = strchr(tok, '\n');
        if (nl)
            *nl = 0;
        char *e = tok + strlen(tok);
        while (e > tok && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        while (*tok == ' ' || *tok == '\t')
            tok++;
        if (*tok) {
            if (*n == cap) {
                cap = cap ? cap * 2 : 32;
                *lines = realloc(*lines, (size_t)cap * sizeof(IniLine));
            }
            IniLine *l = &(*lines)[(*n)++];
            if (*tok == '[') {
                char *c = strchr(tok, ']');
                if (c)
                    *c = 0;
                snprintf(section, sizeof(section), "%s", tok + 1);
                snprintf(l->section, sizeof(l->section), "%s", section);
                l->line[0] = 0;
            } else {
                snprintf(l->section, sizeof(l->section), "%s", section);
                snprintf(l->line, sizeof(l->line), "%s", tok);
            }
        }
        tok = nl ? nl + 1 : NULL;
    }
    free(d);
    return 0;
}

static int t_list_plugins(StrBuf *out)
{
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/plugins", g_gh);
    IniLine *l;
    int n;
    int have_ini = read_plugins_ini(&l, &n) == 0;
    DIR *d = opendir(dir);
    if (!d)
        return fail(out, "No GoldHEN plugins folder (%s).", dir);
    sb_append(out, "Plugins in /data/GoldHEN/plugins and where they are on (plugins.ini):\n");
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".prx"))
            continue;
        count++;
        StrBuf where;
        sb_init(&where);
        for (int i = 0; have_ini && i < n; i++) {
            const char *b = strrchr(l[i].line, '/');
            if (l[i].line[0] && b && !strcmp(b + 1, e->d_name))
                sb_appendf(&where, "%s%s", where.len ? ", " : "", !strcmp(l[i].section, "default") ? "all games" : l[i].section);
        }
        sb_appendf(out, "  %-28s %s\n", e->d_name, where.len ? where.data : "off");
        sb_free(&where);
    }
    closedir(d);
    // Lines pointing at files that are not there.
    for (int i = 0; have_ini && i < n; i++) {
        struct stat st;
        if (l[i].line[0] && l[i].line[0] == '/' && stat(l[i].line, &st) < 0)
            sb_appendf(out, "  Warning: plugins.ini [%s] lists %s, which does not exist.\n", l[i].section, l[i].line);
    }
    free(l);
    sb_appendf(out, "%d plugins. game_patch.prx applies game patches; plugin_loader.prx is GoldHEN's own loader.", count);
    return 0;
}

static int t_set_plugin(Chat *chat, const JVal *in, StrBuf *out)
{
    const char *plugin = json_get_str(in, "plugin");
    const char *tid = json_get_str(in, "title_id");
    JVal *en = json_get(in, "enabled");
    if (!plugin || !plugin[0] || strchr(plugin, '/') || strstr(plugin, ".."))
        return fail(out, "plugin is the file name from list_plugins, like afr.prx", NULL);
    if (!en || en->type != J_BOOL)
        return fail(out, "enabled (true or false) is required", NULL);
    if (!tid || !tid[0] || !strcmp(tid, "all"))
        tid = "default";
    if (strcmp(tid, "default") && !title_ok(tid))
        return fail(out, "title_id must look like CUSA00001, or all", NULL);
    char file[200], full[400];
    snprintf(file, sizeof(file), "%s%s", plugin, strstr(plugin, ".prx") ? "" : ".prx");
    snprintf(full, sizeof(full), "%s/plugins/%s", g_gh, file);
    struct stat st;
    if (en->boolean && stat(full, &st) < 0)
        return fail(out, "There is no plugin %s in /data/GoldHEN/plugins.", file);
    IniLine *l;
    int n;
    if (read_plugins_ini(&l, &n) < 0) {
        l = NULL;
        n = 0;
    }
    int has = 0;
    for (int i = 0; i < n; i++) {
        const char *b = strrchr(l[i].line, '/');
        has |= l[i].line[0] && !strcmp(l[i].section, tid) && b && !strcmp(b + 1, file);
    }
    const char *scope = !strcmp(tid, "default") ? "all games" : tid;
    if (has == en->boolean) {
        free(l);
        sb_appendf(out, "%s is already %s for %s.", file, en->boolean ? "on" : "off", scope);
        return 0;
    }
    char q[400], gname[128] = "";
    if (strcmp(tid, "default"))
        tools_app_title(tid, gname, sizeof(gname));
    snprintf(q, sizeof(q), "%s the GoldHEN plugin %s for %s%s%s%s?\n(Works from the next game start.)",
             en->boolean ? "Turn on" : "Turn off", file, scope, gname[0] ? " (" : "", gname, gname[0] ? ")" : "");
    if (!chat_ask_confirm(chat, q)) {
        free(l);
        return fail(out, "The user declined.", NULL);
    }
    // Rebuild the file.
    StrBuf s;
    sb_init(&s);
    int section_seen = 0;
    for (int i = 0; i < n; i++) {
        if (!l[i].line[0]) {
            if (s.len)
                sb_append(&s, "\n");
            sb_appendf(&s, "[%s]\n", l[i].section);
            if (!strcmp(l[i].section, tid) && en->boolean) {
                sb_appendf(&s, "/data/GoldHEN/plugins/%s\n", file);
                section_seen = 1;
            }
            continue;
        }
        const char *b = strrchr(l[i].line, '/');
        if (!en->boolean && !strcmp(l[i].section, tid) && b && !strcmp(b + 1, file))
            continue;
        sb_appendf(&s, "%s\n", l[i].line);
    }
    if (en->boolean && !section_seen)
        sb_appendf(&s, "%s[%s]\n/data/GoldHEN/plugins/%s\n", s.len ? "\n" : "", tid, file);
    free(l);
    char ini[300];
    snprintf(ini, sizeof(ini), "%s/plugins.ini", g_gh);
    backup_file(ini);
    int r = write_file(ini, s.data ? s.data : "", s.len, out);
    sb_free(&s);
    if (r)
        return 1;
    sb_appendf(out, "%s is now %s for %s (old file kept as plugins.ini.claude-bak). It applies the next time the game starts.",
               file, en->boolean ? "on" : "off", scope);
    return 0;
}

// ---- config.ini ---------------------------------------------------------------

static int t_goldhen_settings(StrBuf *out)
{
    char p[300];
    snprintf(p, sizeof(p), "%s/config.ini", g_gh);
    char *d = read_file(p, 64 * 1024, NULL);
    if (!d)
        return fail(out, "GoldHEN's config.ini was not found (%s).", p);
    sb_append(out, "GoldHEN settings (config.ini; 1 = on, 0 = off):\n");
    sb_append(out, d);
    free(d);
    return 0;
}

static int t_set_goldhen_setting(Chat *chat, const JVal *in, StrBuf *out)
{
    const char *sec = json_get_str(in, "section"), *key = json_get_str(in, "key");
    JVal *vv = json_get(in, "value");
    char value[32];
    if (vv && vv->type == J_NUM)
        snprintf(value, sizeof(value), "%d", (int)vv->num);
    else if (vv && vv->type == J_STR)
        snprintf(value, sizeof(value), "%s", vv->str);
    else if (vv && vv->type == J_BOOL)
        snprintf(value, sizeof(value), "%d", vv->boolean);
    else
        return fail(out, "section, key and value are required", NULL);
    for (const char *c = value; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-')
            return fail(out, "value may only contain letters, digits, . _ -", NULL);
    if (!sec || !key || !value[0])
        return fail(out, "section, key and value are required", NULL);
    char p[300];
    snprintf(p, sizeof(p), "%s/config.ini", g_gh);
    char *d = read_file(p, 64 * 1024, NULL);
    if (!d)
        return fail(out, "GoldHEN's config.ini was not found (%s).", p);
    // Find [sec] then "key =" before the next section.
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "[%s]", sec);
    char *s = strstr(d, hdr);
    char *line = NULL;
    while (s) {
        char *nx = strstr(s + 1, "\n[");
        size_t kl = strlen(key);
        for (char *q = strchr(s, '\n'); q && (!nx || q < nx); q = strchr(q + 1, '\n')) {
            char *k = q + 1;
            if (!strncmp(k, key, kl) && (k[kl] == ' ' || k[kl] == '=')) {
                line = k;
                break;
            }
        }
        break;
    }
    if (!line) {
        free(d);
        return fail(out, "No such setting in GoldHEN's config.ini (see goldhen_settings).", NULL);
    }
    char *eol = line + strcspn(line, "\r\n");
    char q[300];
    snprintf(q, sizeof(q), "Change the GoldHEN setting\n[%s] %s to %s ?", sec, key, value);
    if (!chat_ask_confirm(chat, q)) {
        free(d);
        return fail(out, "The user declined.", NULL);
    }
    StrBuf nb;
    sb_init(&nb);
    sb_appendn(&nb, d, (size_t)(line - d));
    sb_appendf(&nb, "%s = %s", key, value);
    sb_append(&nb, eol);
    free(d);
    backup_file(p);
    int r = write_file(p, nb.data, nb.len, out);
    sb_free(&nb);
    if (r)
        return 1;
    sb_appendf(out, "Set [%s] %s = %s (old file kept as config.ini.claude-bak). Some settings apply only after GoldHEN "
                    "is loaded again (restart and run GoldHEN).",
               sec, key, value);
    return 0;
}

int goldhen_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out)
{
    if (!strcmp(name, "list_cheats"))
        return t_list_cheats(in, out);
    if (!strcmp(name, "list_game_patches"))
        return t_list_game_patches(in, out);
    if (!strcmp(name, "list_plugins"))
        return t_list_plugins(out);
    if (!strcmp(name, "set_plugin"))
        return t_set_plugin(chat, in, out);
    if (!strcmp(name, "goldhen_settings"))
        return t_goldhen_settings(out);
    if (!strcmp(name, "set_goldhen_setting"))
        return t_set_goldhen_setting(chat, in, out);
    return -1;
}
