// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#include "net.h"
#include "stream.h"
#include "tools.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

#define USER_AGENT     "ClaudeForPS4/1.0"
#define NET_POOL_SIZE  (16 * 1024)
#define SSL_POOL_SIZE  (256 * 1024)
#define HTTP_POOL_SIZE (256 * 1024)

// SCE https option flags
#define HTTPS_FLAG_SERVER_VERIFY   0x01
#define HTTPS_FLAG_CN_CHECK        0x04
#define HTTPS_FLAG_NOT_AFTER_CHECK 0x08
#define HTTPS_FLAG_NOT_BEFORE_CHECK 0x10
#define HTTPS_FLAG_KNOWN_CA_CHECK  0x20

// sceHttpAddRequestHeader mode
#define HTTP_HEADER_OVERWRITE 0

static int g_net_pool = -1, g_ssl_ctx = -1, g_http_ctx = -1;

int net_init(char *err, int errlen)
{
    int ret;
    if ((ret = (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET)) < 0 ||
        (ret = (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL)) < 0 ||
        (ret = (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP)) < 0) {
        snprintf(err, (size_t)errlen, "Could not load network modules (0x%08X).", ret);
        return -1;
    }
    sceNetInit();
    if ((g_net_pool = sceNetPoolCreate("claudeNet", NET_POOL_SIZE, 0)) < 0) {
        snprintf(err, (size_t)errlen, "sceNetPoolCreate failed (0x%08X).", g_net_pool);
        return -1;
    }
    if ((g_ssl_ctx = sceSslInit(SSL_POOL_SIZE)) < 0) {
        snprintf(err, (size_t)errlen, "sceSslInit failed (0x%08X).", g_ssl_ctx);
        return -1;
    }
    if ((g_http_ctx = sceHttpInit(g_net_pool, g_ssl_ctx, HTTP_POOL_SIZE)) < 0) {
        snprintf(err, (size_t)errlen, "sceHttpInit failed (0x%08X).", g_http_ctx);
        return -1;
    }
    return 0;
}

static int accept_any_cert(int ssl_ctx, unsigned int verify_err, void *const certs[], int ncerts, void *arg)
{
    (void)ssl_ctx; (void)verify_err; (void)certs; (void)ncerts; (void)arg;
    return 1;
}

#define MAX_TOOL_ROUNDS 25

typedef struct {
    Chat  *chat;
    Config cfg;
} Job;

static void notice(Chat *chat, const char *fmt, unsigned code)
{
    char buf[512];
    snprintf(buf, sizeof(buf), fmt, code);
    chat_add(chat, ROLE_NOTICE, buf);
}

