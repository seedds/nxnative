// Replay a menu-to-game CPU-demand transition without changing the test runner's
// scheduling policy. A deadline overrun must not wait for a 120-frame window.
#include "../nvn/nvn_rt_comp.h"
#include <stdio.h>
static int failures;
static void check(int ok, const char *why) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", why);
    if (!ok) failures++;
}
int main(void) {
    const double period = 1000.0 / 60.0;
    unsigned bad = 0;
    for (unsigned phase = 0; phase < 120; phase++) {
        nvn_rt_comp s = {0};
        for (unsigned i = 0; i < phase; i++) nvn_rt_comp_sample(&s, 2.6, period, 4.1);
        double next = nvn_rt_comp_sample(&s, 11.0, period, 4.1);
        if (next < 11.0 || next > period * 0.9) bad++;
    }
    check(bad == 0, "first returning gameplay frame raises demand at all 120 window phases");
    nvn_rt_comp s = {0}; double declared = 12.1;
    bad = 0;
    for (unsigned i = 0; i < 119; i++) {
        if (nvn_rt_comp_sample(&s, 4.0, period, declared) != 0) bad++;
    }
    check(bad == 0, "demand reduction waits for a complete quiet window");
    double next = nvn_rt_comp_sample(&s, 4.0, period, declared);
    check(fabs(next - 5.0) < 0.0001, "quiet window returns to measured demand with headroom");
    s = (nvn_rt_comp){0};
    next = nvn_rt_comp_sample(&s, 30.0, period, 4.1);
    check(fabs(next - 15.0) < 0.0001, "overloaded frame respects the existing 90-percent deadline cap");
    s = (nvn_rt_comp){0};
    bad = 0;
    for (unsigned i = 0; i < 480; i++) if (nvn_rt_comp_sample(&s, 20.0, period, 15.0)) bad++;
    check(bad == 0, "sustained overload does not repeatedly redeclare an unchanged cap");
    s = (nvn_rt_comp){0}; declared = 4.1; bad = 0;
    for (unsigned i = 0; i < 480; i++) {
        next = nvn_rt_comp_sample(&s, 30.0, 16.666666, declared);
        if (next) { declared = (double)llround(next * 1e6) / 1e6; bad++; }
    }
    check(bad == 1, "whole-nanosecond policy feedback reaches the cap once without chatter");
    s = (nvn_rt_comp){0};
    check(nvn_rt_comp_sample(&s, NAN, period, 4.0) == 0 && s.count == 0, "invalid samples do not pollute the window");
    return failures ? 1 : 0;
}
