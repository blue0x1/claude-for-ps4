// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
#define _GNU_SOURCE
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
#include "../src/homefolders.h"
#include "../src/actions.h"
#include "../src/storage.h"
#include "../src/saves.h"
#include "../src/pcinstall.h"
#include "../src/goldhen.h"
#include "../src/health.h"
#include "../src/prefs.h"
#include "sqlite3.h"

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
    CHECK(json_get(v, "tools") && json_get(v, "tools")->count == 51);
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

// Home screen folders, on a database laid out like the PS4's app.db.
static long long db_int(const char *sql)
{
    sqlite3 *db;
    long long v = -1;
    if (sqlite3_open("/tmp/claude-ps4-test/app.db", &db) == SQLITE_OK) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                v = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }
    sqlite3_close(db);
    return v;
}

static void test_home_folders(void)
{
    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    confirm_answer = CONFIRM_YES;
    system("rm -rf /tmp/claude-ps4-test/app.db* /tmp/claude-ps4-test/backups");
    sqlite3 *db;
    CHECK(sqlite3_open("/tmp/claude-ps4-test/app.db", &db) == SQLITE_OK);
    const char *T = "tbl_appbrowse_0000000001";
    char sql[2048];
    snprintf(sql, sizeof(sql),
             "CREATE TABLE %s (titleId TEXT NOT NULL,contentId TEXT,titleName TEXT COLLATE NOCASE,metaDataPath TEXT,"
             "lastAccessTime DATETIME,contentStatus INT,onDisc INT,parentalLevel INT,visible INT,sortPriority INT,"
             "pathInfo INT,lastAccessIndex INT,dispLocation INT,canRemove INT,category TEXT,contentType INT,"
             "pathInfo2 INT,presentBoxStatus INT,entitlement INT,thumbnailUrl TEXT,lastUpdateTime TEXT,"
             "playableDate DATETIME,contentSize INT,installDate DATETIME,platform INT,uiCategory TEXT,skuId TEXT,"
             "disableLiveDetail INT,linkType INT,linkUri TEXT,folderType INT,folderInfo TEXT,parentFolderId TEXT,"
             "positionInFolder INT,PRIMARY KEY(titleId));"
             "CREATE TABLE tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category));"
             "INSERT INTO tbl_version VALUES ('folder_index',8),('access_index',100);"
             "INSERT INTO %s (titleId,titleName,visible,folderType,positionInFolder) VALUES "
             "('CUSA00001','Alpha',1,0,0),('CUSA00002','Beta',1,0,0),('CUSA00003','Gamma',1,0,0),('NPXS00001','Sys',0,0,0);"
             "INSERT INTO %s (titleId,titleName,visible,folderType,folderInfo) VALUES "
             "('000000005','Old',1,1,'{\"titleId\":\"CUSA00003\",\"index\":0}');"
             "UPDATE %s SET parentFolderId='000000005', positionInFolder=1 WHERE titleId='CUSA00003';",
             T, T, T, T);
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    home_set_paths("/tmp/claude-ps4-test/app.db", "/tmp/claude-ps4-test/backups");
    StrBuf out;
    sb_init(&out);
    // Wrong user: the error names the users there are
    CHECK(run_tool(&chat, "list_home_folders", "{}", &out) && strstr(out.data, "0000000001"));
    CHECK(!run_tool(&chat, "list_home_folders", "{\"user\":\"0000000001\"}", &out) &&
          strstr(out.data, "[000000005] Old") && strstr(out.data, "CUSA00001  Alpha") && !strstr(out.data, "Sys"));
    // Create: new number after folder_index, games by ID or name, rows like the PS4's own
    CHECK(!run_tool(&chat, "create_home_folder",
                    "{\"user\":\"0000000001\",\"name\":\"Shooters\",\"games\":[\"cusa00001\",\"beta\"]}", &out) &&
          strstr(out.data, "000000009"));
    CHECK(db_int("SELECT status FROM tbl_version WHERE category='folder_index'") == 9);
    CHECK(db_int("SELECT count(*) FROM tbl_appbrowse_0000000001 WHERE titleId='000000009' AND folderType=1 AND "
                 "visible=1 AND dispLocation=5 AND sortPriority=100 AND canRemove=1 AND lastAccessIndex=100 AND "
                 "folderInfo='{\"titleId\":\"CUSA00001\",\"index\":0}'") == 1);
    CHECK(db_int("SELECT positionInFolder FROM tbl_appbrowse_0000000001 WHERE titleId='CUSA00002' AND "
                 "parentFolderId='000000009'") == 2);
    CHECK(run_tool(&chat, "create_home_folder", "{\"user\":\"0000000001\",\"name\":\"shooters\",\"games\":[\"Gamma\"]}", &out) &&
          strstr(out.data, "already exists"));
    CHECK(run_tool(&chat, "create_home_folder", "{\"user\":\"0000000001\",\"name\":\"X\",\"games\":[\"Nope\"]}", &out) &&
          strstr(out.data, "Not found"));
    // Move the only game out of Old: Old is removed
    CHECK(!run_tool(&chat, "move_to_home_folder", "{\"user\":\"0000000001\",\"games\":[\"CUSA00003\"],\"folder\":\"Shooters\"}", &out) &&
          strstr(out.data, "removed"));
    CHECK(db_int("SELECT count(*) FROM tbl_appbrowse_0000000001 WHERE titleId='000000005'") == 0);
    CHECK(!run_tool(&chat, "rename_home_folder", "{\"user\":\"0000000001\",\"folder\":\"000000009\",\"new_name\":\"Action\"}", &out));
    // Taking the cover game out moves the cover to the next game
    CHECK(!run_tool(&chat, "move_to_home_folder", "{\"user\":\"0000000001\",\"games\":[\"Alpha\"],\"folder\":\"\"}", &out));
    CHECK(db_int("SELECT count(*) FROM tbl_appbrowse_0000000001 WHERE titleId='000000009' AND titleName='Action' AND "
                 "folderInfo LIKE '%CUSA00002%'") == 1);
    CHECK(db_int("SELECT count(*) FROM tbl_appbrowse_0000000001 WHERE titleId='CUSA00001' AND parentFolderId IS NULL "
                 "AND positionInFolder=0") == 1);
    // Declined: nothing changes
    confirm_answer = CONFIRM_NO;
    CHECK(run_tool(&chat, "delete_home_folder", "{\"user\":\"0000000001\",\"folder\":\"Action\"}", &out));
    CHECK(db_int("SELECT count(*) FROM tbl_appbrowse_0000000001 WHERE titleId='000000009'") == 1);
    confirm_answer = CONFIRM_YES;
    CHECK(!run_tool(&chat, "delete_home_folder", "{\"user\":\"0000000001\",\"folder\":\"Action\"}", &out));
    CHECK(db_int("SELECT count(*) FROM tbl_appbrowse_0000000001 WHERE folderType=1 OR parentFolderId IS NOT NULL") == 0);
    // Undo the delete
    CHECK(!run_tool(&chat, "restore_home_backup", "{}", &out));
    CHECK(db_int("SELECT count(*) FROM tbl_appbrowse_0000000001 WHERE titleId='000000009' AND titleName='Action'") == 1);
    CHECK(db_int("SELECT count(*) FROM tbl_appbrowse_0000000001 WHERE parentFolderId='000000009'") == 2);
    sb_free(&out);
}

