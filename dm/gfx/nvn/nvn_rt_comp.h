// CPU demand estimator for a periodic thread. Clock reads and Mach policy calls
// stay with the caller; this state only consumes measured per-frame CPU time.
#ifndef NVN_RT_COMP_H
#define NVN_RT_COMP_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double window[120]; unsigned count; } nvn_rt_comp;
static int nvn_rt_comp_cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double nvn_rt_comp_limit(double need, double period_ms) {
    // The caller applies whole nanoseconds. Quantize here too, so a sub-ns
    // floating-point remainder cannot repeatedly reapply the same policy.
    return floor(fmin(fmax(need, 1.0), period_ms * 0.9) * 1e6) / 1e6;
}
// Return a changed declaration, or zero to leave the caller's policy unchanged.
static double nvn_rt_comp_sample(nvn_rt_comp *s, double cpu_ms, double period_ms, double declared_ms) {
    if (!isfinite(cpu_ms) || cpu_ms < 0 || !isfinite(period_ms) || period_ms <= 0 || !isfinite(declared_ms) || declared_ms <= 0) return 0;
    s->window[s->count++] = cpu_ms;
    // A light scene's declaration is already too small when the next frame
    // exceeds it. Raise demand now; waiting 120 frames stretches the response
    // to several seconds precisely while the thread is missing its deadline.
    double peak = nvn_rt_comp_limit(cpu_ms * 1.1, period_ms);
    double raise = cpu_ms > declared_ms && peak > declared_ms ? peak : 0;
    if (s->count < 120) return raise;
    double v[120]; memcpy(v, s->window, sizeof v); s->count = 0;
    qsort(v, 120, sizeof *v, nvn_rt_comp_cmp);
    double next = nvn_rt_comp_limit(fmax(v[107] * 1.25, v[119] * 1.1), period_ms);
    return next > declared_ms * 1.15 || next < declared_ms * 0.85 ? next : raise;
}
#endif
