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

    // Capture the function on key-down: modifier changes, repeat events and other keys cannot steal its release.
    key_map_default(&k); key_state state = {0};
    CHECK(key_state_event(&state, &k, 126, 1, 1) == (1u << IM_RUP), "Shift+Up drives the right stick, not the D-pad");
    CHECK(key_state_event(&state, &k, 126, 1, 0) == (1u << IM_RUP), "repeat after releasing Shift keeps the initial function");
    CHECK(key_state_event(&state, &k, 126, 0, 0) == 0, "releasing the arrow without Shift releases the right stick");
    CHECK(key_state_event(&state, &k, 123, 1, 0) == (1u << 12), "unshifted Left remains D-pad");
    CHECK(key_state_event(&state, &k, 123, 1, 1) == (1u << 12), "pressing Shift during a held arrow does not switch functions");
    CHECK(key_state_event(&state, &k, 123, 0, 1) == 0, "D-pad releases even when Shift was subsequently pressed");
    CHECK(key_state_event(&state, &k, 124, 1, 1) == (1u << IM_RRIGHT), "Shift+Right drives right stick right");
    CHECK(key_state_event(&state, &k, 125, 1, 1) == ((1u << IM_RRIGHT) | (1u << IM_RDOWN)), "Shift arrows combine diagonally");
    key_state_clear(&state);
    CHECK(key_state_event(&state, &k, 124, 0, 0) == 0, "focus/modal clear releases every captured function");
    CHECK(key_map_parse(&k, "A=126,RUp=15") == 0, "custom arrow and explicit right-stick binding parse");
    CHECK(key_state_event(&state, &k, 126, 1, 1) == 1, "a remapped arrow respects its custom function with Shift");
    key_state_clear(&state); key_map_default(&k); key_map_parse(&k, "RUp=15");
    CHECK(key_state_event(&state, &k, 126, 1, 1) == (1u << IM_RUP), "default shifted arrow remains available with another right-stick binding");
    CHECK(key_state_event(&state, &k, 15, 1, 0) == (1u << IM_RUP), "two physical keys may hold the same function");
    CHECK(key_state_event(&state, &k, 126, 0, 0) == (1u << IM_RUP), "releasing one key preserves the other held right-stick binding");
    CHECK(key_state_event(&state, &k, 15, 0, 0) == 0, "last physical release clears the function");
    CHECK(key_state_event(&state, &k, 200, 1, 1) == 0, "out-of-range key cannot change input");
    key_map_parse(&k, "Up=none");
    CHECK(key_state_event(&state, &k, 126, 1, 1) == 0, "an explicitly disabled arrow is not re-enabled by Shift");

    pad_map m; pad_map_default(&m);
    CHECK(pad_map_is_default(&m) && pad_map_apply(&m, DM_BTN_A | DM_BTN_ZR | DM_BTN_SHAKE) == (DM_BTN_A | DM_BTN_ZR | DM_BTN_SHAKE), "default: every button sends itself, Shake passes");
    CHECK(pad_map_parse(&m, "A=B,B=A") == 0 && !pad_map_is_default(&m), "a swap parses");
    CHECK(pad_map_apply(&m, DM_BTN_A) == DM_BTN_B && pad_map_apply(&m, DM_BTN_B) == DM_BTN_A && pad_map_apply(&m, DM_BTN_A | DM_BTN_B) == (DM_BTN_A | DM_BTN_B), "A=B,B=A swaps them");
    CHECK(pad_map_apply(&m, DM_BTN_X | DM_BTN_DOWN) == (DM_BTN_X | DM_BTN_DOWN), "buttons not named stay");
    CHECK(pad_map_parse(&m, "ZL=L,minus=none") == 0, "two buttons on one, and none");
    CHECK(pad_map_apply(&m, DM_BTN_ZL) == DM_BTN_L && pad_map_apply(&m, DM_BTN_L | DM_BTN_ZL) == DM_BTN_L && pad_map_apply(&m, DM_BTN_MINUS) == 0, "ZL sends L, Minus sends nothing");
    pad_map_default(&m);
    CHECK(pad_map_parse(&m, "A=LUp,LUp=A,Shake=B,Y=X") == 3 && m.to[3] == 2 && m.to[0] == 0, "sticks and Shake are no buttons here: left out; Y=X applies");

    CHECK(input_map_valid("A=Shake", 1) && pad_map_parse(&m,"A=Shake")==0 && pad_map_apply(&m,DM_BTN_A)==DM_BTN_SHAKE, "Shake is an additive target, keeping per-player Shake mappings in the shared store");

    // input_map_valid: what the launcher checks a saved DM_KEYMAP / DM_PAD_MAP with; the whole value must be one the host takes
    CHECK(input_map_valid("A=40,B=38", 0) && input_map_valid("RUp=126,Up=none,Shake=49", 0) && input_map_valid("a=49,", 0), "keymap: a swap, the right stick, none, a trailing comma are valid");
    CHECK(input_map_valid("A=B,B=A", 1) && input_map_valid("ZL=L,minus=none", 1), "pad map: a swap and none are valid");
    CHECK(!input_map_valid("Q=1", 0) && !input_map_valid("A=40,Q=1", 0), "keymap: an unknown name is invalid, also next to a good entry");
    CHECK(!input_map_valid("A=128", 0) && !input_map_valid("A=abc", 0) && !input_map_valid("A=-1", 0) && !input_map_valid("A=", 0) && !input_map_valid("A", 0), "keymap: a key code past 127, text, a sign or no value is invalid");
    CHECK(!input_map_valid("A=LUp", 1) && !input_map_valid("Shake=B", 1) && !input_map_valid("A=Q", 1) && !input_map_valid("A=40", 1), "pad map: sticks, Shake, unknown names and key codes are invalid");
    CHECK(!input_map_valid("A=B", 0) && !input_map_valid("A=40", 1) && !input_map_valid("A=40,B=38", 1), "a value of one map is not valid for the other");
    CHECK(!input_map_valid("", 0) && !input_map_valid("", 1) && !input_map_valid(NULL, 0) && !input_map_valid(NULL, 1), "empty and missing values are invalid (nothing to apply)");
    CHECK(!input_map_valid(",", 0), "a lone comma holds an entry with no name: invalid");

    printf("input map: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