static void test_trash(void)
{
    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    confirm_answer = CONFIRM_YES;
    system("rm -rf /tmp/cps4tr /tmp/claude-ps4-test/trash && mkdir -p /tmp/cps4tr/dir/sub && echo one > /tmp/cps4tr/f.txt && echo two > /tmp/cps4tr/dir/sub/g.txt");
    StrBuf out;
    sb_init(&out);
    struct stat st;
    CHECK(!run_tool(&chat, "list_trash", "{}", &out) && strstr(out.data, "empty"));
    CHECK(!run_tool(&chat, "delete_path", "{\"path\":\"/tmp/cps4tr/f.txt\"}", &out) && strstr(out.data, "trash as "));
    CHECK(stat("/tmp/cps4tr/f.txt", &st) < 0);
    char id[40] = "";
    const char *p = strstr(out.data, "trash as ");
    if (p)
        sscanf(p + 9, "%39[0-9-]", id);
    CHECK(!run_tool(&chat, "delete_path", "{\"path\":\"/tmp/cps4tr/dir\",\"recursive\":true}", &out));
    CHECK(!run_tool(&chat, "list_trash", "{}", &out) && strstr(out.data, "/tmp/cps4tr/f.txt") &&
          strstr(out.data, "/tmp/cps4tr/dir") && strstr(out.data, "2 items"));
    char js[100];
    snprintf(js, sizeof(js), "{\"id\":\"%s\"}", id);
    CHECK(!run_tool(&chat, "restore_from_trash", js, &out) && stat("/tmp/cps4tr/f.txt", &st) == 0);
    CHECK(run_tool(&chat, "restore_from_trash", js, &out));  // gone from the trash now
    // Something back in the way: restore refuses
    system("mkdir -p /tmp/cps4tr/dir");
    CHECK(!run_tool(&chat, "list_trash", "{}", &out) && strstr(out.data, "1 item"));
    // Permanent delete skips the trash
    CHECK(!run_tool(&chat, "delete_path", "{\"path\":\"/tmp/cps4tr/f.txt\",\"permanent\":true}", &out) &&
          strstr(out.data, "Deleted"));
    confirm_answer = CONFIRM_NO;
    CHECK(run_tool(&chat, "empty_trash", "{}", &out));
    confirm_answer = CONFIRM_YES;
    CHECK(!run_tool(&chat, "empty_trash", "{}", &out) && strstr(out.data, "Emptied 1"));
    CHECK(!run_tool(&chat, "list_trash", "{}", &out) && strstr(out.data, "empty"));
    // History: changes are logged with the result, reads are not
    CHECK(!run_tool(&chat, "show_history", "{\"count\":50}", &out));
    CHECK(strstr(out.data, "delete_path\t/tmp/cps4tr/f.txt\tdone") && strstr(out.data, "empty_trash\t\tdeclined") &&
          strstr(out.data, "restore_from_trash") && !strstr(out.data, "\tlist_trash\t"));
    CHECK(!run_tool(&chat, "show_history", "{\"count\":1}", &out) && strstr(out.data, "empty_trash") &&
          !strstr(out.data, "delete_path"));
    sb_free(&out);
}

