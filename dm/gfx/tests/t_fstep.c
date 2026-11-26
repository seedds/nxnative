// Frame step controller unit tests (nvn_fstep.h): frames are simulated in vsyncs. A frame whose work w fits its hold k stays on screen k vsyncs; a longer one takes w
// (the next frame starts at once, as the display-link wait does when behind). Game time is the sum of the steps, real time the sum of the frame times.
#include "../nvn/nvn_fstep.h"
#include <stdio.h>
#include <string.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-78s %s\n", what, ok ? "✓" : "✗"); }

typedef struct { nvn_fstep s; double real, game; int next, slow; } sim;
// n frames of work w (vsyncs) each; GPU-or-CPU cost = w; slow: the CPU is reported slowed for reasons other than heat (probes)
static void run(sim *m, int n, double w, int kmin, int kmax, int fixed) {
    for (int i = 0; i < n; i++) {
        int k = m->s.k ? m->s.k : kmin, step = m->next ? m->next : kmin;
        double shown = w > k ? w : k;
        m->real += shown; m->game += step;
        m->next = nvn_fstep_next(&m->s, shown, w, kmin, kmax, fixed, m->slow);
    }
}

// A hitch that steps hold 1 up: seven frames of 1.6 vsyncs (pipeline compiles), 4.2 vsyncs behind in the last 30 frames and 3.6 without the longest, more than FSTEP_LATE a frame on average. Three such
// frames among frames on time are paid back by the step and keep hold 1 (below)
static void hitch(sim *m) { run(m, 7, 1.6, 1, 3, 0); }

