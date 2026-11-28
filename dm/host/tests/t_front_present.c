// Check the Mac presentation policies (../front_mac_present.h): the layer's opacity (GitHub issue #1821) and the screen's mode in fullscreen.
#include "../front_mac_present.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int main(void) {
    // Environment variable parsing
    CHECK(front_mac_composited_mode(NULL) == FRONT_MAC_COMP_AUTO, "NULL -> auto");
    CHECK(front_mac_composited_mode("") == FRONT_MAC_COMP_AUTO, "empty -> auto");
    CHECK(front_mac_composited_mode("auto") == FRONT_MAC_COMP_AUTO, "auto -> auto");
    CHECK(front_mac_composited_mode("0") == FRONT_MAC_COMP_OFF, "0 -> forced opaque");
    CHECK(front_mac_composited_mode("1") == FRONT_MAC_COMP_ON, "1 -> forced non-opaque");
    CHECK(front_mac_composited_mode("2") == FRONT_MAC_COMP_ON, "nonzero -> forced non-opaque");
    CHECK(front_mac_composited_mode("bogus") == FRONT_MAC_COMP_AUTO, "invalid -> auto");

    // Auto mode: non-opaque only in native fullscreen on a mirrored display
    CHECK(front_mac_layer_opaque(FRONT_MAC_COMP_AUTO, 0, 0) == 1, "auto window unmirrored -> opaque");
    CHECK(front_mac_layer_opaque(FRONT_MAC_COMP_AUTO, 0, 1) == 1, "auto window mirrored -> opaque");
    CHECK(front_mac_layer_opaque(FRONT_MAC_COMP_AUTO, 1, 0) == 1, "auto fullscreen unmirrored -> opaque");
    CHECK(front_mac_layer_opaque(FRONT_MAC_COMP_AUTO, 1, 1) == 0, "auto fullscreen mirrored -> non-opaque");

    // Forced overrides apply in every display and window state
    for (int fs = 0; fs <= 1; fs++) {
        for (int mir = 0; mir <= 1; mir++) {
            CHECK(front_mac_layer_opaque(FRONT_MAC_COMP_ON, fs, mir) == 0, "DM_MAC_COMPOSITED=1 -> always non-opaque");
            CHECK(front_mac_layer_opaque(FRONT_MAC_COMP_OFF, fs, mir) == 1, "DM_MAC_COMPOSITED=0 -> always opaque");
        }
    }

    // Reason strings
    CHECK(!strcmp(front_mac_layer_opaque_why(FRONT_MAC_COMP_ON, 0, 0), "DM_MAC_COMPOSITED=1"), "why ON");
    CHECK(!strcmp(front_mac_layer_opaque_why(FRONT_MAC_COMP_OFF, 1, 1), "DM_MAC_COMPOSITED=0"), "why OFF");
    CHECK(!strcmp(front_mac_layer_opaque_why(FRONT_MAC_COMP_AUTO, 0, 1), "window"), "why window");
    CHECK(!strcmp(front_mac_layer_opaque_why(FRONT_MAC_COMP_AUTO, 1, 0), "fullscreen on an unmirrored display"), "why fullscreen unmirrored");
    CHECK(!strcmp(front_mac_layer_opaque_why(FRONT_MAC_COMP_AUTO, 1, 1), "fullscreen on a mirrored display"), "why fullscreen mirrored");

    // DM_SCREEN_MATCH parsing
    CHECK(front_mac_screen_match_mode(NULL) == FRONT_MAC_MATCH_AUTO, "unset -> auto");
    CHECK(front_mac_screen_match_mode("0") == FRONT_MAC_MATCH_OFF, "0 -> keeps every mode");
    CHECK(front_mac_screen_match_mode("1") == FRONT_MAC_MATCH_FIXED, "1 -> fixed-rate mode for variable-refresh screens too");
    CHECK(front_mac_screen_match_mode("") == FRONT_MAC_MATCH_AUTO, "empty -> auto");
    CHECK(front_mac_screen_match_mode("2") == FRONT_MAC_MATCH_AUTO, "other -> auto");

    // A variable-refresh mode is kept only by default, for 30/60 Hz requests, where the 120 Hz link paces it and no fixed >=120 Hz candidate exists
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 60, 60, 1) == 1, "auto, VRR, 60 Hz req, fixed 60 candidate, 120 Hz link -> kept");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 30, 60, 1) == 1, "auto, VRR, 30 Hz req, fixed 60 candidate, 120 Hz link -> kept");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 60, 0, 1) == 1, "auto, VRR, 60 Hz req, no fixed candidate, 120 Hz link -> kept");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 60, 120, 1) == 0, "240 Hz VRR with fixed 120 Hz candidate -> switches to fixed 120 Hz");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 30, 120, 1) == 0, "240 Hz VRR at 30 Hz req with fixed 120 Hz candidate -> switches to fixed 120 Hz");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 120, 60, 1) == 0, "120 Hz req -> handled by fixed/VRR mode search");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_FIXED, 1, 60, 60, 1) == 0, "DM_SCREEN_MATCH=1 -> switches to fixed-rate mode");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_FIXED, 1, 30, 60, 1) == 0, "DM_SCREEN_MATCH=1 at 30 Hz req -> switches to fixed-rate mode");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 0, 60, 60, 1) == 0, "fixed-rate mode -> not kept as VRR");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 60, 60, 0) == 0, "no 120 Hz link (144 Hz or DM_PRESENT_LINK set) -> switches to fixed-rate mode");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 0, 60, 1) == 0, "zero request -> not kept");

    printf("front present policy: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
