// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
#include "setup.h"
#include "prefs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_REQUEST (16 * 1024)

#include "plat.h"

void setup_new_pin(Setup *s)
{
    uint32_t r = 0;
    if (plat_random(&r, sizeof(r)) < 0) {
        static unsigned seed;
        if (!seed)
            seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)s;
        seed = seed * 1103515245u + 12345u;
        r = seed ^ (seed >> 13);
    }
    snprintf(s->pin, sizeof(s->pin), "%06u", r % 1000000u);
    s->bad_pins = 0;
}

void setup_init(Setup *s, Chat *chat)
{
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
    config_defaults(&s->cfg);
    s->chat = chat;
    s->listen_fd = -1;
    setup_new_pin(s);
}

void setup_set_config(Setup *s, const Config *c, int ok)
{
    pthread_mutex_lock(&s->lock);
    s->cfg = *c;
    s->cfg_ok = ok;
    pthread_mutex_unlock(&s->lock);
}

int setup_take_config(Setup *s, Config *cfg)
{
    int got = 0;
    pthread_mutex_lock(&s->lock);
    if (s->cfg_changed) {
        snprintf(cfg->model, sizeof(cfg->model), "%s", s->cfg.model);
        snprintf(cfg->bridge_model, sizeof(cfg->bridge_model), "%s", s->cfg.bridge_model);
        snprintf(cfg->effort, sizeof(cfg->effort), "%s", s->cfg.effort);
        s->cfg_changed = 0;
        got = 1;
    }
    pthread_mutex_unlock(&s->lock);
    return got;
}

int setup_take_text(Setup *s, char *text, size_t len)
{
    int got = 0;
    pthread_mutex_lock(&s->lock);
    if (s->want_send) {
        snprintf(text, len, "%s", s->send_text);
        s->want_send = 0;
        got = 1;
    }
    pthread_mutex_unlock(&s->lock);
    return got;
}

void setup_set_chats(Setup *s, const HistoryEntry *list, int n, const char *current)
{
    pthread_mutex_lock(&s->lock);
    if (n > HISTORY_MAX)
        n = HISTORY_MAX;
    if (n > 0)
        memcpy(s->chats, list, sizeof(HistoryEntry) * (size_t)n);
    s->chat_count = n > 0 ? n : 0;
    snprintf(s->chat_cur, sizeof(s->chat_cur), "%s", current ? current : "");
    pthread_mutex_unlock(&s->lock);
}

void setup_set_profile(Setup *s, const char *name, const char *sub)
{
    pthread_mutex_lock(&s->lock);
    snprintf(s->profile, sizeof(s->profile), "%s", name ? name : "");
    snprintf(s->profile_sub, sizeof(s->profile_sub), "%s", sub ? sub : "");
    pthread_mutex_unlock(&s->lock);
}

int setup_take_open(Setup *s, char id[HISTORY_ID_MAX])
{
    int got = 0;
    pthread_mutex_lock(&s->lock);
    if (s->want_open[0]) {
        snprintf(id, HISTORY_ID_MAX, "%s", s->want_open);
        s->want_open[0] = 0;
        got = 1;
    }
    pthread_mutex_unlock(&s->lock);
    return got;
}

void setup_sign_out_all(Setup *s)
{
    pthread_mutex_lock(&s->lock);
    memset(s->sessions, 0, sizeof(s->sessions));
    s->next_session = 0;
    setup_new_pin(s);
    pthread_mutex_unlock(&s->lock);
}

static void html_escape(StrBuf *out, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '<': sb_append(out, "&lt;"); break;
        case '>': sb_append(out, "&gt;"); break;
        case '&': sb_append(out, "&amp;"); break;
        case '"': sb_append(out, "&quot;"); break;
        case '\'': sb_append(out, "&#39;"); break;
        default: sb_appendc(out, *s);
        }
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Finds key in an x-www-form-urlencoded body and decodes it into out.
// Single-line fields have CR/LF removed so nothing can inject ini lines.
static int form_get(const char *body, const char *key, char *out, size_t outlen, int multiline)
{
    size_t kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < outlen) {
                char c = *p++;
                if (c == '+')
                    c = ' ';
                else if (c == '%' && hexval(p[0]) >= 0 && hexval(p[1]) >= 0) {
                    c = (char)(hexval(p[0]) * 16 + hexval(p[1]));
                    p += 2;
                }
                if ((c == '\r' || c == '\n') && !multiline)
                    continue;
                if (c == '\r')
                    continue;
                out[o++] = c;
            }
            out[o] = 0;
            // Trim spaces (keys pasted from a phone often carry them).
            while (o && out[o - 1] == ' ')
                out[--o] = 0;
            size_t lead = 0;
            while (out[lead] == ' ')
                lead++;
            if (lead)
                memmove(out, out + lead, o - lead + 1);
            return 1;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    out[0] = 0;
    return 0;
}

