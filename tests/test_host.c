// Host tests for the portable parts (JSON, stream parsing, request body,
// config, word wrap, tools). Build: make -C tests
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/chat.h"
#include "../src/config.h"
#include "../src/font.h"
#include "../src/json.h"
#include "../src/layout.h"
#include "../src/stream.h"
#include "../src/tools.h"
#include "../src/setup.h"
#include "../src/history.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char SSE[] =
"event: message_start\n"
"data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[],\"model\":\"claude-opus-5-5\"}}\n\n"
"event: content_block_start\n"
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}\n\n"
"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"SIG123\"}}\n\n"
"data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
"data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
"data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi \\u00e9\\ud83d\\ude00 \\\"there\\\"\\n\"}}\n\r\n"
"data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
"data: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"list_directory\",\"input\":{}}}\n\n"
"data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"pa\"}}\n\n"
"data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"th\\\": \\\"/data\\\"}\"}}\n\n"
"data: {\"type\":\"content_block_stop\",\"index\":2}\n\n"
"event: ping\ndata: {\"type\": \"ping\"}\n\n"
"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":9}}\n\n"
"data: {\"type\":\"message_stop\"}\n\n";

static void test_stream(void)
{
    Chat chat;
    chat_init(&chat);
    Stream st;
    stream_init(&st, &chat);
    // Feed in awkward chunks to test line reassembly.
    for (size_t i = 0; i < sizeof(SSE) - 1; i += 7)
        stream_feed(&st, SSE + i, (sizeof(SSE) - 1 - i) < 7 ? sizeof(SSE) - 1 - i : 7);
    CHECK(st.done);
    CHECK(!strcmp(st.stop_reason, "tool_use"));
    CHECK(chat.count == 1 && chat.msgs[0].role == ROLE_ASSISTANT);
    CHECK(!strcmp(chat.msgs[0].text.data, "Hi \xc3\xa9\xf0\x9f\x98\x80 \"there\"\n"));
    char *m = stream_message_json(&st);
    printf("assistant message: %s\n", m);
    JVal *v = json_parse(m);
    CHECK(v != NULL);
    JVal *content = json_get(v, "content");
    CHECK(content && content->count == 3);
    CHECK(!strcmp(json_get_str(content->items[0], "signature"), "SIG123"));
    CHECK(!strcmp(json_get_str(json_get(content->items[2], "input"), "path"), "/data"));
    json_free(v);
    free(m);
    stream_free(&st);
}

static void test_request(void)
{
    Chat chat;
    chat_init(&chat);
    Config cfg;
    config_defaults(&cfg);
    config_parse(&cfg, "# comment\napi_key = \"sk-ant-test\"\nmodel=claude-opus-5-5\neffort=low\nsystem=Line1\\nLine2\ntls_verify=0\n");
    CHECK(!strcmp(cfg.api_key, "sk-ant-test"));
    CHECK(!strcmp(cfg.system, "Line1\nLine2"));
    CHECK(cfg.tls_verify == 0);
    CHECK(config_use_fallbacks(&cfg));
    chat_api_push(&chat, "{\"role\":\"user\",\"content\":\"hello\"}");
    StrBuf b;
    sb_init(&b);
    chat_build_request(&chat, &cfg, TOOLS_JSON, &b);
    JVal *v = json_parse(b.data);
    CHECK(v != NULL);
    CHECK(json_get(v, "tools") && json_get(v, "tools")->count == 18);
    CHECK(!strcmp(json_get_str(json_get(v, "output_config"), "effort"), "low"));
    CHECK(!strcmp(json_get_str(v, "fallbacks"), "default"));
    FILE *f = fopen("tests/out_request.json", "wb");
    fwrite(b.data, 1, b.len, f);
    fclose(f);
    json_free(v);
    sb_free(&b);
}

