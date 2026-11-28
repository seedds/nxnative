// Mac presentation layer opacity policy (front_mac.m, GitHub issue #1821).
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
