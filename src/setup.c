// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "setup.h"

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

static const char PAGE_HEAD[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Claude for PS4</title><style>"
"body{font-family:system-ui,sans-serif;background:#1f1e1d;color:#ecebe6;margin:0 auto;padding:16px;max-width:640px}"
"h1{color:#d97757;font-size:1.4em;margin:0 0 8px}label{display:block;margin:14px 0 4px;color:#c9c7bf}"
"input,select,textarea{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border-radius:8px;"
"border:1px solid #4a4843;background:#2b2a27;color:#ecebe6;font-family:inherit}"
".row{display:flex;gap:12px}.row label{flex:1;padding:10px;border:1px solid #4a4843;border-radius:8px;margin:6px 0}"
".row input{width:auto}button{margin-top:16px;width:100%;padding:14px;font-size:17px;border:0;"
"border-radius:8px;background:#d97757;color:#1f1e1d;font-weight:600}"
"button.alt{background:#3a3935;color:#ecebe6}"
".hint{color:#9c9a92;font-size:.9em}.err{color:#e5a15b}fieldset{border:0;padding:0;margin:0}"
"a{color:#d97757}"
"</style></head><body>";

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
    sb_append(html, PAGE_HEAD);
    sb_append(html, "<h1>Claude for PS4</h1>");
    if (error) {
        sb_append(html, "<p class=err>");
        html_escape(html, error);
        sb_append(html, "</p>");
    }
    sb_append(html, "<form method=post action=/login><label>PIN shown on the TV (press Options on the PS4)</label>"
                    "<input name=pin inputmode=numeric autocomplete=off autofocus required>"
                    "<button type=submit>Sign in</button></form>"
                    "<p class=hint>This page is served by your PS4 on your home network. "
                    "It is plain HTTP, so use it on a network you trust.</p></body></html>");
}

static void build_form(Setup *s, StrBuf *html, const char *error)
{
    Config c;
    pthread_mutex_lock(&s->lock);
    c = s->cfg;
    pthread_mutex_unlock(&s->lock);

    sb_append(html, PAGE_HEAD);
    sb_append(html, "<h1>Settings</h1>");
    if (error) {
        sb_append(html, "<p class=err>");
        html_escape(html, error);
        sb_append(html, "</p>");
    }
    sb_append(html, "<form method=post action=/save>"
                    "<label>How should the PS4 reach Claude?</label><div class=row>");
    sb_appendf(html, "<label><input type=radio name=mode value=api %s> API key</label>",
               c.bridge ? "" : "checked");
    sb_appendf(html, "<label><input type=radio name=mode value=bridge %s> PC bridge (Claude account)</label></div>",
               c.bridge ? "checked" : "");
    sb_append(html, "<fieldset><label>API key</label>"
                    "<input name=api_key type=password autocomplete=off placeholder='");
    sb_append(html, c.api_key[0] ? "(saved; leave empty to keep it)" : "sk-ant-...");
    sb_append(html, "'><p class=hint>From console.anthropic.com. Billed to your API account.</p></fieldset>"
                    "<fieldset><label>PC bridge address</label><input name=bridge_url placeholder='http://192.168.1.50:8787' value='");
    html_escape(html, c.bridge_url);
    sb_append(html, "'><label>Bridge pairing code</label><input name=bridge_token autocomplete=off placeholder='");
    sb_append(html, c.bridge_token[0] ? "(saved; leave empty to keep it)" : "shown by claude-bridge.py");
    sb_append(html, "'><p class=hint>Run claude-bridge.py on a PC where Claude Code is signed in to your Claude account. "
                    "Console tools (files, apps) are off in this mode.</p></fieldset>"
                    "<label>Model (API key mode)</label><input name=model value='");
    html_escape(html, c.model);
    sb_append(html, "'><label>Effort</label><select name=effort>");
    static const char *const efforts[] = { "", "low", "medium", "high", "xhigh", "max", NULL };
    for (int i = 0; efforts[i]; i++)
        sb_appendf(html, "<option value='%s'%s>%s</option>", efforts[i],
                   !strcmp(c.effort, efforts[i]) ? " selected" : "", efforts[i][0] ? efforts[i] : "default");
    sb_append(html, "</select><button type=submit>Save to PS4</button></form>"
                    "<p><a href=/>Back to chat</a></p></body></html>");
}

