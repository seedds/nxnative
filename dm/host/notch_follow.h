// Fullscreen around the notch follows the screen (DM_FULLSCREEN=2, front_mac.m notch_follow): a full screen game window that moves to another screen (undocking, docking, clamshell,
// dragging) takes the full screen that screen calls for. Native full screen on a screen with a notch becomes the panel around it; the panel on a screen without one becomes native
// full screen. A window stays a window, and settings 0 and 1 never switch. The front end coalesces a burst of screen events (a dock or undock posts several, with in-between
// sizes) and decides once they have settled, never during a native full screen transition, and never on no screen at all (the window between screens). The decision reads only
// the setting, whether the screen has a notch and the window's mode, never the drawable's size. Pure, so tests/t_notch_follow.c checks it without a screen
#pragma once

typedef enum { NF_WINDOW, NF_NATIVE, NF_PANEL } nf_mode;   // a window, native full screen (its own Space), the panel around the notch
typedef enum {
    NF_KEEP,              // nothing to do
    NF_WAIT,              // decide later: a native full screen transition is under way, or the window has no screen yet
    NF_NATIVE_TO_PANEL,   // leave native full screen, then (on DidExit) enter the panel
    NF_PANEL_TO_NATIVE,   // leave the panel, then ask for native full screen
} nf_action;

// notch2: DM_FULLSCREEN is 2. screen: 1 the window's screen has a notch, 0 it has none, -1 no screen. busy: a native full screen transition (or a switch of this follow) is under way
static inline nf_action nf_decide(int notch2, int screen, nf_mode mode, int busy) {
    if (!notch2 || mode == NF_WINDOW) return NF_KEEP;
    if (busy || screen < 0) return NF_WAIT;
    if (mode == NF_NATIVE && screen == 1) return NF_NATIVE_TO_PANEL;
    if (mode == NF_PANEL && screen == 0) return NF_PANEL_TO_NATIVE;
    return NF_KEEP;
}

static inline const char *nf_action_name(nf_action a) {
    switch (a) {
    case NF_KEEP: return "keep";
    case NF_WAIT: return "wait";
    case NF_NATIVE_TO_PANEL: return "native full screen to the panel";
    case NF_PANEL_TO_NATIVE: return "the panel to native full screen";
    }
    return "?";
}

// A last guard against a loop (a screen whose notch comes and goes, an AppKit transition that undoes itself): at most NF_GUARD_N switches within NF_GUARD_SPAN seconds; past that a switch is
// refused (the full screen stays as it is, ⌃⌘F still works) until the oldest of them is NF_GUARD_SPAN old. now: seconds on any monotonic clock. Returns 1 and records the switch when it is allowed
#define NF_GUARD_N 4
#define NF_GUARD_SPAN 30.0
typedef struct { double at[NF_GUARD_N]; unsigned n; } nf_guard;   // the last NF_GUARD_N switches' times, a ring; n counts all switches
static inline int nf_guard_allow(nf_guard *g, double now) {
    if (g->n >= NF_GUARD_N && now - g->at[g->n % NF_GUARD_N] < NF_GUARD_SPAN) return 0;   // the oldest of the last N is still within the span
    g->at[g->n % NF_GUARD_N] = now; g->n++;
    return 1;
}
