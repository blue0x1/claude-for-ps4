// Host-only harness: serves the phone page on 127.0.0.1:8765 with a fake
// chat, so the page can be checked in a desktop browser.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/setup.h"

static Setup s;
static Chat chat;

static void *fake_main(void *arg)
{
    (void)arg;
    char text[REMOTE_TEXT_MAX];
    for (;;) {
        if (setup_take_text(&s, text, sizeof(text))) {
            chat_add(&chat, ROLE_USER, text);
            chat.busy = 1;
            int a = chat_add(&chat, ROLE_ASSISTANT, "");
            const char *words[] = { "Sure. ", "Your PS4 has ", "412 GB free ", "on internal storage." };
            for (int i = 0; i < 4; i++) {
                usleep(400000);
                chat_append(&chat, a, words[i], strlen(words[i]));
            }
            chat.busy = 0;
        }
        if (s.want_new) {
            s.want_new = 0;
            chat_clear(&chat);
        }
        usleep(20000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    chat_init(&chat);
    setup_init(&s, &chat);
    Config c;
    config_defaults(&c);
    snprintf(c.api_key, sizeof(c.api_key), "sk-ant-test");
    setup_set_config(&s, &c, 1);
    snprintf(s.pin, sizeof(s.pin), "%s", argc > 1 ? argv[1] : "123456");
    chat_add(&chat, ROLE_USER, "How much space is left?");
    chat_add(&chat, ROLE_TOOL, "get_system_info");
    chat_add(&chat, ROLE_ASSISTANT, "You have 412 GB free of 825 GB. The SoC is at 54 C.");
    if (argc > 2) {
        chat.confirm = CONFIRM_PENDING;
        snprintf(chat.confirm_text, sizeof(chat.confirm_text), "Install package\n/mnt/usb0/RetroArch_PS4.pkg\nUP0001-CUSA00001_00-RETROARCH0000000, PS4GD, 402.7 MB ?");
    }
    pthread_t th;
    pthread_create(&th, NULL, fake_main, NULL);

    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(SETUP_PORT) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0)
        return 1;
    printf("listening\n");
    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        char buf[16385];
        size_t len = 0;
        long want = -1;
        while (len < 16384) {
            ssize_t n = recv(cfd, buf + len, 16384 - len, 0);
            if (n <= 0)
                break;
            len += (size_t)n;
            buf[len] = 0;
            char *end = strstr(buf, "\r\n\r\n");
            if (end && want < 0) {
                const char *cl = strcasestr(buf, "content-length:");
                want = (long)(end + 4 - buf) + (cl ? atol(cl + 15) : 0);
            }
            if (want >= 0 && (long)len >= want)
                break;
        }
        buf[len] = 0;
        StrBuf r;
        sb_init(&r);
        setup_handle_request(&s, buf, len, &r);
        send(cfd, r.data, r.len, 0);
        sb_free(&r);
        close(cfd);
    }
}
