// Button triples captured in nxnative-joycon-shoulder-input-test.log.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../joycon_rails.h"
int main(void) {
    const uint8_t captured[] = {0, 0x20, 0, 0x10, 0, 0x20, 0x30, 0x20, 0,
                                0x10, 0x30, 0x20, 0x30, 0x10, 0, 0x40, 0, 0x80, 0};
    uint8_t r[49] = {0x30}, bits;
    for (int side = 0; side < 2; side++) {
        uint32_t product = side == 0 ? 0x2006 : 0x2007;
        for (size_t i = 0; i < sizeof captured; i++) {
            r[3] = r[5] = 0; r[side == 0 ? 5 : 3] = captured[i];
            assert(dm_joycon_rails_decode(product, 0x30, r, sizeof r, &bits));
            assert(bits == (captured[i] & 0x30));
            assert(dm_joycon_rails_decode(side == 0 ? 0x2007 : 0x2006, 0x30, r, sizeof r, &bits));
            assert(bits == 0); // opposite side never supplies rail buttons
        }
        for (size_t len = 0; len < sizeof r; len++) {
            bits = 0x30;
            assert(!dm_joycon_rails_decode(product, 0x30, r, len, &bits) && bits == 0);
        }
    }
    assert(!dm_joycon_rails_decode(0x2009, 0x30, r, 49, &bits));
    assert(!dm_joycon_rails_decode(0x2006, 0x21, r, 49, &bits));
    assert(!dm_joycon_rails_decode(0x2006, 0x30, NULL, 49, &bits));
    assert(!dm_joycon_rails_decode(0x2006, 0x30, r, 49, NULL));
    r[0] = 0x21;
    assert(!dm_joycon_rails_decode(0x2006, 0x30, r, 49, &bits));
    puts("t_joycon_rails: passed");
}
