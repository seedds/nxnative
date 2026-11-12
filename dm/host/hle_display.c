// The size of the display the host tells games about, when the player turns on Native resolution (DM_NATIVE_RESOLUTION=1, or the profile's `native_res 1`): the 16:9 picture that fits the
// screen's pixels instead of the Switch's 1920x1080 (docked) or 1280x720 (handheld). nn::oe::GetDefaultDisplayResolution (hle_oe.c) and nn::vi::ListDisplays (hle_vi.c) answer it, so a
// game that sizes its picture from them draws at it; a game build whose renderer takes the size from elsewhere has its own rows (hle_nro.c). DM_DISPLAY_SIZE=WxH sets another size and
// DM_DISPLAY_SIZE=off turns it off whatever the setting says.
#include "hle.h"
#include <TargetConditionals.h>

__attribute__((weak)) int front_display_pixels(int *w, int *h) { (void)w; (void)h; return 0; }   // front_mac.m, front_ios.m

// The 16:9 picture that fits dw x dh (the pixels of the screen in either orientation): at most 3840x2160, at least 1280x720, even numbers
void dm_display_fit(int dw, int dh, int *w, int *h) {
    if (dw < dh) { int t = dw; dw = dh; dh = t; }
    int W, H;
    if (dw * 9 >= dh * 16) { H = dh; W = (dh * 16) / 9; } else { W = dw; H = (dw * 9) / 16; }
    if (W > 3840) { W = 3840; H = 2160; }
    if (W < 1280) { W = 1280; H = 720; }
    *w = W & ~1; *h = H & ~1;
}

static int chosen_size(int *w, int *h) {
    const char *e = getenv("DM_DISPLAY_SIZE");
    if (e && *e) {
        if (!strcmp(e, "off")) return 0;
        if (sscanf(e, "%dx%d", w, h) == 2 && *w >= 320 && *h >= 240 && *w <= 7680 && *h <= 4320) return 1;
        LOG("[RED] DM_DISPLAY_SIZE=%s: must be WxH between 320x240 and 7680x4320 or off; using the setting Native resolution", e);
    }
    int on = DM_GAME_NATIVE_RES;
#if !TARGET_OS_OSX
    on = 0;   // an iPhone or iPad turns it on only when asked: the launcher's setting or DM_NATIVE_RESOLUTION=1
#endif
    const char *n = getenv("DM_NATIVE_RESOLUTION");
    if (n && *n) on = strcmp(n, "0") != 0;
    int dw = 0, dh = 0;
    if (!on || !front_display_pixels(&dw, &dh)) return 0;
    dm_display_fit(dw, dh, w, h); return 1;
}

// The display size games are told, 0 when they keep the Switch's. Decided once, at the first ask (the loader asks before any game code runs)
int dm_display_target(int *w, int *h) {
    static int state, tw, th;   // 0 undecided, 1 none, 2 a size
    if (!state) {
        int cw = 0, ch = 0;
        if (chosen_size(&cw, &ch)) { tw = cw; th = ch; state = 2; LOG("main: native resolution: games are told the display is %dx%d", tw, th); }
        else state = 1;
    }
    if (state != 2) return 0;
    *w = tw; *h = th; return 1;
}
