// Mac presentation policies (front_mac.m): the layer's opacity (GitHub issue #1821) and the screen's mode in fullscreen (below).
// On M4 Pro / M5 Pro Macs, macOS Direct-to-Display scanout of an opaque fullscreen CAMetalLayer on a mirrored HDMI
// display flickers a purple/pink horizontal band across the top of the picture. Setting CAMetalLayer.opaque = NO opts
// the layer out of Direct-to-Display into WindowServer compositing while keeping native fullscreen.
// Auto mode scopes the workaround to native fullscreen on a mirrored display; DM_MAC_COMPOSITED=1 forces non-opaque
// (for non-mirrored setups that hit the same compositor bug or A/B measurement) and =0 forces opaque.
#pragma once
#include <stdlib.h>
#include <string.h>

enum {
    FRONT_MAC_COMP_AUTO = 0,
    FRONT_MAC_COMP_OFF = -1,
    FRONT_MAC_COMP_ON = 1,
};

static inline int front_mac_composited_mode(const char *env) {
    if (!env || !*env || !strcmp(env, "auto")) return FRONT_MAC_COMP_AUTO;
    if (!strcmp(env, "0")) return FRONT_MAC_COMP_OFF;
    if (!strcmp(env, "1") || atoi(env) != 0) return FRONT_MAC_COMP_ON;
    return FRONT_MAC_COMP_AUTO;
}

static inline int front_mac_layer_opaque(int mode, int fullscreen, int mirrored) {
    if (mode == FRONT_MAC_COMP_ON) return 0;
    if (mode == FRONT_MAC_COMP_OFF) return 1;
    return !(fullscreen && mirrored);
}

static inline const char *front_mac_layer_opaque_why(int mode, int fullscreen, int mirrored) {
    if (mode == FRONT_MAC_COMP_ON) return "DM_MAC_COMPOSITED=1";
    if (mode == FRONT_MAC_COMP_OFF) return "DM_MAC_COMPOSITED=0";
    if (!fullscreen) return "window";
    return mirrored ? "fullscreen on a mirrored display" : "fullscreen on an unmirrored display";
}

// The screen's mode in fullscreen (front_mac.m screen_match_once, DM_SCREEN_MATCH): 0 keeps every screen's mode; 1 switches a variable-refresh screen to a fixed-rate mode
// at a multiple of the game's rate wherever it has one; unset (or any other value) the same, except that a variable-refresh screen the 120 Hz display link paces in fullscreen
// keeps its mode when switching would drop it below 120 Hz (e.g. 120 Hz ProMotion -> fixed 60 Hz; a 240 Hz VRR screen with a fixed 120 Hz mode still switches to fixed 120 Hz).
// Keeping the variable mode avoids both the fixed-60 Hz link's extra refresh of latency and mid-gameplay mode switches across 60/30 fps changes (docs/reference/frame-rate.md)
enum {
    FRONT_MAC_MATCH_OFF = 0,
    FRONT_MAC_MATCH_FIXED = 1,
    FRONT_MAC_MATCH_AUTO = 2,
};

static inline int front_mac_screen_match_mode(const char *env) {
    if (env && !strcmp(env, "0")) return FRONT_MAC_MATCH_OFF;
    if (env && !strcmp(env, "1")) return FRONT_MAC_MATCH_FIXED;
    return FRONT_MAC_MATCH_AUTO;
}

// Whether fullscreen keeps the screen's current variable-refresh mode instead of switching to a fixed-rate candidate pick_hz:
// vrr: current mode is variable-refresh; want: link rate requested by link_sync (30, 60, or 120); pick_hz: highest fixed-rate candidate in Hz (0 = none);
// link: the display link automatically runs its 120 Hz VRR path at this screen's rate (nvn_mtl.m mtl_link_vrr_120_hz)
static inline int front_mac_keep_vrr(int mode, int vrr, int want, int pick_hz, int link) {
    return mode == FRONT_MAC_MATCH_AUTO && vrr && want > 0 && want <= 60 && pick_hz < 120 && link;
}