int main(void) {
    {   // 120 fps (hle_nvn.c fps_high): kmin 1, kmax 2, in ticks of 1/120 s. The target follows the hold, so a hold above what frames take is the game at the right speed
        sim m = {0}; m.s.per_vsync = 2; m.s.no_payback = 1;
        run(&m, 120, 0.7, 1, 2, 0); check(m.s.k == 1, "120 fps: frames of 5.8 ms hold 120");
        check(m.s.hold == FSTEP_HOLD0 * 2, "120 fps: the wait before a trial is the same real time");
        run(&m, 10, 1.6, 1, 2, 0); check(m.s.k == 2, "120 fps: frames of 13 ms step down to 60");
        run(&m, 600, 1.6, 1, 2, 0); check(m.s.k == 2, "120 fps: they stay at 60 while they do not fit");
        run(&m, 60, 4.0, 1, 2, 0); check(m.s.k == 2, "120 fps: 33 ms frames stop at 60, never a longer hold");
        run(&m, 1200, 0.7, 1, 2, 0); check(m.s.k == 1, "120 fps: frames that fit return to 120");
        sim c = {0}; c.s.per_vsync = 2; c.s.no_payback = 1;
        run(&c, 120, 0.7, 1, 2, 0); run(&c, 30, 0.7, 2, 2, 0); check(c.s.k == 2, "120 fps: a cap of 60 (a movie, a 60 Hz screen) holds two ticks");
        run(&c, 120, 0.7, 1, 2, 0); check(c.s.k == 1, "120 fps: the cap's end returns to 120");
    }
    char b[160];
    {   // fps_hold off at 120 (hle_nvn.c fstep_kmax: kmax is the cap's vsyncs, 1 tick): frames 11 % over the tick (9.25 ms, just past FSTEP_LATE) stay at 120, each shown late rather than stepping the hold to 60
        sim f = {0}; f.s.per_vsync = 2; f.s.no_payback = 1;
        run(&f, 600, 1.11, 1, 1, 0);
        snprintf(b, sizeof b, "120 fps, fps_hold off: frames of 9.2 ms hold 120, no step to 60 (k %d, ups %llu, late %llu)", f.s.k, (unsigned long long)f.s.ups, (unsigned long long)f.s.late);
        check(f.s.k == 1 && f.s.ups == 0 && f.s.late > 100, b);
    }
    {   sim m = {0}; run(&m, 600, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.paid == 0 && m.game == m.real, "steady 60: hold 1, step 1, game time = real time");
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0); run(&m, 600, 1.32, 1, 3, 0);   // 22 ms frames
        snprintf(b, sizeof b, "steady 22 ms frames: 30 fps after three late frames, no flapping (k %d, ups %llu, downs %llu)", m.s.k, (unsigned long long)m.s.ups, (unsigned long long)m.s.downs);
        check(m.s.k == 2 && m.s.ups == 1 && m.s.downs == 0, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.debt < 1, b);
    }
    {   // Mario Kart World on an M4 Max (issue #1823): frames scattered around one vsync, three in a row late by 4 ms every 20 frames, the rest on time. The rate holds 60
        // and the step pays the late frames back, so hold 1 stays (three late frames alone stepped to 30 every second)
        sim m = {0}; run(&m, 120, 0.5, 1, 3, 0);
        for (int i = 0; i < 3000; i++) run(&m, 1, i % 20 < 3 ? 1.25 : 0.9, 1, 3, 0);
        snprintf(b, sizeof b, "near-misses, 3 of every 20 frames late by 4 ms: hold 1 (k %d, ups %llu, late %llu)", m.s.k, (unsigned long long)m.s.ups, (unsigned long long)m.s.late);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.late >= 400, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, paid %llu)", m.game, m.real, m.s.debt, (unsigned long long)m.s.paid);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.paid > 0, b);
        sim h = {0}; run(&h, 120, 0.5, 1, 3, 0); run(&h, 600, 0.9, 1, 3, 0); run(&h, 3, 1.6, 1, 3, 0); run(&h, 60, 0.9, 1, 3, 0);
        check(h.s.k == 1 && h.s.ups == 0, "  a hitch of three 27 ms frames among frames on time: hold 1");
    }
    {   // Lone stalls among near-misses (issue #1823, the reporter's M4 Max retest of 2026-11-12, defaults): every frame costs 0.45 vsyncs (GPU 6.7-7.8 ms) and the screen
        // shows three in a row 5 ms late once every 20 (15% late, 57.4 fps over 30 frames, 1.35 vsyncs short of the hold there), which keeps hold 1. Then one long frame
        // at each present of the report: 67.7 ms (693 first texture uploads), 57.7 and 43.1 ms (shader libraries, a hybrid pipeline), 54.9 and 65.4 ms (nothing listed).
        // Each counted FSTEP_SHORT_MAX (2) of the 30 frames' limit of 3, so the next three late frames stepped to 30 for 30 presents, 60 once (the probe's run before it
        // had not lasted 8 s). The step pays a stall back (game time = real time either way): one long frame is not a rate, so hold 1 throughout
        static const struct { int at; double ms; } st[] = {{1913, 67.7}, {10267, 57.7}, {10286, 43.1}, {10454, 54.9}, {11396, 65.4}};
        nvn_fstep s = {0}; s.smax = 2; int next = 1, at2 = 0; double real = 0, game = 0;
        for (int p = 0; p < 12000; p++) {
            int k = s.k ? s.k : 1; double shown = k == 1 && p % 20 >= 5 && p % 20 < 8 ? 1.3 : k;
            for (unsigned i = 0; i < sizeof st / sizeof *st; i++) if (p == st[i].at) shown = st[i].ms / (1000.0 / 60);
            real += shown; game += next; next = nvn_fstep_next(&s, shown, 0.45, 1, 3, 0, 0); at2 += s.k > 1;
        }
        snprintf(b, sizeof b, "lone stalls of 43-68 ms among near-misses (MKW, M4 Max): hold 1 (ups %llu, %d frames at hold 2)", (unsigned long long)s.ups, at2);
        check(s.ups == 0 && at2 == 0, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f)", game, real, s.debt);
        check(real - game - s.debt > -1.01 && real - game - s.debt < 1.01, b);
    }
    {   // a scene at 1.3 vsyncs whose twelfth late frame is a 50 ms hitch: the step goes to the cheapest of the last three (2), not to the hitch (3)
        sim m = {0}; run(&m, 120, 0.5, 1, 3, 0); run(&m, 11, 1.3, 1, 3, 0);
        check(m.s.k == 1, "scene at 1.3 vsyncs: eleven late frames are 3 vsyncs behind without the longest, not yet short of hold 1");
        run(&m, 1, 3.0, 1, 3, 0);
        snprintf(b, sizeof b, "  a 3-vsync frame then: hold 2, not 3 (k %d)", m.s.k);
        check(m.s.k == 2 && m.s.ups == 1, b);
    }
    {   // a game whose stub takes steps up to 2 (Mario Kart World's STEP_MAX): frames of 2.6 vsyncs stop at hold 2, and no step above 2 is handed out
        sim m = {0}; m.s.smax = 2; int over = 0;
        run(&m, 120, 0.5, 1, 3, 0);
        for (int i = 0; i < 600; i++) { run(&m, 1, i < 300 ? 2.6 : 1.3, 1, 3, 0); over += m.next > 2; }
        snprintf(b, sizeof b, "step cap 2: frames of 2.6 then 1.3 vsyncs hold 2 at most, steps at most 2 (k %d, steps over 2: %d)", m.s.k, over);
        check(m.s.k == 2 && over == 0, b);
        sim f = {0}; f.s.smax = 2; run(&f, 10, 0.5, 1, 3, 3);
        check(f.s.k == 2 && f.next == 2, "  a step the game fixes at 3 is held at the cap");
    }
    {   // 120 fps (no payback: the game steps by its target, so a late frame is slow motion): three late frames step down to 60 whatever the average
        sim m = {0}; m.s.per_vsync = 2; m.s.no_payback = 1; run(&m, 120, 0.7, 1, 2, 0); run(&m, 3, 1.25, 1, 2, 0);
        check(m.s.k == 2, "120 fps, no payback: three frames late by 2 ms step down to 60");
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0); run(&m, 300, 1.12, 1, 3, 0);   // 18.7 ms frames: 53 fps
        check(m.s.k == 2 && m.s.ups == 1, "steady 18.7 ms frames (53 fps): 30 fps, not 53 with a payback every 8 frames");
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0);   // present snapshots: the caller judges lateness by the screen
        for (int i = 0; i < 20; i++) { m.s.screen = 1; m.next = nvn_fstep_next(&m.s, 1.0, 1.3, 1, 3, 0, 0); }
        check(m.s.k == 1 && m.s.ups == 0, "snapshots: costly frames the screen shows on time keep hold 1");
        for (int i = 0; i < 3; i++) { m.s.screen = 2; m.next = nvn_fstep_next(&m.s, 1.0, 0.8, 1, 3, 0, 0); }
        check(m.s.k == 2 && m.s.ups == 1, "  three frames late on screen step up, whatever their cost");
        m.next = nvn_fstep_next(&m.s, 2.0, 0.5, 1, 3, 0, 0);
        check(m.s.screen == 0, "  the screen verdict holds for one call");
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0);   // snapshots, a game steadily too slow: frames shown on time keep the cost they do not fit out of the probes' reference
        for (int i = 0; i < 600; i++) { m.s.screen = 1; m.next = nvn_fstep_next(&m.s, 1.0, 1.12, 1, 3, 0, 0); }
        for (int i = 0; i < 3; i++) { m.s.screen = 2; m.next = nvn_fstep_next(&m.s, 1.0, 1.12, 1, 3, 0, 0); }
        for (int i = 0; i < 600; i++) { m.s.screen = 1; m.next = nvn_fstep_next(&m.s, 2.0, 1.12, 1, 3, 0, 0); }
        snprintf(b, sizeof b, "snapshots, 18.7 ms work shown on time at hold 1, then late: hold 2 stays, no probes (k %d, probes %llu)", m.s.k, (unsigned long long)m.s.probes);
        check(m.s.k == 2 && m.s.probes == 0 && m.s.downs == 0, b);
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        m.real += 6; m.game += m.next; m.next = nvn_fstep_next(&m.s, 6, 6, 1, 3, 0, 0);   // one 100 ms hitch
        check(m.next == 2 && m.s.k == 1, "100 ms hitch: the next step pays back one vsync, hold stays 1");
        int two = 0; for (int i = 0; i < 20; i++) { int st = m.next; run(&m, 1, 0.5, 1, 3, 0); two += st == 2; }
        snprintf(b, sizeof b, "  five steps of 2, then 1 (%d steps of 2); game time %.0f = real time %.0f", two, m.game, m.real);
        check(two == 5 && m.game == m.real && m.s.k == 1, b);
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        m.real += 30; m.game += m.next; m.next = nvn_fstep_next(&m.s, 30, 30, 1, 3, 0, 0);   // a 0.5 s stall
        run(&m, 60, 0.5, 1, 3, 0);
        snprintf(b, sizeof b, "0.5 s stall: 15 vsyncs paid back, %.0f dropped, not replayed (paid %llu)", m.s.dropped, (unsigned long long)m.s.paid);
        check(m.s.dropped == 14 && m.s.paid == 15 && m.real - m.game == 14 && m.s.k == 1, b);
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0);   // a heavy scene: 30 fps
        check(m.s.k == 2, "heavy scene: hold 2");
        run(&m, 29, 0.6, 1, 3, 0);
        check(m.s.k == 2, "  cost drops: 29 frames that fit 1 are not enough");
        run(&m, 1, 0.6, 1, 3, 0);
        check(m.s.k == 1 && m.s.trial == FSTEP_RUN, "  the 30th starts a trial at hold 1");
        run(&m, FSTEP_RUN + 10, 0.6, 1, 3, 0);
        check(m.s.k == 1 && m.s.trial == 0 && m.s.hold == FSTEP_HOLD0 && m.s.failed == 0, "  the trial holds: 60 fps again");
    }
    {   // the cost says 0.6, but frames at hold 1 take 1.5 (the cost misses something): each trial fails, and the trials spread out 1 → 2 → 4 → 8 s
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 30, 1.5, 1, 3, 0);
        check(m.s.k == 2, "frames at hold 1 take 1.5 vsyncs: hold 2");
        double last = m.real, gaps[4]; int ng = 0; uint64_t downs = m.s.downs;
        for (int i = 0; i < 4000 && ng < 4; i++) {
            double w = m.s.k == 1 ? 1.5 : 0.6;
            run(&m, 1, w, 1, 3, 0);
            if (m.s.downs != downs) { downs = m.s.downs; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "  trials %.0f, %.0f, %.0f, %.0f vsyncs apart; %llu failed", gaps[0], gaps[1], gaps[2], gaps[3], (unsigned long long)m.s.failed);
        check(ng == 4 && gaps[0] >= 60 && gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && gaps[3] < 560 && m.s.failed >= 3, b);
    }
    {   sim m = {0}; run(&m, 300, 0.5, 2, 2, 0);
        check(m.s.k == 2 && m.next == 2 && m.game == m.real, "30 fps cap: hold 2, step 2");
        m.real += 6; m.game += m.next; m.next = nvn_fstep_next(&m.s, 6, 6, 2, 2, 0, 0);
        int three = 0; for (int i = 0; i < 10; i++) { int st = m.next; run(&m, 1, 0.5, 2, 2, 0); three += st == 3; }
        snprintf(b, sizeof b, "  a hitch at the 30 cap is paid back with steps of 3 (%d), hold stays 2", three);
        check(three == 4 && m.s.k == 2 && m.game == m.real, b);
        run(&m, 300, 3.5, 2, 2, 0);
        check(m.s.k == 2, "  frames longer than the hold at the 30 cap: hold stays 2 (the 30 Hz link holds only even counts)");
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 1, 0.5, 2, 3, 0);
        check(m.s.k == 2, "movie flag (kmin 2): hold 2 at once");
        run(&m, 120, 0.5, 2, 3, 0); run(&m, 1, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.late == 0, "  movie ends (kmin 1): hold 1 at once");
    }
    {   sim m = {0}; run(&m, 600, 0.86, 1, 3, 0); run(&m, 600, 1.5, 2, 2, 0); run(&m, 1, 1.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.grace == FSTEP_GRACE && m.s.trial == FSTEP_RUN && m.s.probes == 1,
              "cap release: count a probe, with all grace frames left for hold 1");
        int frames = 0;
        while (m.s.k == 1 && frames < 100) { run(&m, 1, 1.3, 1, 3, 0); frames++; }
        check(frames == FSTEP_GRACE + 3 && m.s.k == 2 && m.s.failed == 1 && m.s.hold == 2 * FSTEP_HOLD0,
              "  recovery fails after 30 grace frames plus 3 late frames, then doubles the hold-off");
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 120, 0.5, 1, 3, 2);
        check(m.s.k == 2 && m.next == 2 && m.s.debt == 0, "the game fixes its step at 2 (BotW's VFR override): hold 2, step 2");
        run(&m, 2, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.next == 1 && m.s.late == 0, "  the override ends: hold 1, step 1, no late frame counted");
    }
    {   // a parked CPU: 20 ms frames at hold 2 never fit hold 1, and without the slow report nothing tries it
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); run(&m, 2000, 1.2, 1, 3, 0);
        check(m.s.k == 2 && m.s.downs == 0 && m.s.probes == 0, "not slowed: 1.2-vsync frames at hold 2 stay at hold 2, no probe");
    }
    {   // the same with the CPU reported slowed: the probe goes to hold 1, its frames take 1.5 vsyncs for 20 frames while the clock rises, then 0.6
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1;
        int at1 = 0, up_during_grace = 0; uint64_t ups = m.s.ups;
        for (int i = 0; i < 400; i++) {
            double w = m.s.k == 1 ? (at1++ < 20 ? 1.5 : 0.6) : 1.2;
            run(&m, 1, w, 1, 3, 0);
            if (m.s.grace > 0 && m.s.ups != ups) up_during_grace = 1;
        }
        snprintf(b, sizeof b, "slowed, the clock rises: one probe to hold 1 that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && m.s.ups == ups && !up_during_grace && m.s.trial == 0 && m.s.probe == 2 && m.s.hold == FSTEP_HOLD0, b);
        snprintf(b, sizeof b, "  20 late frames in the grace are paid back: game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {   // slowed, but hold 1 really does not fit: each probe fails after its grace, and the probes spread out 1 → 2 → 4 → 8 s
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1;
        double last = m.real, gaps[5]; int ng = 0, at1 = 0, first_len = -1; uint64_t probes = m.s.probes;
        for (int i = 0; i < 6000 && ng < 5; i++) {
            run(&m, 1, m.s.k == 1 ? 1.5 : 1.2, 1, 3, 0);
            if (m.s.k == 1) at1++; else if (at1 && first_len < 0) first_len = at1;
            if (m.s.probes != probes) { probes = m.s.probes; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "slowed, hold 1 does not fit: the first probe lasts its grace + 3 late frames (%d frames at hold 1)", first_len);
        check(first_len == FSTEP_GRACE + 3, b);
        snprintf(b, sizeof b, "  probes %.0f, %.0f, %.0f, %.0f vsyncs apart; %llu failed", gaps[1], gaps[2], gaps[3], gaps[4], (unsigned long long)m.s.failed);
        check(ng == 5 && gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && gaps[4] >= 480 && gaps[4] < 600 && m.s.failed >= 4, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, dropped %.0f)", m.game, m.real, m.s.debt, m.s.dropped);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {   // slowed, hold 1 holds for a while: a probe that holds but whose run at hold 1 ends within 8 s counts as failed, and the next waits twice as long
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1;
        for (int i = 0; i < 1000 && m.s.probe != 2; i++) run(&m, 1, m.s.k == 1 ? 0.6 : 1.2, 1, 3, 0);
        double hold0 = m.s.hold; uint64_t failed = m.s.failed;
        run(&m, 120, 0.6, 1, 3, 0); hitch(&m);
        snprintf(b, sizeof b, "slowed, hold 1 lasts 2 s after the probe: counted as failed (failed %llu, hold-off %.0f → %.0f vsyncs)", (unsigned long long)m.s.failed, hold0, m.s.hold);
        check(m.s.k == 2 && m.s.failed == failed + 1 && m.s.hold == 2 * hold0 && m.s.probe == 0, b);
        for (int i = 0; i < 2000 && m.s.probe != 2; i++) run(&m, 1, m.s.k == 1 ? 0.6 : 1.2, 1, 3, 0);
        hold0 = m.s.hold; failed = m.s.failed;
        run(&m, 600, 0.6, 1, 3, 0); hitch(&m);
        snprintf(b, sizeof b, "  hold 1 lasts 10 s after the next probe: the hold-off returns to 1 s (failed %llu, hold-off %.0f → %.0f vsyncs)", (unsigned long long)m.s.failed, hold0, m.s.hold);
        check(m.s.k == 2 && m.s.failed == failed && hold0 == 120 && m.s.hold == FSTEP_HOLD0, b);
    }
    {   sim m = {0}; m.slow = 1; run(&m, 600, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.probes == 0 && m.s.downs == 0 && m.game == m.real, "slowed at hold 1 already: nothing to probe");
        sim c = {0}; c.slow = 1; run(&c, 300, 0.5, 2, 2, 0);
        check(c.s.k == 2 && c.s.probes == 0, "  slowed at the 30 cap (kmin 2): nothing to probe");
    }
    {   // Breath of the Wild on an iPhone: GPU 0.86 at hold 1, three heavy frames step to 2, where the GPU clock falls (1.5); after the probe it rises again within 20 frames
        sim m = {0}; nvn_fgpu g = {0}; int at1 = 0, stepped = 0;
        for (int i = 0; i < 2000; i++) {
            double w = i < 600 ? 0.86 : i < 603 ? 1.25 : m.s.k >= 2 ? 1.5 : at1++ < 20 ? 1.5 : 0.86;
            stepped |= i >= 603 && m.s.k == 2;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
            run(&m, 1, w, 1, 3, 0);
        }
        snprintf(b, sizeof b, "GPU clock follows the hold (BotW): one probe back to hold 1 that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(stepped && m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, dropped %.0f)", m.game, m.real, m.s.debt, m.s.dropped);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {   // a heavier scene at the same clock: 0.86 at hold 1, then 1.12 (19 ms) for good, which is 1.3 times less than enough to call the GPU slowed
        sim m = {0}; nvn_fgpu g = {0};
        for (int i = 0; i < 2000; i++) { double w = i < 600 ? 0.86 : 1.12; m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1); run(&m, 1, w, 1, 3, 0); }
        check(m.s.k == 2 && m.s.probes == 0, "heavier scene, GPU 0.86 → 1.12 at hold 2: not slowed, no probe");
    }
    {   // a loading screen: the GPU at 0.03, then lag on the CPU steps to 2, where the GPU takes 0.17 (5.7 times as much but 3 ms): the CPU holds the frame back, not the GPU
        sim m = {0}; nvn_fgpu g = {0}; int slowed = 0;
        for (int i = 0; i < 1200; i++) { double gp = i < 600 || m.s.k == 1 ? 0.03 : 0.17, w = i < 600 ? 0.5 : 1.3; m.slow = nvn_fstep_gpu_slowed(&g, gp, &m.s, 1); slowed |= m.slow; run(&m, 1, w, 1, 3, 0); }
        check(m.s.k == 2 && !slowed && m.s.probes == 0, "GPU 0.03 → 0.17 under CPU lag: a GPU at 3 ms is not called slowed, no probe");
    }
    {   // Ocarina of Time on a Mac: GPU 0.75 at hold 1, an area's load steps to 3, where the clock falls: 1.3 with a frame of 2.0 in every 12 (no 30 frames in a row fit hold 2), and 1.0 at hold 2
        sim m = {0}; nvn_fgpu g = {0}; int at1 = 0, at3 = 0, at2 = 0;
        for (int i = 0; i < 3000; i++) {
            int k = m.s.k ? m.s.k : 1;
            double w = i < 600 ? 0.75 : i < 603 ? 3.4 : k == 3 ? (i % 12 ? 1.3 : 2.0) : k == 2 ? 1.0 : at1++ < 20 ? 1.0 : 0.75;
            at3 += i >= 603 && k == 3; at2 += i >= 603 && k == 2;
            m.slow = nvn_fstep_gpu_slowed(&g, i >= 600 && i < 603 ? 0.75 : w, &m.s, 1);   // the load's late frames are the CPU's
            run(&m, 1, w, 1, 3, 0);
        }
        snprintf(b, sizeof b, "frames over the fit at hold 3 (OoT): a trial to 2, a probe to 1 (k %d, failed %llu, %d frames at hold 3, %d at hold 2)", m.s.k, (unsigned long long)m.s.failed, at3, at2);
        check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && at3 <= 40 && at2 <= 70, b);
        sim c = {0};   // a mean that does not fit hold 2 keeps hold 3
        run(&c, 600, 0.75, 1, 3, 0); run(&c, 3, 3.4, 1, 3, 0);
        for (int i = 0; i < 2400; i++) run(&c, 1, i % 12 ? 1.92 : 2.0, 1, 3, 0);
        check(c.s.k == 3 && c.s.downs == 0, "  a mean of 1.93 at hold 3 does not fit hold 2: no trial");
    }
    {   // a scene at the edge that holds 60 (0.8, one frame in 10 at 1.05, none late at hold 1): a load's ten late frames step to 2 with nothing learned at hold 1 and the GPU as fast as before
        sim m = {0}; int at2 = 0;
        for (int i = 0; i < 3000; i++) {
            double w = i >= 300 && i < 310 ? 1.4 : i % 10 ? 0.8 : 1.05;
            at2 += i >= 310 && m.s.k == 2;
            run(&m, 1, w, 1, 3, 0);
        }
        snprintf(b, sizeof b, "edge scene after a hitch (OoT): frames of 1.05 among 0.8 do not keep hold 2 (k %d, ups %llu, failed %llu, %d frames at hold 2)", m.s.k, (unsigned long long)m.s.ups, (unsigned long long)m.s.failed, at2);
        check(m.s.k == 1 && m.s.ups == 1 && m.s.failed == 0 && at2 <= 70, b);
    }
    {   // issue #2400 (OoT on an M4): 26 ms frames at hold 3 with every sixth at 30 or 31.7 ms, over the fit of hold 2 (28.3 ms) but all on time there
        for (int v = 0; v < 2; v++) {
            sim m = {0}; int at3 = 0; run(&m, 60, 0.5, 1, 3, 0); run(&m, 3, 3.5, 1, 3, 0);
            for (int i = 0; i < 600; i++) { at3 += m.s.k == 3; run(&m, 1, i % 6 == 5 ? (v ? 1.9 : 1.8) : 1.56, 1, 3, 0); }
            snprintf(b, sizeof b, "1.56 at hold 3, every sixth %.1f: hold 2 and it holds (k %d, %d frames at hold 3, failed %llu)", v ? 1.9 : 1.8, m.s.k, at3, (unsigned long long)m.s.failed);
            check(m.s.k == 2 && at3 <= 120 && m.s.failed == 0, b);
        }
    }
    {   // issue #2400 (OoT on a base M5 and an M4 Pro, whose GPU clock barely follows the hold): a scene at 0.90 vsyncs (15 ms, ±0.03) at either hold after a heavier moment
        // stepped up from a light one. Hold 1 keeps it (none late), but with a fit of 0.85 it stayed at hold 2 for good: the GPU reference was the light scene's (0.76, and
        // 0.90 is not 1.3 times it), and a scene that fits starts no probe that fails, so no ratio. A scene at 1.14 (19 ms, the M5's frame time walking in Kokiri Forest with
        // the hold off: GPU 17.7 ms, 53 fps) stays at 30
        for (int v = 0; v < 2; v++) {
            sim m = {0}; nvn_fgpu g = {0}; uint32_t r = 3; int at1 = 0; double c = v ? 1.14 : 0.90;
            for (int i = 0; i < 6700; i++) {
                r = r * 1664525u + 1013904223u; double j = ((r >> 8) % 1000) / 1000.0 * 0.06 - 0.03;
                double w = i < 600 ? 0.5 + j / 3 : i < 700 ? 1.2 + j : c + j;
                at1 += i >= 700 && m.s.k == 1;
                m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
                run(&m, 1, w, 1, 3, 0);
            }
            snprintf(b, sizeof b, "%s at either hold after a heavier moment: %s (frames at hold 1 %d of 6000, failed %llu)", v ? "  1.14" : "0.90", v ? "30 fps" : "back to 60",
                     at1, (unsigned long long)m.s.failed);
            check(v ? at1 <= 200 && m.s.k == 2 : at1 >= 5900 && m.s.k == 1 && m.s.failed <= 1, b);
        }
    }
    {   // the cost of the mean: a scene at the edge whose mean fits hold 1 (0.7, a quarter of its frames at 1.3, late there) goes back and forth between holds 1 and 2,
        // its trials held and the hold stepping up again later, about once per FSTEP_HOLD_MAX (the per-frame rule kept hold 2 for good); this pins that rate
        sim m = {0}; uint32_t r = 1; run(&m, 10, 1.6, 1, 3, 0);
        double t0 = m.real;
        for (int i = 0; i < 36000; i++) { r = r * 1664525u + 1013904223u; run(&m, 1, (r >> 8) % 4 ? 0.7 : 1.3, 1, 3, 0); }
        double cap = (m.real - t0) / FSTEP_HOLD_MAX;
        snprintf(b, sizeof b, "  edge scene with a heavy tail: steps down at most once per 8 s (downs %llu, at most %.0f, failed %llu)", (unsigned long long)m.s.downs, cap, (unsigned long long)m.s.failed);
        check(m.s.downs <= cap, b);
    }
    {   // the GPU stays slow at hold 1 too (1.5 at either hold): the probes fail after their grace, and their frames there give the ratio 1: the scene does not fit hold 1 at hold 1's
        // clock, so no further probe after the second while it costs the same (before the ratio, one every 1, 2, 4, 8 s, each 33 frames late). A lighter scene (0.95 at either hold,
        // over the fit of hold 1, so no trial) is 0.95 at hold 1 by that ratio, and its probe holds
        sim m = {0}; nvn_fgpu g = {0}; uint64_t p1 = 0;
        for (int i = 0; i < 9000; i++) {
            double w = i < 600 ? 0.86 : i < 603 ? 1.25 : i < 6000 ? 1.5 : 0.95;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
            run(&m, 1, w, 1, 3, 0);
            if (i == 5999) p1 = m.s.probes;
        }
        snprintf(b, sizeof b, "GPU slow at hold 1 too: two probes fail, none after them (probes %llu, failed %llu, ratio %.2f)", (unsigned long long)p1, (unsigned long long)m.s.failed, g.ratio_at[1][2]);
        check(p1 == FSTEP_GPU_FAILS && m.s.failed == FSTEP_GPU_FAILS && g.ratio_at[1][2] == 1, b);
        snprintf(b, sizeof b, "  a lighter scene that fits hold 1 by the ratio: its probe holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(m.s.k == 1 && m.s.probes == FSTEP_GPU_FAILS + 1 && m.s.failed == FSTEP_GPU_FAILS, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01, b);
    }
    {   // Ocarina of Time on an M4 Pro at render scale 1.333 (2026-11-15), a clock that barely moves: Hyrule Field takes 1.00 vsyncs at hold 2 and more at hold 1 (frames back to back:
        // GPU 1.03-1.09, cost 1.13-1.26 at the failures; 1.12 here), stepped up by a hitch. Two failed probes give a ratio under 1, so the estimate at hold 1 is over it and no probe follows (with the ratio taken as 1 the estimate was 1.00, and they
        // went on failing); a lighter scene after it (0.90 at hold 2, 0.94 at hold 1) fits hold 1: a trial, which holds, with no probe and no failure
        sim m = {0}; nvn_fgpu g = {0}; uint64_t p1 = 0, f1 = 0; uint32_t r = 7;
        for (int i = 0; i < 9600; i++) {
            int k = m.s.k ? m.s.k : 1; r = r * 1664525u + 1013904223u; double j = ((r >> 8) % 1000) / 1000.0 * 0.06 - 0.03;
            double w = i < 600 ? 0.25 : i < 603 ? 3.0 : i < 6600 ? (k == 1 ? 1.12 : 1.00) + j : (k == 1 ? 0.94 : 0.90) + j / 3;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
            run(&m, 1, w, 1, 3, 0);
            if (i == 6599) { p1 = m.s.probes; f1 = m.s.failed; }
        }
        snprintf(b, sizeof b, "GPU a little slower at hold 1 (OoT, M4 Pro): %llu probes, %llu failed, then none (ratio %.2f)", (unsigned long long)p1, (unsigned long long)f1, g.ratio_at[1][2]);
        check(p1 == FSTEP_GPU_FAILS && f1 == FSTEP_GPU_FAILS && g.ratio_at[1][2] < 1, b);
        snprintf(b, sizeof b, "  a lighter scene after it: a trial back to hold 1 that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(m.s.k == 1 && m.s.probes == p1 && m.s.failed == f1, b);
    }
    {   // probes failed by a stall (a load: GPU 5 vsyncs for 10 frames after the grace, Ocarina of Time on an M4 Pro gave 4.4 and 5.7) give no ratio: a scene of 0.84 at hold 1
        // and 1.34 at hold 2 (the clock lowered, over the fit, so no trial) is probed again after them, and holds; taken as the smallest ratio (0.75) they made it 1.79 at hold 1
        sim m = {0}; nvn_fgpu g = {0}; int stalls = 0, at1 = 0, post = 0;
        for (int i = 0; i < 4000; i++) {
            int k = m.s.k ? m.s.k : 1; at1 = k == 1 ? at1 + 1 : 0;
            post = m.s.probe == 1 && k == 1 && m.s.grace == 0 ? post + 1 : 0;
            double w = i < 600 ? 0.25 : i < 603 ? 3.0 : k > 1 ? 1.34 : stalls < 2 && m.s.probe == 1 && post >= 1 && post <= 10 ? 5.0 : at1 <= 20 ? 1.34 : 0.84;
            uint64_t f = m.s.failed;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
            run(&m, 1, w, 1, 3, 0);
            stalls += m.s.failed != f;
        }
        snprintf(b, sizeof b, "two probes failed by a stall give no ratio, the next probe holds (k %d, probes %llu, failed %llu, ratios %d)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed, g.ratio_n[1][2] + g.ratio_n[1][3]);
        check(m.s.k == 1 && m.s.failed == 2 && m.s.probes == 3 && g.ratio_n[1][2] + g.ratio_n[1][3] == 0, b);
    }
    // A GPU clock that follows the hold (dvfs): w is the scene's GPU time at hold 1's clock; at a longer hold it takes drop times as long, and the first 20 frames back at hold 1
    // still do. Each case starts on a title screen (0.25), whose reference the heavy scene's few frames at hold 1 move only part of the way before it steps up
    for (int c = 0; c < 4; c++)
        for (int d = 0; d < 3; d++) {
            static const double drops[] = {1.6, 2.0, 3.0};
            sim m = {0}; nvn_fgpu g = {0}; int ramp = 0, k0 = 1, at1 = 0, n = c == 3 ? 12000 : 7800, from = c == 3 ? 6000 : 600; uint64_t p0 = 0, f0 = 0;
            for (int i = 0; i < n; i++) {
                int k = m.s.k ? m.s.k : 1;
                if (k == 1 && k0 > 1) ramp = 20;
                k0 = k;
                // c 0: Kokiri Forest (Ocarina of Time on an Android port): 1.25 at hold 1, 2.05 for 60 frames in every 400 as the camera turns; c 1: Hyrule Field at night
                // (OoT on an M2 Pro): 1.12; c 2: a scene at 0.6 after 1.25 from the start (the heavy scene's reference, over hold 1); c 3: Kokiri Forest, then 0.6
                double w = c == 2 ? (i < 1800 ? 1.25 : 0.6) : i < 600 ? 0.25 : c == 1 ? 1.12 : c == 3 && i >= 6000 ? 0.6 : i % 400 >= 340 ? 2.05 : 1.25;
                if (c == 2 && i == 0) from = 1800;
                double gt = k > 1 || ramp-- > 0 ? w * drops[d] : w;
                if (i == from) { p0 = m.s.probes; f0 = m.s.failed; }
                at1 += i >= from && k == 1;
                m.slow = nvn_fstep_gpu_slowed(&g, gt, &m.s, 1);
                run(&m, 1, gt, 1, 3, 0);
            }
            unsigned long long pr = m.s.probes - p0, fl = m.s.failed - f0;
            if (c < 2) {
                snprintf(b, sizeof b, "%s, GPU %.1fx at a longer hold: %llu probes, all failed (frames at hold 1: %d)", c ? "  Hyrule Field 1.12 (OoT, M2 Pro)" : "Kokiri Forest 1.25 / 2.05 (OoT)", drops[d], pr, at1);
                check(pr <= FSTEP_GPU_FAILS + 1 && fl == pr && at1 <= 120, b);   // two failed probes give a hold its ratio (3 here at 1.6x: they fail from hold 3 and from 2); before it, 15-48
            } else {
                snprintf(b, sizeof b, "  %s, GPU %.1fx: back to hold 1 (k %d, frames at hold 1 %d of %d, probes %llu, failed %llu)", c == 2 ? "1.25 from the start, then 0.6" : "Kokiri Forest, then 0.6", drops[d], m.s.k, at1, n - from, pr, fl);
                check(m.s.k == 1 && at1 >= n - from - 600 && fl == 0, b);
            }
        }
    {   // Breath of the Wild on an iPhone (2026-10-21): frames of 0.80 / 0.92 vsyncs at hold 1 for 20 s, pipeline compiles step to 2, and the scene costs the same there:
        // the cost that held hold 1 starts one probe, which holds (both frames fit FSTEP_FIT too, but the probe starts before 30 of them in a row)
        sim m = {0};
        for (int i = 0; i < 1200; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        hitch(&m);
        int stepped = m.s.k == 2; double ref = m.s.cref;
        for (int i = 0; i < 600; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        snprintf(b, sizeof b, "hitch after 20 s at hold 1, same cost at hold 2 (BotW): one probe back that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(stepped && ref > 0.84 && ref < 0.88 && m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && m.s.probe == 2 && m.s.downs == 1, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, dropped %.0f)", m.game, m.real, m.s.debt, m.s.dropped);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {   // the scene gets heavier (1.15 / 1.25 at hold 1, 1.4 times what held): hold 2 and no probe; back to the old scene, the cost that held starts one
        sim m = {0};
        for (int i = 0; i < 1200; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        for (int i = 0; i < 1200; i++) run(&m, 1, i % 2 ? 1.25 : 1.15, 1, 3, 0);
        check(m.s.k == 2 && m.s.probes == 0 && m.s.downs == 0, "heavier scene, cost 1.4 times what held hold 1: hold 2, no probe");
        for (int i = 0; i < 600; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0, "  the scene gets lighter again, back to the cost that held: one probe back that holds");
    }
    {   // hold 1 held only 5 s before the hitch: that run proves nothing (a device that steps up every few seconds keeps 30 rather than flap). Frames of 0.92 / 1.02,
        // whose mean is over FSTEP_FIT, so no trial either
        sim m = {0};
        for (int i = 0; i < 300; i++) run(&m, 1, i % 2 ? 1.02 : 0.92, 1, 3, 0);
        hitch(&m);
        for (int i = 0; i < 2000; i++) run(&m, 1, i % 2 ? 1.02 : 0.92, 1, 3, 0);
        check(m.s.k == 2 && m.s.probes == 0 && m.s.cref == 0, "hitch after 5 s at hold 1: no cost that held, no probe");
    }
    {   // the cost at hold 2 is what held, but hold 1 no longer fits (1.3 there): each probe fails, the probes spread out 1 → 2 → 4 → 8 s, and the cost that held is kept
        sim m = {0}; double last = 0, gaps[5]; int ng = 0; uint64_t probes = 0;
        run(&m, 1200, 0.97, 1, 3, 0); hitch(&m);
        for (int i = 0; i < 6000 && ng < 5; i++) {
            run(&m, 1, m.s.k == 1 ? 1.3 : 0.97, 1, 3, 0);
            if (m.s.probes != probes) { probes = m.s.probes; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "same cost at hold 2 but hold 1 does not fit: probes fail (%llu of %llu), cost that held kept at %.2f", (unsigned long long)m.s.failed, (unsigned long long)m.s.probes, m.s.cref);
        check(ng == 5 && m.s.failed >= 4 && m.s.cref > 0.96 && m.s.cref < 0.98, b);
        snprintf(b, sizeof b, "  probes %.0f, %.0f, %.0f, %.0f vsyncs apart; game time %.0f against real time %.2f (debt %.2f)", gaps[1], gaps[2], gaps[3], gaps[4], m.game, m.real, m.s.debt);
        check(gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && gaps[4] >= 480 && gaps[4] < 600 && m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01, b);
    }
    for (int sustained = 0; sustained < 2; sustained++) {
        // The first movie follows an uninterrupted run: no earlier hitch has saved cref. Only a run of at least 8 s proves that this cost held 60.
        sim m = {0}; run(&m, sustained ? 1200 : 300, 0.97, 1, 3, 0);
        run(&m, 600, 0.5, 2, 2, 0);
        check(sustained ? m.s.cref_at[1] > 0.96 && m.s.cref_at[1] < 0.98 : m.s.cref_at[1] == 0,
              sustained ? "first movie after 20 s at 60: save the sustained run without an earlier hitch"
                        : "first movie after only 5 s at 60: no proven cost to save");
        run(&m, 60, 0.97, 1, 3, 0); uint64_t probes = m.s.probes; hitch(&m);
        for (int i = 0; i < 1200; i++) run(&m, 1, m.s.k == 1 ? 0.97 : 1.0, 1, 3, 0);   // over FSTEP_FIT at hold 2: no trial
        check(sustained ? m.s.k == 1 && m.s.probes > probes : m.s.k == 2 && m.s.probes == probes,
              sustained ? "  hitch 1 s after the first movie: the saved run brings 60 back"
                        : "  hitch after a short unproven run: keep 30 without a same-cost probe");
    }
    {   // Later movies keep the last proven cost through short runs, and replace it after another sustained run at a different cost.
        sim m = {0}; run(&m, 1200, 0.86, 1, 3, 0); run(&m, 120, 0.5, 2, 2, 0);
        run(&m, 120, 0.6, 1, 3, 0); run(&m, 120, 0.5, 2, 2, 0);
        check(m.s.cref_at[1] > 0.85 && m.s.cref_at[1] < 0.87, "second movie after only 2 s: keep the earlier proven cost");
        run(&m, 1200, 0.6, 1, 3, 0); run(&m, 120, 0.5, 2, 2, 0);
        check(m.s.cref_at[1] > 0.59 && m.s.cref_at[1] < 0.61, "  another sustained run before a movie: update the proven cost");
    }
    {   sim m = {0}; run(&m, 1200, 0.86, 1, 3, 0); hitch(&m); double held = m.s.cref; run(&m, 1, 0.5, 2, 3, 0);
        check(held > 0.85 && m.s.cref == 0 && m.s.run <= 2, "movie flag (kmin 2): the cost that held kmin 1 is set aside");
        run(&m, 120, 0.5, 2, 3, 0); run(&m, 1, 0.5, 1, 3, 0);
        check(m.s.cref == held, "  movie ends (kmin 1): the cost that held kmin 1 comes back");
    }
    {   // Tears of the Kingdom's prerendered movies set a 30 cap (kmin 2). A GPU-bound game at 0.86 vsyncs at hold 1 and 0.9 at hold 2 (the clock lowered for the longer hold)
        // has learned the cost that held hold 1 from a hitch; after a 10 s movie, pipeline compiles 1 s in (hitch) step it up before 8 s at hold 1 relearn it
        sim m = {0}; run(&m, 1200, 0.86, 1, 3, 0); hitch(&m);
        for (int i = 0; i < 1200; i++) run(&m, 1, m.s.k == 1 ? 0.86 : 0.9, 1, 3, 0);
        double held = m.s.cref; int at1 = 0;
        run(&m, 600, 0.5, 2, 2, 0); run(&m, 60, 0.86, 1, 3, 0); hitch(&m);
        for (int i = 0; i < 1200; i++) { run(&m, 1, m.s.k == 1 ? 0.86 : 0.9, 1, 3, 0); at1 += i >= 600 && m.s.k == 1; }
        snprintf(b, sizeof b, "  a hitch 1 s after a movie: the cost that held hold 1 starts a probe (k %d, at hold 1 %d of the last 600)", m.s.k, at1);
        check(held > 0.85 && m.s.k == 1 && at1 == 600, b);
    }
    // The iOS thermal cap (hle_nvn.c thermal_capped): 30 fps from frame 600 for 30 s. Breath of the Wild on an iPhone, GPU-bound: 0.86 vsyncs (14.4 ms) at hold 1, 1.5 (25 ms)
    // at hold 2, where the clock follows the hold. After the cap and after each run at hold 2, the first 10 frames at hold 1 take 1.5 until the clock rises again; c = 1: the
    // first 60 frames after the cap (a clock slower to come back), so the probe at the cap's end fails; c = 2: hold 1 takes 1.3 for good after the cap (a heavier scene)
    for (int c = 0; c < 3; c++) {
        static const char *const what[] = {"thermal cap ends: the probe's grace covers the clock, hold 1 at once and it stays (k %d, %d steps up after the cap, at hold 1 %d of the last 600)",
                                           "  the clock takes 1 s: the probe fails, the GPU against hold 1's own reference starts another (k %d, %d steps up, at hold 1 %d of the last 600)",
                                           "  hold 1 no longer fits (1.3): a probe at most every 8 s, at hold 1 %d of the last 600; game time %.0f against real time %.2f (probes %llu)"};
        int rise = 600 + 1800, since1 = 0, at1 = 0, n = rise + 3600; uint64_t probes = 0, ups = 0; double last = 0, gap = 1e9;
        sim m = {0}; nvn_fgpu g = {0};
        for (int i = 0; i < n; i++) {
            int kmin = i >= 600 && i < rise ? 2 : 1, kmax = kmin == 2 ? 2 : 3, k = m.s.k ? m.s.k : kmin;
            since1 = k == 1 ? since1 + 1 : 0;
            double w = k >= 2 ? 1.5 : i < rise ? 0.86 : c == 2 ? 1.3 : since1 <= 10 || (c == 1 && i < rise + 60) ? 1.5 : 0.86;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, kmin);
            run(&m, 1, w, kmin, kmax, 0);
            if (i == rise - 1) ups = m.s.ups;
            if (i >= n - 600) at1 += m.s.k == 1;
            if (i > rise + 1200 && m.s.probes != probes) { if (last) gap = gap < m.real - last ? gap : m.real - last; last = m.real; }
            probes = m.s.probes;
        }
        int up = (int)(m.s.ups - ups);
        if (c < 2) { snprintf(b, sizeof b, what[c], m.s.k, up, at1); check(m.s.k == 1 && at1 == 600 && (c || !up), b); }
        else {
            snprintf(b, sizeof b, what[c], at1, m.game, m.real, (unsigned long long)m.s.probes);
            check(at1 < 100 && gap >= FSTEP_HOLD_MAX && m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01, b);
        }
    }
    {   // 165 Hz with virtual ticks (nvn_mtl.m link_range): cheap frames show on the refreshes nearest their slots, 12.1 / 18.2 ms at hold 1, 30.3 / 36.4 at 2, 48.5 / 54.5 at 3
        for (int sl = 1; sl >= 0; sl--) {
            nvn_fstep s = {.k = 3, .step = 3, .kmin = 1, .hold = FSTEP_HOLD0, .shown_slack = sl ? 60.0 / 165 : 0};
            for (int i = 0; i < 360; i++) {
                double ms = s.k == 3 ? (i % 4 ? 48.485 : 54.545) : s.k == 2 ? (i % 2 ? 30.303 : 36.364) : (i % 4 ? 18.182 : 12.121);
                nvn_fstep_next(&s, ms * 60 / 1000, 0.6, 1, 3, 0, 0);
            }
            snprintf(b, sizeof b, sl ? "165 Hz, cheap frames at hold 3, one refresh of slack: trials back to 2 and 1 (k %d, downs %llu)"
                                     : "  without the slack the 54.5 ms frames count as late: hold 3 (20 fps) for good (k %d, downs %llu)", s.k, (unsigned long long)s.downs);
            check(sl ? s.k == 1 && s.downs == 2 : s.k == 3 && s.downs == 0, b);
        }
        sim m = {0}; m.s.shown_slack = 60.0 / 165; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.32, 1, 3, 0);
        check(m.s.k == 2, "  the slack does not hide work: 22 ms frames still step to hold 2");
    }
    {   // A controller that raises the clock slowly (the Mac): the probe's frames at hold 1 start at 1.5 vsyncs and get 0.6% cheaper each frame, so they fit only after about 52 frames.
        // Without the ramp rule the probe fails after its 30 grace frames and 3 late ones; with it (nvn_fstep.ramp) the grace is renewed while the frames keep getting cheaper and the probe holds
        for (int ramp = 0; ramp <= 1; ramp++) {
            sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1; m.s.ramp = ramp;
            int at1 = 0, first_len = -1;
            for (int i = 0; i < 400; i++) {
                double w = m.s.k == 1 ? fmax(0.6, 1.5 * pow(0.994, at1)) : 1.2;
                run(&m, 1, w, 1, 3, 0);
                if (m.s.k == 1) at1++; else if (at1 && first_len < 0) first_len = at1;
            }
            if (!ramp) {
                snprintf(b, sizeof b, "slowed, the clock rises slowly: without the ramp rule the probe fails after grace + 3 frames (%d frames at hold 1, failed %llu)", first_len, (unsigned long long)m.s.failed);
                check(first_len == FSTEP_GRACE + 3 && m.s.failed >= 1, b);
            } else {
                snprintf(b, sizeof b, "  with it the grace is renewed twice and the probe holds (k %d, probes %llu, failed %llu, grace frames %d)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed, m.s.ptotal);
                check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && m.s.probe == 2 && m.s.trial == 0 && m.s.ptotal == 3 * FSTEP_GRACE && first_len < 0, b);
                snprintf(b, sizeof b, "  its late frames are paid back: game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
                check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
            }
        }
    }
    {   // The ramp rule with a cost that does not fall (1.5 flat), one that falls and then stops short of the hold (1.6 down to 1.3), and one that keeps falling but never reaches it (3.0 at 0.5% a frame):
        // the probe lasts grace + 3, two windows + 3, and FSTEP_PROBE_MAX + 3 frames
        static const char *const what[] = {"ramp rule, the cost does not fall: the probe lasts its grace + 3 late frames as before (%d frames at hold 1)",
                                           "  the cost falls, then holds at 1.3: one more window, then the probe fails (%d frames at hold 1)",
                                           "  the cost keeps falling without reaching the hold: the grace ends at FSTEP_PROBE_MAX (%d frames at hold 1, failed %llu)"};
        static const int want[] = {FSTEP_GRACE + 3, 2 * FSTEP_GRACE + 3, FSTEP_PROBE_MAX + 3};
        for (int c = 0; c < 3; c++) {
            sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1; m.s.ramp = 1;
            int at1 = 0, first_len = -1;
            for (int i = 0; i < 600 && first_len < 0; i++) {
                double w = m.s.k != 1 ? 1.2 : c == 0 ? 1.5 : c == 1 ? fmax(1.3, 1.6 * pow(0.99, at1)) : 3.0 * pow(0.995, at1);
                run(&m, 1, w, 1, 3, 0);
                if (m.s.k == 1) at1++; else if (at1 && first_len < 0) first_len = at1;
            }
            snprintf(b, sizeof b, what[c], first_len, (unsigned long long)m.s.failed);
            check(first_len == want[c] && m.s.k == 2 && m.s.failed == 1, b);
        }
    }
    {   // A drain of standing frames (nvn_fstep_drain_cut), in ms against a 16.7 ms hold
        uint64_t pw = 20; check(nvn_fstep_drain_cut(33, 17, 16, 0, &pw) == 17 && pw == 4, "a drain of one tick: the frame shows its hold, the pacing wait loses the tick");
        pw = 10; check(nvn_fstep_drain_cut(17, 17, 6, 0, &pw) == 17 && pw == 10, "a drain inside the hold is pacing: nothing cut");
        pw = 10; check(nvn_fstep_drain_cut(30, 17, 6, 0, &pw) == 24 && pw == 4, "a late frame keeps its lateness beyond the drain");
        pw = 12; check(nvn_fstep_drain_cut(34, 33, 5, 0, &pw) == 33 && pw == 11, "only the part past the hold is cut (hold 2)");
        pw = 2; check(nvn_fstep_drain_cut(33, 17, 0, 16, &pw) == 17 && pw == 2, "a drain in a later acquire of the previous frame: the time, not the pacing wait");
        pw = 3; check(nvn_fstep_drain_cut(40, 17, 9, 0, &pw) == 37 && pw == 0, "a drain counted no larger than the pacing wait it was part of");
        pw = 0; check(nvn_fstep_drain_cut(0, 17, 0, 5, &pw) == 0, "the first frame (no previous start): nothing to cut");
    }
    {   // Idle-GPU late frames (#1942): a warm-start progress screen (GPU 0.02 vsyncs) with 10 late frames does not step up, while FSTEP_RUN (30) late frames in a row still step up
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        for (int i = 0; i < 10; i++) { m.s.gpu_on = 1; m.s.gpu = 0.02; run(&m, 1, 2.5, 1, 3, 0); }
        check(m.s.k == 1 && m.s.ups == 0, "idle GPU (0.02 vsyncs): 10 late frames on a progress screen keep hold 1");
        for (int i = 0; i < FSTEP_RUN; i++) { m.s.gpu_on = 1; m.s.gpu = 0.02; run(&m, 1, 2.5, 1, 3, 0); }
        check(m.s.k > 1 && m.s.ups == 1 && m.s.up_idle == 1, "idle GPU: FSTEP_RUN late frames in a row still step up (CPU-heavy scene)");
    }
    {   // Unpaced hold probe (#1942): a game that waits outside acquire shows cost == shown == hold at hold 3 while GPU fits k - 1 (0.75 vsyncs); probe brings it back to hold 1
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        for (int i = 0; i < 10; i++) { m.s.gpu_on = 1; m.s.gpu = 2.5; run(&m, 1, 2.5, 1, 3, 0); }
        int at3 = m.s.k == 3;
        for (int i = 0; i < 300; i++) {
            double k = m.s.k; double w = k == 1 ? 0.75 : k;   // at hold 3, game's own wait makes cost == shown == 3, GPU 0.75
            m.s.gpu_on = 1; m.s.gpu = 0.75; run(&m, 1, w, 1, 3, 0);
        }
        check(at3 && m.s.k == 1 && m.s.probes >= 1 && m.s.failed == 0, "unpaced hold at hold 3 with GPU 0.75: unpaced probe returns to hold 1");
    }
    {   // Unpaced hold probe on a genuinely CPU-bound game (CPU needs hold 2 even when probed): uhold backs off up to FSTEP_UHOLD_MAX (60 s)
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        for (int i = 0; i < 10; i++) { m.s.gpu_on = 1; m.s.gpu = 0.5; run(&m, 1, 1.8, 1, 3, 0); }
        int at2 = m.s.k == 2;
        for (int i = 0; i < 6000; i++) {
            double w = m.s.k == 1 ? 1.8 : 2.0;   // CPU really needs 1.8 vsyncs, GPU is only 0.5
            m.s.gpu_on = 1; m.s.gpu = 0.5; run(&m, 1, w, 1, 3, 0);
        }
        check(at2 && m.s.k == 2 && m.s.failed >= 2 && m.s.uhold > FSTEP_HOLD_MAX, "unpaced probe when CPU really needs hold 2: uhold doubles past 8 s toward 60 s");
    }
    {   // Late frames at a 30 fps cap's hold 2 (kmin == kmax == 2): 3 late frames set fine (60 Hz link at next vsync); 8 s on time clears it; fixed step or cap rise clears it
        sim m = {0}; run(&m, 60, 1.5, 2, 2, 0);
        check(m.s.k == 2 && !m.s.fine && m.s.fines == 0, "30 fps cap on time: fine stays off");
        for (int i = 0; i < 3; i++) { m.s.gpu_on = 1; m.s.gpu = 2.0; run(&m, 1, 2.2, 2, 2, 0); }
        check(m.s.k == 2 && m.s.fine == 1 && m.s.fines == 1, "30 fps cap, 3 late frames at hold 2: fine turns on");
        // An idle-GPU late frame does not reset fine_ok; 8 s (480 vsyncs = 240 frames at hold 2) on time ends fine
        for (int i = 0; i < 250; i++) { m.s.gpu_on = 1; m.s.gpu = 1.5; run(&m, 1, 1.5, 2, 2, 0); }
        check(m.s.fine == 0 && m.s.fines == 1, "30 fps cap, 8 s with no late frame: fine turns off");
        // Re-enter fine, then fixed step clears it
        for (int i = 0; i < 3; i++) { m.s.gpu_on = 1; m.s.gpu = 2.0; run(&m, 1, 2.2, 2, 2, 0); }
        int was_fine = m.s.fine == 1 && m.s.fines == 2;
        run(&m, 1, 1.5, 2, 2, 2);
        check(was_fine && m.s.fine == 0, "30 fps cap: a game-fixed step clears fine");
        // Re-enter fine, then raising the cap (kmax > k) clears it
        for (int i = 0; i < 3; i++) { m.s.gpu_on = 1; m.s.gpu = 2.0; run(&m, 1, 2.2, 2, 2, 0); }
        int was_fine2 = m.s.fine == 1;
        run(&m, 1, 1.5, 1, 3, 0);
        check(was_fine2 && m.s.fine == 0, "30 fps cap: raising the cap clears fine");
    }
    {   // Stalls (FSTEP_STALL) and host-named pauses / window changes (settle):
        // A pause followed by severe cold-GPU / link-recovery frames (which cross over_sum - omax > 3.0 on main) keeps hold 1
        sim m = {0}; run(&m, 600, 0.9, 1, 3, 0); uint64_t late0 = m.s.late;
        run(&m, 1, 114, 1, 3, 0); run(&m, 1, 3.0, 1, 3, 0); run(&m, 1, 2.5, 1, 3, 0); run(&m, 1, 2.0, 1, 3, 0); run(&m, 60, 0.9, 1, 3, 0);
        snprintf(b, sizeof b, "stall: a pause and severe late frames keep hold 1 (k %d, ups %llu, stalls %llu)", m.s.k, (unsigned long long)m.s.ups, (unsigned long long)m.s.stalls);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.stalls == 1 && m.s.late == late0 + 3, b);
        check(m.s.crun < 0.95, "  the stall's time stays out of the cost that held hold 1");
        // Fullscreen switch: two display-link stalls (10 and 17 vsyncs) with late frames around them keep hold 1
        run(&m, 2, 1.3, 1, 3, 0); run(&m, 1, 10, 1, 3, 0); run(&m, 3, 1.4, 1, 3, 0); run(&m, 1, 17, 1, 3, 0); run(&m, 6, 1.3, 1, 3, 0); run(&m, 120, 0.9, 1, 3, 0);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.stalls == 3, "  a fullscreen switch (two stalls, late frames around them) keeps hold 1");
        // A scene that really is heavier after a stall steps up once stall grace is over
        sim h = {0}; run(&h, 600, 0.9, 1, 3, 0); run(&h, 1, 114, 1, 3, 0);
        int n = 0; while (h.s.k == 1 && n < 600) { run(&h, 1, 1.32, 1, 3, 0); n++; }
        snprintf(b, sizeof b, "  22 ms frames after a stall step up once its grace is over (%d frames)", n);
        check(h.s.k == 2 && n == FSTEP_GRACE + 3, b);
        // Long frames below FSTEP_STALL still step up as before
        sim l = {0}; run(&l, 600, 0.9, 1, 3, 0); run(&l, 3, 5.0, 1, 3, 0);
        check(l.s.k == 3 && l.s.ups == 1 && l.s.stalls == 0, "  three 83 ms frames in a row are not stalls: they step up as before");
        // 120 fps (per_vsync = 2, no_payback = 1): same 133 ms real-time threshold
        sim f = {0}; f.s.per_vsync = 2; f.s.no_payback = 1; run(&f, 240, 0.7, 1, 2, 0); run(&f, 3, 10, 1, 2, 0);
        check(f.s.k == 2 && f.s.stalls == 0, "  120 fps: 83 ms frames are not stalls");
        sim g = {0}; g.s.per_vsync = 2; g.s.no_payback = 1; run(&g, 240, 0.7, 1, 2, 0); run(&g, 1, 200, 1, 2, 0); run(&g, 3, 1.5, 1, 2, 0); run(&g, 60, 0.7, 1, 2, 0);
        check(g.s.k == 1 && g.s.ups == 0 && g.s.stalls == 1, "  120 fps: a pause and three late frames keep 120");
        // Host-named pause (settle): not paid back; unnamed stall still pays back FSTEP_DEBT_MAX (15)
        sim p = {0}; run(&p, 600, 0.9, 1, 3, 0); uint64_t paid0 = p.s.paid;
        p.s.settle = 1; run(&p, 1, 114, 1, 3, 0); run(&p, 60, 0.9, 1, 3, 0);
        snprintf(b, sizeof b, "  a host-named pause is not paid back (paid %llu, debt %.2f)", (unsigned long long)(p.s.paid - paid0), p.s.debt);
        check(p.s.paid == paid0 && p.s.debt < 1 && p.s.k == 1 && p.s.settle == 0, b);
        // Named pause on a frame already stepping 2 to pay back prior jitter: accounts for that step without double-paying
        sim pb = {0}; run(&pb, 60, 0.9, 1, 3, 0); run(&pb, 1, 2.2, 1, 3, 0); uint64_t paid_b = pb.s.paid;
        pb.s.settle = 1; run(&pb, 1, 114, 1, 3, 0); run(&pb, 30, 0.9, 1, 3, 0);
        check(pb.s.paid == paid_b && pb.s.debt < 1, "  named pause on a payback frame drops only the pause, no double-pay");
        sim q = {0}; run(&q, 600, 0.9, 1, 3, 0); paid0 = q.s.paid; run(&q, 1, 114, 1, 3, 0); run(&q, 60, 0.9, 1, 3, 0);
        check(q.s.paid - paid0 == 15, "  an unnamed stall pays back 15 vsyncs as before");
        // Named window change: mildly late animation frames keep hold 1; unnamed they step up
        sim w = {0}; run(&w, 600, 0.9, 1, 3, 0);
        w.s.settle = 1; run(&w, 20, 1.34, 1, 3, 0); w.s.settle = 1; run(&w, 20, 1.34, 1, 3, 0); run(&w, 120, 0.9, 1, 3, 0);
        check(w.s.k == 1 && w.s.ups == 0, "  a host-named window change: 40 mildly late frames keep hold 1");
        sim x = {0}; run(&x, 600, 0.9, 1, 3, 0); run(&x, 20, 1.34, 1, 3, 0);
        check(x.s.k == 2 && x.s.ups == 1, "  the same frames with no window change step up as before");
        // 30 fps cap (fine mode): a pause + 3 late frames do not turn fine on; a 10 s pause while fine is on does not clear fine
        sim fn = {0}; run(&fn, 60, 1.5, 2, 2, 0);
        fn.s.settle = 1; run(&fn, 1, 114, 2, 2, 0); run(&fn, 3, 2.5, 2, 2, 0); run(&fn, 60, 1.5, 2, 2, 0);
        check(fn.s.fine == 0 && fn.s.fines == 0, "  30 fps cap: pause + 3 late frames do not flip fine on");
        for (int i = 0; i < 3; i++) { fn.s.gpu_on = 1; fn.s.gpu = 2.0; run(&fn, 1, 2.2, 2, 2, 0); }
        int fine_on = fn.s.fine == 1;
        fn.s.settle = 1; run(&fn, 1, 600, 2, 2, 0); run(&fn, 5, 1.5, 2, 2, 0);
        check(fine_on && fn.s.fine == 1, "  30 fps cap: a 10 s pause does not clear fine early");
        // Pause duration does not inflate run / tsince, and a stall during a trial does not let late frames pass the trial
        sim ti = {0}; run(&ti, 5, 0.9, 1, 3, 0); ti.s.settle = 1; run(&ti, 1, 600, 1, 3, 0);
        check(ti.s.run < 20 && ti.s.cref == 0, "  a 10 s pause does not inflate run into an 8 s proven hold");
        sim tr = {0}; run(&tr, 60, 0.5, 1, 3, 0); run(&tr, 300, 1.3, 1, 3, 0); run(&tr, FSTEP_RUN, 0.6, 1, 3, 0);
        int in_trial = tr.s.k == 1 && tr.s.trial == FSTEP_RUN;
        tr.s.settle = 1; run(&tr, 1, 600, 1, 3, 0); run(&tr, FSTEP_GRACE, 1.5, 1, 3, 0);
        check(in_trial && tr.s.trial == FSTEP_RUN && !tr.s.tpend, "  a stall during a trial pauses trial countdown across stall grace");
        // Idle-GPU late frames during stall grace do not accumulate ilate_run
        sim ig = {0}; run(&ig, 60, 0.5, 1, 3, 0); ig.s.settle = 1; run(&ig, 1, 114, 1, 3, 0);
        for (int i = 0; i < FSTEP_GRACE; i++) { ig.s.gpu_on = 1; ig.s.gpu = 0.02; run(&ig, 1, 2.5, 1, 3, 0); }
        check(ig.s.k == 1 && ig.s.ilate_run == 0, "  idle-GPU late frames during stall grace do not advance ilate_run");
        // Early returns (!s->k, fixed > 0) clear settle so it does not leak onto a later frame
        sim er = {0}; er.s.settle = 1; nvn_fstep_next(&er.s, 0, 0, 1, 3, 0, 0);
        int clr1 = er.s.settle == 0;
        er.s.settle = 1; nvn_fstep_next(&er.s, 1, 0.5, 1, 3, 2, 0);
        int clr2 = er.s.settle == 0;
        nvn_fstep_next(&er.s, 1, 0.5, 1, 3, 0, 0);
        check(clr1 && clr2 && er.s.stalls == 0, "  settle is cleared on first-frame and fixed-step early returns");
    }
    {   // Ticks of 1/120 s (nvn_fstep.t120: a page whose unit at +0x38 is 120, a FRAME_STEP_120 stub): 2 / 3 / 4 ticks are 60 / 40 / 30 fps
        _Alignas(8) uint8_t page[64] = {0}; nvn_fstep_target_put(page, 60);
        check(!nvn_fstep_t120(page) && !nvn_fstep_t120(NULL), "a page in vsyncs (+0x38 0) is not in 1/120 s ticks");
        uint32_t unit = 120; memcpy(page + FSTEP_UNIT_OFF, &unit, 4);
        check(nvn_fstep_t120(page), "  a page whose unit is 120 counts in ticks of 1/120 s");
        check(nvn_fstep_cap(40, 0, 0) == 40 && nvn_fstep_cap(40, 30, 0) == 30 && nvn_fstep_cap(40, 0, 1) == 30 && nvn_fstep_cap(40, 60, 0) == 60,
              "  cap at 40: 40 normally, 30 for a 30 fps movie or thermal cap, 60 for a 60 fps movie");
        check(nvn_fstep_cap(0, 0, 0) == 0 && nvn_fstep_cap(30, 60, 0) == 60 && nvn_fstep_cap(30, 60, 1) == 30,
              "  cap at 0 / 30: target 0 stays off; 60 fps movie overrides 30 unless thermally capped");
    }
    {   // 40 fps: kmin 3, kmax 4 (hle_nvn.c fstep_tick). Frames that fit hold 3 ticks (25 ms) and step 3: game time = real time
        sim m = {0}; m.s.t120 = 1; m.s.per_vsync = 2;
        run(&m, 600, 2.0, 3, 4, 0);
        snprintf(b, sizeof b, "40 fps: 16.7 ms frames hold 3 ticks and step 3, game time %.0f = real time %.0f ticks (k %d, ups %llu)", m.game, m.real, m.s.k, (unsigned long long)m.s.ups);
        check(m.s.k == 3 && m.next == 3 && m.s.ups == 0 && m.s.paid == 0 && m.game == m.real, b);
        int seen[8] = {0}; double real0 = m.real, game0 = m.game;
        for (int i = 0; i < 600; i++) { run(&m, 1, 3.6, 3, 4, 0); seen[m.s.k]++; }   // 30 ms frames: 40 does not hold
        snprintf(b, sizeof b, "  30 ms frames: back to 30 (4 ticks) once the rate falls short over the window, never 5 or 6 (k %d: 3 %d / 4 %d / 5 %d / 6 %d frames)", m.s.k, seen[3], seen[4], seen[5], seen[6]);
        check(m.s.k == 4 && !seen[5] && !seen[6] && seen[3] == 11 && seen[4] == 589, b);
        snprintf(b, sizeof b, "  game time %.1f against real time %.1f ticks (debt %.2f)", m.game - game0, m.real - real0, m.s.debt);
        check(fabs((m.real - real0) - (m.game - game0) - m.s.debt) < 2.01 && m.s.debt < 2.01, b);
        run(&m, 1200, 2.0, 3, 4, 0);
        snprintf(b, sizeof b, "  frames that fit 3 ticks again: a trial back to 40 holds (k %d, downs %llu)", m.s.k, (unsigned long long)m.s.downs);
        check(m.s.k == 3 && m.s.downs >= 1, b);
        uint64_t paid = m.s.paid; real0 = m.real; game0 = m.game;
        m.real += 12; m.game += m.next; m.next = nvn_fstep_next(&m.s, 12, 12, 3, 4, 0, 0);   // one 100 ms hitch
        run(&m, 60, 2.0, 3, 4, 0);
        snprintf(b, sizeof b, "  a 100 ms hitch: %llu ticks paid back one a frame (steps of 4), hold stays 3; game time %.0f = real time %.0f", (unsigned long long)(m.s.paid - paid), m.game - game0, m.real - real0);
        check(m.s.k == 3 && m.s.paid - paid == 9 && m.game - game0 == m.real - real0, b);
    }
    {   // 60 and 30 in ticks: holds stay whole vsyncs (even ticks), margins and limits stay the same real time as in vsyncs (including idle-GPU, unpaced probes and fine mode)
        sim m = {0}; m.s.t120 = 1; m.s.per_vsync = 2; int odd = 0;
        for (int i = 0; i < 600; i++) { run(&m, 1, i < 120 ? 1.0 : 2.6, 2, 6, 0); odd += m.s.k % 2; }   // 21.7 ms frames at a 60 cap
        snprintf(b, sizeof b, "ticks at 60: 21.7 ms frames step to 4 ticks (30 fps), not 3 (k %d, odd holds %d)", m.s.k, odd);
        check(m.s.k == 4 && !odd, b);
        for (int i = 0; i < 300; i++) { run(&m, 1, 4.6, 2, 6, 0); odd += m.s.k % 2; }
        snprintf(b, sizeof b, "  38 ms frames: 6 ticks (20 fps), not 5 (k %d, odd holds %d)", m.s.k, odd);
        check(m.s.k == 6 && !odd, b);
        for (int i = 0; i < 2400; i++) { run(&m, 1, 1.0, 2, 6, 0); odd += m.s.k % 2; }
        snprintf(b, sizeof b, "  cheap frames again: trials back down by whole vsyncs to 2 ticks (k %d, odd holds %d)", m.s.k, odd);
        check(m.s.k == 2 && !odd, b);
        sim s = {0}; s.s.t120 = 1; s.s.per_vsync = 2; run(&s, 60, 1.0, 2, 6, 0);
        s.real += 60; s.game += s.next; s.next = nvn_fstep_next(&s.s, 60, 60, 2, 6, 0, 0);   // a 0.5 s stall (not settle: debt capped at 30 ticks = 0.25 s)
        run(&s, 60, 1.0, 2, 6, 0);
        snprintf(b, sizeof b, "  0.5 s stall: 30 ticks (0.25 s) paid back, %.0f dropped, as 15 / 14 vsyncs in vsyncs (paid %llu)", s.s.dropped, (unsigned long long)s.s.paid);
        check(s.s.dropped == 28 && s.s.paid == 30 && s.real - s.game == 28 && s.s.k == 2, b);
        sim t = {0}; t.s.t120 = 1; t.s.per_vsync = 2; run(&t, 600, 1.0, 2, 6, 0);
        run(&t, 20, 2.15, 2, 6, 0);   // 17.9 ms: late by 0.15 ticks, within the 0.2-tick (1.7 ms) margin of a vsync
        check(t.s.k == 2 && t.s.ups == 0, "  17.9 ms frames at 60 are on time: the late margin is 1.7 ms, as in vsyncs");
        // Idle GPU scaled to ticks: 0.08 ticks (0.67 ms < 0.1 ticks = 0.83 ms) counts as idle and does not step up in 10 frames
        sim ig = {0}; ig.s.t120 = 1; run(&ig, 60, 1.0, 2, 6, 0);
        for (int i = 0; i < 10; i++) { ig.s.gpu_on = 1; ig.s.gpu = 0.08; run(&ig, 1, 5.0, 2, 6, 0); }
        check(ig.s.k == 2 && ig.s.ups == 0, "  idle GPU in ticks (0.08 ticks < 0.1): 10 late frames keep hold 2");
        // Unpaced hold probe in ticks: at hold 4 (kmin 2, q 2), GPU 1.5 ticks (< 0.95 * 2) triggers probe back to hold 2
        sim up = {0}; up.s.t120 = 1; run(&up, 60, 1.0, 2, 6, 0);
        for (int i = 0; i < 12; i++) { up.s.gpu_on = 1; up.s.gpu = 3.5; run(&up, 1, 3.5, 2, 6, 0); }
        int at4 = up.s.k == 4;
        for (int i = 0; i < 300; i++) {
            double k = up.s.k, w = k == 2 ? 1.5 : k;
            up.s.gpu_on = 1; up.s.gpu = 1.5; run(&up, 1, w, 2, 6, 0);
        }
        check(at4 && up.s.k == 2 && up.s.probes >= 1 && up.s.failed == 0, "  unpaced hold at 4 ticks with GPU 1.5 ticks: probe returns to 2 ticks");
        // 30 fps cap in ticks (kmin 4, kmax 4): 3 late frames turn fine on; 8 s (960 ticks = 240 frames) on time clears it
        sim fn = {0}; fn.s.t120 = 1; run(&fn, 60, 3.0, 4, 4, 0);
        for (int i = 0; i < 3; i++) { fn.s.gpu_on = 1; fn.s.gpu = 4.0; run(&fn, 1, 4.4, 4, 4, 0); }
        int fine_on = fn.s.fine == 1 && fn.s.fines == 1;
        for (int i = 0; i < 250; i++) { fn.s.gpu_on = 1; fn.s.gpu = 3.0; run(&fn, 1, 3.0, 4, 4, 0); }
        check(fine_on && fn.s.fine == 0, "  30 fps cap in ticks (hold 4): 3 late frames set fine, 8 s on time clears it");
    }
    {   // Movie / thermal cap transitions in ticks: 40 → 30 (movie) → 40 (cap_probe), 40 → 60 (60 fps movie) → 40
        sim m = {0}; m.s.t120 = 1; m.s.per_vsync = 2;
        run(&m, 300, 1.5, 3, 4, 0); int k40 = m.s.k;
        run(&m, 1, 1.5, 4, 4, 0); int k30 = m.s.k;
        run(&m, 300, 1.5, 4, 4, 0);
        run(&m, 1, 1.5, 3, 4, 0); int k40b = m.s.k, probe = m.s.probe;
        run(&m, 300, 1.5, 3, 4, 0);
        run(&m, 1, 1.5, 2, 6, 0); int k60 = m.s.k;
        run(&m, 300, 1.5, 2, 6, 0);
        run(&m, 1, 1.5, 3, 4, 0); int k40c = m.s.k;
        snprintf(b, sizeof b, "cap transitions in ticks (40 → 30 → 40 → 60 → 40): holds %d → %d → %d (probe %d) → %d → %d", k40, k30, k40b, probe, k60, k40c);
        check(k40 == 3 && k30 == 4 && k40b == 3 && probe == 1 && k60 == 2 && k40c == 3 && m.s.ups == 0, b);
    }
    {   // The GPU reference keeps a slot for each kmin up to 4 ticks (30 fps in ticks), and its ratios one for each hold up to 6 ticks (20 fps); a hold compares against kmin
        nvn_fgpu g = {0}; nvn_fstep at = {0}; at.t120 = 1; at.k = 4;
        for (int i = 0; i < 40; i++) nvn_fstep_gpu_slowed(&g, 1.0, &at, 4);
        check(g.ref_at[4] > 0.99 && g.ref_at[4] < 1.01, "GPU reference at kmin 4 ticks is kept (30 fps in ticks)");
        nvn_fgpu h = {0}; nvn_fstep s = {0}; s.t120 = 1; s.k = 2;
        for (int i = 0; i < 40; i++) nvn_fstep_gpu_slowed(&h, 1.0, &s, 2);
        s.k = 4; int sl = 0; for (int i = 0; i < 40; i++) sl = nvn_fstep_gpu_slowed(&h, 2.2, &s, 2);
        check(sl, "  60 in ticks, hold 4: a GPU 2.2× its hold-2 time that does not fit 2 ticks counts as slowed");
        nvn_fgpu r = {0}; nvn_fstep p = {0}; p.t120 = 1;
        for (int n = 0; n < 2; n++) {
            p.k = 6; p.probe = 0; for (int i = 0; i < 40; i++) nvn_fstep_gpu_slowed(&r, 4.0, &p, 2);
            p.k = 2; p.probe = 1; p.grace = FSTEP_GRACE / 2; for (int i = 0; i < 10; i++) nvn_fstep_gpu_slowed(&r, 2.0, &p, 2);
            p.k = 6; p.probe = 0; for (int i = 0; i < 40; i++) nvn_fstep_gpu_slowed(&r, 4.0, &p, 2);
        }
        snprintf(b, sizeof b, "  two failed probes from 6 ticks give the hold-6 ratio %.2f (%d probes) and it decides (%.2f)", r.ratio_at[2][6], r.ratio_n[2][6], nvn_fgpu_ratio(&r));
        check(r.ratio_n[2][6] >= FSTEP_GPU_FAILS && r.ratio_at[2][6] > 1.9 && r.ratio_at[2][6] < 2.1 && nvn_fgpu_ratio(&r) > 1.9, b);
    }
    printf("== %d checks, %d failed ==\n", checks, fail);
    return fail != 0;
}