static void respond_type(StrBuf *resp, int code, const char *status, const char *type,
                         const char *extra_headers, const char *body)
{
    sb_appendf(resp,
               "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
               "X-Frame-Options: DENY\r\nConnection: close\r\n%sContent-Length: %zu\r\n\r\n%s",
               code, status, type, extra_headers ? extra_headers : "", strlen(body), body);
}

static void respond(StrBuf *resp, int code, const char *status, const char *html)
{
    respond_type(resp, code, status, "text/html; charset=utf-8", NULL, html);
}

static void respond_json(StrBuf *resp, int code, const char *status, const char *json)
{
    respond_type(resp, code, status, "application/json", NULL, json);
}

static void redirect(StrBuf *resp, const char *to, const char *cookie)
{
    char h[200];
    if (cookie)
        snprintf(h, sizeof(h), "Location: %s\r\nSet-Cookie: s=%s; Path=/; HttpOnly; SameSite=Strict\r\n", to, cookie);
    else
        snprintf(h, sizeof(h), "Location: %s\r\n", to);
    respond_type(resp, 303, "See Other", "text/html", h, "");
}

#include "web_assets.h"

static void page_head(StrBuf *html)
{
    sb_append(html, "<!doctype html><html><head><meta charset=utf-8>"
                    "<meta name=viewport content='width=device-width,initial-scale=1,viewport-fit=cover'>"
                    "<meta name=theme-color content='#262624'>"
                    "<title>Claude for PS4</title><style>");
    sb_append(html, WEB_STYLE_CSS);
    sb_append(html, "</style></head><body>");
}

static const char MARK_SVG[] =
"<svg class=mark style='width:44px;height:44px' viewBox='0 0 512 512'>"
"<rect width=512 height=512 rx=128 fill='#e07a56'/><path d='M150 170h212c50 0 76 30 90 90l22 96c8 38-22 64-52 52-20-8-36-34-52-60H142c-16 26-32 52-52 60-30 12-60-14-52-52l22-96c14-60 40-90 90-90z' fill='#1c1917'/><g fill='#f4ede4'><circle cx=196 cy=268 r=24 /><circle cx=256 cy=268 r=24 /><circle cx=316 cy=268 r=24 /></g></svg>";

static void select_options(StrBuf *html, const char *const *values, const char *const *names, const char *cur)
{
    int found = 0;
    for (int i = 0; values[i]; i++) {
        int sel = !strcmp(values[i], cur);
        found |= sel;
        sb_appendf(html, "<option value='%s'%s>%s</option>", values[i], sel ? " selected" : "", names[i]);
    }
    if (!found && cur[0]) {
        sb_append(html, "<option value='");
        html_escape(html, cur);
        sb_append(html, "' selected>");
        html_escape(html, cur);
        sb_append(html, "</option>");
    }
}

static const char *const BRIDGE_MODEL_NAMES[] = { "PC default", "Opus", "Sonnet", "Haiku", "Fable", NULL };
static const char *const EFFORT_LABELS[] = { "Default", "Low", "Medium", "High", "Extra high", "Max", NULL };

// ---- sessions -----------------------------------------------------------

static void hex_token(char out[33])
{
    unsigned char b[16];
    if (plat_random(b, sizeof(b)) < 0) {
        // Very unlikely; fall back to time-based bytes so the page still works.
        unsigned v = (unsigned)time(NULL) * 2654435761u;
        for (int i = 0; i < 16; i++) {
            v = v * 1103515245u + 12345u;
            b[i] = (unsigned char)(v >> 16);
        }
    }
    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", b[i]);
}

