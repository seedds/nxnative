// DM_PAD_BACKGROUND (pad_focus.h): which focus losses keep the controllers with the game
#include "../pad_focus.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int main(void) {
    CHECK(!pad_focus_keep(0, 0, 0), "switch off: another app in front releases the controllers");
    CHECK(pad_focus_keep(1, 0, 0), "switch on: another app in front keeps them");
    CHECK(!pad_focus_keep(1, 1, 0), "switch on: a minimized window still releases them");
    CHECK(!pad_focus_keep(1, 0, 1), "switch on: the software keyboard's input box still releases them");
    printf("pad focus: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
