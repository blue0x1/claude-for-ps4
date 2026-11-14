// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "json.h"
#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    int depth;
} Parser;

static JVal *parse_value(Parser *ps);

static void skip_ws(Parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

static JVal *new_val(JType t)
{
    JVal *v = calloc(1, sizeof(JVal));
    if (v)
        v->type = t;
    return v;
}

static void utf8_put(StrBuf *sb, unsigned cp)
{
    char b[4];
    if (cp < 0x80) {
        sb_appendc(sb, (char)cp);
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        sb_appendn(sb, b, 2);
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        sb_appendn(sb, b, 3);
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        sb_appendn(sb, b, 4);
    }
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *parse_string_raw(Parser *ps)
{
    if (*ps->p != '"')
        return NULL;
    ps->p++;
    StrBuf sb;
    sb_init(&sb);
    sb_reserve(&sb, 16);
    for (;;) {
        char c = *ps->p;
        if (c == 0) {
            sb_free(&sb);
            return NULL;
        }
        ps->p++;
        if (c == '"')
            break;
        if (c != '\\') {
            sb_appendc(&sb, c);
            continue;
        }
        char e = *ps->p++;
        switch (e) {
        case '"': sb_appendc(&sb, '"'); break;
        case '\\': sb_appendc(&sb, '\\'); break;
        case '/': sb_appendc(&sb, '/'); break;
        case 'b': sb_appendc(&sb, '\b'); break;
        case 'f': sb_appendc(&sb, '\f'); break;
        case 'n': sb_appendc(&sb, '\n'); break;
        case 'r': sb_appendc(&sb, '\r'); break;
        case 't': sb_appendc(&sb, '\t'); break;
        case 'u': {
            unsigned cp;
            if (!hex4(ps->p, &cp)) {
                sb_free(&sb);
                return NULL;
            }
            ps->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    ps->p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = 0xFFFD;
            utf8_put(&sb, cp);
            break;
        }
        default:
            sb_free(&sb);
            return NULL;
        }
    }
    return sb.data;
}

static int push_item(JVal *v, JVal *item, char *key)
{
    JVal **items = realloc(v->items, sizeof(JVal *) * (size_t)(v->count + 1));
    if (!items)
        return 0;
    v->items = items;
    if (v->type == J_OBJ) {
        char **keys = realloc(v->keys, sizeof(char *) * (size_t)(v->count + 1));
        if (!keys)
            return 0;
        v->keys = keys;
        v->keys[v->count] = key;
    }
    v->items[v->count++] = item;
    return 1;
}

static JVal *parse_value(Parser *ps)
{
    skip_ws(ps);
    if (++ps->depth > 64)
        return NULL;
    JVal *v = NULL;
    char c = *ps->p;
    if (c == '{') {
        ps->p++;
        v = new_val(J_OBJ);
        skip_ws(ps);
        if (*ps->p == '}') {
            ps->p++;
            goto done;
        }
        for (;;) {
            skip_ws(ps);
            char *key = parse_string_raw(ps);
            if (!key)
                goto fail;
            skip_ws(ps);
            if (*ps->p != ':') {
                free(key);
                goto fail;
            }
            ps->p++;
            JVal *item = parse_value(ps);
            if (!item || !push_item(v, item, key)) {
                free(key);
                json_free(item);
                goto fail;
            }
            skip_ws(ps);
            if (*ps->p == ',') {
                ps->p++;
                continue;
            }
            if (*ps->p == '}') {
                ps->p++;
                break;
            }
            goto fail;
        }
    } else if (c == '[') {
        ps->p++;
        v = new_val(J_ARR);
        skip_ws(ps);
        if (*ps->p == ']') {
            ps->p++;
            goto done;
        }
        for (;;) {
            JVal *item = parse_value(ps);
            if (!item || !push_item(v, item, NULL)) {
                json_free(item);
                goto fail;
            }
            skip_ws(ps);
            if (*ps->p == ',') {
                ps->p++;
                continue;
            }
            if (*ps->p == ']') {
                ps->p++;
                break;
            }
            goto fail;
        }
    } else if (c == '"') {
        char *s = parse_string_raw(ps);
        if (!s)
            return NULL;
        v = new_val(J_STR);
        v->str = s;
    } else if (!strncmp(ps->p, "true", 4)) {
        ps->p += 4;
        v = new_val(J_BOOL);
        v->boolean = 1;
    } else if (!strncmp(ps->p, "false", 5)) {
        ps->p += 5;
        v = new_val(J_BOOL);
    } else if (!strncmp(ps->p, "null", 4)) {
        ps->p += 4;
        v = new_val(J_NULL);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double d = strtod(ps->p, &end);
        if (end == ps->p)
            return NULL;
        ps->p = end;
        v = new_val(J_NUM);
        v->num = d;
    } else {
        return NULL;
    }
done:
    ps->depth--;
    return v;
fail:
    json_free(v);
    return NULL;
}

JVal *json_parse(const char *text)
{
    if (!text)
        return NULL;
    Parser ps = { text, 0 };
    JVal *v = parse_value(&ps);
    if (!v)
        return NULL;
    skip_ws(&ps);
    if (*ps.p) {
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(JVal *v)
{
    if (!v)
        return;
    for (int i = 0; i < v->count; i++) {
        json_free(v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

JVal *json_get(const JVal *v, const char *key)
{
    if (!v || v->type != J_OBJ)
        return NULL;
    for (int i = 0; i < v->count; i++)
        if (!strcmp(v->keys[i], key))
            return v->items[i];
    return NULL;
}

const char *json_get_str(const JVal *v, const char *key)
{
    JVal *s = json_get(v, key);
    return (s && s->type == J_STR) ? s->str : NULL;
}

JVal *json_new_str(const char *s)
{
    JVal *v = new_val(J_STR);
    v->str = strdup(s ? s : "");
    return v;
}

JVal *json_new_obj(void) { return new_val(J_OBJ); }
JVal *json_new_arr(void) { return new_val(J_ARR); }

void json_obj_set(JVal *obj, const char *key, JVal *item)
{
    for (int i = 0; i < obj->count; i++) {
        if (!strcmp(obj->keys[i], key)) {
            json_free(obj->items[i]);
            obj->items[i] = item;
            return;
        }
    }
    push_item(obj, item, strdup(key));
}

void json_arr_push(JVal *arr, JVal *item)
{
    push_item(arr, item, NULL);
}

void json_write(const JVal *v, StrBuf *out)
{
    if (!v) {
        sb_append(out, "null");
        return;
    }
    switch (v->type) {
    case J_NULL: sb_append(out, "null"); break;
    case J_BOOL: sb_append(out, v->boolean ? "true" : "false"); break;
    case J_NUM:
        if (v->num == (double)(long long)v->num)
            sb_appendf(out, "%lld", (long long)v->num);
        else
            sb_appendf(out, "%.17g", v->num);
        break;
    case J_STR: sb_append_json_string(out, v->str); break;
    case J_ARR:
        sb_appendc(out, '[');
        for (int i = 0; i < v->count; i++) {
            if (i) sb_appendc(out, ',');
            json_write(v->items[i], out);
        }
        sb_appendc(out, ']');
        break;
    case J_OBJ:
        sb_appendc(out, '{');
        for (int i = 0; i < v->count; i++) {
            if (i) sb_appendc(out, ',');
            sb_append_json_string(out, v->keys[i]);
            sb_appendc(out, ':');
            json_write(v->items[i], out);
        }
        sb_appendc(out, '}');
        break;
    }
}
