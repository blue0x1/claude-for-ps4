// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// Health monitor: samples the temperatures every 30 s while the app is open,
// warns when the console runs hot, and keeps a fan profile. The fan
// threshold resets on every reboot, so the saved profile is applied again
// when the app starts. The "auto" profile lowers the threshold while the
// console is hot and puts it back when it cooled down.
#include "health.h"
#include "actions.h"
#include "plat.h"
#include "tools.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SAMPLES 480            // 4 hours at one per 30 s
#define SAMPLE_EVERY 30
#define HOT 78                 // auto: boost at or above this
#define COOL 64                // auto: back to normal at or below this
#define WARN 85                // pop-up warning

typedef struct {
    time_t t;
    short cpu, soc;
} Sample;

static Sample g_s[SAMPLES];
static int g_count, g_head;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const char *name;
    int threshold;
    const char *about;
} Profile;

static const Profile PROFILES[] = {
    { "quiet", 79, "system default, quietest" },
    { "balanced", 70, "a bit cooler, still quiet" },
    { "cool", 62, "cooler, fan is audible" },
    { "max", 50, "coolest, loud fan" },
    { "auto", 70, "70 C normally; 55 C while the console is hot (78 C+), back when it cooled to 64 C" },
    { NULL, 0, NULL },
};

static char g_profile[16] = "";   // "" = never set by the app
static int g_threshold;           // what the app last set
static int g_boosted, g_hot_n, g_cool_n;
static time_t g_last_warn;
static int g_loaded;

static void conf_path(char *p, size_t n) { snprintf(p, n, "%s/fan.conf", actions_dir()); }