// Finds header name (lower case, with colon) in the request head.
static int header_get(const char *req, const char *name, char *out, size_t outlen)
{
    size_t nl = strlen(name);
    const char *end = strstr(req, "\r\n\r\n");
    const char *p = strstr(req, "\r\n");
    while (p && (!end || p < end)) {
        p += 2;
        if (!strncasecmp(p, name, nl)) {
            p += nl;
            while (*p == ' ')
                p++;
            size_t o = 0;
            while (*p && *p != '\r' && o + 1 < outlen)
                out[o++] = *p++;
            out[o] = 0;
            return 1;
        }
        p = strstr(p, "\r\n");
    }
    out[0] = 0;
    return 0;
}

static int signed_in(Setup *s, const char *req)
{
    char cookie[512];
    if (!header_get(req, "cookie:", cookie, sizeof(cookie)))
        return 0;
    const char *p = cookie;
    while ((p = strstr(p, "s=")) != NULL) {
        if (p == cookie || p[-1] == ' ' || p[-1] == ';')
            break;
        p += 2;
    }
    if (!p)
        return 0;
    p += 2;
    char tok[33];
    size_t n = 0;
    while (n < 32 && ((p[n] >= '0' && p[n] <= '9') || (p[n] >= 'a' && p[n] <= 'f')))
        n++;
    if (n != 32)
        return 0;
    memcpy(tok, p, 32);
    tok[32] = 0;
    int ok = 0;
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < SETUP_SESSIONS; i++) {
        // Constant time compare.
        unsigned diff = s->sessions[i][0] ? 0 : 1;
        for (int k = 0; k < 32; k++)
            diff |= (unsigned)(s->sessions[i][k] ^ tok[k]);
        if (!diff)
            ok = 1;
    }
    pthread_mutex_unlock(&s->lock);
    return ok;
}

// ---- pages --------------------------------------------------------------

static void build_login(Setup *s, StrBuf *html, const char *error)
{
    (void)s;
    page_head(html);
    sb_append(html, "<div class=form><div class=hello style='margin-top:10vh'>");
    sb_append(html, MARK_SVG);
    sb_append(html, "<h2>Claude for PS4</h2><p>Sign in with the PIN on your TV.</p></div>");
    if (error) {
        sb_append(html, "<p class=err>");
        html_escape(html, error);
        sb_append(html, "</p>");
    }
    sb_append(html, "<form method=post action=/login><label>PIN (press OPTIONS on the PS4 to see it)</label>"
                    "<input name=pin inputmode=numeric autocomplete=off autofocus required>"
                    "<button class='btn pri' type=submit>Sign in</button></form>"
                    "<p class=hint>This page is served by your PS4 on your home network. "
                    "It is plain HTTP, so use it on a network you trust.</p></div></body></html>");
}

static void build_form(Setup *s, StrBuf *html, const char *error)
{
    Config c;
    pthread_mutex_lock(&s->lock);
    c = s->cfg;
    pthread_mutex_unlock(&s->lock);

    page_head(html);
    sb_append(html, "<div class=form><p><a href=/>&larr; Back to chat</a></p><h1>Settings</h1>");
    if (error) {
        sb_append(html, "<p class=err>");
        html_escape(html, error);
        sb_append(html, "</p>");
    }
    sb_append(html, "<form method=post action=/save><label>How should the PS4 reach Claude?</label><div class=seg>");
    sb_appendf(html, "<label><input type=radio name=mode value=api %s> API key</label>", c.bridge ? "" : "checked");
    sb_appendf(html, "<label><input type=radio name=mode value=bridge %s> PC bridge</label></div>",
               c.bridge ? "checked" : "");
    sb_append(html, "<label>API key</label><input name=api_key type=password autocomplete=off placeholder='");
    sb_append(html, c.api_key[0] ? "(saved; leave empty to keep it)" : "sk-ant-...");
    sb_append(html, "'><p class=hint>From console.anthropic.com. Billed to your API account.</p>"
                    "<label>Model (API key mode)</label><select name=model>");
    select_options(html, CONFIG_MODELS, CONFIG_MODEL_NAMES, c.model);
    sb_append(html, "</select><label>PC bridge address</label><input name=bridge_url placeholder='http://192.168.1.50:8787' value='");
    html_escape(html, c.bridge_url);
    sb_append(html, "'><label>Bridge pairing code</label><input name=bridge_token autocomplete=off placeholder='");
    sb_append(html, c.bridge_token[0] ? "(saved; leave empty to keep it)" : "shown by claude-bridge.py");
    sb_append(html, "'><label>Model (PC bridge mode)</label><select name=bridge_model>");
    select_options(html, CONFIG_BRIDGE_MODELS, BRIDGE_MODEL_NAMES, c.bridge_model);
    sb_append(html, "</select><p class=hint>The bridge runs Claude Code on a PC signed in to your Claude account. "
                    "Console tools (files, apps) are off in this mode.</p><label>Effort</label><select name=effort>");
    select_options(html, CONFIG_EFFORTS, EFFORT_LABELS, c.effort);
    sb_append(html, "</select><button class='btn pri' type=submit>Save to PS4</button></form></div></body></html>");
}

