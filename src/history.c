// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
#include "history.h"
#include "json.h"
#include "strbuf.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *const ROLE_KEYS[] = { "user", "assistant", "notice", "tool" };
static const char *const ROLE_LABELS[] = { "You", "Claude", "Note", "Tool" };

int history_id_ok(const char *id)
{
    if (!id || !*id || strlen(id) >= HISTORY_ID_MAX)
        return 0;
    for (const char *p = id; *p; p++) {
        char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_'))
            return 0;
    }
    return 1;
}

void history_new_id(char id[HISTORY_ID_MAX])
{
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    snprintf(id, HISTORY_ID_MAX, "chat-%04d%02d%02d-%02d%02d%02d", tm->tm_year + 1900, tm->tm_mon + 1,
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
}

void history_title(const char *text, char *out, size_t n)
{
    out[0] = 0;
    if (!text || n < 2)
        return;
    while (*text == '\n' || *text == '\r' || *text == ' ' || *text == '\t')
        text++;
    size_t len = strcspn(text, "\r\n");
    if (len > n - 1) {
        len = n - 1;
        // Do not cut a UTF-8 sequence in half.
        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80)
            len--;
    }
    memcpy(out, text, len);
    out[len] = 0;
}

static void path_of(char *out, size_t n, const char *dir, const char *id, const char *ext)
{
    snprintf(out, n, "%s/%s.%s", dir, id, ext);
}

static int write_file(const char *path, const StrBuf *sb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(sb->data ? sb->data : "", 1, sb->len, f) == sb->len;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

int history_save(Chat *c, const char *dir, const char *id)
{
    if (!history_id_ok(id))
        return -1;
    mkdir("/data/claude", 0777);
    mkdir(dir, 0777);

    StrBuf txt, js;
    sb_init(&txt);
    sb_init(&js);
    char title[HISTORY_TITLE_MAX] = "";

    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < c->count; i++) {
        const Msg *m = &c->msgs[i];
        if (!title[0] && m->role == ROLE_USER)
            history_title(m->text.data, title, sizeof(title));
    }
    // The title goes on the first line so the list can read it quickly.
    sb_append(&js, "{\"v\":1,\"title\":");
    sb_append_json_string(&js, title);
    sb_append(&js, ",\n\"conv\":");
    sb_append_json_string(&js, c->conv_id);
    sb_append(&js, ",\"msgs\":[");
    for (int i = 0; i < c->count; i++) {
        const Msg *m = &c->msgs[i];
        const char *t = m->text.data ? m->text.data : "";
        sb_appendf(&txt, "%s:\n%s\n\n", ROLE_LABELS[m->role], t);
        sb_append(&js, i ? ",\n{\"r\":\"" : "\n{\"r\":\"");
        sb_append(&js, ROLE_KEYS[m->role]);
        sb_append(&js, "\",\"t\":");
        sb_append_json_string(&js, t);
        sb_append(&js, "}");
    }
    sb_append(&js, "],\n\"api\":[");
    for (int i = 0; i < c->api_count; i++) {
        if (i)
            sb_append(&js, ",\n");
        sb_append(&js, c->api[i]);
    }
    sb_append(&js, "]}\n");
    pthread_mutex_unlock(&c->lock);

    char path[300];
    path_of(path, sizeof(path), dir, id, "txt");
    int r1 = write_file(path, &txt);
    path_of(path, sizeof(path), dir, id, "json");
    int r2 = write_file(path, &js);
    sb_free(&txt);
    sb_free(&js);
    return (r1 == 0 && r2 == 0) ? 0 : -1;
}

// ---- list -------------------------------------------------------------------

static int has_ext(const char *name, const char *ext, size_t *base_len)
{
    size_t n = strlen(name), e = strlen(ext);
    if (n <= e + 1 || name[n - e - 1] != '.' || strcmp(name + n - e, ext))
        return 0;
    *base_len = n - e - 1;
    return 1;
}

static void title_from_json(const char *path, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    char line[1024];
    if (fgets(line, sizeof(line) - 16, f)) {
        // First line is {"v":1,"title":"...",  -> close it and parse.
        size_t len = strcspn(line, "\r\n");
        line[len] = 0;
        if (len && line[len - 1] == ',') {
            line[len - 1] = '}';
            JVal *v = json_parse(line);
            const char *t = v ? json_get_str(v, "title") : NULL;
            if (t)
                history_title(t, out, n);
            json_free(v);
        }
    }
    fclose(f);
}