static void make_pkg(const char *path, const char *cid, unsigned type, unsigned flags)
{
    unsigned char h[0x100] = { 0x7F, 'C', 'N', 'T' };
    memcpy(h + 0x40, cid, 36);
    h[0x74] = (unsigned char)(type >> 24); h[0x75] = (unsigned char)(type >> 16);
    h[0x76] = (unsigned char)(type >> 8); h[0x77] = (unsigned char)type;
    h[0x78] = (unsigned char)(flags >> 24); h[0x79] = (unsigned char)(flags >> 16);
    h[0x7A] = (unsigned char)(flags >> 8); h[0x7B] = (unsigned char)flags;
    FILE *f = fopen(path, "wb");
    fwrite(h, 1, sizeof(h), f);
    fclose(f);
}

static void test_storage(void)
{
    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    confirm_answer = CONFIRM_YES;
    system("rm -rf /tmp/cps4st && mkdir -p /tmp/cps4st/app/CUSA00001 /tmp/cps4st/app/CUSA00002 "
           "/tmp/cps4st/patch/CUSA00001 /tmp/cps4st/patch/CUSA00009 /tmp/cps4st/addcont/CUSA00001/ABCDEFGHIJKLMNOP "
           "/tmp/cps4st/data/pkg /tmp/cps4st/av_contents /tmp/cps4st/home && "
           "head -c 300000 /dev/zero > /tmp/cps4st/app/CUSA00001/app.pkg && "
           "head -c 1000 /dev/zero > /tmp/cps4st/app/CUSA00002/app.pkg && "
           "head -c 5000 /dev/zero > /tmp/cps4st/patch/CUSA00009/patch.pkg && "
           "head -c 70 /dev/zero > /tmp/cps4st/data/pkg/junk.pkg");
    make_pkg("/tmp/cps4st/data/pkg/game.pkg", "UP0000-CUSA00001_00-GAMEGAMEGAMEGAME", 0x1A, 0);
    make_pkg("/tmp/cps4st/data/pkg/new.pkg", "UP0000-CUSA00003_00-GAMEGAMEGAMEGAME", 0x1A, 0);
    make_pkg("/tmp/cps4st/data/pkg/dlc.pkg", "UP0000-CUSA00001_00-ABCDEFGHIJKLMNOP", 0x1B, 0);
    make_pkg("/tmp/cps4st/data/pkg/upd.pkg", "UP0000-CUSA00002_00-GAMEGAMEGAMEGAME", 0x1A, 0x400000);
    storage_set_root("/tmp/cps4st");
    StrBuf out;
    sb_init(&out);
    CHECK(!run_tool(&chat, "storage_report", "{}", &out));
    const char *a = strstr(out.data, "CUSA00001"), *b = strstr(out.data, "CUSA00002");
    CHECK(strstr(out.data, "2 installed") && a && b && a < b);  // biggest first
    CHECK(strstr(out.data, "/tmp/cps4st/patch/CUSA00009") && !strstr(out.data, "/tmp/cps4st/patch/CUSA00001 "));
    CHECK(strstr(out.data, "game.pkg (256 B): game CUSA00001, already installed"));
    CHECK(strstr(out.data, "new.pkg (256 B): game CUSA00003, not installed"));
    CHECK(strstr(out.data, "dlc.pkg (256 B): DLC CUSA00001, already installed"));
    CHECK(strstr(out.data, "upd.pkg (256 B): update CUSA00002, not installed yet"));
    CHECK(strstr(out.data, "junk.pkg (70 B): not a readable"));
    // delete_paths: refuses game folders, one confirm for many
    CHECK(run_tool(&chat, "delete_paths", "{\"paths\":[\"/user/app/CUSA00001\"]}", &out) && strstr(out.data, "protected"));
    confirm_answer = CONFIRM_NO;
    CHECK(run_tool(&chat, "delete_paths", "{\"paths\":[\"/tmp/cps4st/data/pkg/game.pkg\"]}", &out));
    confirm_answer = CONFIRM_YES;
    CHECK(!run_tool(&chat, "delete_paths",
                    "{\"paths\":[\"/tmp/cps4st/data/pkg/game.pkg\",\"/tmp/cps4st/data/pkg/dlc.pkg\"]}", &out) &&
          strstr(out.data, "2 of 2 done"));
    CHECK(!run_tool(&chat, "delete_paths", "{\"paths\":[\"/tmp/cps4st/data/pkg/junk.pkg\"],\"permanent\":true}", &out) &&
          strstr(out.data, "freed"));
    struct stat st;
    CHECK(stat("/tmp/cps4st/data/pkg/game.pkg", &st) < 0 && stat("/tmp/cps4st/data/pkg/junk.pkg", &st) < 0);
    CHECK(!run_tool(&chat, "list_trash", "{}", &out) && strstr(out.data, "game.pkg"));
    sb_free(&out);
}

