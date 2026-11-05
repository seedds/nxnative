// The display a game is told of (hle_vi.c): the largest display buffer (QueryDisplayBufferMaxResolution) and the largest layer ListDisplays gives, handheld and docked on
// screens of a few sizes, for a Switch 2 build and a Switch 1 one. Expectations written by hand: a Switch 1's layer is 1920x1080; a Switch 2's is the largest buffer
#include "../../gfx/hle_vi.c"
static int g_mode = 1, g_ounce = 1;
extern int g_w, g_h;   // the screen front_display_pixels reports (t_vi_display_front.c, its own file: hle_vi.c has a weak one)
int dm_opmode(void) { return g_mode; }
int dm_sdk_ounce(void) { return g_ounce; }
static int checks, fails;
#define CHECK(c, s) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "\xe2\x9c\x93" : "\xe2\x9c\x97", s); } while (0)
static int layer(uint64_t *w, uint64_t *h) {
    uint8_t p[0x60]; int n = h_vi_ListDisp(p, 1); memcpy(w, p + 0x50, 8); memcpy(h, p + 0x58, 8);
    return n == 1 && !strcmp((char *)p, "Default") && p[0x40] == 1;
}
int main(void) {
    struct { int mode, ounce, w, h; uint32_t buf; uint64_t lw, lh; const char *what; } c[] = {
        {1, 1, 5120, 2880, 3, 3840, 2160, "Switch 2 docked on a 5K screen: 4K buffer and layer"},
        {1, 1, 2560, 1440, 2, 2560, 1440, "Switch 2 docked on a 1440p screen: 1440p"},
        {1, 1, 1600, 1000, 0, 1280, 720, "Switch 2 docked on a screen below 1080p tall: 720p"},
        {1, 1, 0, 0, 1, 1920, 1080, "Switch 2 docked, screen not known: 1080p"},
        {0, 1, 5120, 2880, 1, 1920, 1080, "Switch 2 handheld: its own 1080p screen"},
        {1, 0, 5120, 2880, 3, 1920, 1080, "Switch 1 docked on a 5K screen: a 1080p layer"},
    };
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) {
        g_mode = c[i].mode; g_ounce = c[i].ounce; g_w = c[i].w; g_h = c[i].h;
        uint32_t b = 99; uint64_t w = 0, h = 0;
        CHECK(h_vi_BufMaxRes(&b, NULL) == RES_OK && b == c[i].buf && layer(&w, &h) && w == c[i].lw && h == c[i].lh, c[i].what);
    }
    printf("== vi display %d checks, %d failed ==\n", checks, fails); return fails != 0;
}
