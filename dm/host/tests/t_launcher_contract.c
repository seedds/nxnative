/* The write half is ControlLayout.environment in frontend/Sources/NSPortBuilder/Controls.swift;
   the read half is input_map.h. The launcher sends exactly these strings, so this checks the
   contract between the two: a rename on either side, or a change of syntax, fails here rather
   than at a player's keyboard. */
#include "../input_map.h"
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
int main(void) {
    key_map k; key_map_default(&k);
    /* Settings: A moved onto K (40), B left on none — ControlsTests' first case */
    CHECK(key_map_parse(&k, "A=40,B=none") == 0, "A=40,B=none parses");
    CHECK(key_map_fn(&k, 40) == 0, "K now sends A");
    int bfound = 0; for (int c = 0; c < IM_KEYS; c++) if (k.fn[c] == 1) bfound = 1;
    CHECK(!bfound, "B is on no key");

    pad_map m; pad_map_default(&m);
    /* Settings: A and B swapped, Minus sends nothing */
    CHECK(pad_map_parse(&m, "A=B,B=A,Minus=none") == 0, "A=B,B=A,Minus=none parses");
    CHECK(pad_map_apply(&m, DM_BTN_A) == DM_BTN_B, "pad A sends B");
    CHECK(pad_map_apply(&m, DM_BTN_B) == DM_BTN_A, "pad B sends A");
    CHECK(pad_map_apply(&m, DM_BTN_MINUS) == 0, "Minus sends nothing");
    CHECK(pad_map_apply(&m, DM_BTN_Y) == DM_BTN_Y, "untouched buttons stay");

    /* the right stick on I, as the Controls sheet's own capture sets it */
    key_map_default(&k);
    CHECK(key_map_parse(&k, "RUp=34") == 0, "RUp=34 parses");
    CHECK(key_map_fn(&k, 34) == IM_RUP, "I now sends RUp (and Y left I)");
    int yfound = 0; for (int c = 0; c < IM_KEYS; c++) if (k.fn[c] == 3) yfound = 1;
    CHECK(!yfound, "Y is on no key any more");
    int rup = 0; for (int c = 0; c < IM_KEYS; c++) if (k.fn[c] == IM_RUP) rup = 1;
    CHECK(rup, "RUp is on a key");

    printf("launcher contract: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