static int query_int(const char *req, const char *key, int dflt)
{
    const char *eol = strstr(req, "\r\n");
    const char *q = strchr(req, '?');
    if (!q || (eol && q > eol))
        return dflt;
    size_t kl = strlen(key);
    for (const char *p = q + 1; p && *p && (!eol || p < eol);) {
        if (!strncmp(p, key, kl) && p[kl] == '=')
            return atoi(p + kl + 1);
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return dflt;
}

static void build_state(Setup *s, const char *req, StrBuf *json)
{
    Chat *c = s->chat;
    int have = query_int(req, "n", 0), gen = query_int(req, "g", -1);
    pthread_mutex_lock(&s->lock);
    int ready = s->cfg_ok, bridge = s->cfg.bridge;
    char model[96], effort[16], profile[96], profile_sub[96], cur[HISTORY_ID_MAX];
    snprintf(profile, sizeof(profile), "%s", s->profile);
    snprintf(profile_sub, sizeof(profile_sub), "%s", s->profile_sub);
    snprintf(cur, sizeof(cur), "%s", s->chat_cur);
    snprintf(model, sizeof(model), "%s", config_active_model(&s->cfg));
    snprintf(effort, sizeof(effort), "%s", s->cfg.effort);
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_lock(&c->lock);
    int from = (gen == c->gen) ? have - 1 : 0;  // resend the last one, it may still be growing
    if (from < 0)
        from = 0;
    if (from > c->count)
        from = 0;
    sb_appendf(json, "{\"gen\":%d,\"count\":%d,\"from\":%d,\"busy\":%s,\"ready\":%s,\"bridge\":%s,\"model\":",
               c->gen, c->count, from, c->busy ? "true" : "false", ready ? "true" : "false", bridge ? "true" : "false");
    sb_append_json_string(json, model);
    sb_append(json, ",\"effort\":");
    sb_append_json_string(json, effort);
    sb_append(json, ",\"profile\":");
    sb_append_json_string(json, profile);
    sb_append(json, ",\"profile_sub\":");
    sb_append_json_string(json, profile_sub);
    sb_append(json, ",\"mode\":");
    sb_append_json_string(json, prefs_mode());
    sb_append(json, ",\"chat\":");
    sb_append_json_string(json, cur);
    sb_append(json, ",\"confirm\":");
    if (c->confirm == CONFIRM_PENDING) {
        sb_append_json_string(json, c->confirm_text);
        sb_append(json, ",\"confirm_title\":");
        sb_append_json_string(json, c->confirm_title[0] ? c->confirm_title : "Allow this action?");
        sb_append(json, ",\"confirm_yes\":");
        sb_append_json_string(json, c->confirm_yes[0] ? c->confirm_yes : "Allow");
        sb_append(json, ",\"confirm_no\":");
        sb_append_json_string(json, c->confirm_no[0] ? c->confirm_no : "Deny");
    } else {
        sb_append(json, "null");
    }
    sb_append(json, ",\"msgs\":[");
    for (int i = from; i < c->count; i++) {
        const Msg *m = &c->msgs[i];
        static const char *const roles[] = { "u", "a", "n", "tl" };
        sb_appendf(json, "%s{\"r\":\"%s\",\"t\":", i > from ? "," : "", roles[m->role]);
        sb_append_json_string(json, m->text.data ? m->text.data : "");
        sb_appendc(json, '}');
    }
    sb_append(json, "]}");
    pthread_mutex_unlock(&c->lock);
}

static int handle_save(Setup *s, const char *body, StrBuf *html, StrBuf *resp)
{
    char mode[16], key[256], burl[160], btok[96], model[96], bmodel[64], effort[16];
    form_get(body, "bridge_model", bmodel, sizeof(bmodel), 0);
    form_get(body, "mode", mode, sizeof(mode), 0);
    form_get(body, "api_key", key, sizeof(key), 0);
    form_get(body, "bridge_url", burl, sizeof(burl), 0);
    form_get(body, "bridge_token", btok, sizeof(btok), 0);
    form_get(body, "model", model, sizeof(model), 0);
    form_get(body, "effort", effort, sizeof(effort), 0);

    pthread_mutex_lock(&s->lock);
    Config c = s->cfg;
    pthread_mutex_unlock(&s->lock);

    const char *error = NULL;
    int bridge = !strcmp(mode, "bridge");
    if (!bridge && !key[0] && !c.api_key[0])
        error = "Enter your API key.";
    else if (!bridge && key[0] && strncmp(key, "sk-ant-", 7))
        error = "That does not look like an Anthropic API key (it starts with sk-ant-).";
    else if (bridge && strncmp(burl, "http://", 7) && strncmp(burl, "https://", 8))
        error = "The bridge address must start with http:// or https:// (for example http://192.168.1.50:8787).";
    else if (bridge && !btok[0] && !c.bridge_token[0])
        error = "Enter the pairing code that claude-bridge.py printed.";
    else if ((model[0] && !config_model_ok(model)) || (bmodel[0] && !config_model_ok(bmodel)) ||
             !config_effort_ok(effort))
        error = "That model or effort is not valid.";
    if (error) {
        build_form(s, html, error);
        respond(resp, 400, "Bad Request", html->data);
        return 0;
    }
    c.bridge = bridge;
    if (key[0])
        snprintf(c.api_key, sizeof(c.api_key), "%s", key);
    if (burl[0]) {
        size_t n = strlen(burl);
        while (n && burl[n - 1] == '/')
            burl[--n] = 0;
        snprintf(c.bridge_url, sizeof(c.bridge_url), "%s", burl);
    }
    if (btok[0])
        snprintf(c.bridge_token, sizeof(c.bridge_token), "%s", btok);
    if (model[0])
        snprintf(c.model, sizeof(c.model), "%s", model);
    if (bmodel[0])
        snprintf(c.bridge_model, sizeof(c.bridge_model), "%s", bmodel);
    snprintf(c.effort, sizeof(c.effort), "%s", effort);
    if (config_save(&c) != 0) {
        build_form(s, html, "Could not write /data/claude/claude.ini on the PS4.");
        respond(resp, 500, "Error", html->data);
        return 0;
    }
    pthread_mutex_lock(&s->lock);
    s->cfg = c;
    s->cfg_ok = 1;
    pthread_mutex_unlock(&s->lock);
    page_head(html);
    sb_append(html, "<div class=form><div class=hello style='margin-top:10vh'>");
    sb_append(html, MARK_SVG);
    sb_append(html, bridge ? "<h2>Saved</h2><p>The PS4 now uses your PC bridge.</p></div>"
                           : "<h2>Saved</h2><p>The PS4 now uses your API key.</p></div>");
    sb_append(html, "<a class='btn pri' style='display:block;text-align:center;text-decoration:none;margin-top:24px' "
                    "href=/>Chat from this phone</a></div></body></html>");
    respond(resp, 200, "OK", html->data);
    return 1;
}

static int is_req(const char *req, const char *method_path)
{
    size_t n = strlen(method_path);
    return !strncmp(req, method_path, n) && (req[n] == ' ' || req[n] == '?');
}

int setup_handle_request(Setup *s, const char *req, size_t len, StrBuf *resp)
{
    (void)len;
    StrBuf html;
    sb_init(&html);
    int saved = 0;
    const char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    int in = signed_in(s, req);
    int api = !strncmp(req, "GET /api/", 9) || !strncmp(req, "POST /api/", 10);

    if (is_req(req, "POST /login")) {
        char pin[16];
        form_get(body, "pin", pin, sizeof(pin), 0);
        pthread_mutex_lock(&s->lock);
        long now = (long)time(NULL), wait_s = s->lock_until - now;
        if (wait_s > 0) {
            // Locked after wrong PINs: no guess is checked at all.
            pthread_mutex_unlock(&s->lock);
            char msg[160];
            snprintf(msg, sizeof(msg), "Too many wrong PINs. Try again in %ld s with the new PIN on the TV.", wait_s);
            build_login(s, &html, msg);
            respond(resp, 429, "Too Many Requests", html.data);
            sb_free(&html);
            return 0;
        }
        int ok = s->pin[0] && !strcmp(pin, s->pin);
        char tok[33] = "";
        int ready = s->cfg_ok;
        if (ok) {
            hex_token(tok);
            memcpy(s->sessions[s->next_session], tok, 33);
            s->next_session = (s->next_session + 1) % SETUP_SESSIONS;
            setup_new_pin(s);  // each PIN signs in one phone
            s->lock_round = 0;
        } else if (++s->bad_pins >= 5) {
            setup_new_pin(s);  // a new PIN appears on the TV
            // Then a wait that doubles each round: 30 s, 1 min, 2 min ... 15 min.
            int r = s->lock_round < 5 ? s->lock_round : 5;
            s->lock_round++;
            s->lock_until = now + (30L << r) > now + 900 ? now + 900 : now + (30L << r);
        }
        pthread_mutex_unlock(&s->lock);
        if (ok) {
            redirect(resp, ready ? "/" : "/settings", tok);
        } else {
            build_login(s, &html, "Wrong PIN. Check the number on the TV.");
            respond(resp, 403, "Forbidden", html.data);
        }
    } else if (!in) {
        if (api) {
            respond_json(resp, 401, "Unauthorized", "{\"error\":\"Sign in again.\"}");
        } else if (is_req(req, "GET /") || is_req(req, "GET /settings")) {
            build_login(s, &html, NULL);
            respond(resp, 200, "OK", html.data);
        } else {
            respond(resp, 404, "Not Found", "<p>Not found</p>");
        }
    } else if (is_req(req, "GET /")) {
        if (!s->chat) {
            redirect(resp, "/settings", NULL);
        } else {
            page_head(&html);
            sb_append(&html, WEB_REMOTE_HTML);
            respond(resp, 200, "OK", html.data);
        }
    } else if (is_req(req, "GET /settings")) {
        build_form(s, &html, NULL);
        respond(resp, 200, "OK", html.data);
    } else if (is_req(req, "POST /save")) {
        saved = handle_save(s, body, &html, resp);
    } else if (api && !s->chat) {
        respond_json(resp, 404, "Not Found", "{\"error\":\"not available\"}");
    } else if (is_req(req, "GET /api/state")) {
        build_state(s, req, &html);
        respond_json(resp, 200, "OK", html.data);
    } else if (is_req(req, "GET /api/chats")) {
        pthread_mutex_lock(&s->lock);
        sb_append(&html, "{\"current\":");
        sb_append_json_string(&html, s->chat_cur);
        sb_append(&html, ",\"chats\":[");
        for (int i = 0; i < s->chat_count; i++) {
            sb_append(&html, i ? ",{\"id\":" : "{\"id\":");
            sb_append_json_string(&html, s->chats[i].id);
            sb_append(&html, ",\"title\":");
            sb_append_json_string(&html, s->chats[i].title);
            sb_appendc(&html, '}');
        }
        sb_append(&html, "]}");
        pthread_mutex_unlock(&s->lock);
        respond_json(resp, 200, "OK", html.data);
    } else if (api) {
        // Writes need a custom header, which other web pages cannot send
        // here without a CORS preflight that this server never allows.
        char hv[8];
        if (!header_get(req, "x-claude-remote:", hv, sizeof(hv)) || strcmp(hv, "1")) {
            respond_json(resp, 403, "Forbidden", "{\"error\":\"missing header\"}");
        } else if (is_req(req, "POST /api/send")) {
            const char *err = NULL;
            pthread_mutex_lock(&s->lock);
            if (!s->cfg_ok)
                err = "Set up the PS4 first (Settings).";
            else if (s->chat->busy || s->want_send)
                err = "Claude is still answering.";
            else if (!body[0])
                err = "Empty message.";
            else {
                snprintf(s->send_text, sizeof(s->send_text), "%s", body);
                s->want_send = 1;
            }
            pthread_mutex_unlock(&s->lock);
            if (err) {
                sb_append(&html, "{\"error\":");
                sb_append_json_string(&html, err);
                sb_appendc(&html, '}');
                respond_json(resp, 409, "Conflict", html.data);
            } else {
                respond_json(resp, 200, "OK", "{\"ok\":true}");
            }
        } else if (is_req(req, "POST /api/confirm")) {
            if (s->chat->confirm == CONFIRM_PENDING) {
                s->chat->confirm_by = 1;
                s->chat->confirm = !strncmp(body, "yes", 3) ? CONFIRM_YES : CONFIRM_NO;
            }
            s->chat->dirty = 1;
            respond_json(resp, 200, "OK", "{\"ok\":true}");
        } else if (is_req(req, "POST /api/model")) {
            char model[96], effort[16];
            form_get(body, "model", model, sizeof(model), 0);
            form_get(body, "effort", effort, sizeof(effort), 0);
            int ok = config_model_ok(model) && config_effort_ok(effort);
            if (ok) {
                pthread_mutex_lock(&s->lock);
                Config c = s->cfg;
                if (c.bridge)
                    snprintf(c.bridge_model, sizeof(c.bridge_model), "%.63s", model);
                else
                    snprintf(c.model, sizeof(c.model), "%s", model);
                snprintf(c.effort, sizeof(c.effort), "%s", effort);
                ok = config_save(&c) == 0;
                if (ok) {
                    s->cfg = c;
                    s->cfg_changed = 1;
                }
                pthread_mutex_unlock(&s->lock);
            }
            respond_json(resp, ok ? 200 : 400, ok ? "OK" : "Bad Request",
                         ok ? "{\"ok\":true}" : "{\"error\":\"Could not change the model.\"}");
        } else if (is_req(req, "POST /api/stop")) {
            s->want_stop = 1;
            respond_json(resp, 200, "OK", "{\"ok\":true}");
        } else if (is_req(req, "POST /api/new")) {
            s->want_new = 1;
            respond_json(resp, 200, "OK", "{\"ok\":true}");
        } else if (is_req(req, "POST /api/open")) {
            char id[HISTORY_ID_MAX] = "";
            form_get(body, "id", id, sizeof(id), 0);
            int ok = 0;
            pthread_mutex_lock(&s->lock);
            for (int i = 0; i < s->chat_count && !ok; i++)
                ok = history_id_ok(id) && !strcmp(s->chats[i].id, id);
            if (ok && s->chat->busy)
                ok = -1;
            if (ok > 0)
                snprintf(s->want_open, sizeof(s->want_open), "%s", id);
            pthread_mutex_unlock(&s->lock);
            if (ok > 0)
                respond_json(resp, 200, "OK", "{\"ok\":true}");
            else if (ok < 0)
                respond_json(resp, 409, "Conflict", "{\"error\":\"Claude is still answering.\"}");
            else
                respond_json(resp, 404, "Not Found", "{\"error\":\"That chat was not found.\"}");
        } else if (is_req(req, "POST /api/permissions")) {
            char mode[16] = "";
            form_get(body, "mode", mode, sizeof(mode), 0);
            if (prefs_set_mode(mode) == 0)
                respond_json(resp, 200, "OK", "{\"ok\":true}");
            else
                respond_json(resp, 400, "Bad Request", "{\"error\":\"Unknown permission mode.\"}");
        } else if (is_req(req, "POST /api/logout")) {
            s->want_logout = 1;
            respond_json(resp, 200, "OK", "{\"ok\":true}");
        } else {
            respond_json(resp, 404, "Not Found", "{\"error\":\"not found\"}");
        }
    } else {
        respond(resp, 404, "Not Found", "<p>Not found</p>");
    }
    sb_free(&html);
    return saved;
}

// ---- socket server (PS4) ------------------------------------------------
#ifdef __ORBIS__
#include <orbis/Net.h>

struct bsd_sockaddr_in {
    uint8_t  len;
    uint8_t  family;
    uint16_t port;
    uint32_t addr;
    char     zero[8];
};

#define BSD_SOL_SOCKET   0xffff
#define BSD_SO_REUSEADDR 0x0004
#define BSD_SO_RCVTIMEO  0x1006

static void serve_client(Setup *s, int fd)
{
    struct { long sec; long usec; } tv = { 5, 0 };
    sceNetSetsockopt(fd, BSD_SOL_SOCKET, BSD_SO_RCVTIMEO, &tv, sizeof(tv));
    char *buf = malloc(MAX_REQUEST + 1);
    if (!buf)
        return;
    size_t len = 0;
    long want = -1;
    while (len < MAX_REQUEST) {
        int n = sceNetRecv(fd, buf + len, MAX_REQUEST - len, 0);
        if (n <= 0)
            break;
        len += (size_t)n;
        buf[len] = 0;
        char *end = strstr(buf, "\r\n\r\n");
        if (end && want < 0) {
            const char *cl = strstr(buf, "Content-Length:");
            if (!cl)
                cl = strstr(buf, "content-length:");
            want = (long)(end + 4 - buf) + (cl ? atol(cl + 15) : 0);
        }
        if (want >= 0 && (long)len >= want)
            break;
    }
    buf[len] = 0;
    if (len) {
        StrBuf resp;
        sb_init(&resp);
        if (setup_handle_request(s, buf, len, &resp))
            s->saved = 1;
        size_t off = 0;
        while (off < resp.len) {
            int n = sceNetSend(fd, resp.data + off, resp.len - off, 0);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
        sb_free(&resp);
    }
    free(buf);
}

static void *server_main(void *arg)
{
    Setup *s = arg;
    while (!s->stop) {
        struct bsd_sockaddr_in peer;
        OrbisNetSocklen_t plen = sizeof(peer);
        int fd = sceNetAccept(s->listen_fd, (OrbisNetSockaddr *)&peer, &plen);
        if (fd < 0)
            break;  // aborted by setup_stop
        // Home network only: drop anything not from a private address, so a
        // router port forward or DMZ cannot expose the page to the internet.
        const unsigned char *ip = (const unsigned char *)&peer.addr;
        int local = ip[0] == 10 || ip[0] == 127 || (ip[0] == 192 && ip[1] == 168) ||
                    (ip[0] == 172 && (ip[1] & 0xf0) == 16) || (ip[0] == 169 && ip[1] == 254);
        if (local)
            serve_client(s, fd);
        sceNetSocketClose(fd);
    }
    sceNetSocketClose(s->listen_fd);
    s->active = 0;
    return NULL;
}

int setup_start(Setup *s, const char *ip)
{
    if (s->active)
        return 0;
    s->stop = 0;
    s->saved = 0;
    snprintf(s->url, sizeof(s->url), "http://%s:%d/", ip, SETUP_PORT);

    int fd = sceNetSocket("claudeSetup", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    sceNetSetsockopt(fd, BSD_SOL_SOCKET, BSD_SO_REUSEADDR, &one, sizeof(one));
    struct bsd_sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.len = sizeof(a);
    a.family = ORBIS_NET_AF_INET;
    a.port = sceNetHtons(SETUP_PORT);
    a.addr = 0;
    if (sceNetBind(fd, (OrbisNetSockaddr *)&a, sizeof(a)) < 0 || sceNetListen(fd, 4) < 0) {
        sceNetSocketClose(fd);
        return -1;
    }
    s->listen_fd = fd;
    s->active = 1;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, server_main, s) != 0) {
        sceNetSocketClose(fd);
        s->active = 0;
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

void setup_stop(Setup *s)
{
    if (!s->active)
        return;
    s->stop = 1;
    sceNetSocketAbort(s->listen_fd, 0);
}
#endif