static void test_saves(void)
{
    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    confirm_answer = CONFIRM_YES;
    system("rm -rf /tmp/cps4sv /tmp/claude-ps4-test/save-backups && "
           "mkdir -p /tmp/cps4sv/home/00000000/savedata/CUSA00001 /tmp/cps4sv/home/0000000a/savedata/CUSA00002 "
           "/tmp/cps4sv/usb0 /tmp/cps4sv/home/00000000/savedata_meta/user/CUSA00001/SAVE0 && "
           "printf 'player1\\0' > /tmp/cps4sv/home/00000000/username.dat && "
           "echo icon > /tmp/cps4sv/home/00000000/savedata_meta/user/CUSA00001/SAVE0/icon0.png && echo v1 > /tmp/cps4sv/home/00000000/savedata/CUSA00001/sdimg_SAVE0 && "
           "echo k > /tmp/cps4sv/home/00000000/savedata/CUSA00001/SAVE0.bin && "
           "echo o > /tmp/cps4sv/home/0000000a/savedata/CUSA00002/sdimg_X");
    saves_set_paths("/tmp/cps4sv/home", "/tmp/cps4sv/usb");
    StrBuf out;
    sb_init(&out);
    CHECK(!run_tool(&chat, "list_game_saves", "{}", &out) && strstr(out.data, "User 00000000 player1 (signed in now)") &&
          strstr(out.data, "CUSA00001") && strstr(out.data, "1 save,") && strstr(out.data, "User 0000000a"));
    CHECK(!run_tool(&chat, "backup_saves", "{\"title_id\":\"CUSA00001\",\"note\":\"before boss\"}", &out) &&
          strstr(out.data, "backed up 2 files"));
    CHECK(run_tool(&chat, "backup_saves", "{\"title_id\":\"CUSA00002\"}", &out));  // other user's only
    CHECK(!run_tool(&chat, "backup_saves", "{\"title_id\":\"all\",\"all_users\":true,\"destination\":\"usb0\"}", &out) &&
          strstr(out.data, "2 games backed up"));
    CHECK(run_tool(&chat, "backup_saves", "{\"title_id\":\"all\",\"destination\":\"usb1\"}", &out));
    CHECK(!run_tool(&chat, "list_save_backups", "{\"title_id\":\"CUSA00001\"}", &out) && strstr(out.data, "before boss") &&
          strstr(out.data, "/tmp/cps4sv/usb0/claude-save-backups/CUSA00001/") && strstr(out.data, "2 backups"));
    // Restore the internal one after the save changed
    char bk[600] = "";
    const char *p = strstr(out.data, "/tmp/claude-ps4-test/save-backups/CUSA00001/");
    if (p)
        sscanf(p, "%599s", bk);
    system("echo v2 > /tmp/cps4sv/home/00000000/savedata/CUSA00001/sdimg_SAVE0");
    char js[700];
    snprintf(js, sizeof(js), "{\"backup\":\"%s\"}", bk);
    confirm_answer = CONFIRM_NO;
    CHECK(run_tool(&chat, "restore_saves", js, &out));
    confirm_answer = CONFIRM_YES;
    CHECK(!run_tool(&chat, "restore_saves", js, &out) && strstr(out.data, "Restored 2 files"));
    FILE *f = fopen("/tmp/cps4sv/home/00000000/savedata/CUSA00001/sdimg_SAVE0", "rb");
    char line[8] = "";
    if (f) {
        fgets(line, sizeof(line), f);
        fclose(f);
    }
    CHECK(!strcmp(line, "v1\n"));
    struct stat st;
    CHECK(stat("/tmp/cps4sv/home/00000000/savedata_meta/user/CUSA00001/SAVE0/icon0.png", &st) == 0);
    CHECK(!run_tool(&chat, "list_save_backups", "{\"title_id\":\"CUSA00001\"}", &out) && strstr(out.data, "before a restore"));
    CHECK(run_tool(&chat, "restore_saves", "{\"backup\":\"/etc\"}", &out));
    sb_free(&out);
}

