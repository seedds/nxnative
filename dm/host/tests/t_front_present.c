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
    CHECK(front_mac_screen_match_mode(NULL) == FRONT_MAC_MATCH_AUTO, "unset -> default");
    CHECK(front_mac_screen_match_mode("0") == FRONT_MAC_MATCH_OFF, "0 -> keeps every mode");
    CHECK(front_mac_screen_match_mode("1") == FRONT_MAC_MATCH_FIXED, "1 -> fixed-rate mode for variable-refresh screens too");
    CHECK(front_mac_screen_match_mode("") == FRONT_MAC_MATCH_AUTO, "empty -> default");
    CHECK(front_mac_screen_match_mode("2") == FRONT_MAC_MATCH_AUTO, "other -> default");

    // A variable-refresh mode is kept only by default, at a 60 fps target, where the 120 Hz link paces it
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 1, 1) == 1, "default, variable refresh, 60 fps, 120 Hz link -> kept");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_FIXED, 1, 1, 1) == 0, "DM_SCREEN_MATCH=1 -> fixed-rate mode");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 0, 1, 1) == 0, "fixed-rate mode -> the rate match decides");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 0, 1) == 0, "30 or 40 fps target -> fixed-rate mode");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 1, 0) == 0, "no 120 Hz link (144 Hz, DM_PRESENT_LINK set) -> fixed-rate mode");

    // A fixed mode the host picked goes back to the screen's own variable mode where the keep rule holds for that mode
    CHECK(front_mac_restore_vrr(FRONT_MAC_MATCH_AUTO, 0, 1, 2, 1) == 1, "default, fixed pick, own mode variable, 60 fps held, 120 Hz link -> taken back");
    CHECK(front_mac_restore_vrr(FRONT_MAC_MATCH_AUTO, 0, 1, 1, 1) == 0, "60 fps not held for the hold-off yet -> stays fixed");
    CHECK(front_mac_keep_vrr(FRONT_MAC_MATCH_AUTO, 1, 2, 1) == 1, "60 fps held -> a variable mode is still kept");
    CHECK(front_mac_restore_vrr(FRONT_MAC_MATCH_FIXED, 0, 1, 2, 1) == 0, "DM_SCREEN_MATCH=1 -> stays fixed");
    CHECK(front_mac_restore_vrr(FRONT_MAC_MATCH_AUTO, 1, 1, 2, 1) == 0, "already in a variable mode -> nothing to take back");
    CHECK(front_mac_restore_vrr(FRONT_MAC_MATCH_AUTO, 0, 0, 2, 1) == 0, "own mode fixed -> stays");
    CHECK(front_mac_restore_vrr(FRONT_MAC_MATCH_AUTO, 0, 1, 0, 1) == 0, "30 fps target -> stays fixed");
    CHECK(front_mac_restore_vrr(FRONT_MAC_MATCH_AUTO, 0, 1, 2, 0) == 0, "own rate not paced by the 120 Hz link -> stays fixed");

    printf("front present policy: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
