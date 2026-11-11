// Uniform buffers the replay cannot give a shader (nvn_cbmiss.h): a slot the game left unbound reads the zero bank (its own state, no [RED]); an address in no memory pool, or a slot the
// replay does not keep, is a binding the host lost ([RED]). The host printed "[RED] stage 5 cb1 has no uniform buffer bound" for both, without the shader, on every PRAGMATA boot
#include "../nvn/nvn_cbmiss.h"
#include <stdio.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-78s %s\n", what, ok ? "✓" : "✗"); }

int main(void) {
    check(nvn_cb_state(1, 0, 0) == CB_UNBOUND && !nvn_cb_red(CB_UNBOUND), "cb1 at address 0: the game's own state, no [RED]");
    check(nvn_cb_state(1, 0x381f8c000ull, 0) == CB_LOST && nvn_cb_red(CB_LOST), "cb1 at an address in no pool: lost, [RED]");
    check(nvn_cb_state(1, 0x381f8c000ull, 1) == CB_BOUND && !nvn_cb_red(CB_BOUND), "cb1 at a pool address: bound");
    check(nvn_cb_state(16, 0, 0) == CB_SLOT && nvn_cb_red(CB_SLOT), "cb16 (past the replay's 16 slots): [RED]");
    check(nvn_cb_state(-1, 0x1000, 1) == CB_SLOT, "a negative slot: [RED]");
    printf("%d/%d checks passed\n", checks - fail, checks);
    return fail != 0;
}