static int file_has(const char *path, const char *text)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return memmem(buf, n, text, strlen(text)) != NULL;
}

extern const char *plat_stub_bridge_reply;
extern int plat_stub_progress_state;

static void test_pcinstall(void)
{
    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    confirm_answer = CONFIRM_YES;
    StrBuf out;
    sb_init(&out);
    plat_stub_bridge_reply = NULL;
    CHECK(run_tool(&chat, "list_pc_packages", "{}", &out) && strstr(out.data, "bridge"));
    CHECK(!run_tool(&chat, "install_status", "{}", &out) && strstr(out.data, "No installs"));
    plat_stub_bridge_reply =
        "{\"dir\":\"/home/u/PS4/pkg\",\"packages\":[{\"name\":\"games/Test.pkg\",\"size\":2147483648,\"sent\":0,"
        "\"content_id\":\"UP0000-CUSA09999_00-TESTTESTTESTTEST\",\"type\":\"PS4GD\",\"title\":\"My Test Game\","
        "\"version\":\"01.05\",\"url\":\"/pkg/KEY/games/Test.pkg\"},{\"name\":\"upd.pkg\",\"size\":1000,"
        "\"content_id\":\"UP0000-CUSA09999_00-TESTTESTTESTTEST\",\"type\":\"PS4PATCH\",\"title\":\"My Test Game\","
        "\"url\":\"/pkg/KEY/upd.pkg\"}]}";
    CHECK(!run_tool(&chat, "list_pc_packages", "{}", &out) && strstr(out.data, "games/Test.pkg  (2.0 GB)  game My Test Game v01.05") &&
          strstr(out.data, "upd.pkg  (1000 B)  update"));
    CHECK(run_tool(&chat, "install_from_pc", "{\"names\":[\"nope.pkg\"]}", &out) && strstr(out.data, "No package named"));
    confirm_answer = CONFIRM_NO;
    CHECK(run_tool(&chat, "install_from_pc", "{\"names\":[\"Test.pkg\"]}", &out));
    confirm_answer = CONFIRM_YES;
    CHECK(!run_tool(&chat, "install_from_pc", "{\"names\":[\"Test.pkg\",\"upd.pkg\"]}", &out) &&
          strstr(out.data, "http://pc:8787/pkg/KEY/games/Test.pkg") && strstr(out.data, "2 installs queued"));
    plat_stub_progress_state = 0;
    CHECK(!run_tool(&chat, "install_status", "{}", &out) && strstr(out.data, "My Test Game: 40%"));
    plat_stub_progress_state = 1;
    CHECK(!run_tool(&chat, "install_status", "{}", &out) && strstr(out.data, "installed"));
    plat_stub_bridge_reply = "{\"version\":\"99.00\",\"file\":\"x.pkg\"}";
    CHECK(!run_tool(&chat, "check_app_update", "{}", &out) && strstr(out.data, "Version 99.00 is available") &&
          strstr(out.data, "uninstall"));
    plat_stub_bridge_reply = "{\"version\":\"01.00\"}";
    CHECK(!run_tool(&chat, "check_app_update", "{}", &out) && strstr(out.data, "Up to date"));
    plat_stub_bridge_reply = NULL;
    sb_free(&out);
}


