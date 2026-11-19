// DM_SCREEN_ASPECT: the state page a screen shape is written into (loader.c dm_fps_state_apply), for the shapes the iOS front end asks for.
// A phone standing up asks for its upper half's shape, 0.92 on a 19.5:9 screen, so the accepted range has to reach below 1.3: the lower bound
// once sat at 1.3, and the front end's 0.92 was refused with "[RED] a width / height from 1.3 to 2.4; keeping 16:9" while the screen never changed.
#include "../loader.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void mtl_present_screen(uint32_t w, uint32_t h) { (void)w; (void)h; }   // nvn_mtl.m: the picture's shape, no renderer here
// What loader.c links against elsewhere (as t_nro.c provides): the flags, the clock, the gate and symbol tables, and the module loader's
// deactivation, which this fixture never reaches.
struct dm_flags g_flag;
uint64_t now_ns(void) { return 0; }
void gate_register(uint64_t a, const char *k, int n, uint64_t o) { (void)a; (void)k; (void)n; (void)o; }
void hlecount_seal(void) {}
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
static uint64_t missing;
uint64_t resolve_import(module_t *m, const char *name) { (void)m; (void)name; return (uint64_t)&missing; }
int hle_implements(const char *name) { (void)name; return 0; }
int svc_nro_deactivate(module_t *m) { m->loaded = 0; return 1; }
void dm_thread_service_connect(void) {}

static uint8_t g_page[0x100];
static int g_cases, g_fails;

static uint32_t at(size_t off) { uint32_t v; memcpy(&v, g_page + off, 4); return v; }

static void shape(const char *value, uint32_t want_w, uint32_t want_h, const char *why) {
    g_cases++;
    memset(g_page, 0, sizeof g_page);
    if (value) setenv("DM_SCREEN_ASPECT", value, 1); else unsetenv("DM_SCREEN_ASPECT");
    dm_fps_state_apply();
    uint32_t w = at(0x64), h = at(0x48);
    if (w != want_w || h != want_h) {
        printf("✗ %s: DM_SCREEN_ASPECT=%s gave %ux%u, wanted %ux%u\n", why, value ? value : "(unset)", w, h, want_w, want_h);
        g_fails++;
    } else printf("✓ %s: DM_SCREEN_ASPECT=%s → %ux%u\n", why, value ? value : "(unset)", w, h);
}

int main(void) {
    dm_fps_state = g_page;
    g_fps_mod = "test";

#if DM_GAME_SCREEN_ASPECT
    shape("0.92", 1880, 2040, "a phone standing up: its upper half's shape, below the overlay's 1.3");
#else
    shape("0.92", 0, 0, "a profile without screen-shape stubs keeps the 1.3 range: 16:9");
#endif
    shape("1.43", 1920, 1344, "an iPhone Duo");
#if DM_GAME_SCREEN_ASPECT
    shape("2.17", 1920, 888, "a 19.5:9 phone lying down");
#else
    shape("2.17", 0, 0, "without the stubs the range stops at 1.77: 16:9");
#endif
#if DM_GAME_SCREEN_ASPECT
    shape("0.9", 1840, 2048, "the range's own floor");   // 2048 * 0.9 rounded down to a multiple of 8, its height capped at the layouts' 2048
#endif
    shape("0.89", 0, 0, "below the range: 16:9");
    shape(DM_GAME_SCREEN_ASPECT ? "2.4" : "1.77", 0, 0, "the range's ceiling is exclusive: 16:9");
    shape("0", 0, 0, "0 is 16:9");
    shape(NULL, 0, 0, "unset is 16:9");

    printf("== %d checks, %d failed ==\n", g_cases, g_fails);
    return g_fails != 0;
}