static const char REMOTE_PAGE[] =
"<h1>Claude for PS4</h1><p class=hint id=st>Connecting...</p>"
"<div id=log></div>"
"<div id=cf style='display:none;border:2px solid #d97757;border-radius:8px;padding:12px;margin:12px 0'>"
"<b>Claude wants to:</b><pre id=cft style='white-space:pre-wrap;font-family:inherit'></pre>"
"<div class=row><button onclick=\"post('/api/confirm','yes')\">Allow</button>"
"<button class=alt onclick=\"post('/api/confirm','no')\">Deny</button></div></div>"
"<textarea id=t rows=3 placeholder='Message Claude'></textarea>"
"<div class=row><button id=sb onclick=send()>Send</button>"
"<button class=alt id=xb onclick=\"post('/api/stop','')\">Stop</button>"
"<button class=alt onclick=\"if(confirm('Start a new chat?'))post('/api/new','')\">New chat</button></div>"
"<p><a href=/settings>Settings</a></p>"
"<style>.m{margin:10px 0;padding:10px 12px;border-radius:10px;white-space:pre-wrap;word-wrap:break-word}"
".u{background:#3a3935;margin-left:15%}.a{background:#2b2a27}.n{color:#e5a15b;font-size:.9em}"
".tl{color:#8fb3d9;font-size:.9em}</style>"
"<script>"
"var n=0,g=-1,busy=0,log=document.getElementById('log');"
"function post(u,b){return fetch(u,{method:'POST',headers:{'X-Claude-Remote':'1','Content-Type':'text/plain'},body:b})"
".then(function(r){if(r.status==401)location.reload();return r.json()}).then(function(j){if(j.error)alert(j.error);poll()})"
".catch(function(){})}"
"function send(){var t=document.getElementById('t');if(!t.value.trim())return;post('/api/send',t.value);t.value=''}"
"function poll(){fetch('/api/state?n='+n+'&g='+g).then(function(r){if(r.status==401){location.reload();return}return r.json()})"
".then(function(j){if(!j)return;if(j.gen!=g){g=j.gen;n=0;log.innerHTML=''}"
"var atEnd=window.innerHeight+window.scrollY>=document.body.scrollHeight-40;"
"for(var i=0;i<j.msgs.length;i++){var k=j.from+i,d=log.children[k];"
"if(!d){d=document.createElement('div');log.appendChild(d)}"
"d.className='m '+j.msgs[i].r;d.textContent=j.msgs[i].t}"
"n=j.count;busy=j.busy;"
"document.getElementById('st').textContent=!j.ready?'Not set up yet: open Settings.':busy?'Claude is working...':'Ready';"
"document.getElementById('sb').disabled=busy||!j.ready;document.getElementById('xb').disabled=!busy;"
"var cf=document.getElementById('cf');if(j.confirm!==null){cf.style.display='block';"
"document.getElementById('cft').textContent=j.confirm}else cf.style.display='none';"
"if(atEnd&&j.msgs.length)window.scrollTo(0,document.body.scrollHeight)})"
".catch(function(){document.getElementById('st').textContent='PS4 not reachable'})}"
"poll();setInterval(poll,1000);"
"</script></body></html>";

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
    int ready = s->cfg_ok;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_lock(&c->lock);
    int from = (gen == c->gen) ? have - 1 : 0;  // resend the last one, it may still be growing
    if (from < 0)
        from = 0;
    if (from > c->count)
        from = 0;
    sb_appendf(json, "{\"gen\":%d,\"count\":%d,\"from\":%d,\"busy\":%s,\"ready\":%s,\"confirm\":", c->gen,
               c->count, from, c->busy ? "true" : "false", ready ? "true" : "false");
    if (c->confirm == CONFIRM_PENDING)
        sb_append_json_string(json, c->confirm_text);
    else
        sb_append(json, "null");
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
    char mode[16], key[256], burl[160], btok[96], model[96], effort[16];
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
    else if (bridge && strncmp(burl, "http://", 7))
        error = "The bridge address must start with http:// (for example http://192.168.1.50:8787).";
    else if (bridge && !btok[0] && !c.bridge_token[0])
        error = "Enter the pairing code that claude-bridge.py printed.";
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
    sb_append(html, PAGE_HEAD);
    sb_append(html, bridge ? "<h1>Saved</h1><p>The PS4 now uses your PC bridge.</p>"
                           : "<h1>Saved</h1><p>The PS4 now uses your API key.</p>");
    sb_append(html, "<p><a href=/>Chat from this phone</a></p></body></html>");
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
        int ok = s->pin[0] && !strcmp(pin, s->pin);
        char tok[33] = "";
        int ready = s->cfg_ok;
        if (ok) {
            hex_token(tok);
            memcpy(s->sessions[s->next_session], tok, 33);
            s->next_session = (s->next_session + 1) % SETUP_SESSIONS;
            setup_new_pin(s);  // each PIN signs in one phone
        } else if (++s->bad_pins >= 5) {
            setup_new_pin(s);  // a new PIN appears on the TV
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
            sb_append(&html, PAGE_HEAD);
            sb_append(&html, REMOTE_PAGE);
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
            if (s->chat->confirm == CONFIRM_PENDING)
                s->chat->confirm = !strncmp(body, "yes", 3) ? CONFIRM_YES : CONFIRM_NO;
            s->chat->dirty = 1;
            respond_json(resp, 200, "OK", "{\"ok\":true}");
        } else if (is_req(req, "POST /api/stop")) {
            s->want_stop = 1;
            respond_json(resp, 200, "OK", "{\"ok\":true}");
        } else if (is_req(req, "POST /api/new")) {
            s->want_new = 1;
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