static void title_from_txt(const char *path, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    char buf[2048];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;
    const char *p = strncmp(buf, "You:\n", 5) ? strstr(buf, "\n\nYou:\n") : buf - 2;
    if (p)
        history_title(p + 7, out, n);
}

static int cmp_desc(const void *a, const void *b)
{
    return strcmp(((const HistoryEntry *)b)->id, ((const HistoryEntry *)a)->id);
}

int history_list(const char *dir, HistoryEntry *out, int max)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    // Collect every chat id (newest names sort last in time order).
    int cap = 256, count = 0;
    HistoryEntry *all = malloc(sizeof(HistoryEntry) * (size_t)cap);
    struct dirent *e;
    while (all && (e = readdir(d))) {
        size_t bl;
        if (!(has_ext(e->d_name, "json", &bl) || has_ext(e->d_name, "txt", &bl)) || bl >= HISTORY_ID_MAX)
            continue;
        char id[HISTORY_ID_MAX];
        memcpy(id, e->d_name, bl);
        id[bl] = 0;
        if (!history_id_ok(id))
            continue;
        int dup = 0;
        for (int i = 0; i < count && !dup; i++)
            dup = !strcmp(all[i].id, id);
        if (dup)
            continue;
        if (count == cap) {
            HistoryEntry *n = realloc(all, sizeof(HistoryEntry) * (size_t)(cap * 2));
            if (!n)
                break;
            all = n;
            cap *= 2;
        }
        snprintf(all[count].id, HISTORY_ID_MAX, "%s", id);
        all[count].title[0] = 0;
        count++;
    }
    closedir(d);
    if (!all)
        return 0;
    qsort(all, (size_t)count, sizeof(HistoryEntry), cmp_desc);
    int n = count < max ? count : max;
    int kept = 0;
    for (int i = 0; i < n; i++) {
        char path[300];
        HistoryEntry *h = &all[i];
        path_of(path, sizeof(path), dir, h->id, "json");
        title_from_json(path, h->title, sizeof(h->title));
        if (!h->title[0]) {
            path_of(path, sizeof(path), dir, h->id, "txt");
            title_from_txt(path, h->title, sizeof(h->title));
        }
        if (!h->title[0])
            continue;  // empty chat (nothing sent)
        out[kept++] = *h;
    }
    free(all);
    return kept;
}

// ---- load -------------------------------------------------------------------

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (n >= 0 && n < 64L * 1024 * 1024) ? malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        buf[n] = 0;
    return buf;
}

static int role_of(const char *s)
{
    for (int i = 0; i < 4; i++)
        if (s && !strcmp(s, ROLE_KEYS[i]))
            return i;
    return -1;
}

// Plain text history for chats saved before .json files existed: rebuilds
// the API history from your messages and Claude's replies.
static void push_text_turn(Chat *c, Role role, StrBuf *pending, Role *pending_role, int *have)
{
    if (*have && *pending_role != role) {
        StrBuf m;
        sb_init(&m);
        sb_append(&m, *pending_role == ROLE_USER ? "{\"role\":\"user\",\"content\":"
                                                 : "{\"role\":\"assistant\",\"content\":");
        sb_append_json_string(&m, pending->data ? pending->data : "");
        sb_append(&m, "}");
        chat_api_push(c, m.data);
        sb_free(&m);
        pending->len = 0;
        if (pending->data)
            pending->data[0] = 0;
        *have = 0;
    }
}

static void add_turn(Chat *c, Role role, const char *text, StrBuf *pending, Role *pending_role, int *have)
{
    if (role != ROLE_USER && role != ROLE_ASSISTANT)
        return;
    if (!*have && role == ROLE_ASSISTANT && c->api_count == 0)
        return;  // history must start with a user message
    if (!text[0])
        return;
    push_text_turn(c, role, pending, pending_role, have);
    if (*have)
        sb_append(pending, "\n\n");
    sb_append(pending, text);
    *pending_role = role;
    *have = 1;
}

static void finish_turns(Chat *c, StrBuf *pending, Role pending_role, int have)
{
    // An unanswered last message is dropped from the API history.
    if (have && pending_role == ROLE_ASSISTANT)
        push_text_turn(c, ROLE_USER, pending, &pending_role, &have);
}

