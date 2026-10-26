// Synthetic, asset-free CPU fixture for the coordinate math used by MSL.
// clang -O1 -Wall dm/gfx/tests/t_reduction_footprint.c -o /tmp/t_reduction_footprint
#include "../nvn/nvn_reduction_footprint.h"
#include <assert.h>
#include <stdio.h>

static float sample(const float *image, unsigned width, unsigned height, float u, float v, int maximum) {
    dm_reduction_footprint p = dm_reduction_footprint_make(u, v, width, height);
    float result = image[p.y.lo * width + p.x.lo];
    const unsigned x[4] = {p.x.lo, p.x.hi, p.x.lo, p.x.hi};
    const unsigned y[4] = {p.y.lo, p.y.lo, p.y.hi, p.y.hi};
    for (unsigned i = 1; i < 4; i++) if (p.mask & (1u << i)) {
        float tap = image[y[i] * width + x[i]];
        result = maximum ? fmaxf(result, tap) : fminf(result, tap);
    }
    return result;
}
static void expect(float actual, float expected) { assert(fabsf(actual - expected) < 1e-6f); }

int main(void) {
    const float image[] = {
        .8f, .2f, .9f, .7f,
        .6f, .4f, .3f, .1f,
        .75f, .65f, .55f, .45f,
        .35f, .25f, .15f, .05f
    };
    dm_reduction_footprint p = dm_reduction_footprint_make(.125f, .125f, 4, 4);
    assert(p.mask == 1 && p.x.lo == 0 && p.y.lo == 0);
    // The unused right/bottom neighbors are smaller: unconditional four-tap MIN
    // would fail this test even though all coordinates were clamped correctly.
    expect(sample(image, 4, 4, .125f, .125f, 0), .8f);
    expect(sample(image, 4, 4, .125f, .125f, 1), .8f);
    p = dm_reduction_footprint_make(.25f, .125f, 4, 4); assert(p.mask == 3);
    expect(sample(image, 4, 4, .25f, .125f, 0), .2f);
    p = dm_reduction_footprint_make(.125f, .25f, 4, 4); assert(p.mask == 5);
    expect(sample(image, 4, 4, .125f, .25f, 0), .6f);
    p = dm_reduction_footprint_make(.25f, .25f, 4, 4); assert(p.mask == 15);
    expect(sample(image, 4, 4, .25f, .25f, 0), .2f);
    expect(sample(image, 4, 4, .25f, .25f, 1), .8f);
    puts("footprint: texel center/axis zero weights and four-tap MIN/MAX: PASS");

    const float edge_u[] = {0, 1, 0, 1, .5f}, edge_v[] = {0, 0, 1, 1, .5f};
    const float edge_min[] = {.8f, .7f, .35f, .05f, .3f};
    const float edge_max[] = {.8f, .7f, .35f, .05f, .65f};
    for (unsigned i = 0; i < 5; i++) {
        expect(sample(image, 4, 4, edge_u[i], edge_v[i], 0), edge_min[i]);
        expect(sample(image, 4, 4, edge_u[i], edge_v[i], 1), edge_max[i]);
    }
    expect(sample(image, 4, 4, -100, 100, 0), .35f);
    expect(sample(image, 4, 4, INFINITY, -INFINITY, 0), .7f);
    expect(sample(image, 4, 4, NAN, NAN, 0), .8f);
    puts("address: five corner/center probes, outside/Inf clamps and NaN sanitize: PASS");

    const float singleton[] = {.43f};
    expect(sample(singleton, 1, 1, 0, 1, 0), .43f);
    expect(sample(singleton, 1, 1, .5f, .5f, 1), .43f);
    const float odd[] = {.1f, .8f, .4f, .7f, .9f, .3f};
    expect(sample(odd, 3, 2, .5f, .25f, 0), .8f);
    expect(sample(odd, 3, 2, .5f, .5f, 0), .8f);
    expect(sample(odd, 3, 2, .5f, .5f, 1), .9f);
    puts("extents: single texel and non-power-of-two/unequal dimensions: PASS");

    for (unsigned i = 0; i < 11; i++) assert(dm_reduction_mip_nearest((float)i, 0, 1000, 11) == i);
    assert(dm_reduction_mip_nearest(-10, 0, 1000, 11) == 0);
    assert(dm_reduction_mip_nearest(1000, 0, 1000, 11) == 10);
    assert(dm_reduction_mip_nearest(9, 2, 4, 11) == 4);
    assert(dm_reduction_mip_nearest(0, 2, 4, 11) == 2);
    assert(dm_reduction_mip_nearest(9, 0, 1000, 1) == 0);
    // Fractional behavior is the documented GL prototype, not an NVN hardware
    // validation claim. Both observed game shaders supply integer LODs.
    assert(dm_reduction_mip_nearest(.5f, 0, 1000, 11) == 0);
    assert(dm_reduction_mip_nearest(.5001f, 0, 1000, 11) == 1);
    assert(dm_reduction_mip_nearest(1.5f, 0, 1000, 11) == 1);
    assert(dm_reduction_mip_nearest(1.5001f, 0, 1000, 11) == 2);
    puts("LOD: all 11 observed integer levels, view/sampler clamps and GL half-tie prototype: PASS");
    return 0;
}
