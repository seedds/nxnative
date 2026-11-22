// Fullscreen around the notch follows the screen (notch_follow.h): the whole setting × screen × mode table, the waits (a transition under way, no screen), the undock and dock
// sequences from the play test of 2026-11-18, and the loop guard
#include "../notch_follow.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int main(void) {
    // setting (0/1 = not 2, 2) × screen (no notch, notch, none) × mode, nothing under way
    static const char *mode_name[] = {"window", "native", "panel"};
    for (int notch2 = 0; notch2 <= 1; notch2++)
        for (int screen = -1; screen <= 1; screen++)
            for (int mode = NF_WINDOW; mode <= NF_PANEL; mode++) {
                nf_action want = NF_KEEP;
                if (notch2 && mode != NF_WINDOW) {
                    if (screen < 0) want = NF_WAIT;
                    else if (mode == NF_NATIVE && screen == 1) want = NF_NATIVE_TO_PANEL;
                    else if (mode == NF_PANEL && screen == 0) want = NF_PANEL_TO_NATIVE;
                }
                char name[128];
                snprintf(name, sizeof name, "setting %s, screen %s, %s: %s", notch2 ? "2" : "0/1", screen < 0 ? "none" : screen ? "notched" : "without a notch", mode_name[mode], nf_action_name(want));
                CHECK(nf_decide(notch2, screen, (nf_mode)mode, 0) == want, name);
                // under way: a full screen under setting 2 waits, everything else is the same
                nf_action busy = notch2 && mode != NF_WINDOW ? NF_WAIT : NF_KEEP;
                snprintf(name, sizeof name, "setting %s, screen %s, %s, a transition under way: %s", notch2 ? "2" : "0/1", screen < 0 ? "none" : screen ? "notched" : "without a notch", mode_name[mode], nf_action_name(busy));
                CHECK(nf_decide(notch2, screen, (nf_mode)mode, 1) == busy, name);
            }
    CHECK(nf_decide(1, 1, NF_WINDOW, 0) == NF_KEEP, "a window on the MacBook stays a window");
    CHECK(nf_decide(0, 1, NF_NATIVE, 0) == NF_KEEP, "setting 1: native full screen on the MacBook stays");
    CHECK(nf_decide(1, 1, NF_PANEL, 0) == NF_KEEP, "the panel on the MacBook stays");
    CHECK(nf_decide(1, 0, NF_NATIVE, 0) == NF_KEEP, "native full screen on the Studio Display stays");

    // The play test: started on the Studio Display (native), undocked. The window has no screen for a moment, then the built-in one (3024x1964, then 3024x1898: sizes are no input)
    nf_mode m = NF_NATIVE;
    CHECK(nf_decide(1, 0, m, 0) == NF_KEEP, "undock 1: still on the Studio Display");
    CHECK(nf_decide(1, -1, m, 0) == NF_WAIT, "undock 2: no screen");
    CHECK(nf_decide(1, 1, m, 0) == NF_NATIVE_TO_PANEL, "undock 3: the built-in screen has a notch: leave native full screen");
    CHECK(nf_decide(1, 1, m, 1) == NF_WAIT, "undock 4: leaving (WillExit..DidExit): wait");
    m = NF_WINDOW;   // DidExit: the front end enters the panel itself (notch_follow_exited), not through nf_decide
    CHECK(nf_decide(1, 1, m, 1) == NF_KEEP, "undock 5: between DidExit and the panel, a window is never touched");
    m = NF_PANEL;
    CHECK(nf_decide(1, 1, m, 0) == NF_KEEP, "undock 6: the panel: settled");
    // Docked again (lid closed): the panel on the Studio Display
    CHECK(nf_decide(1, 0, m, 0) == NF_PANEL_TO_NATIVE, "dock 1: no notch: leave the panel for native full screen");
    m = NF_WINDOW;
    CHECK(nf_decide(1, 0, m, 1) == NF_KEEP, "dock 2: the window before native full screen comes: not touched");
    m = NF_NATIVE;
    CHECK(nf_decide(1, 0, m, 0) == NF_KEEP, "dock 3: native full screen on the Studio Display: settled");

    // The guard: 4 switches within 30 s, the 5th refused until the first is 30 s old
    nf_guard g = {0};
    CHECK(nf_guard_allow(&g, 0) && nf_guard_allow(&g, 1) && nf_guard_allow(&g, 2) && nf_guard_allow(&g, 3), "4 switches in 3 s: allowed");
    CHECK(!nf_guard_allow(&g, 4), "the 5th in 4 s: refused");
    CHECK(!nf_guard_allow(&g, 29.9), "29.9 s after the first: still refused");
    CHECK(nf_guard_allow(&g, 30), "30 s after the first: allowed");
    CHECK(!nf_guard_allow(&g, 30.5), "then the window holds 1, 2, 3, 30: refused");
    CHECK(nf_guard_allow(&g, 31), "31 s: the switch at 1 s is 30 s old");
    nf_guard h = {0}; int ok = 1;
    for (int i = 0; i < 100; i++) ok &= nf_guard_allow(&h, i * 10.0);
    CHECK(ok, "one switch every 10 s: always allowed (3 within any 30 s)");

    printf("t_notch_follow: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
