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
// at a multiple of the game's rate wherever it has one; unset (or any other value) the same, except that a variable-refresh screen the 120 Hz display link paces at 60 fps
// keeps its mode: that link shows the game's frames sooner than the fixed 60 Hz mode does (docs/reference/frame-rate.md)
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

// Whether fullscreen keeps the screen's current mode instead of switching it to a fixed-rate one: vrr, the mode is variable-refresh; at60, front_screen_match's (nonzero:
// the game targets 60 fps, nvn_mtl.m link_sync); link, the display link takes its 120 Hz link on a variable-refresh screen at this rate (nvn_mtl.m mtl_link_vrr_120_hz)
static inline int front_mac_keep_vrr(int mode, int vrr, int at60, int link) {
    return mode == FRONT_MAC_MATCH_AUTO && vrr && at60 && link;
}

// Whether fullscreen takes back the screen's own mode, which this host left for a fixed-rate one at another target: vrr, the current mode (the host's pick) is
// variable-refresh; orig_vrr, the screen's own mode is; link as above, for the own mode's rate. The keep rule for the own mode once the 60 fps target has held for
// nvn_mtl.m LINK_VRR_BACK_NS (at60 2), so the screen's mode follows the current target, and a target that falls back to 30 within LINK_VRR_BACK_NS switches nothing
static inline int front_mac_restore_vrr(int mode, int vrr, int orig_vrr, int at60, int link) {
    return !vrr && at60 >= 2 && front_mac_keep_vrr(mode, orig_vrr, at60, link);
}
