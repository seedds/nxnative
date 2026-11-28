// Check the Mac presentation layer opacity policy (../front_mac_present.h, GitHub issue #1821).
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

    printf("front present policy: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