static void test_goldhen(void)
{
    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    confirm_answer = CONFIRM_YES;
    system("rm -rf /tmp/cps4gh && mkdir -p /tmp/cps4gh/cheats/json /tmp/cps4gh/cheats/shn /tmp/cps4gh/patches/xml /tmp/cps4gh/plugins && "
           "touch /tmp/cps4gh/plugins/afr.prx /tmp/cps4gh/plugins/game_patch.prx && "
           "printf '\\357\\273\\277[CUSA00419]\\r\\n/data/GoldHEN/plugins/Explusion.prx\\r\\n' > /tmp/cps4gh/plugins.ini && "
           "printf '[FTPServer]\\nEnabled = 1\\n\\n[GameOverlay]\\nFPS_Enabled = 0\\nFPS_Visible = 1\\n' > /tmp/cps4gh/config.ini && "
           "echo '{\"name\":\"G\",\"id\":\"CUSA00002\",\"version\":\"01.00\",\"mods\":[{\"name\":\"Godmode\",\"type\":\"checkbox\"}]}' "
           "> /tmp/cps4gh/cheats/json/CUSA00002_01.00.json && "
           "echo '<Trainer Game=\"G\" Version=\"01.02\"><Cheat Text=\"Max money\"></Cheat></Trainer>' > /tmp/cps4gh/cheats/shn/CUSA00002_01.02.shn && "
           "printf '<Patch><Metadata Title=\"G\"\\n Name=\"60 FPS\"\\n Note=\"smooth\"\\n Author=\"me\"\\n AppVer=\"01.28\"\\n AppElf=\"eboot.bin\">"
           "</Metadata></Patch>' > /tmp/cps4gh/patches/xml/CUSA00003.xml");
    goldhen_set_dir("/tmp/cps4gh");
    StrBuf out;
    sb_init(&out);
    CHECK(!run_tool(&chat, "list_cheats", "{\"title_id\":\"CUSA00002\"}", &out) && strstr(out.data, "- Godmode") &&
          strstr(out.data, "- Max money") && strstr(out.data, "version 01.02"));
    CHECK(!run_tool(&chat, "list_cheats", "{\"title_id\":\"CUSA00005\"}", &out) && strstr(out.data, "No cheat files"));
    CHECK(!run_tool(&chat, "list_game_patches", "{\"title_id\":\"CUSA00003\"}", &out) &&
          strstr(out.data, "- 60 FPS (game version 01.28): smooth by me") && strstr(out.data, "1 patch,"));
    CHECK(!run_tool(&chat, "list_plugins", "{}", &out) && strstr(out.data, "afr.prx") && strstr(out.data, "Explusion.prx, which does not exist"));
    CHECK(run_tool(&chat, "set_plugin", "{\"plugin\":\"nope.prx\",\"enabled\":true}", &out));
    CHECK(run_tool(&chat, "set_plugin", "{\"plugin\":\"../x.prx\",\"enabled\":true}", &out));
    CHECK(!run_tool(&chat, "set_plugin", "{\"plugin\":\"afr\",\"title_id\":\"CUSA00419\",\"enabled\":true}", &out));
    CHECK(file_has("/tmp/cps4gh/plugins.ini", "[CUSA00419]\n/data/GoldHEN/plugins/afr.prx\n/data/GoldHEN/plugins/Explusion.prx"));
    CHECK(!run_tool(&chat, "set_plugin", "{\"plugin\":\"game_patch.prx\",\"enabled\":true}", &out));
    CHECK(file_has("/tmp/cps4gh/plugins.ini", "\n[default]\n/data/GoldHEN/plugins/game_patch.prx\n"));
    CHECK(!run_tool(&chat, "list_plugins", "{}", &out) && strstr(out.data, "game_patch.prx               all games"));
    CHECK(!run_tool(&chat, "set_plugin", "{\"plugin\":\"afr.prx\",\"title_id\":\"CUSA00419\",\"enabled\":false}", &out));
    CHECK(!file_has("/tmp/cps4gh/plugins.ini", "afr.prx") && file_has("/tmp/cps4gh/plugins.ini.claude-bak", "afr.prx"));
    CHECK(!run_tool(&chat, "goldhen_settings", "{}", &out) && strstr(out.data, "FPS_Enabled = 0"));
    CHECK(run_tool(&chat, "set_goldhen_setting", "{\"section\":\"GameOverlay\",\"key\":\"Nope\",\"value\":\"1\"}", &out));
    CHECK(run_tool(&chat, "set_goldhen_setting", "{\"section\":\"GameOverlay\",\"key\":\"FPS_Enabled\",\"value\":\"1\\n[x]\"}", &out));
    CHECK(!run_tool(&chat, "set_goldhen_setting", "{\"section\":\"GameOverlay\",\"key\":\"FPS_Enabled\",\"value\":\"1\"}", &out));
    CHECK(file_has("/tmp/cps4gh/config.ini", "[GameOverlay]\nFPS_Enabled = 1\nFPS_Visible = 1\n") &&
          file_has("/tmp/cps4gh/config.ini", "[FTPServer]\nEnabled = 1"));
    sb_free(&out);
}