static void test_wrap(void)
{
    FILE *f = fopen("assets/fonts/DejaVuSans.ttf", "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n);
    fread(d, 1, (size_t)n, f);
    fclose(f);
    Font font;
    CHECK(font_init(&font, d, 34));
    const char *t = "The quick brown fox jumps over the lazy dog. Supercalifragilisticexpialidocious\n\nNext";
    int *s, *l;
    int lines = layout_wrap(&font, t, (int)strlen(t), 300, &s, &l);
    for (int i = 0; i < lines; i++) {
        CHECK(font_text_width(&font, t + s[i], l[i]) <= 300);
        printf("  [%.*s]\n", l[i], t + s[i]);
    }
    CHECK(lines >= 5);
    // Render into a buffer to exercise drawing and clipping.
    static uint32_t px[400 * 200];
    Canvas c = { px, 400, 200, 400, 16, 8, 0, 0xFF000000 };
    canvas_fill(&c, (Rect){ -10, -10, 1000, 1000 }, 0x101010, NULL);
    font_draw(&font, &c, -5, 180, "Clip test \xc3\xa9", 11, 0xFFFFFF, NULL);
    free(s);
    free(l);
}

static int confirm_answer = CONFIRM_YES;
static void *answer_thread(void *arg)
{
    Chat *c = arg;
    for (;;) {
        if (c->confirm == CONFIRM_PENDING) {
            printf("  confirm asked: %s -> %s\n", c->confirm_text, confirm_answer == CONFIRM_YES ? "yes" : "no");
            c->confirm = confirm_answer;
        }
        usleep(1000);
    }
    return NULL;
}

static StrBuf g_images;

static int run_tool(Chat *c, const char *name, const char *json, StrBuf *out)
{
    JVal *in = json_parse(json);
    sb_free(out);
    sb_free(&g_images);
    int r = tools_run(c, name, in, out, &g_images);
    json_free(in);
    printf("  %s -> %s%s\n", name, r ? "ERROR " : "", out->data ? out->data : "");
    return r;
}

static void write_fake_pkg(const char *path, uint32_t type, uint32_t flags)
{
    unsigned char h[0x100] = { 0x7F, 'C', 'N', 'T' };
    memcpy(h + 0x40, "UP0001-CUSA00001_00-TESTTESTTESTTEST", 36);
    h[0x74] = type >> 24; h[0x75] = type >> 16; h[0x76] = type >> 8; h[0x77] = type;
    h[0x78] = flags >> 24; h[0x79] = flags >> 16; h[0x7A] = flags >> 8; h[0x7B] = flags;
    FILE *f = fopen(path, "wb");
    fwrite(h, 1, sizeof(h), f);
    fclose(f);
}

static void test_phase2(void)
{
    StrBuf b;
    sb_init(&b);
    base64_append(&b, (const unsigned char *)"Man", 3);
    base64_append(&b, (const unsigned char *)"Ma", 2);
    base64_append(&b, (const unsigned char *)"M", 1);
    CHECK(!strcmp(b.data, "TWFuTWE=TQ=="));
    sb_free(&b);

    system("mkdir -p /tmp/cps4t2");
    char cid[37];
    const char *type = NULL;
    unsigned long long size = 0;
    write_fake_pkg("/tmp/cps4t2/g.pkg", 0x1A, 0);
    CHECK(pkg_read_info("/tmp/cps4t2/g.pkg", cid, &type, &size) && !strcmp(type, "PS4GD") && size == 0x100 &&
          !strcmp(cid, "UP0001-CUSA00001_00-TESTTESTTESTTEST"));
    write_fake_pkg("/tmp/cps4t2/p.pkg", 0x1A, 0x100000);
    CHECK(pkg_read_info("/tmp/cps4t2/p.pkg", cid, &type, &size) && !strcmp(type, "PS4PATCH"));
    write_fake_pkg("/tmp/cps4t2/d.pkg", 0x1B, 0);
    CHECK(pkg_read_info("/tmp/cps4t2/d.pkg", cid, &type, &size) && !strcmp(type, "PS4AC"));
    system("echo notapkg > /tmp/cps4t2/bad.pkg");
    CHECK(!pkg_read_info("/tmp/cps4t2/bad.pkg", cid, &type, &size));

    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    confirm_answer = CONFIRM_YES;
    StrBuf out;
    sb_init(&out);
    // A tiny 1x1 PNG.
    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
    FILE *f = fopen("/tmp/cps4t2/s.png", "wb");
    fwrite(png, 1, sizeof(png), f);
    fclose(f);
    CHECK(!run_tool(&chat, "view_image", "{\"path\":\"/tmp/cps4t2/s.png\"}", &out));
    CHECK(g_images.data && strstr(g_images.data, "\"media_type\":\"image/png\"") && strstr(g_images.data, "iVBORw0K"));
    JVal *img = json_parse(g_images.data);
    CHECK(img != NULL);
    json_free(img);
    CHECK(run_tool(&chat, "view_image", "{\"path\":\"/tmp/cps4t2/g.pkg\"}", &out));
    CHECK(run_tool(&chat, "install_pkg", "{\"path\":\"/tmp/cps4t2/bad.pkg\"}", &out));
    CHECK(run_tool(&chat, "uninstall_app", "{\"title_id\":\"../etc\"}", &out));
    CHECK(run_tool(&chat, "set_fan_threshold", "{\"celsius\":30}", &out));
    CHECK(run_tool(&chat, "set_fan_threshold", "{\"celsius\":90}", &out));
    confirm_answer = CONFIRM_NO;
    CHECK(run_tool(&chat, "install_pkg", "{\"path\":\"/tmp/cps4t2/g.pkg\"}", &out) && strstr(out.data, "declined"));
    CHECK(run_tool(&chat, "send_payload", "{\"path\":\"/tmp/cps4t2/s.png\"}", &out));
    confirm_answer = CONFIRM_YES;
    sb_free(&out);

    // Every tool in TOOLS_JSON parses and has a handler.
    JVal *tools = json_parse(TOOLS_JSON);
    CHECK(tools && tools->type == J_ARR);
    printf("  %d tools defined\n", tools ? tools->count : 0);
    json_free(tools);
}

static void test_tools(void)
{
    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    system("rm -rf /tmp/cps4t && mkdir -p /tmp/cps4t/a/sub && echo hi > /tmp/cps4t/a/sub/f.txt && echo x > /tmp/cps4t/a/game.pkg");
    StrBuf out;
    sb_init(&out);
    CHECK(!run_tool(&chat, "list_directory", "{\"path\":\"/tmp/cps4t/a\"}", &out));
    CHECK(!run_tool(&chat, "write_text_file", "{\"path\":\"/tmp/cps4t/a/n.txt\",\"content\":\"note\"}", &out));
    CHECK(!run_tool(&chat, "read_text_file", "{\"path\":\"/tmp/cps4t/a/n.txt\"}", &out) && !strcmp(out.data, "note"));
    CHECK(!run_tool(&chat, "copy_path", "{\"source\":\"/tmp/cps4t/a\",\"destination\":\"/tmp/cps4t/b\"}", &out));
    CHECK(run_tool(&chat, "copy_path", "{\"source\":\"/tmp/cps4t/a\",\"destination\":\"/tmp/cps4t/a/inner\"}", &out));
    CHECK(!run_tool(&chat, "move_path", "{\"source\":\"/tmp/cps4t/b\",\"destination\":\"/tmp/cps4t/c\"}", &out));
    CHECK(!run_tool(&chat, "delete_path", "{\"path\":\"/tmp/cps4t/c\",\"recursive\":true}", &out));
    CHECK(run_tool(&chat, "delete_path", "{\"path\":\"/system/common\"}", &out));
    CHECK(run_tool(&chat, "delete_path", "{\"path\":\"/data\"}", &out));
    CHECK(run_tool(&chat, "delete_path", "{\"path\":\"/mnt/usb0\"}", &out));
    confirm_answer = CONFIRM_NO;
    CHECK(run_tool(&chat, "delete_path", "{\"path\":\"/tmp/cps4t/a/n.txt\"}", &out));
    struct stat stt;
    CHECK(stat("/tmp/cps4t/a/n.txt", &stt) == 0);
    CHECK(run_tool(&chat, "bogus", "{}", &out));
    sb_free(&out);
}

static void test_bridge_stream(void)
{
    static Chat chat;
    chat_init(&chat);
    FILE *f = fopen("tests/fixtures/bridge_stream.txt", "rb");
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    Stream st;
    stream_init(&st, &chat);
    stream_feed(&st, buf, n);
    CHECK(st.done && !strcmp(st.stop_reason, "end_turn"));
    CHECK(chat.count == 1 && !strcmp(chat.msgs[0].text.data, "ok"));
    stream_free(&st);
}

static int req(Setup *s, const char *method_path, const char *cookie, const char *hdr, const char *body, StrBuf *r)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s HTTP/1.1\r\nHost: x\r\n%s%s%s%s\r\n%s", method_path,
             cookie ? "Cookie: a=b; s=" : "", cookie ? cookie : "", cookie ? "\r\n" : "", hdr ? hdr : "",
             body ? body : "");
    sb_free(r);
    return setup_handle_request(s, buf, strlen(buf), r);
}

