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
    CHECK(json_get(v, "tools") && json_get(v, "tools")->count == 12);
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

static int run_tool(Chat *c, const char *name, const char *json, StrBuf *out)
{
    JVal *in = json_parse(json);
    sb_free(out);
    int r = tools_run(c, name, in, out);
    json_free(in);
    printf("  %s -> %s%s\n", name, r ? "ERROR " : "", out->data ? out->data : "");
    return r;
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

int main(void)
{
    test_stream();
    test_request();
    test_wrap();
    test_tools();
    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