extern int plat_stub_cpu, plat_stub_fan;

static void test_health(void)
{
    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    confirm_answer = CONFIRM_YES;
    unlink("/tmp/claude-ps4-test/fan.conf");
    health_reset();
    StrBuf out;
    sb_init(&out);
    plat_stub_cpu = 60;
    CHECK(!run_tool(&chat, "health_report", "{}", &out) && strstr(out.data, "CPU 60 C, SoC 55 C (fine)") &&
          strstr(out.data, "not set by this app") && strstr(out.data, "1 h 02 min"));
    CHECK(!run_tool(&chat, "temperature_history", "{}", &out) && strstr(out.data, "No samples"));
    CHECK(run_tool(&chat, "set_fan_profile", "{\"profile\":\"loud\"}", &out) && strstr(out.data, "balanced"));
    CHECK(!run_tool(&chat, "set_fan_profile", "{\"profile\":\"auto\"}", &out) && plat_stub_fan == 70);
    time_t t0 = time(NULL) - 1200;
    for (int i = 0; i < 10; i++)
        health_sample(t0 + i * 30, 70 + i, 60);    // up to 79: two hot samples boost
    CHECK(plat_stub_fan == 55);
    for (int i = 10; i < 25; i++)
        health_sample(t0 + i * 30, 60, 55);        // cooled down
    CHECK(plat_stub_fan == 70);
    CHECK(!run_tool(&chat, "health_report", "{}", &out) && strstr(out.data, "Fan profile: auto, threshold 70 C") &&
          strstr(out.data, "highest 79 C"));
    CHECK(!run_tool(&chat, "temperature_history", "{\"minutes\":30}", &out) && strstr(out.data, " 79 C  ####"));
    // The saved profile is applied again after a "reboot".
    CHECK(!run_tool(&chat, "set_fan_profile", "{\"profile\":\"cool\"}", &out));
    health_reset();
    plat_stub_fan = 0;
    health_sample(time(NULL), 50, 45);
    CHECK(plat_stub_fan == 62);
    sb_free(&out);
}

