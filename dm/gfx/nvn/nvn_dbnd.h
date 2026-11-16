// Depth bounds test: the bounds a draw runs with, from what the game set (nvnCommandBufferSetDepthBounds, recorded as OP_DEPTH_BOUNDS; Vulkan's depthBoundsTestEnable with
// minDepthBounds / maxDepthBounds in hle_vk.m). The Switch's GPU passes a fragment when the depth already stored at its sample lies in [min, max], as GL's EXT_depth_bounds_test and
// Vulkan define it, and Metal's -setDepthTestMinBound:maxBound: tests the stored depth the same way. Metal takes only 0 <= min <= max <= 1 and reads (0, 1) as the test turned off.
// The stored depth is always in [0, 1], so a bound outside it is the same as the bound clamped; bounds that no stored depth can meet (min above max, min above 1, max below 0, or a NaN,
// which fails every comparison on the GPU) pass nothing, and on a GPU with the test the replay skips those draws instead of handing Metal bounds it refuses; a GPU without the
// test draws them, as every draw, as before the test was applied.
// Pure C so the Vulkan layer and the tests share it (t_depth_bounds.m)
#pragma once

enum { DBND_OFF, DBND_TEST, DBND_EMPTY };   // the test is off or cannot reject anything / bounds for the encoder / no fragment passes

// b receives the bounds for the encoder: (0, 1) unless DBND_TEST
static inline int dbnd_resolve(int enable, float lo, float hi, float b[2]) {
    b[0] = 0; b[1] = 1;
    if (!enable) return DBND_OFF;
    if (!(lo <= hi) || !(lo <= 1) || !(hi >= 0)) return DBND_EMPTY;   // written so that a NaN on either side lands here
    b[0] = lo > 0 ? lo : 0; b[1] = hi >= 1 ? 1 : hi > 0 ? hi : 0;   // -0 becomes 0
    return b[0] == 0 && b[1] == 1 ? DBND_OFF : DBND_TEST;
}
