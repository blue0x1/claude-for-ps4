// Renders the UI on a PC to PNG files for a quick visual check.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "../src/ui.h"

Font g_body, g_small, g_title;
Chat g_chat;
Config g_cfg;
int g_cfg_ok = 1, g_scroll, g_max_scroll;

static void save(Canvas *c, const char *path)
{
    unsigned char *rgb = malloc((size_t)c->w * c->h * 3);
    for (int i = 0; i < c->w * c->h; i++) {
        uint32_t p = c->px[i];
        rgb[i * 3] = (p >> 16) & 0xFF;
        rgb[i * 3 + 1] = (p >> 8) & 0xFF;
        rgb[i * 3 + 2] = p & 0xFF;
    }
    stbi_write_png(path, c->w, c->h, 3, rgb, c->w * 3);
    free(rgb);
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    FILE *f = fopen("assets/fonts/DejaVuSans.ttf", "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n);
    fread(d, 1, (size_t)n, f);
    fclose(f);
    font_init(&g_body, d, 34);
    font_init(&g_small, d, 28);
    font_init(&g_title, d, 44);
    config_defaults(&g_cfg);
    chat_init(&g_chat);
    static uint32_t px[SCREEN_W * SCREEN_H];
    Canvas c = { px, SCREEN_W, SCREEN_H, SCREEN_W, 16, 8, 0, 0xFF000000 };
    char path[512];

    render(&c, 0);
    snprintf(path, sizeof(path), "%s/preview-empty.png", outdir);
    save(&c, path);

    chat_add(&g_chat, ROLE_USER, "How much space is left on my PS4, and are there any .pkg files on my USB drive?");
    chat_add(&g_chat, ROLE_TOOL, "Tool: get_system_info");
    chat_add(&g_chat, ROLE_TOOL, "Tool: find_pkg_files");
    chat_add(&g_chat, ROLE_ASSISTANT,
             "Your internal storage has 312.4 GB free of 862.0 GB, and the CPU is at 52 C, which is normal.\n\n"
             "I found two packages on the USB drive:\n"
             "- /mnt/usb0/pkg/Kali-PS4-Launcher.pkg (14.2 MB)\n"
             "- /mnt/usb0/pkg/RetroArch_PS4.pkg (402.7 MB)\n\n"
             "Want me to back up your saves to the USB drive before you install anything?");
    chat_add(&g_chat, ROLE_USER, "Yes, copy my saves to the USB.");
    g_chat.busy = 1;
    g_chat.confirm = CONFIRM_PENDING;
    snprintf(g_chat.confirm_text, sizeof(g_chat.confirm_text),
             "Copy folder\n/user/home/1a2b3c4d/savedata\nto\n/mnt/usb0/backup/savedata ?");
    render(&c, 0);
    snprintf(path, sizeof(path), "%s/preview-confirm.png", outdir);
    save(&c, path);
    printf("ok\n");
    return 0;
}