// One streamed API call. Returns 1 on success with the assistant message
// stored in *message_json, 0 on failure (a notice was already shown).
static int stream_call(Job *job, Stream *st, char **message_json)
{
    Chat *chat = job->chat;
    int tpl = -1, conn = -1, req = -1, ret, status = 0, ok = 0;
    *message_json = NULL;

    StrBuf body;
    sb_init(&body);
    chat_build_request(chat, &job->cfg, TOOLS_JSON, &body);

    tpl = sceHttpCreateTemplate(g_http_ctx, USER_AGENT, ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) {
        notice(chat, "Network error: could not create HTTP template (0x%08X).", (unsigned)tpl);
        goto out;
    }
    sceHttpSetConnectTimeOut(tpl, 30 * 1000 * 1000);
    if (!job->cfg.tls_verify) {
        sceHttpsDisableOption(tpl, HTTPS_FLAG_SERVER_VERIFY | HTTPS_FLAG_CN_CHECK |
                                   HTTPS_FLAG_NOT_AFTER_CHECK | HTTPS_FLAG_NOT_BEFORE_CHECK |
                                   HTTPS_FLAG_KNOWN_CA_CHECK);
        sceHttpsSetSslCallback(tpl, accept_any_cert, NULL);
    }
    conn = sceHttpCreateConnectionWithURL(tpl, API_URL, true);
    if (conn < 0) {
        notice(chat, "Network error: could not create connection (0x%08X).", (unsigned)conn);
        goto out;
    }
    req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_POST, API_URL, body.len);
    if (req < 0) {
        notice(chat, "Network error: could not create request (0x%08X).", (unsigned)req);
        goto out;
    }
    chat->req_id = req;
    sceHttpAddRequestHeader(req, "content-type", "application/json", HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(req, "accept", "text/event-stream", HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(req, "x-api-key", job->cfg.api_key, HTTP_HEADER_OVERWRITE);
    sceHttpAddRequestHeader(req, "anthropic-version", API_VERSION, HTTP_HEADER_OVERWRITE);
    if (config_use_fallbacks(&job->cfg))
        sceHttpAddRequestHeader(req, "anthropic-beta", API_BETA_FALLBACK, HTTP_HEADER_OVERWRITE);

    ret = sceHttpSendRequest(req, body.data, body.len);
    if (ret < 0) {
        if (!chat->cancel)
            notice(chat, job->cfg.tls_verify
                       ? "Could not reach api.anthropic.com (0x%08X). Check the network and the PS4 "
                         "date and time. If it is a certificate error, see tls_verify in the README."
                       : "Could not reach api.anthropic.com (0x%08X).", (unsigned)ret);
        goto out;
    }
    sceHttpGetStatusCode(req, &status);

    char buf[4096];
    int n = 0;
    if (status != 200) {
        StrBuf eb;
        sb_init(&eb);
        while (eb.len < 65536 && (n = sceHttpReadData(req, buf, sizeof(buf))) > 0)
            sb_appendn(&eb, buf, (size_t)n);
        char msg[700];
        api_error_message(eb.data ? eb.data : "", status, msg, sizeof(msg));
        chat_add(chat, ROLE_NOTICE, msg);
        sb_free(&eb);
        goto out;
    }
    while (!chat->cancel && (n = sceHttpReadData(req, buf, sizeof(buf))) > 0) {
        stream_feed(st, buf, (size_t)n);
        if (st->done)
            break;
    }
    if (chat->cancel)
        goto out;
    if (st->error[0]) {
        char msg[700];
        snprintf(msg, sizeof(msg), "Stream error: %s", st->error);
        chat_add(chat, ROLE_NOTICE, msg);
        goto out;
    }
    if (!st->done) {
        notice(chat, "Connection lost while reading the reply (0x%08X).", (unsigned)n);
        goto out;
    }
    *message_json = stream_message_json(st);
    ok = *message_json != NULL;
    if (!ok)
        chat_add(chat, ROLE_NOTICE, "The reply was empty.");

out:
    chat->req_id = 0;
    if (req >= 0)
        sceHttpDeleteRequest(req);
    if (conn >= 0)
        sceHttpDeleteConnection(conn);
    if (tpl >= 0)
        sceHttpDeleteTemplate(tpl);
    sb_free(&body);
    return ok;
}

// Runs every tool_use block of the last reply and builds the user message
// carrying the results.
static char *run_tools(Job *job, Stream *st)
{
    StrBuf msg;
    sb_init(&msg);
    sb_append(&msg, "{\"role\":\"user\",\"content\":[");
    int first = 1;
    for (int i = 0; i < st->content->count; i++) {
        JVal *b = st->content->items[i];
        const char *bt = json_get_str(b, "type");
        if (!bt || strcmp(bt, "tool_use"))
            continue;
        const char *id = json_get_str(b, "id");
        const char *name = json_get_str(b, "name");
        JVal *input = json_get(b, "input");
        if (!id || !name)
            continue;
        char desc[300];
        tools_describe(name, input, desc, sizeof(desc));
        chat_add(job->chat, ROLE_TOOL, desc);

        StrBuf result;
        sb_init(&result);
        int is_error = job->chat->cancel ? (sb_append(&result, "Stopped by the user."), 1)
                                         : tools_run(job->chat, name, input, &result);
        if (is_error) {
            char shown[300];
            snprintf(shown, sizeof(shown), "  %s", result.data ? result.data : "error");
            chat_add(job->chat, ROLE_TOOL, shown);
        }
        if (!first)
            sb_appendc(&msg, ',');
        first = 0;
        sb_append(&msg, "{\"type\":\"tool_result\",\"tool_use_id\":");
        sb_append_json_string(&msg, id);
        sb_append(&msg, ",\"content\":");
        sb_append_json_string(&msg, result.data ? result.data : "");
        if (is_error)
            sb_append(&msg, ",\"is_error\":true");
        sb_append(&msg, "}");
        sb_free(&result);
    }
    sb_append(&msg, "]}");
    if (first) {
        sb_free(&msg);
        return NULL;
    }
    return msg.data;
}

static void run_conversation(Job *job)
{
    Chat *chat = job->chat;
    int base = chat->api_count - 1;  // index of the user message that started this turn
    for (int round = 0; round < MAX_TOOL_ROUNDS; round++) {
        if (chat->cancel) {
            chat_add(chat, ROLE_NOTICE, "Stopped.");
            chat_api_truncate(chat, base);
            return;
        }
        Stream st;
        stream_init(&st, chat);
        char *reply = NULL;
        int ok = stream_call(job, &st, &reply);
        if (!ok) {
            if (chat->cancel)
                chat_add(chat, ROLE_NOTICE, "Stopped.");
            // Drop this whole turn so the history stays valid (the API needs
            // every tool_use answered and no unanswered user message).
            chat_api_truncate(chat, base);
            stream_free(&st);
            return;
        }
        chat_api_push(chat, reply);
        free(reply);

        if (strcmp(st.stop_reason, "tool_use")) {
            if (!strcmp(st.stop_reason, "max_tokens"))
                chat_add(chat, ROLE_NOTICE, "Reply cut off: max_tokens reached.");
            else if (!strcmp(st.stop_reason, "refusal"))
                chat_add(chat, ROLE_NOTICE, "Claude declined to answer this request.");
            stream_free(&st);
            return;
        }
        char *results = run_tools(job, &st);
        stream_free(&st);
        if (!results) {
            chat_add(chat, ROLE_NOTICE, "Claude asked for a tool but sent no tool call.");
            chat_api_truncate(chat, base);
            return;
        }
        chat_api_push(chat, results);
        free(results);
        chat->got_text = 0;
    }
    chat_add(chat, ROLE_NOTICE, "Stopped after too many tool steps. Send a message to continue.");
    chat_api_truncate(chat, base);
}

static void *worker_main(void *arg)
{
    Job *job = arg;
    Chat *chat = job->chat;
    run_conversation(job);
    memset(job->cfg.api_key, 0, sizeof(job->cfg.api_key));
    free(job);
    chat->busy = 0;
    chat->dirty = 1;
    return NULL;
}

int net_send(Chat *chat, const Config *cfg, const char *user_text)
{
    if (chat->busy)
        return -1;
    Job *job = calloc(1, sizeof(Job));
    if (!job)
        return -1;
    job->chat = chat;
    job->cfg = *cfg;

    chat_add(chat, ROLE_USER, user_text);
    StrBuf m;
    sb_init(&m);
    sb_append(&m, "{\"role\":\"user\",\"content\":");
    sb_append_json_string(&m, user_text);
    sb_append(&m, "}");
    chat_api_push(chat, m.data);
    sb_free(&m);

    chat->cancel = 0;
    chat->got_text = 0;
    chat->busy = 1;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, worker_main, job) != 0) {
        pthread_attr_destroy(&attr);
        chat->busy = 0;
        chat_api_truncate(chat, chat->api_count - 1);
        free(job);
        chat_add(chat, ROLE_NOTICE, "Could not start the network thread.");
        return -1;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

void net_cancel(Chat *chat)
{
    if (!chat->busy)
        return;
    chat->cancel = 1;
    int req = chat->req_id;
    if (req > 0)
        sceHttpAbortRequest(req);
}
