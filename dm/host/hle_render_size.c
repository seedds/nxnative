// Super Smash Bros. Ultimate 13.0.1: the docked render size, DM_RENDER_SIZE=2560x1440 (the launcher's Resolution). The game makes its render targets once, at boot, so it is chosen before the start.
// The rewriter's stubs (dm/games/01006A800016E000/patches.py) read the size from the frame step state page; here the host writes it, and scales the two kinds of pipeline data that hold sizes of
// their own, at the places the game reads them, before the original load runs (after MacNativeGames' games/ssbu/runtime/game_patches.c, groups G and H; MIT):
// - Framebuffer records {name, width +8, height +0xc, ...} (FB_MAIN, DEPTH_MAIN, OffscreenFB 1280x720, FB_BLEND_64 240x135 ...): OBS_RENDER_RECORD.
// - Viewport records {name offset, x, y, w, h, 0.0, 1.0, ...}, read as a nu::ViewportPlug is made: OBS_RENDER_VIEWPORT.
// A record follows the screen when its size is not square and from 16x8 to 1920x1080: squares are shadows, reflections and portraits, and the 10x1 / 4x4 helpers stay. Each is scaled once, since the
// game reads a record again and a scaled one can still look like a screen size; a record whose values are no longer the scaled ones was loaded again at that address, and is scaled anew.
// Not for handheld, whose targets stay 1920x1080 and whose final copy reads a 1280x720 part of them.
#include "hle.h"
#include <pthread.h>

enum { STATE_SIZE = 0xa0, STATE_FLOATS = 0xa8, RECORDS = 1024 };
extern uint8_t *dm_fps_state;   // loader.c
int dm_opmode(void);            // hle_oe.c
static unsigned g_w, g_h;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int follows_screen(double w, double h) { return w != h && w >= 16 && h >= 8 && w <= 1920 && h <= 1080; }

typedef struct { const void *record; uint32_t now[4]; } seen_t;
typedef struct { seen_t seen[RECORDS]; unsigned n, scaled, kept; const char *what; } table_t;
// The record's entry, new if it has none; 0 when it was seen with these very values (nothing to do), or the table is full
static seen_t *record_entry(table_t *t, const void *record, const uint32_t *now, unsigned words) {
    unsigned i = 0; while (i < t->n && t->seen[i].record != record) i++;
    if (i < t->n) return memcmp(t->seen[i].now, now, words * 4) ? &t->seen[i] : NULL;
    if (t->n == RECORDS) { static int once; if (!once++) LOG("[RED] render size: more than %d %s records; the others keep their size", RECORDS, t->what); return NULL; }
    t->n++; t->seen[i].record = record; return &t->seen[i];
}
static void note(table_t *t, int follows) {
    if (follows) t->scaled++; else t->kept++;
    if ((t->scaled + t->kept) % 32 == 1) LOG("render size: %s records: %u scaled, %u kept", t->what, t->scaled, t->kept);
}
static void record_copied(uint64_t *regs, int reg) {
    static table_t t = {.what = "framebuffer"};
    uint32_t *r = (uint32_t *)regs[reg] + 2;   // width +8, height +0xc
    pthread_mutex_lock(&g_lock);
    seen_t *e = record_entry(&t, (const void *)regs[reg], r, 2);
    if (e) {
        int follows = follows_screen(r[0], r[1]);
        if (follows) { r[0] = (r[0] * g_w + 960) / 1920; r[1] = (r[1] * g_h + 540) / 1080; }
        memcpy(e->now, r, 8); note(&t, follows);
    }
    pthread_mutex_unlock(&g_lock);
}
static void viewport_read(uint64_t *regs, int reg) {
    static table_t t = {.what = "viewport"};
    float *v = (float *)(regs[reg] + 8);   // x, y, width, height
    pthread_mutex_lock(&g_lock);
    seen_t *e = record_entry(&t, (const void *)regs[reg], (const uint32_t *)v, 4);
    if (e) {
        int follows = follows_screen(v[0] + v[2], v[1] + v[3]);
        if (follows) { float sx = (float)g_w / 1920.0f, sy = (float)g_h / 1080.0f; v[0] *= sx; v[1] *= sy; v[2] *= sx; v[3] *= sy; }
        memcpy(e->now, v, 16); note(&t, follows);
    }
    pthread_mutex_unlock(&g_lock);
}

// When the game starts (main.c): the state page is the rewriter's, so only the build it was checked on gets a size
void dm_render_size_apply(void) {
    const char *e = getenv("DM_RENDER_SIZE");
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "1920x1080")) return;   // 0: the game's own, as the launcher writes it
    if (strcmp(e, "2560x1440")) { LOG("[RED] DM_RENDER_SIZE=%s: 0 (the game's own 1920x1080) or 2560x1440; keeping the game's size", e); return; }
    if (!dm_fps_state || strcmp(dm_main_build(), "b9b166df1db90baffbd8027eb0df14d6")) { LOG("[RED] DM_RENDER_SIZE=%s: this game build has no render size stubs; keeping the game's size", e); return; }
    if (!dm_opmode()) { LOG("DM_RENDER_SIZE=%s: docked only; handheld keeps the game's size", e); return; }
    g_w = 2560; g_h = 1440;
    uint32_t size[2] = {g_w, g_h}; float floats[2] = {(float)g_w, (float)g_h};
    memcpy(dm_fps_state + STATE_SIZE, size, sizeof size); memcpy(dm_fps_state + STATE_FLOATS, floats, sizeof floats);
    gate_obs_set(OBS_RENDER_RECORD, record_copied); gate_obs_set(OBS_RENDER_VIEWPORT, viewport_read);
    LOG("render size: docked %ux%u (the game's own 1920x1080; DM_RENDER_SIZE)", g_w, g_h);
}
