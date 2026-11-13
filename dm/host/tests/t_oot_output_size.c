#include "../../mac/oot_output_size.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const uint32_t cases[][4] = {
        {3024, 1964, 3024, 1701}, {2560, 1664, 2560, 1440},
        {3840, 2160, 3840, 2160}, {3440, 1440, 2560, 1440},
        {1080, 1920, 1080, 607}, {0, 1964, 0, 0},
        {3024, 0, 0, 0}, {16385, 9000, 0, 0}, {1, 1, 0, 0}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        oot_output_size s = oot_output_fit(cases[i][0], cases[i][1]);
        assert(s.width == cases[i][2] && s.height == cases[i][3]);
    }
    puts("OOT output: Retina, ultrawide, portrait and invalid dimensions pass");
}
