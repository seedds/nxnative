// The Mac keyboard map (pad_kbd.h): the default layout, DM_KBD_MAP overrides, alternates, aliases, and a key moving between actions
#include <stdio.h>
#include "../pad_kbd.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    pad_kbd k;
    pad_kbd_init(&k, NULL);
    // the default map, as front_mac.m documented it before DM_KBD_MAP
    CHECK(pad_kbd_action(&k, PK_KEY_J) == PK_A);
    CHECK(pad_kbd_action(&k, PK_KEY_K) == PK_B);
    CHECK(pad_kbd_action(&k, PK_KEY_U) == PK_X);
    CHECK(pad_kbd_action(&k, PK_KEY_I) == PK_Y);
    CHECK(pad_kbd_action(&k, PK_KEY_F) == PK_LS);
    CHECK(pad_kbd_action(&k, PK_KEY_G) == PK_RS);
    CHECK(pad_kbd_action(&k, PK_KEY_Q) == PK_L);
    CHECK(pad_kbd_action(&k, PK_KEY_E) == PK_R);
    CHECK(pad_kbd_action(&k, PK_KEY_Z) == PK_ZL);
    CHECK(pad_kbd_action(&k, PK_KEY_C) == PK_ZR);
    CHECK(pad_kbd_action(&k, PK_KEY_RETURN) == PK_PLUS);
    CHECK(pad_kbd_action(&k, PK_KEY_TAB) == PK_MINUS);
    CHECK(pad_kbd_action(&k, PK_KEY_UP) == PK_UP && pad_kbd_action(&k, PK_KEY_DOWN) == PK_DOWN);
    CHECK(pad_kbd_action(&k, PK_KEY_LEFT) == PK_LEFT && pad_kbd_action(&k, PK_KEY_RIGHT) == PK_RIGHT);
    CHECK(pad_kbd_action(&k, PK_KEY_W) == PK_LUP && pad_kbd_action(&k, PK_KEY_S) == PK_LDOWN);
    CHECK(pad_kbd_action(&k, PK_KEY_A) == PK_LLEFT && pad_kbd_action(&k, PK_KEY_D) == PK_LRIGHT);
    CHECK(pad_kbd_action(&k, PK_KEY_SPACE) == PK_SHAKE);
    CHECK(pad_kbd_action(&k, PK_KEY_O) == -1 && pad_kbd_action(&k, PK_KEY_H) == -1);
    CHECK(pad_kbd_action(&k, 200) == -1);

    // Shift + an arrow is the right stick, each arrow to its own direction (the D-pad's and the right stick's actions are in different orders)
    CHECK(pad_kbd_as_right_stick(PK_LEFT) == PK_RLEFT && pad_kbd_as_right_stick(PK_UP) == PK_RUP);
    CHECK(pad_kbd_as_right_stick(PK_RIGHT) == PK_RRIGHT && pad_kbd_as_right_stick(PK_DOWN) == PK_RDOWN);
    CHECK(pad_kbd_as_right_stick(PK_A) == PK_A && pad_kbd_as_right_stick(PK_RUP) == PK_RUP);
    CHECK(pad_kbd_as_right_stick(PK_SHAKE) == PK_SHAKE);

    // the face buttons and the right stick moved onto other keys, as a player's own layout does
    pad_kbd_init(&k, "A=C,B=X,X=V,Y=Z,RSTICK_UP=I,RSTICK_DOWN=K,RSTICK_LEFT=J,RSTICK_RIGHT=L,RS=H");
    CHECK(pad_kbd_action(&k, PK_KEY_C) == PK_A && pad_kbd_action(&k, PK_KEY_X) == PK_B);
    CHECK(pad_kbd_action(&k, PK_KEY_V) == PK_X && pad_kbd_action(&k, PK_KEY_Z) == PK_Y);
    CHECK(pad_kbd_action(&k, PK_KEY_I) == PK_RUP && pad_kbd_action(&k, PK_KEY_K) == PK_RDOWN);
    CHECK(pad_kbd_action(&k, PK_KEY_J) == PK_RLEFT && pad_kbd_action(&k, PK_KEY_L) == PK_RRIGHT);
    CHECK(pad_kbd_action(&k, PK_KEY_H) == PK_RS);
    // the keys an action gave up are free, and one a later entry names moves, as pad_hid.h's map does
    CHECK(pad_kbd_action(&k, PK_KEY_U) == -1 && pad_kbd_action(&k, PK_KEY_G) == -1);
    CHECK(pad_kbd_action(&k, PK_KEY_F) == PK_LS);
    // one key, one action: ZR keeps C until A names it, and then A has it
    pad_kbd_init(&k, "A=C"); CHECK(pad_kbd_action(&k, PK_KEY_C) == PK_A);
    pad_kbd_init(&k, "A=C,ZR=C"); CHECK(pad_kbd_action(&k, PK_KEY_C) == PK_ZR);
    pad_kbd_init(&k, "ZR=C,A=C"); CHECK(pad_kbd_action(&k, PK_KEY_C) == PK_A);

    // alternates (the keypad's + beside =), the empty value, aliases, unknown names, and a symbol's own name
    pad_kbd_init(&k, "PLUS=EQUAL|KEYPAD_PLUS,MINUS=-|KEYPAD_MINUS,SHAKE=");
    CHECK(pad_kbd_action(&k, PK_KEY_EQUAL) == PK_PLUS && pad_kbd_action(&k, PK_KEY_KEYPAD_PLUS) == PK_PLUS);
    CHECK(pad_kbd_action(&k, PK_KEY_MINUS) == PK_MINUS && pad_kbd_action(&k, PK_KEY_KEYPAD_MINUS) == PK_MINUS);
    CHECK(pad_kbd_action(&k, PK_KEY_RETURN) == -1 && pad_kbd_action(&k, PK_KEY_TAB) == -1);
    CHECK(pad_kbd_action(&k, PK_KEY_SPACE) == -1);                          // SHAKE= gave the key up
    pad_kbd_init(&k, "LS_UP=I,ls_left=j, BOGUS=1, A=NOSUCHKEY, ZZ=ZR");
    CHECK(pad_kbd_action(&k, PK_KEY_I) == PK_LUP && pad_kbd_action(&k, PK_KEY_J) == PK_LLEFT);   // LS_UP, ls_left: case does not matter
    CHECK(pad_kbd_action(&k, PK_KEY_K) == PK_B);                             // the unparsed entries left the rest alone
    CHECK(pad_kbd_action(&k, PK_KEY_J) != PK_A);                             // A=NOSUCHKEY cleared A, and J is the left stick's now
    CHECK(pad_kbd_action(&k, PK_KEY_Z) == PK_ZL);                            // ZZ is not an action: ZR kept its key
    // five keys for one action: PK_MAX_KEYS keeps the first four
    pad_kbd_init(&k, "A=J|K|L|U|I");
    CHECK(pad_kbd_action(&k, PK_KEY_J) == PK_A && pad_kbd_action(&k, PK_KEY_I) == -1);
    // a map that is empty or NULL is the default
    pad_kbd_init(&k, ""); CHECK(pad_kbd_action(&k, PK_KEY_J) == PK_A);
    pad_kbd_init(&k, NULL); CHECK(pad_kbd_action(&k, PK_KEY_RETURN) == PK_PLUS);

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