static int load_txt(Chat *c, char *buf)
{
    static const char *const heads[] = { "You:\n", "Claude:\n", "Note:\n", "Tool:\n" };
    static const Role roles[] = { ROLE_USER, ROLE_ASSISTANT, ROLE_NOTICE, ROLE_TOOL };
    StrBuf pending;
    sb_init(&pending);
    Role prole = ROLE_USER;
    int have = 0, found = 0;
    char *p = buf;
    while (*p) {
        int k = -1;
        for (int i = 0; i < 4 && k < 0; i++)
            if (!strncmp(p, heads[i], strlen(heads[i])))
                k = i;
        if (k < 0)
            break;
        char *body = p + strlen(heads[k]);
        // The message runs to the next "\n\n<Head>:\n" or the end.
        char *end = NULL;
        for (char *q = strstr(body, "\n\n"); q; q = strstr(q + 1, "\n\n")) {
            int next = 0;
            for (int i = 0; i < 4 && !next; i++)
                next = !strncmp(q + 2, heads[i], strlen(heads[i]));
            if (next || !q[2]) {
                end = q;
                break;
            }
        }
        char save = 0;
        if (end) {
            save = *end;
            *end = 0;
        }
        chat_add(c, roles[k], body);
        add_turn(c, roles[k], body, &pending, &prole, &have);
        found++;
        if (!end)
            break;
        *end = save;
        p = end + 2;
    }
    finish_turns(c, &pending, prole, have);
    sb_free(&pending);
    return found ? 0 : -1;
}

static int asks_tool(const JVal *msg)
{
    JVal *content = json_get(msg, "content");
    if (!content || content->type != J_ARR)
        return 0;
    for (int i = 0; i < content->count; i++) {
        const char *t = json_get_str(content->items[i], "type");
        if (t && !strcmp(t, "tool_use"))
            return 1;
    }
    return 0;
}

static int load_json(Chat *c, const char *text)
{
    JVal *v = json_parse(text);
    if (!v)
        return -1;
    JVal *msgs = json_get(v, "msgs"), *api = json_get(v, "api");
    const char *conv = json_get_str(v, "conv");
    if (!msgs || msgs->type != J_ARR) {
        json_free(v);
        return -1;
    }
    for (int i = 0; i < msgs->count; i++) {
        int r = role_of(json_get_str(msgs->items[i], "r"));
        const char *t = json_get_str(msgs->items[i], "t");
        if (r >= 0 && t)
            chat_add(c, (Role)r, t);
    }
    int api_ok = api && api->type == J_ARR && api->count > 0;
    if (api_ok) {
        for (int i = 0; i < api->count; i++) {
            StrBuf sb;
            sb_init(&sb);
            json_write(api->items[i], &sb);
            chat_api_push(c, sb.data ? sb.data : "{}");
            sb_free(&sb);
        }
        // Drop an unanswered tail so the next message starts a new turn.
        while (c->api_count > 0) {
            JVal *last = api->items[c->api_count - 1];
            const char *role = json_get_str(last, "role");
            if (role && !strcmp(role, "assistant") && !asks_tool(last))
                break;
            // A turn that stopped half way (your message, or a tool call
            // without its answer) is not sent again.
            chat_api_truncate(c, c->api_count - 1);
        }
    } else {
        StrBuf pending;
        sb_init(&pending);
        Role prole = ROLE_USER;
        int have = 0;
        pthread_mutex_lock(&c->lock);
        int n = c->count;
        pthread_mutex_unlock(&c->lock);
        for (int i = 0; i < n; i++)
            add_turn(c, c->msgs[i].role, c->msgs[i].text.data ? c->msgs[i].text.data : "", &pending, &prole,
                     &have);
        finish_turns(c, &pending, prole, have);
        sb_free(&pending);
    }
    if (conv && history_id_ok(conv))
        snprintf(c->conv_id, sizeof(c->conv_id), "%s", conv);
    json_free(v);
    return 0;
}

int history_load(Chat *c, const char *dir, const char *id)
{
    if (!history_id_ok(id))
        return -1;
    char path[300];
    path_of(path, sizeof(path), dir, id, "json");
    char *buf = read_all(path);
    int json = buf != NULL;
    if (!buf) {
        path_of(path, sizeof(path), dir, id, "txt");
        buf = read_all(path);
    }
    if (!buf)
        return -1;
    chat_clear(c);
    int r = json ? load_json(c, buf) : load_txt(c, buf);
    free(buf);
    if (r != 0)
        chat_clear(c);
    c->dirty = 1;
    return r;
}