static void test_prefs(void)
{
    static Chat chat;
    chat_init(&chat);
    pthread_t th;
    pthread_create(&th, NULL, answer_thread, &chat);
    unlink("/tmp/claude-ps4-test/permissions.conf");
    unlink("/tmp/claude-ps4-test/memory.txt");
    prefs_reset();
    StrBuf out;
    sb_init(&out);
    CHECK(!strcmp(prefs_mode(), "full"));
    CHECK(!run_tool(&chat, "remember", "{\"text\":\"USB 0 is for\\nbackups\"}", &out));
    CHECK(!run_tool(&chat, "remember", "{\"text\":\"Likes racing games\"}", &out));
    CHECK(!run_tool(&chat, "get_preferences", "{}", &out) && strstr(out.data, "1. USB 0 is for backups") &&
          strstr(out.data, "2. Likes racing games"));
    // Remembered items go into the system prompt
    Config cfg;
    config_defaults(&cfg);
    StrBuf req;
    sb_init(&req);
    chat_build_request(&chat, &cfg, NULL, &req);
    CHECK(strstr(req.data, "2. Likes racing games"));
    sb_free(&req);
    CHECK(!run_tool(&chat, "forget", "{\"text\":\"USB\"}", &out) && strstr(out.data, "Forgot: USB 0"));
    prefs_reset();  // survives a restart
    CHECK(!run_tool(&chat, "get_preferences", "{}", &out) && strstr(out.data, "1. Likes racing games"));
    // Trusted: safe tools skip the confirm, risky ones still ask
    prefs_set_mode("trusted");
    confirm_answer = CONFIRM_NO;
    system("rm -rf /tmp/cps4pf && mkdir -p /tmp/cps4pf && echo x > /tmp/cps4pf/f");
    CHECK(!run_tool(&chat, "make_directory", "{\"path\":\"/tmp/cps4pf/d\"}", &out));
    CHECK(run_tool(&chat, "delete_path", "{\"path\":\"/tmp/cps4pf/f\",\"permanent\":true}", &out) &&
          strstr(out.data, "declined"));
    CHECK(run_tool(&chat, "uninstall_app", "{\"title_id\":\"CUSA00001\"}", &out) && strstr(out.data, "declined"));
    CHECK(!run_tool(&chat, "delete_path", "{\"path\":\"/tmp/cps4pf/f\"}", &out) && strstr(out.data, "trash as"));
    CHECK(!run_tool(&chat, "show_history", "{\"count\":6}", &out) && strstr(out.data, "auto\tmake_directory") &&
          strstr(out.data, "auto\tdelete_path"));
    // Claude can tighten but not loosen
    CHECK(!run_tool(&chat, "set_permission_mode", "{\"mode\":\"read-only\"}", &out));
    CHECK(run_tool(&chat, "set_permission_mode", "{\"mode\":\"trusted\"}", &out) && strstr(out.data, "cannot loosen"));
    CHECK(run_tool(&chat, "make_directory", "{\"path\":\"/tmp/cps4pf/e\"}", &out) && strstr(out.data, "Read-only mode"));
    CHECK(!run_tool(&chat, "list_directory", "{\"path\":\"/tmp/cps4pf\"}", &out));
    CHECK(!run_tool(&chat, "set_permission_mode", "{\"mode\":\"kid\"}", &out));
    CHECK(run_tool(&chat, "launch_app", "{\"title_id\":\"CUSA00001\"}", &out) && strstr(out.data, "Kid mode"));
    sb_init(&req);
    chat_build_request(&chat, &cfg, NULL, &req);
    CHECK(strstr(req.data, "Kid mode is on"));
    sb_free(&req);
    prefs_set_mode("full");
    confirm_answer = CONFIRM_YES;
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
    // ... and lock the login, even for the right PIN, until the wait is over
    snprintf(body, sizeof(body), "pin=%s", s.pin);
    req(&s, "POST /login", NULL, NULL, body, &r);
    CHECK(strstr(r.data, "429") && strstr(r.data, "Too many wrong PINs") && !strstr(r.data, "Set-Cookie"));
    s.lock_until = 0;
    req(&s, "POST /login", NULL, NULL, body, &r);
    CHECK(strstr(r.data, "Set-Cookie: s="));
    CHECK(s.lock_round == 0);
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
    system("rm -rf /tmp/claude-ps4-test/trash /tmp/claude-ps4-test/history.log /tmp/claude-ps4-test/permissions.conf /tmp/claude-ps4-test/memory.txt /tmp/claude-ps4-test/fan.conf");
    mkdir("/tmp/claude-ps4-test", 0777);
    actions_set_dir("/tmp/claude-ps4-test");
    test_history();
    test_bridge_stream();
    test_setup();
    test_stream();
    test_request();
    test_wrap();
    test_tools();
    test_home_folders();
    test_trash();
    test_storage();
    test_saves();
    test_pcinstall();
    test_goldhen();
    test_health();
    test_prefs();
    test_phase2();
    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