static void save_conf(void)
{
    char p[300];
    conf_path(p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (f) {
        fprintf(f, "%s %d\n", g_profile, g_threshold);
        fclose(f);
    }
}

static void set_fan(int t)
{
    StrBuf junk;
    sb_init(&junk);
    if (!plat_set_fan_threshold(t, &junk))
        g_threshold = t;
    sb_free(&junk);
}

static const Profile *find_profile(const char *name)
{
    for (int i = 0; PROFILES[i].name; i++)
        if (!strcmp(PROFILES[i].name, name))
            return &PROFILES[i];
    return NULL;
}

void health_reset(void)
{
    pthread_mutex_lock(&g_lock);
    g_count = g_head = 0;
    g_profile[0] = 0;
    g_threshold = g_boosted = g_hot_n = g_cool_n = 0;
    g_last_warn = 0;
    g_loaded = 0;
    pthread_mutex_unlock(&g_lock);
}

// Lock held. Loads fan.conf once and applies it (the PS4 forgets on reboot).
static void load_once(void)
{
    if (g_loaded)
        return;
    g_loaded = 1;
    char p[300];
    conf_path(p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f)
        return;
    char name[16] = "";
    int t = 0;
    if (fscanf(f, "%15s %d", name, &t) == 2 && t >= 45 && t <= 80) {
        snprintf(g_profile, sizeof(g_profile), "%s", name);
        set_fan(!strcmp(name, "auto") ? 70 : t);
    }
    fclose(f);
}

// Lock held.
static void add_sample(time_t now, int cpu, int soc)
{
    g_s[g_head] = (Sample){ now, (short)cpu, (short)soc };
    g_head = (g_head + 1) % SAMPLES;
    if (g_count < SAMPLES)
        g_count++;
    if (!strcmp(g_profile, "auto")) {
        g_hot_n = cpu >= HOT ? g_hot_n + 1 : 0;
        g_cool_n = cpu <= COOL ? g_cool_n + 1 : 0;
        if (!g_boosted && g_hot_n >= 2) {
            set_fan(55);
            g_boosted = 1;
            plat_notify("Claude: the PS4 is hot, fan turned up");
        } else if (g_boosted && g_cool_n >= 10) {
            set_fan(70);
            g_boosted = 0;
        }
    }
    if (cpu >= WARN && now - g_last_warn > 600) {
        g_last_warn = now;
        char m[120];
        snprintf(m, sizeof(m), "Claude: the PS4 is very hot (%d C). Check the vents and airflow.", cpu);
        plat_notify(m);
    }
}

void health_sample(time_t now, int cpu, int soc)
{
    pthread_mutex_lock(&g_lock);
    load_once();
    add_sample(now, cpu, soc);
    pthread_mutex_unlock(&g_lock);
}

void health_tick(void)
{
    static time_t last;
    time_t now = time(NULL);
    if (last && now - last < SAMPLE_EVERY)
        return;
    last = now;
    int cpu, soc;
    if (plat_temperatures(&cpu, &soc) == 0 && cpu > 0)
        health_sample(now, cpu, soc);
}

// i = 0 is the oldest sample. Lock held.
static const Sample *at(int i) { return &g_s[(g_head - g_count + i + SAMPLES) % SAMPLES]; }

static void stats(time_t since, int *mx, int *avg, int *n)
{
    long sum = 0;
    *mx = *n = 0;
    for (int i = 0; i < g_count; i++) {
        const Sample *s = at(i);
        if (s->t < since)
            continue;
        sum += s->cpu;
        if (s->cpu > *mx)
            *mx = s->cpu;
        (*n)++;
    }
    *avg = *n ? (int)(sum / *n) : 0;
}

static int t_health_report(StrBuf *out)
{
    int cpu = 0, soc = 0;
    int have = plat_temperatures(&cpu, &soc) == 0 && cpu > 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    load_once();
    if (have)
        sb_appendf(out, "Temperature now: CPU %d C, SoC %d C (%s)\n", cpu, soc,
                   cpu >= WARN ? "VERY HOT" : cpu >= HOT ? "hot" : cpu >= 65 ? "warm" : "fine");
    else
        sb_append(out, "Temperature: not readable\n");
    int mx, avg, n;
    stats(now - 1800, &mx, &avg, &n);
    if (n)
        sb_appendf(out, "Last 30 min: average %d C, highest %d C\n", avg, mx);
    stats(0, &mx, &avg, &n);
    if (n > 1)
        sb_appendf(out, "Since the app opened (%d min): average %d C, highest %d C\n",
                   (int)((now - at(0)->t) / 60), avg, mx);
    if (g_profile[0])
        sb_appendf(out, "Fan profile: %s, threshold %d C%s\n", g_profile, g_threshold,
                   g_boosted ? " (turned up right now because it is hot)" : "");
    else
        sb_append(out, "Fan profile: not set by this app (system default threshold 79 C)\n");
    pthread_mutex_unlock(&g_lock);
    long up = plat_uptime();
    sb_appendf(out, "Console on for %ld h %02ld min\n", up / 3600, (up / 60) % 60);
    unsigned long long f, t;
    if (plat_free_space("/user", &f, &t) == 0 && t) {
        char a[32], b[32];
        tools_human_size(f, a, sizeof(a));
        tools_human_size(t, b, sizeof(b));
        double used = 100.0 * (double)(t - f) / (double)t;
        sb_appendf(out, "Internal storage: %s free of %s%s\n", a, b,
                   used > 95 ? " (almost full: games may fail to update; try storage_report)" : "");
    }
    if (have && cpu >= HOT)
        sb_append(out, "Tip: it runs hot. Check that the vents are free and not dusty, give it space, "
                       "and consider the cool or auto fan profile.\n");
    sb_append(out, "Note: samples are taken while Claude for PS4 is open.");
    return 0;
}

static int t_temperature_history(const JVal *in, StrBuf *out)
{
    JVal *m = json_get(in, "minutes");
    int minutes = m && m->type == J_NUM ? (int)m->num : 60;
    if (minutes < 5)
        minutes = 5;
    if (minutes > 240)
        minutes = 240;
    time_t now = time(NULL), since = now - minutes * 60;
    int step = minutes <= 60 ? 300 : 900;
    pthread_mutex_lock(&g_lock);
    int shown = 0;
    time_t bucket = 0;
    int bmax = 0;
    sb_appendf(out, "CPU temperature, last %d min (highest per %d min):\n", minutes, step / 60);
    for (int i = 0; i <= g_count; i++) {
        const Sample *s = i < g_count ? at(i) : NULL;
        if (s && s->t < since)
            continue;
        time_t b = s ? s->t - s->t % step : 0;
        if (bucket && (!s || b != bucket)) {
            struct tm *tm = gmtime(&bucket);
            sb_appendf(out, "  %02d:%02d UTC  %3d C  ", tm->tm_hour, tm->tm_min, bmax);
            for (int k = 40; k < bmax && k < 95; k += 2)
                sb_append(out, "#");
            sb_append(out, "\n");
            shown++;
        }
        if (!s)
            break;
        if (b != bucket) {
            bucket = b;
            bmax = 0;
        }
        if (s->cpu > bmax)
            bmax = s->cpu;
    }
    pthread_mutex_unlock(&g_lock);
    if (!shown)
        sb_append(out, "  No samples yet (one is taken every 30 s while the app is open).\n");
    return 0;
}

static int t_set_fan_profile(Chat *chat, const JVal *in, StrBuf *out)
{
    const char *name = json_get_str(in, "profile");
    const Profile *p = name ? find_profile(name) : NULL;
    if (!p) {
        sb_append(out, "profile must be one of:");
        for (int i = 0; PROFILES[i].name; i++)
            sb_appendf(out, " %s (%d C: %s)%s", PROFILES[i].name, PROFILES[i].threshold, PROFILES[i].about,
                       PROFILES[i + 1].name ? "," : "");
        return 1;
    }
    char q[300];
    snprintf(q, sizeof(q), "Use the %s fan profile?\n%s\nKept after a reboot once Claude for PS4 is opened.", p->name,
             p->about);
    if (!chat_ask_confirm(chat, q)) {
        sb_append(out, "The user declined.");
        return 1;
    }
    StrBuf r;
    sb_init(&r);
    int e = plat_set_fan_threshold(p->threshold, &r);
    if (e) {
        sb_append(out, r.data ? r.data : "Fan change failed.");
        sb_free(&r);
        return 1;
    }
    sb_free(&r);
    pthread_mutex_lock(&g_lock);
    g_loaded = 1;
    snprintf(g_profile, sizeof(g_profile), "%s", p->name);
    g_threshold = p->threshold;
    g_boosted = g_hot_n = g_cool_n = 0;
    save_conf();
    pthread_mutex_unlock(&g_lock);
    sb_appendf(out, "Fan profile %s on (threshold %d C). It is applied again each time the app starts after a reboot.",
               p->name, p->threshold);
    return 0;
}

int health_tool_run(Chat *chat, const char *name, const JVal *in, StrBuf *out)
{
    if (!strcmp(name, "health_report"))
        return t_health_report(out);
    if (!strcmp(name, "temperature_history"))
        return t_temperature_history(in, out);
    if (!strcmp(name, "set_fan_profile"))
        return t_set_fan_profile(chat, in, out);
    return -1;
}
