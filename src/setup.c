// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "setup.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_REQUEST (16 * 1024)

void setup_new_pin(Setup *s)
{
    static unsigned seed;
    if (!seed)
        seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)s;
    seed = seed * 1103515245u + 12345u;
    unsigned a = seed;
    seed = seed * 1103515245u + 12345u;
    snprintf(s->pin, sizeof(s->pin), "%06u", ((a >> 8) ^ (seed >> 4)) % 1000000u);
    s->bad_pins = 0;
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

static void respond(StrBuf *resp, int code, const char *status, const char *html)
{
    sb_appendf(resp,
               "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
               "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n%s",
               code, status, strlen(html), html);
}

static const char PAGE_HEAD[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Claude for PS4 setup</title><style>"
"body{font-family:system-ui,sans-serif;background:#1f1e1d;color:#ecebe6;margin:0;padding:16px;max-width:560px}"
"h1{color:#d97757;font-size:1.5em}label{display:block;margin:14px 0 4px;color:#c9c7bf}"
"input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border-radius:8px;"
"border:1px solid #4a4843;background:#2b2a27;color:#ecebe6}"
".row{display:flex;gap:12px}.row label{flex:1;padding:10px;border:1px solid #4a4843;border-radius:8px;margin:6px 0}"
".row input{width:auto}button{margin-top:20px;width:100%;padding:14px;font-size:17px;border:0;"
"border-radius:8px;background:#d97757;color:#1f1e1d;font-weight:600}"
".hint{color:#9c9a92;font-size:.9em}.err{color:#e5a15b}fieldset{border:0;padding:0;margin:0}"
"</style></head><body><h1>Claude for PS4</h1>";

static void build_form(Setup *s, StrBuf *html, const char *error)
{
    Config c;
    pthread_mutex_lock(&s->lock);
    c = s->cfg;
    pthread_mutex_unlock(&s->lock);

    sb_append(html, PAGE_HEAD);
    if (error) {
        sb_append(html, "<p class=err>");
        html_escape(html, error);
        sb_append(html, "</p>");
    }
    sb_append(html, "<form method=post action=/save>"
                    "<label>PIN shown on the TV</label>"
                    "<input name=pin inputmode=numeric autocomplete=off required>"
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
                    "<p class=hint>This page is only on your home network and closes after saving. "
                    "It is plain HTTP, so use it on a network you trust.</p></body></html>");
}

int setup_handle_request(Setup *s, const char *req, size_t len, StrBuf *resp)
{
    (void)len;
    StrBuf html;
    sb_init(&html);
    int saved = 0;

    if (!strncmp(req, "GET / ", 6) || !strncmp(req, "GET /?", 6)) {
        build_form(s, &html, NULL);
        respond(resp, 200, "OK", html.data);
    } else if (!strncmp(req, "POST /save ", 11)) {
        const char *body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : "";
        char pin[16], mode[16], key[256], burl[160], btok[96], model[96], effort[16];
        form_get(body, "pin", pin, sizeof(pin), 0);
        form_get(body, "mode", mode, sizeof(mode), 0);
        form_get(body, "api_key", key, sizeof(key), 0);
        form_get(body, "bridge_url", burl, sizeof(burl), 0);
        form_get(body, "bridge_token", btok, sizeof(btok), 0);
        form_get(body, "model", model, sizeof(model), 0);
        form_get(body, "effort", effort, sizeof(effort), 0);

        pthread_mutex_lock(&s->lock);
        int pin_ok = s->pin[0] && !strcmp(pin, s->pin);
        if (!pin_ok && ++s->bad_pins >= 5)
            setup_new_pin(s);  // a new PIN appears on the TV
        Config c = s->cfg;
        pthread_mutex_unlock(&s->lock);

        const char *error = NULL;
        int bridge = !strcmp(mode, "bridge");
        if (!pin_ok)
            error = "Wrong PIN. Check the number on the TV.";
        else if (!bridge && !key[0] && !c.api_key[0])
            error = "Enter your API key.";
        else if (!bridge && key[0] && strncmp(key, "sk-ant-", 7))
            error = "That does not look like an Anthropic API key (it starts with sk-ant-).";
        else if (bridge && strncmp(burl, "http://", 7))
            error = "The bridge address must start with http:// (for example http://192.168.1.50:8787).";
        else if (bridge && !btok[0] && !c.bridge_token[0])
            error = "Enter the pairing code that claude-bridge.py printed.";

        if (error) {
            build_form(s, &html, error);
            respond(resp, 400, "Bad Request", html.data);
        } else {
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
            if (config_save(&c) == 0) {
                pthread_mutex_lock(&s->lock);
                s->cfg = c;
                s->pin[0] = 0;  // one save per session
                pthread_mutex_unlock(&s->lock);
                saved = 1;
                sb_append(&html, PAGE_HEAD);
                sb_append(&html, bridge ? "<p>Saved. The PS4 now uses your PC bridge.</p>"
                                        : "<p>Saved. The PS4 now uses your API key.</p>");
                sb_append(&html, "<p class=hint>You can close this page.</p></body></html>");
                respond(resp, 200, "OK", html.data);
            } else {
                build_form(s, &html, "Could not write /data/claude/claude.ini on the PS4.");
                respond(resp, 500, "Error", html.data);
            }
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

int setup_start(Setup *s, const Config *current, const char *ip)
{
    if (s->active)
        return 0;
    pthread_mutex_init(&s->lock, NULL);
    s->cfg = *current;
    s->stop = 0;
    s->saved = 0;
    setup_new_pin(s);
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