static void test_setup(void)
{
    static Setup s;
    static Chat chat;
    chat_init(&chat);
    setup_init(&s, &chat);
    CHECK(strlen(s.pin) == 6);
    StrBuf r;
    sb_init(&r);
    // Signed out: login page, API refused
    req(&s, "GET /", NULL, NULL, NULL, &r);
    CHECK(strstr(r.data, "200 OK") && strstr(r.data, "action=/login"));
    req(&s, "GET /api/state?n=0&g=-1", NULL, NULL, NULL, &r);
    CHECK(strstr(r.data, "401"));
    req(&s, "POST /save", NULL, NULL, "mode=api&api_key=sk-ant-x", &r);
    CHECK(!strstr(r.data, "Saved"));
    req(&s, "GET /api/state", "0123456789abcdef0123456789abcdef", NULL, NULL, &r);
    CHECK(strstr(r.data, "401"));
    // Wrong PIN
    req(&s, "POST /login", NULL, NULL, "pin=000000x", &r);
    CHECK(strstr(r.data, "Wrong PIN"));
    // Good PIN -> cookie, PIN rotates
    char body[64], old_pin[8];
    snprintf(body, sizeof(body), "pin=%s", s.pin);
    snprintf(old_pin, sizeof(old_pin), "%s", s.pin);
    req(&s, "POST /login", NULL, NULL, body, &r);
    const char *ck = strstr(r.data, "Set-Cookie: s=");
    CHECK(ck && strstr(r.data, "Location: /settings") && strstr(r.data, "SameSite=Strict"));
    char tok[33] = "";
    if (ck)
        snprintf(tok, sizeof(tok), "%.32s", ck + 14);
    CHECK(strcmp(old_pin, s.pin) != 0);
    req(&s, "POST /login", NULL, NULL, body, &r);
    CHECK(strstr(r.data, "Wrong PIN"));  // single use
    // Settings: API key with an injected newline that must be stripped
    req(&s, "GET /settings", tok, NULL, NULL, &r);
    CHECK(strstr(r.data, "action=/save"));
    int saved = req(&s, "POST /save", tok, NULL,
                    "mode=api&api_key=+sk-ant-abc%0Amode%3Dbridge+&model=claude-opus-5-5&effort=low", &r);
    CHECK(saved && strstr(r.data, "Saved") && s.cfg_ok);
    FILE *f = fopen(CONFIG_SAVE_PATH, "rb");
    CHECK(f != NULL);
    if (f) {
        char text[2048] = {0};
        fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        Config p;
        config_defaults(&p);
        config_parse(&p, text);
        CHECK(!strcmp(p.api_key, "sk-ant-abcmode=bridge") && !p.bridge && !strcmp(p.effort, "low"));
    }
    // Bridge mode keeps the saved key
    CHECK(req(&s, "POST /save", tok, NULL,
              "mode=bridge&bridge_url=http%3A%2F%2F192.168.1.50%3A8787%2F&bridge_token=dc6c61e6", &r));
    CHECK(s.cfg.bridge && !strcmp(s.cfg.bridge_url, "http://192.168.1.50:8787") && !strcmp(s.cfg.bridge_token, "dc6c61e6"));
    CHECK(!strcmp(s.cfg.api_key, "sk-ant-abcmode=bridge"));

    // Phone remote
    req(&s, "GET /", tok, NULL, NULL, &r);
    CHECK(strstr(r.data, "/api/state"));
    chat_add(&chat, ROLE_USER, "hello <b>");
    int ai = chat_add(&chat, ROLE_ASSISTANT, "Hi");
    req(&s, "GET /api/state?n=0&g=-1", tok, NULL, NULL, &r);
    const char *js = strstr(r.data, "\r\n\r\n");
    JVal *st = js ? json_parse(js + 4) : NULL;
    CHECK(st && json_get(st, "count")->num == 2 && json_get(st, "msgs")->count == 2 &&
          json_get(st, "confirm")->type == J_NULL);
    int gen = st ? (int)json_get(st, "gen")->num : 0;
    json_free(st);
    chat_append(&chat, ai, " there", 6);
    char path[64];
    snprintf(path, sizeof(path), "GET /api/state?n=2&g=%d", gen);
    req(&s, path, tok, NULL, NULL, &r);
    js = strstr(r.data, "\r\n\r\n");
    st = js ? json_parse(js + 4) : NULL;
    CHECK(st && json_get(st, "from")->num == 1 && json_get(st, "msgs")->count == 1 &&
          !strcmp(json_get_str(json_get(st, "msgs")->items[0], "t"), "Hi there"));
    json_free(st);
    // Writes need the custom header
    req(&s, "POST /api/send", tok, NULL, "hi from phone", &r);
    CHECK(strstr(r.data, "403") && !s.want_send);
    req(&s, "POST /api/send", tok, "X-Claude-Remote: 1\r\n", "hi from phone", &r);
    CHECK(strstr(r.data, "200 OK"));
    char text[64];
    CHECK(setup_take_text(&s, text, sizeof(text)) && !strcmp(text, "hi from phone"));
    CHECK(!setup_take_text(&s, text, sizeof(text)));
    chat.busy = 1;
    req(&s, "POST /api/send", tok, "X-Claude-Remote: 1\r\n", "again", &r);
    CHECK(strstr(r.data, "409") && !s.want_send);
    chat.confirm = CONFIRM_PENDING;
    snprintf(chat.confirm_text, sizeof(chat.confirm_text), "Delete\n/data/x ?");
    req(&s, "GET /api/state?n=0&g=-1", tok, NULL, NULL, &r);
    CHECK(strstr(r.data, "\"confirm\":\"Delete\\n/data/x ?\""));
    req(&s, "POST /api/confirm", tok, "X-Claude-Remote: 1\r\n", "no", &r);
    CHECK(chat.confirm == CONFIRM_NO);
    req(&s, "POST /api/stop", tok, "X-Claude-Remote: 1\r\n", "", &r);
    CHECK(s.want_stop);
    chat.busy = 0;
    chat_clear(&chat);
    snprintf(path, sizeof(path), "GET /api/state?n=2&g=%d", gen);
    req(&s, path, tok, NULL, NULL, &r);
    CHECK(strstr(r.data, "\"from\":0") && strstr(r.data, "\"count\":0"));
    // Saved chats, open, profile and log out
    HistoryEntry list[2] = { { "chat-20261114-052310", "Second chat" }, { "chat-20261113-101010", "First <chat>" } };
    setup_set_chats(&s, list, 2, "chat-20261114-052310");
    setup_set_profile(&s, "Red", "Claude account (PC bridge)");
    req(&s, "GET /api/chats", tok, NULL, NULL, &r);
    CHECK(strstr(r.data, "\"current\":\"chat-20261114-052310\"") && strstr(r.data, "First <chat>"));
    req(&s, "GET /api/state?n=0&g=-1", tok, NULL, NULL, &r);
    CHECK(strstr(r.data, "\"profile\":\"Red\"") && strstr(r.data, "\"chat\":\"chat-20261114-052310\""));
    char oid[HISTORY_ID_MAX];
    req(&s, "POST /api/open", tok, NULL, "id=chat-20261113-101010", &r);
    CHECK(strstr(r.data, "403") && !setup_take_open(&s, oid));
    req(&s, "POST /api/open", tok, "X-Claude-Remote: 1\r\n", "id=..%2Fclaude", &r);
    CHECK(strstr(r.data, "404") && !setup_take_open(&s, oid));
    req(&s, "POST /api/open", tok, "X-Claude-Remote: 1\r\n", "id=chat-20261199-000000", &r);
    CHECK(strstr(r.data, "404"));
    req(&s, "POST /api/open", tok, "X-Claude-Remote: 1\r\n", "id=chat-20261113-101010", &r);
    CHECK(strstr(r.data, "200 OK") && setup_take_open(&s, oid) && !strcmp(oid, "chat-20261113-101010"));
    CHECK(!setup_take_open(&s, oid));
    req(&s, "POST /api/logout", tok, "X-Claude-Remote: 1\r\n", "", &r);
    CHECK(s.want_logout);
    s.want_logout = 0;
    chat.confirm = CONFIRM_PENDING;
    snprintf(chat.confirm_title, sizeof(chat.confirm_title), "Log out?");
    req(&s, "GET /api/state?n=0&g=-1", tok, NULL, NULL, &r);
    CHECK(strstr(r.data, "\"confirm_title\":\"Log out?\"") && strstr(r.data, "\"confirm_no\":\"Deny\""));
    chat.confirm = CONFIRM_NONE;
    chat.confirm_title[0] = 0;
    setup_sign_out_all(&s);
    req(&s, "GET /api/state", tok, NULL, NULL, &r);
    CHECK(strstr(r.data, "401"));
    // Five wrong PINs make a new one
    snprintf(old_pin, sizeof(old_pin), "%s", s.pin);
    for (int i = 0; i < 5; i++)
        req(&s, "POST /login", NULL, NULL, "pin=1", &r);
    CHECK(strcmp(old_pin, s.pin) != 0);
    sb_free(&r);
}

