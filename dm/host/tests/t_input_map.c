// The Mac front end's button assignments (input_map.h): the default key map, DM_KEYMAP moving and swapping functions, DM_PAD_MAP's swaps, malformed entries left out, DM_PAD_ABXY, and the stick shape (DM_PAD_STICK_DZ, DM_PAD_STICK_SENS)
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

    // input_map_valid: what the launcher checks a saved DM_KEYMAP / DM_PAD_MAP with; the whole value must be one the host takes
    CHECK(input_map_valid("A=40,B=38", 0) && input_map_valid("RUp=126,Up=none,Shake=49", 0) && input_map_valid("a=49,", 0), "keymap: a swap, the right stick, none, a trailing comma are valid");
    CHECK(input_map_valid("A=B,B=A", 1) && input_map_valid("ZL=L,minus=none", 1), "pad map: a swap and none are valid");
    CHECK(!input_map_valid("Q=1", 0) && !input_map_valid("A=40,Q=1", 0), "keymap: an unknown name is invalid, also next to a good entry");
    CHECK(!input_map_valid("A=128", 0) && !input_map_valid("A=abc", 0) && !input_map_valid("A=-1", 0) && !input_map_valid("A=", 0) && !input_map_valid("A", 0), "keymap: a key code past 127, text, a sign or no value is invalid");
    CHECK(!input_map_valid("A=LUp", 1) && !input_map_valid("Shake=B", 1) && !input_map_valid("A=Q", 1) && !input_map_valid("A=40", 1), "pad map: sticks, Shake, unknown names and key codes are invalid");
    CHECK(!input_map_valid("A=B", 0) && !input_map_valid("A=40", 1) && !input_map_valid("A=40,B=38", 1), "a value of one map is not valid for the other");
    CHECK(!input_map_valid("", 0) && !input_map_valid("", 1) && !input_map_valid(NULL, 0) && !input_map_valid(NULL, 1), "empty and missing values are invalid (nothing to apply)");
    CHECK(!input_map_valid(",", 0), "a lone comma holds an entry with no name: invalid");

    // DM_PAD_ABXY: how the face buttons are read
    CHECK(pad_abxy_parse(NULL) == IM_ABXY_AUTO && pad_abxy_parse("") == IM_ABXY_AUTO && pad_abxy_parse("Auto") == IM_ABXY_AUTO, "unset, empty and auto are auto");
    CHECK(pad_abxy_parse("label") == IM_ABXY_LABEL && pad_abxy_parse("POSITION") == IM_ABXY_POSITION, "label and position, any case");
    CHECK(pad_abxy_parse("labels") == -1 && pad_abxy_parse("1") == -1, "anything else is not taken");
    CHECK(pad_abxy_valid("label") && pad_abxy_valid("auto") && !pad_abxy_valid("") && !pad_abxy_valid(NULL) && !pad_abxy_valid("xbox"), "launcher check: a named mode, not empty");
    CHECK(pad_abxy_swap(DM_BTN_A) == DM_BTN_B && pad_abxy_swap(DM_BTN_B) == DM_BTN_A && pad_abxy_swap(DM_BTN_X) == DM_BTN_Y && pad_abxy_swap(DM_BTN_Y) == DM_BTN_X, "the swap trades A with B and X with Y");
    CHECK(pad_abxy_swap(DM_BTN_A | DM_BTN_B | DM_BTN_ZR) == (DM_BTN_A | DM_BTN_B | DM_BTN_ZR) && pad_abxy_swap(DM_BTN_SHAKE | DM_BTN_X) == (DM_BTN_SHAKE | DM_BTN_Y), "both held stay held; other bits pass");
    // Stick shape: DM_PAD_STICK_DZ and DM_PAD_STICK_SENS
    stick_shape s; stick_shape_default(&s);
    float x = 0.03f, y = 1.2f; stick_shape_apply(&s, &x, &y);
    CHECK(x == 0.03f && y == 1.2f, "default: the axes pass as they are");
    CHECK(stick_shape_parse(&s, "0.1", "2") == 0 && s.dz == 0.1f && s.sens == 2.0f, "a dead zone and a sensitivity parse");
    x = 0.05f; y = 0.05f; stick_shape_apply(&s, &x, &y);
    CHECK(x == 0 && y == 0, "inside the dead zone: at rest");
    x = 0; y = 0.4f; stick_shape_apply(&s, &x, &y);
    CHECK(x == 0 && fabsf(y - (0.3f / 0.9f * 2)) < 1e-5f, "past the dead zone: rescaled from 0, times the sensitivity");
    x = 0.8f; y = 0.8f; stick_shape_apply(&s, &x, &y);
    CHECK(fabsf(sqrtf(x * x + y * y) - 1) < 1e-5f && fabsf(x - y) < 1e-6f, "a long diagonal is clamped to the unit circle, its direction kept");
    stick_shape_default(&s);
    CHECK(stick_shape_parse(&s, NULL, "0.5") == 0 && s.dz == 0 && s.sens == 0.5f, "sensitivity alone");
    x = 1; y = 0; stick_shape_apply(&s, &x, &y);
    CHECK(fabsf(x - 0.5f) < 1e-6f, "below 1, full tilt reaches only that much");
    stick_shape_default(&s);
    CHECK(stick_shape_parse(&s, "0.6", "4") == 2 && stick_shape_is_default(&s), "out of range values are left out");
    CHECK(stick_shape_parse(&s, "-0.1", "0x2") == 2 && stick_shape_parse(&s, " 0.1", "1a") == 2 && stick_shape_is_default(&s), "a sign, hex, a space, text after the number are left out");
    CHECK(stick_dz_valid("0") && stick_dz_valid("0.50") && !stick_dz_valid("0.51") && !stick_dz_valid(""), "dead zone check: 0 to 0.5");
    CHECK(stick_sens_valid("0.25") && stick_sens_valid("3") && !stick_sens_valid("0.2") && !stick_sens_valid(NULL), "sensitivity check: 0.25 to 3");

    printf("input map: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
