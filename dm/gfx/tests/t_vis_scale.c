// DM_NVN_VIS_SCALE (nvn_vis_scale.h): samples-passed totals scaled, a visible query never read as occluded
#include <stdio.h>
#include "../nvn/nvn_vis_scale.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    CHECK(nvn_vis_scaled(1000, 1) == 1000, "off: the counted total");
    CHECK(nvn_vis_scaled(1000, 0.25) == 250, "scaled");
    CHECK(nvn_vis_scaled(3, 0.25) == 1, "a nonzero total stays visible");
    CHECK(nvn_vis_scaled(0, 0.25) == 0, "zero stays zero");
    CHECK(nvn_vis_scaled(1ull << 40, 0.5) == 1ull << 39, "large totals");
    CHECK(nvn_vis_scale_env(NULL) == 1 && nvn_vis_scale_env("0.2") == 0.2 && nvn_vis_scale_env("1.5") == 1 && nvn_vis_scale_env("0") == 1 && nvn_vis_scale_env("x") == 1,
          "the switch takes values in (0, 1), anything else is off");
    printf("vis scale: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
