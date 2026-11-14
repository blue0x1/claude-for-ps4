// Renders the UI on a PC to PNG files for a quick visual check.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "../src/ui.h"

Font g_body, g_small, g_tiny, g_title, g_serif, g_serif_b, g_head, g_hero, g_mono;
Chat g_chat;
Config g_cfg;
int g_cfg_ok = 1, g_scroll, g_max_scroll;
Setup g_setup;

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

static unsigned char *load(const char *path)
{
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n);
    fread(d, 1, (size_t)n, f);
    fclose(f);
    return d;
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    unsigned char *sans = load("assets/fonts/DejaVuSans.ttf"), *serif = load("assets/fonts/DejaVuSerif.ttf"),
                  *serif_b = load("assets/fonts/DejaVuSerif-Bold.ttf"), *mono = load("assets/fonts/DejaVuSansMono.ttf");
    font_init(&g_body, sans, 32);
    font_init(&g_small, sans, 27);
    font_init(&g_tiny, sans, 23);
    font_init(&g_title, sans, 44);
    font_init(&g_serif, serif, 34);
    font_init(&g_hero, serif, 64);
    font_init(&g_serif_b, serif_b, 34);
    font_init(&g_head, serif_b, 42);
    font_init(&g_mono, mono, 27);
    ui_init();
    config_defaults(&g_cfg);
    chat_init(&g_chat);
    static uint32_t px[SCREEN_W * SCREEN_H];
    Canvas c = { px, SCREEN_W, SCREEN_H, SCREEN_W, 16, 8, 0, 0xFF000000 };
    char path[512];

    render(&c, 0);
    snprintf(path, sizeof(path), "%s/preview-empty.png", outdir);
    save(&c, path);

    chat_add(&g_chat, ROLE_USER, "How much space is left on my PS4, and are there any .pkg files on my USB drive?");
    chat_add(&g_chat, ROLE_TOOL, "Checked system info");
    chat_add(&g_chat, ROLE_TOOL, "Searched for .pkg files");
    chat_add(&g_chat, ROLE_ASSISTANT,
             "## Storage\n"
             "Your internal storage has **312.4 GB free** of 862.0 GB, and the CPU is at 52 C, which is normal.\n\n"
             "I found two packages on the USB drive:\n"
             "- `Kali-PS4-Launcher.pkg` (14.2 MB)\n"
             "- `RetroArch_PS4.pkg` (402.7 MB)\n\n"
             "To check one yourself from a PC:\n"
             "```\n"
             "curl ftp://192.168.1.100:2121/mnt/usb0/pkg/\n"
             "```\n"
             "Want me to back up your saves to the USB drive before you install anything?");
    render(&c, 0);
    snprintf(path, sizeof(path), "%s/preview-chat.png", outdir);
    save(&c, path);
    g_picker.open = 1;
    g_picker.col = 1;
    g_picker.row = 3;
    snprintf(g_cfg.effort, sizeof(g_cfg.effort), "high");
    render(&c, 0);
    snprintf(path, sizeof(path), "%s/preview-picker.png", outdir);
    save(&c, path);
    g_picker.open = 0;
    chat_add(&g_chat, ROLE_USER, "Yes, copy my saves to the USB.");
    g_chat.busy = 1;
    g_chat.confirm = CONFIRM_PENDING;
    snprintf(g_chat.confirm_text, sizeof(g_chat.confirm_text),
             "Copy folder\n/user/home/1a2b3c4d/savedata\nto\n/mnt/usb0/backup/savedata ?");
    render(&c, 0);
    snprintf(path, sizeof(path), "%s/preview-confirm.png", outdir);
    save(&c, path);
    g_chat.confirm = CONFIRM_NONE;
    g_chat.busy = 0;
    chat_clear(&g_chat);
    g_cfg_ok = 0;
    chat_add(&g_chat, ROLE_NOTICE, "Not set up yet. Press Options to set up from your phone, or put a claude.ini on a USB drive (root folder).");
    g_setup.shown = 1;
    snprintf(g_setup.url, sizeof(g_setup.url), "http://192.168.1.100:8765/");
    snprintf(g_setup.pin, sizeof(g_setup.pin), "482913");
    render(&c, 0);
    snprintf(path, sizeof(path), "%s/preview-setup.png", outdir);
    save(&c, path);
    printf("ok\n");
    return 0;
}
