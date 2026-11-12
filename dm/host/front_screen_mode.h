// Native-pixel fullscreen selection, independent of CoreGraphics for CPU tests.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <math.h>

typedef struct {
    uint32_t id;
    size_t width, height, pixel_width, pixel_height;
    double hz;
    int native, usable;
} front_screen_mode;

static int front_screen_hz_multiple(double hz, int fps) {
    if (fps <= 0 || hz <= 0) return 0;
    double k = round(hz / fps);
    return k >= 1 && fabs(hz / fps - k) < 0.01;
}
// Prefer the current fixed rate when it already fits. On a variable-refresh
// screen prefer a fixed multiple, as the existing fullscreen matcher does;
// keep the variable mode as the fallback when 120 Hz has no fixed counterpart.
// Preserve the user's Retina density, not the oversampled desktop dimensions.
static int front_screen_native_pick(const front_screen_mode *modes, size_t count,
                                    front_screen_mode current, front_screen_mode original,
                                    int current_vrr, int original_vrr, int fps) {
    if (!current.width || !current.height || !original.width || !original.height || fps <= 0) return -1;
    double sx = (double)original.pixel_width / original.width;
    double sy = (double)original.pixel_height / original.height;
    int preserve_rate = !current_vrr && front_screen_hz_multiple(current.hz, fps);
    int best = -1, best_variable = 2;
    double best_hz = 0;
    for (size_t i = 0; i < count; i++) {
        front_screen_mode m = modes[i];
        if (!m.native || !m.usable || !m.width || !m.height || !m.pixel_width || !m.pixel_height ||
            fabs((double)m.pixel_width / m.width - sx) > 0.01 ||
            fabs((double)m.pixel_height / m.height - sy) > 0.01 ||
            !front_screen_hz_multiple(m.hz, fps) || (preserve_rate && fabs(m.hz - current.hz) > 0.5)) continue;
        int variable = (current_vrr && fabs(m.hz - current.hz) < 0.5) ||
                       (original_vrr && fabs(m.hz - original.hz) < 0.5);
        if (best < 0 || variable < best_variable || (variable == best_variable && m.hz > best_hz)) {
            best = (int)i; best_variable = variable; best_hz = m.hz;
        }
    }
    return best;
}
