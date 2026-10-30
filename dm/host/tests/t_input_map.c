// The Mac front end's button assignments (input_map.h): the default key map, DM_KEYMAP moving and swapping functions, DM_PAD_MAP's swaps, and malformed entries left out
#include "../input_map.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int main(void) {
    key_map k; key_map_default(&k);
    CHECK(key_map_fn(&k, 38) == 0 && key_map_fn(&k, 40) == 1 && key_map_fn(&k, 32) == 2 && key_map_fn(&k, 34) == 3, "default: J K U I are A B X Y");
    CHECK(key_map_fn(&k, 13) == IM_LUP && key_map_fn(&k, 0) == IM_LLEFT && key_map_fn(&k, 1) == IM_LDOWN && key_map_fn(&k, 2) == IM_LRIGHT, "default: W A S D are the left stick");
    CHECK(key_map_fn(&k, 49) == IM_SHAKE && key_map_fn(&k, 36) == 10 && key_map_fn(&k, 125) == 15, "default: Space shakes, Return is +, down arrow is Down");
    int right = 0; for (int c = 0; c < IM_KEYS; c++) if (k.fn[c] >= IM_RUP && k.fn[c] <= IM_RRIGHT) right++;
    CHECK(right == 0, "default: the right stick is on no key");
    CHECK(key_map_fn(&k, 200) == -1 && key_map_fn(&k, 50) == -1, "a key code past the table or without a function holds none");

    CHECK(key_map_parse(&k, "A=40,B=38") == 0, "a swap parses");
    CHECK(key_map_fn(&k, 40) == 0 && key_map_fn(&k, 38) == 1, "A=40,B=38 swaps A and B");
    key_map_default(&k);
    CHECK(key_map_parse(&k, "a=49") == 0 && key_map_fn(&k, 49) == 0 && key_map_fn(&k, 38) == -1, "names are case-insensitive; A leaves J, and Space leaves Shake");
    int shake = 0; for (int c = 0; c < IM_KEYS; c++) if (k.fn[c] == IM_SHAKE) shake++;
    CHECK(shake == 0, "the function a key held before is on no key");
    CHECK(key_map_parse(&k, "RUp=126,RLeft=123,Up=none") == 0, "right stick on the arrows, Up on none");
    CHECK(key_map_fn(&k, 126) == IM_RUP && key_map_fn(&k, 123) == IM_RLEFT, "the arrows hold the right stick");
    int up = 0; for (int c = 0; c < IM_KEYS; c++) if (k.fn[c] == 13) up++;
    CHECK(up == 0, "Up=none leaves Up on no key");
    key_map_default(&k);
    CHECK(key_map_parse(&k, "Q=1,ZL=abc,A=128,B=,=5,X=7") == 5, "five malformed entries are counted");
    CHECK(key_map_fn(&k, 7) == 2 && key_map_fn(&k, 6) == 8 && key_map_fn(&k, 38) == 0, "the good entry applies, the malformed ones change nothing");
    CHECK(key_map_parse(&k, "") == 0 && key_map_parse(&k, NULL) == 0 && key_map_parse(&k, "L=12,") == 0, "empty lists and a trailing comma are fine");

    pad_map m; pad_map_default(&m);
    CHECK(pad_map_is_default(&m) && pad_map_apply(&m, DM_BTN_A | DM_BTN_ZR | DM_BTN_SHAKE) == (DM_BTN_A | DM_BTN_ZR | DM_BTN_SHAKE), "default: every button sends itself, Shake passes");
    CHECK(pad_map_parse(&m, "A=B,B=A") == 0 && !pad_map_is_default(&m), "a swap parses");
    CHECK(pad_map_apply(&m, DM_BTN_A) == DM_BTN_B && pad_map_apply(&m, DM_BTN_B) == DM_BTN_A && pad_map_apply(&m, DM_BTN_A | DM_BTN_B) == (DM_BTN_A | DM_BTN_B), "A=B,B=A swaps them");
    CHECK(pad_map_apply(&m, DM_BTN_X | DM_BTN_DOWN) == (DM_BTN_X | DM_BTN_DOWN), "buttons not named stay");
    CHECK(pad_map_parse(&m, "ZL=L,minus=none") == 0, "two buttons on one, and none");
    CHECK(pad_map_apply(&m, DM_BTN_ZL) == DM_BTN_L && pad_map_apply(&m, DM_BTN_L | DM_BTN_ZL) == DM_BTN_L && pad_map_apply(&m, DM_BTN_MINUS) == 0, "ZL sends L, Minus sends nothing");
    pad_map_default(&m);
    CHECK(pad_map_parse(&m, "A=LUp,LUp=A,Shake=B,Y=X") == 3 && m.to[3] == 2 && m.to[0] == 0, "sticks and Shake are no buttons here: left out; Y=X applies");

    printf("input map: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