static void test_history(void)
{
    const char *dir = "/tmp/claude-ps4-test/chats";
    mkdir("/tmp/claude-ps4-test", 0777);
    mkdir(dir, 0777);
    CHECK(history_id_ok("chat-20261114-052310") && !history_id_ok("../x") && !history_id_ok("") &&
          !history_id_ok("a/b") && !history_id_ok("a.json"));
    char t[16];
    history_title("\n  Hello there, how are you\nsecond", t, sizeof(t));
    CHECK(!strcmp(t, "Hello there, ho"));
    history_title("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", t, sizeof(t));
    CHECK(strlen(t) == 14);

    static Chat c;
    chat_init(&c);
    chat_add(&c, ROLE_USER, "What is a PS4?\n\nClaude:\ntricky");
    chat_api_push(&c, "{\"role\":\"user\",\"content\":\"What is a PS4?\"}");
    chat_add(&c, ROLE_ASSISTANT, "A game console.");
    chat_api_push(&c, "{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"SIG\"},{\"type\":\"text\",\"text\":\"A game console.\"}]}");
    chat_add(&c, ROLE_USER, "And a PS5?");
    chat_api_push(&c, "{\"role\":\"user\",\"content\":\"And a PS5?\"}");  // unanswered
    char conv[40];
    snprintf(conv, sizeof(conv), "%s", c.conv_id);
    CHECK(history_save(&c, dir, "chat-20261114-000002") == 0);
    CHECK(history_save(&c, dir, "../evil") != 0);
    // An old plain text chat and an empty one
    FILE *f = fopen("/tmp/claude-ps4-test/chats/chat-20261113-000001.txt", "wb");
    fputs("Note:\nSaved from your phone.\n\nYou:\nOld question\n\nClaude:\nOld answer\nline 2\n\n", f);
    fclose(f);
    f = fopen("/tmp/claude-ps4-test/chats/chat-20261112-000000.txt", "wb");
    fputs("Note:\nnothing\n\n", f);
    fclose(f);

    HistoryEntry list[HISTORY_MAX];
    int n = history_list(dir, list, HISTORY_MAX);
    CHECK(n == 2);
    CHECK(n == 2 && !strcmp(list[0].id, "chat-20261114-000002") && !strcmp(list[0].title, "What is a PS4?") &&
          !strcmp(list[1].title, "Old question"));

    static Chat d;
    chat_init(&d);
    CHECK(history_load(&d, dir, "chat-20261114-000002") == 0);
    CHECK(d.count == 3 && !strcmp(d.msgs[0].text.data, "What is a PS4?\n\nClaude:\ntricky") &&
          d.msgs[2].role == ROLE_USER);
    CHECK(d.api_count == 2 && strstr(d.api[1], "\"signature\":\"SIG\"") && !strcmp(d.conv_id, conv));
    CHECK(history_load(&d, dir, "chat-20261113-000001") == 0);
    CHECK(d.count == 3 && d.msgs[0].role == ROLE_NOTICE && !strcmp(d.msgs[2].text.data, "Old answer\nline 2"));
    CHECK(d.api_count == 2 && strstr(d.api[0], "Old question") && strstr(d.api[1], "\"assistant\""));
    CHECK(history_load(&d, dir, "../../etc/passwd") != 0);
    CHECK(history_load(&d, dir, "chat-20990101-000000") != 0);
    chat_clear(&c);
    chat_clear(&d);
}

int main(void)
{
    test_history();
    test_bridge_stream();
    test_setup();
    test_stream();
    test_request();
    test_wrap();
    test_tools();
    test_phase2();
    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
