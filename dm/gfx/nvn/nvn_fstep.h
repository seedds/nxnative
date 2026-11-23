// Frame step in whole vsyncs (pure function, shared by Mac / iOS; unit test tests/t_fstep.c). The rewriter's frame-step stubs (patch_nso.py FRAME_STEP: Tears of the Kingdom 1.4.3,
// Breath of the Wild 1.9.0) step the game by the vsyncs (1/60 s) the host hands them each frame, and the host keeps each frame on screen for k vsyncs (hle_nvn.c present_itv).
// At a steady rate the step is the time on screen: even motion, and only steps the games' own interval logic uses (60 / 30 / 20 fps). Real time the game falls behind (a late
// frame, a hitch) is a debt paid back one extra vsync per frame, so over time game time = real time. k follows the frames' cost: three late frames in a row step up to the cost
// (30 fps when 60 does not hold: no 40 fps steps on 120 Hz screens) when the frames at k also fall short of it on average (FSTEP_LATE over the last FSTEP_RUN frames); 30 frames
// in a row on time whose cost, or the mean cost of the hold's frames, would fit k − 1 try it, and a trial that fails waits twice as long before the next (1 s → 8 s), so a device on the edge does not flap.
// A trial that held is judged by its run as a probe is: a step up before the run lasts FSTEP_HOLD_MAX counts as a failed trial (an edge scene that steps up again within
// seconds went back and forth every few seconds with a trial wait of 1 s each time).
// The average: a frame late by FSTEP_LATE is the rate of a hold that does not hold (53 fps is not 60), but three such frames in a row among frames on time are not. Frames whose
// cost scatters around k (Mario Kart World on an M4 Max: 20–30% of them 1–5 ms late, 57.5–61 fps and game time 0.99–1.02 of real time with no holds) met three in a row about once
// a second and spent half the time at 30 or 20, up and down 30 times a session (issue #1823); a short hitch (pipeline compiles) stepped to 30 the same way. The late frames are
// paid back by the step, so only a rate that falls short of k is worth a longer hold. Where the step cannot pay them back (120 fps, where the game steps by its target: a late
// frame is slow motion) three late frames still raise the hold. The step goes to the cheapest of the three late frames' cost: a hitch as the third no longer meant 20 fps.
// The average leaves out the window's longest frame: a lone stall (43-68 ms of texture streaming or a pipeline built in play) among those near-misses filled two thirds of
// the limit and stepped to 30 for a second: four of the five rises in a few races (issue #1823, after the fix above). Two stalls in 30 frames still count one.
// A game takes steps up to its stub's largest (smax; Mario Kart World's interval logic stops at 2): k and the paid-back step stay within it, or the frames held longer than the
// game steps play in slow motion and the debt counted as paid is lost.
// A probe is a trial back to kmin that the cost does not justify: while the caller reports the CPU slowed for reasons other than heat (an iPhone's performance controller
// parks a game that waits out a long hold on the efficiency cores at half the clock, where its cost never fits k − 1 again), the hold-off alone starts one. Its first
// FSTEP_GRACE frames may be late without failing it, the time the controller needs to raise the clock for the shorter deadline; after them it is judged like a trial.
// A probe that holds leaves the hold-off as it is, and the run at kmin it starts must last FSTEP_HOLD_MAX (8 s) before the hold-off returns to 1 s: a shorter one counts as
// a failed probe, so a CPU that really cannot hold kmin is not probed every second.
// A step up after a run at kmin of FSTEP_HOLD_MAX keeps the cost that held there, and a cost at the longer hold of at most FSTEP_SAME times it starts a probe too: the scene
// costs what it did, so the step up came from a hitch (Breath of the Wild on an iPhone: pipeline compiles after a cutscene). The fit trial alone does not bring such a game
// back: an iPhone lowers its GPU clock until the work fills most of the frame, so a GPU-bound game's cost sits near FSTEP_FIT at either hold (BotW: GPU 12.8–15.2 ms at hold
// 2, p50, where hold 1 had held for minutes; the same 4,900 draws took 5.8 ms after a trip to the background and 13.5 ms half a minute later).
// A cap that falls and rises again (a movie, hle_movie.c dm_movie_hz; the iOS thermal cap in hle_nvn.c) keeps what was learned at each kmin: the cost that held it
// (cref) and the GPU reference (nvn_fgpu below); and the frames after the cap rises are a probe with its grace, as the clocks are still set for the longer hold. Before, the
// rise set k to kmin with no grace, so three frames late while the clock rose stepped up again, with cref dropped at the cap change and a GPU reference taken at the 30 fps
// cap: neither the fit trial (a GPU-bound cost near FSTEP_FIT) nor a probe brought kmin back (t_fstep: the movie and thermal cap cases). A cref kept from before the cap
// starts a probe only when the cost at the longer hold is within FSTEP_SAME of it, and a failed probe doubles the hold-off as any other does.
// The caller may report each frame's GPU time (gpu_on, gpu). A late frame whose GPU was idle (under FSTEP_GPU_IDLE: a loading or progress screen, the first frames after a
// pipeline prepare) does not count toward the 3 late frames of a step up: nothing is being drawn, so the lateness is the CPU's startup or load work, which the debt rule
// absorbs, not what the scene costs (FSTEP_RUN late frames in a row step up whatever the GPU). Ocarina of Time stepped 1 → 2 → 3 at presents 9 and 12 of a warm start (cost 1.92 and 5.57 vsyncs, GPU 0.02 and 0.01) while a 118 s archive write ran.
// And a longer hold that the GPU does not explain is probed: FSTEP_RUN frames in a row at a hold above kmin whose GPU time fits k − 1 and that waited for nothing in the
// host's pacing (cost within FSTEP_LATE of the time shown) start a probe after the hold-off. Such a game waits for its frames outside the window's acquire (Ocarina of
// Time: no pacing wait at any hold, its frame time all counted as work), so its cost is the hold itself whatever the hold is, never fits k − 1, and the fit trial never comes:
// it stayed at hold 3 (18 fps, work 50 ms, GPU 12.5 ms) for about 1,470 presents where hold 1 held at 16.6 ms before. A game whose CPU really needs the longer hold fails the
// probe; these probes have their own hold-off (uhold), doubled after each one fails up to FSTEP_UHOLD_MAX (60 s), so such a game pays one failed probe a minute at most
// and not one every 8 s. Late frames with the GPU idle still step up after FSTEP_RUN of them in a row: a CPU-heavy scene that draws almost nothing is not a load.
// Late frames at the longest hold allowed (kmax: 2 at a 30 fps cap, or kmin with fps_hold off) cannot step up. The caller's display link at the cap's rate (30 Hz) can then only
// show a late frame on its next tick, 66.7 ms after the one before: Ocarina of Time on an iPhone at the launcher's 30 fps cap (frames 36.5 ms, GPU 32 ms) left one tick in 9 empty,
// 33 intervals of 66.7 ms in each 300 and the rest 33.3, at 27 fps. So the same late frames that would step up (3 in a row, or FSTEP_RUN with the GPU idle among them) set fine:
// the caller then shows each frame at the next vsync after its hold instead (a 60 Hz link that holds each frame its k vsyncs), 33.3 or 50 ms and never 66.7, still never faster
// than the cap. fine ends after FSTEP_HOLD_MAX vsyncs with no such late frame (a scene that fits again; a late frame now and then does not flap the link), when the hold may rise
// again (the cap rose) and while the game fixes its own step.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#define FSTEP_MAX 3           // the largest step: both games' own interval logic uses 1–3 (BotW clamps to [2, 3], TOTK picks 2 or 3), so their integer readers never see 4
#define FSTEP_LATE 0.1        // a frame is late when it or its GPU work took this many vsyncs (1.7 ms) more than k: 53 fps is not 60 (start-to-start jitter on time is well below)
#define FSTEP_SHORT_MAX 2.0  // a frame counts at most this many vsyncs past its hold toward the average (a load's stall is one long frame, not a rate)
#define FSTEP_FIT 0.95        // a frame would fit k − 1 when its cost is at most this share of it. Hold 1 keeps frames of up to about 1.03 vsyncs (three late in a row and
                              // a mean past FSTEP_LATE step up), so a fit of 0.85 left 0.85–1.0 between the two: where the GPU clock barely follows the hold (Ocarina of
                              // Time on a base M5 and an M4 Pro, 15 ms at either hold) a scene there stayed at 30 for good after anything stepped it up (issue #2400)
#define FSTEP_RUN 30          // frames in a row that fit k − 1 before a trial, and frames a trial must hold
#define FSTEP_DEBT_MAX 15.0   // debt kept at most (0.25 s): the rest of a long stall (a load) is dropped, not replayed
#define FSTEP_HOLD0 60.0      // vsyncs since the last step up before a trial: 1 s, doubled after each failed trial up to 8 s
#define FSTEP_HOLD_MAX 480.0
#define FSTEP_GRACE 30        // a probe's first frames, whose lateness neither fails it nor counts toward a step up (the controller lowered the clock within 10 frames of a step to 30)
#define FSTEP_SAME 1.1        // the cost at the longer hold against the cost that held kmin: at most this much is the same scene (BotW at hold 2: 1.0–1.07 times its hold-1 GPU time)
// The grace is renewed (nvn_fstep.ramp) while the probe's frames keep getting cheaper: a controller that raises the clock slowly is still raising it. macOS raises the GPU clock
// of a game that misses its deadline at 85–100% GPU occupancy by about 100 MHz a second (Breath of the Wild on an M5 Pro: 338 → 432 MHz over a 33-frame probe, GPU 24.8 → 18.5 ms,
// then the probe failed at 22 ms frames; hold 1 needs about 560 MHz there), where the iPhone's controller settles within the 30 frames. A window of FSTEP_GRACE frames whose
// second half cost at most 1 − FSTEP_RAMP of its first half, and had a late frame (a window on time is judged as a trial), starts another, up to FSTEP_PROBE_MAX frames in all
#define FSTEP_RAMP 0.05       // the fall in cost from one half of a grace window to the next that shows the clock still rising (the per-frame noise of a 25 ms frame is 4%, of 15 frames 1%)
#define FSTEP_PROBE_MAX 180   // grace frames a probe may take in all (3 s at 60 fps): a clock that keeps rising without reaching the hold is not waited for longer
#define FSTEP_GPU_IDLE 0.05   // GPU time (in the frame's unit) under which nothing is being drawn: 0.8 ms at 60 fps (Ocarina of Time's progress screen 0.2–0.3 ms, its scenes 12 ms)
#define FSTEP_UHOLD_MAX 3600.0   // the longest hold-off before a probe of a hold the GPU does not explain (nvn_fstep_unpaced), doubled after each such probe fails: 60 s
// A stall is a frame that stayed up longer than any hold could explain: a pause (focus lost, Home), a covered window, a fullscreen switch, a silent display link. What it took
// is not what the scene costs, and the frames right after it run late for host reasons (GPU clocks fell while idle, display link resettling). So a stall is not counted as late,
// its pause time does not inflate hold-duration timers or the 30-frame average, and the FSTEP_GRACE frames after it do not count toward a step up, fine mode, or trial completion
// (another stall restarts the grace). When the caller names the disruption (nvn_fstep.settle: focus regained, graphics resumed, window shown/resized/fullscreen), the frame is a
// stall whatever it took (covering mildly late window-animation frames) and its excess time beyond the hold is dropped rather than added to debt (a pause is not time the game owes).
#define FSTEP_STALL 8.0       // vsyncs a frame was shown for that make it a stall (133 ms): more than twice the longest hold, so no step up would have made the frame fit
// Ticks of 1/120 s handed to the game in any count (nvn_fstep.t120: a FRAME_STEP_120 stub, patch_nso.py state +0x38): kmin is 2 at 60 fps, 3 at 40 and 4 at 30
#define FSTEP_KMIN_MAX 4      // the largest kmin: 30 fps in ticks of 1/120 s
#define FSTEP_OWN (FSTEP_KMIN_MAX + 1)   // nvn_fstep.kmin while the game sets its own step (BotW's VFR override)
#define FSTEP_HOLD_TOP (2 * FSTEP_MAX)   // the longest hold in either unit: FSTEP_MAX vsyncs, twice that in ticks of 1/120 s (nvn_fgpu's holds)

typedef struct {
    int k, step;              // vsyncs each frame stays on screen; the step handed to the game for the next frame (k, or k + 1 while paying back)
    int kmin;                 // the last call's kmin (a lower one takes effect at once)
    double debt;              // real time the game is behind, in vsyncs
    int late_run, fit_run, trial;   // late frames in a row; frames in a row whose cost fits k − 1; frames left of a trial at k − 1 (0 = none)
    int sgrace;               // frames left of the grace after a stall (FSTEP_STALL or settle), whose lateness does not count toward a step up, fine mode, or trial completion
    int probe, grace;         // 1: the trial in progress is a probe; 2: a probe held and its run at kmin has not ended yet; frames left of the probe's grace
    int ramp;                 // set by the caller (kept): 1 = a probe's grace is renewed while its frames keep getting cheaper (FSTEP_RAMP; the host's DM_FSTEP_PROBE_RAMP)
    double pa, pb; int plate, ptotal;   // the current grace window: the cost of its first and second halves (sums), its late frames; grace frames this probe has taken in all
    double hold, since;       // vsyncs a trial waits for after a step up; vsyncs since the last step up
    int tpend; double tsince; // a trial that held and whose run at its hold has not lasted FSTEP_HOLD_MAX yet (a step up before then fails it), and vsyncs since it held
    int per_vsync;            // set by the caller: the ticks a 1/60 s vsync is (0 = 1). At 120 fps the unit is 1/120 s (hle_nvn.c fps_high), and the waits between trials stay the same real time
    int t120;                 // set by the caller: the unit is a tick of 1/120 s that the game takes in any count (FRAME_STEP_120; implies per_vsync 2). Every vsync-scaled margin and limit
                              // doubles so real-time thresholds stay the same; at an even kmin (60 / 30 fps) holds move by whole vsyncs (2 ticks), and at an odd one (40 fps: 3 ticks, shown
                              // only on a 120 Hz link) by one tick: 3 or 4, never 5. A payback step is one tick more than the hold (game time only: the stub's ratio is a float)
    double shown_slack;       // vsyncs a frame on time may show longer than k (set by the caller): one refresh on screens whose rate is not a multiple of 60; the cost keeps FSTEP_LATE
    int settle;               // set by the caller before a call, then cleared on every return: this frame spans a host-known pause or window change (a stall whatever it took; excess beyond hold is not debt)
    int screen;               // set by the caller before each call, then cleared on every return: 0 = judge lateness by shown and cost; 1 = the screen showed a recent frame on time, 2 = late. With present
                              // snapshots (nvn_mtl.m) the game runs ahead of the screen and a frame that runs long is absorbed, so only what the screen shows counts
    double run, crun, cref;   // vsyncs at kmin in a row; the cost of its frames on time (running mean; not during a probe's trial, whose frames run while the clock rises);
                              // crun when the last run of FSTEP_HOLD_MAX or more ended in a step up or a cap change (0 = none yet)
    double ccur; int ck, cn;  // the cost of the frames on time at the longer hold ck (running mean) and their count
    int gpu_on; double gpu;   // set by the caller before each call, then cleared: 1 = gpu is the frame's GPU time (the latest finished frame's, in the unit of shown); 0 = not reported
    int urun;                 // frames in a row at a hold above kmin whose GPU time fits k − 1 and that waited for nothing in the host's pacing (nvn_fstep_unpaced)
    int ilate_run, uprobe;    // late frames in a row, those with the GPU idle included; 1 = the probe running was started by nvn_fstep_unpaced alone
    int up_idle;              // 1 = the last step up came from FSTEP_RUN late frames with the GPU idle among them (for the caller's log)
    double uhold;             // vsyncs since the last step up before such a probe: 1 s, doubled after each one fails up to FSTEP_UHOLD_MAX, back to 1 s when kmin holds
    int ckmin;                // the kmin run, crun and cref belong to
    double cref_at[FSTEP_KMIN_MAX + 1];   // cref of each kmin, kept while another kmin is in force
    double over[FSTEP_RUN], over_sum; int over_i, over_k;   // the last FSTEP_RUN frames at the hold over_k: shown − k of each (clamped), their sum; the next slot
    double last_cost[3];      // the last three frames' costs, oldest first
    int smax;                 // set by the caller: the largest step the game takes (its stub's STEP_MAX; 0 = FSTEP_MAX)
    int no_payback;           // set by the caller: 1 = the game does not take the step (120 fps: it steps by its target), so three late frames raise the hold whatever the average
    int fine; double fine_ok; // 1 = frames are late at kmax, shown at the next vsync after their hold; vsyncs since the last late frame while fine (FSTEP_HOLD_MAX ends it)
    uint64_t late, paid, ups, downs, failed, probes, fines, stalls; double dropped;   // statistics for the log (fines: times fine was set; stalls: pauses, window changes, or >= FSTEP_STALL)
} nvn_fstep;

// A drain of frames standing in the display link's queue in a frame's time (hle_nvn.c fstep_tick, g_fs_drain; nvn_link_drain.h): the acquire waited extra link ticks while the
// screen showed every queued frame for its hold, so the part that made the frame longer than its hold is neither a late frame nor time the game owes: it comes out of the
// frame's time. (The wait for the screen without the link is not one: there a frame longer than its hold is one the screen showed late.) span: time since the previous frame's
// start; hold: the hold the frame had; first: drain in the acquire that started this frame, part of *pacing (its pacing wait); before: drain in later acquires of the previous
// frame, in span but not in *pacing. Returns span less the cut; *pacing loses the cut's part in it (the frame's CPU time, span − pacing, keeps what it was, less the later
// acquires' wait, which was no work either). All in the same unit (ns).
static inline uint64_t nvn_fstep_drain_cut(uint64_t span, uint64_t hold, uint64_t first, uint64_t before, uint64_t *pacing) {
    if (first > *pacing) first = *pacing;
    uint64_t over = span > hold ? span - hold : 0, cut = first + before < over ? first + before : over;
    *pacing -= cut < first ? cut : first;
    return span - cut;
}

// Whether the cost at the longer hold is the cost that held kmin (a probe without a slowed report)
static inline int nvn_fstep_same(const nvn_fstep *s) { return s->cref > 0 && s->cn >= FSTEP_RUN && s->ccur <= FSTEP_SAME * s->cref; }
// Whether the longer hold is one the GPU does not explain while the frames' time is all the game's own (a probe without a slowed report)
static inline int nvn_fstep_unpaced(const nvn_fstep *s) { return s->urun >= FSTEP_RUN; }

// Once per frame. shown: real time since the previous frame's start, in vsyncs; cost: the frame's work, max(CPU, GPU), in vsyncs (a GPU that cannot keep up makes frames late too); kmin: 1 at a 60 fps cap, 2 at 30;
// kmax: the longest hold (FSTEP_MAX; 2 at a 30 fps cap, where the display link runs at 30 Hz and holds only even counts); fixed: a step the game has set itself (BotW's
// VFR override; 0 = none), which the host then holds as it is; slow: the CPU runs below its speed for reasons other than heat, so a cost above k − 1 is no evidence
// against kmin (probes). Returns the step for the next frame; s->k is how long that frame stays on screen. With s->t120 every count here is in ticks of 1/120 s
static inline int nvn_fstep_next(nvn_fstep *s, double shown, double cost, int kmin, int kmax, int fixed, int slow) {
    const int u = s->t120 ? 2 : 1;   // ticks per vsync of the margins and limits when t120 is on (120 fps keeps u = 1 with per_vsync = 2)
    const int pv = s->per_vsync ? s->per_vsync : u;
    const double hold0 = FSTEP_HOLD0 * pv, hold_max = FSTEP_HOLD_MAX * pv, uhold_max = FSTEP_UHOLD_MAX * pv;
    const double late_by = FSTEP_LATE * u, short_max = FSTEP_SHORT_MAX * u, debt_max = FSTEP_DEBT_MAX * u, gpu_idle = FSTEP_GPU_IDLE * u, ceil_slack = 0.05 * u;
    const int smax = (s->smax > 0 && s->smax < FSTEP_MAX ? s->smax : FSTEP_MAX) * u;
    if (kmin < 1) kmin = 1;
    if (kmin > FSTEP_KMIN_MAX) kmin = FSTEP_KMIN_MAX;
    if (kmin > smax) kmin = smax;
    const int q = s->t120 && kmin % 2 == 0 ? 2 : 1;   // the ticks a hold moves by (whole vsyncs at 60 / 30 fps, one tick at 40)
    if (kmax > smax) kmax = smax;
    if (kmax < kmin) kmax = kmin;
    if (!s->hold) s->hold = hold0;
    if (!s->uhold) s->uhold = hold0;
    int gpu_on = s->gpu_on; double gpu = s->gpu; s->gpu_on = 0; s->gpu = 0;   // cleared on every return
    int settle = s->settle; s->settle = 0;
    int scr = s->screen; s->screen = 0;
    if (fixed > 0) {   // the game's own step: speed is right by construction, nothing to pay back
        s->k = s->step = fixed < smax ? fixed : smax;
        s->debt = 0; s->late_run = s->ilate_run = s->fit_run = s->trial = s->probe = s->grace = s->sgrace = s->tpend = 0; s->run = 0; s->fine = 0;
        s->kmin = FSTEP_OWN;   // when it ends, k starts again from kmin
        return s->step;
    }
    if (!s->k) { s->k = s->step = s->kmin = kmin; return s->step; }   // first frame: nothing measured yet
    int held = s->k;   // the frame that just ended stayed on screen this long
    int stall = settle || shown >= FSTEP_STALL * pv, sgrace = stall || s->sgrace > 0;   // a stall/window change, or a frame in the grace after one
    if (stall) { s->sgrace = FSTEP_GRACE; s->stalls++; } else if (s->sgrace) s->sgrace--;
    if (held != s->over_k) { s->over_k = held; s->over_sum = 0; s->over_i = 0; memset(s->over, 0, sizeof s->over); }   // judged against the hold in force, not the frames before a step
    double over = stall ? 0 : shown - held; if (over < -u) over = -u; if (over > short_max) over = short_max;
    s->over_sum += over - s->over[s->over_i]; s->over[s->over_i] = over; s->over_i = (s->over_i + 1) % FSTEP_RUN;
    if (!stall) { s->last_cost[0] = s->last_cost[1]; s->last_cost[1] = s->last_cost[2]; s->last_cost[2] = cost; }
    double dt = stall && shown > held ? held : shown;   // pause or stall time is not elapsed gameplay at this hold
    if (settle && shown > held) { s->dropped += shown - held; s->debt += held - s->step; }   // a pause or window change is not time the game owes
    else s->debt += shown - s->step;
    if (s->debt > debt_max) { s->dropped += s->debt - debt_max; s->debt = debt_max; }
    if (s->debt < -u) s->debt = -u;   // ahead (a frame shorter than its step): nothing slows the game down, so at most one vsync is remembered
    s->since += dt; s->tsince += dt;
    if (s->tpend && s->tsince >= hold_max) { s->tpend = 0; s->hold = hold0; }   // the trial's run lasted
    int cap_probe = kmin < s->kmin && s->kmin <= FSTEP_KMIN_MAX && s->k > kmin;
    if (cap_probe) {   // the cap rose: a probe at once, whose grace covers the clocks still set for the longer hold
        s->k = kmin; s->probes++; s->trial = FSTEP_RUN; s->probe = 1; s->uprobe = 0; s->grace = FSTEP_GRACE; s->fit_run = s->late_run = s->ilate_run = 0; s->pa = s->pb = 0; s->plate = s->ptotal = 0;
    } else if (kmin < s->kmin || s->k < kmin) s->k = kmin;   // the game's own step ended, or the cap fell
    if (s->k > kmax) s->k = kmax;
    s->kmin = kmin;
    int idle = gpu_on && gpu < gpu_idle;   // nothing drawn: a late frame is load or startup work, not the scene's cost
    int late = scr ? scr == 2 : shown > held + late_by + s->shown_slack || cost > held + late_by, grace = s->grace > 0;
    if (grace && !cap_probe && !stall) {   // the completed frame at the old cap (or a stall) does not consume or skew the new hold's grace
        if (s->grace > FSTEP_GRACE / 2) s->pa += cost; else s->pb += cost;
        s->plate += late; s->ptotal++;
        if (!--s->grace && s->ramp && s->plate && s->ptotal + FSTEP_GRACE <= FSTEP_PROBE_MAX && s->pb <= (1 - FSTEP_RAMP) * s->pa) { s->grace = FSTEP_GRACE; s->pa = s->pb = 0; s->plate = 0; }   // still getting cheaper: another window
    }
    // A late frame with the GPU idle neither counts toward the 3 nor ends their run; FSTEP_RUN late frames in a row step up whatever the GPU (a CPU-heavy scene, not a load)
    if (late) { if (!stall) s->late++; s->late_run = grace || sgrace ? 0 : s->late_run + !idle; s->ilate_run = grace || sgrace ? 0 : s->ilate_run + 1; s->fit_run = 0; } else s->late_run = s->ilate_run = 0;
    // The frame waited for nothing in the host's pacing while held longer than kmin, and its GPU time fits a shorter hold: the hold is not what the GPU needs (nvn_fstep_unpaced)
    // (k − q, not kmin: the GPU clock falls at a longer hold, so a GPU that fits kmin there may read above it; a GPU that really needs the hold fails the probe)
    s->urun = held > kmin && s->k == held && gpu_on && gpu <= FSTEP_FIT * (held - q) && cost >= shown - late_by ? s->urun + 1 : 0;
    // The cost of the frames on time (a hitch is not what the scene costs): at kmin, and at the longer hold
    if (kmin != s->ckmin) {   // another cap: its run starts over, and what held it before comes back
        if (s->ckmin >= 1 && s->ckmin <= FSTEP_KMIN_MAX) {
            // A cap can end a sustained run before any hitch saved its cost. Short runs keep the earlier proven reference.
            if (s->run >= hold_max && s->crun > 0) s->cref = s->crun;
            s->cref_at[s->ckmin] = s->cref;
        }
        s->ckmin = kmin; s->run = s->crun = 0; s->cref = s->cref_at[kmin];
    }
    // A frame the screen showed on time while its cost did not fit (absorbed by present snapshots) is not what the hold costs: the probes would take it for the scene that held kmin
    int fits = !scr || cost <= held;
    if (held <= kmin) { s->run += dt; s->cn = 0; if (!late && !stall && fits && s->probe != 1) s->crun = s->crun > 0 ? s->crun + (cost - s->crun) / 32 : cost; }
    else {
        s->run = 0;
        if (held != s->ck) { s->ck = held; s->cn = 0; }
        if (!late && !stall && fits) { s->ccur = s->cn ? s->ccur + (cost - s->ccur) / 8 : cost; s->cn++; }
    }
    // A rate below k over the last FSTEP_RUN frames (the slots not filled yet since the hold changed count as on time); the screen's verdict (present snapshots) is its own.
    // The longest frame of the window is left out: one stall is not a rate. Counted at FSTEP_SHORT_MAX it was two thirds of the limit, and with near-misses at 57 fps
    // around it the next three late frames stepped to 30 for a second (Mario Kart World on an M4 Max, 43-68 ms stalls; issue #1823)
    double omax = 0; for (int i = 0; i < FSTEP_RUN; i++) if (s->over[i] > omax) omax = s->over[i];
    int short_of_k = scr || s->no_payback || s->over_sum - omax > late_by * FSTEP_RUN;
    if ((s->late_run >= 3 || s->ilate_run >= FSTEP_RUN) && s->k < kmax && short_of_k) {   // step up to the cost of the cheapest of the three late frames, at least one vsync (q ticks)
        double c3 = s->last_cost[0] < s->last_cost[1] ? s->last_cost[0] : s->last_cost[1]; if (s->last_cost[2] < c3) c3 = s->last_cost[2];
        int want = (int)ceil(c3 - ceil_slack);
        if (want < s->k + q) want = s->k + q;
        want = (want + q - 1) / q * q;
        if (want > kmax) want = kmax;
        if (s->trial || s->tpend || (s->probe == 2 && s->since < hold_max)) {
            s->failed++; s->trial = 0; s->hold = s->hold * 2 < hold_max ? s->hold * 2 : hold_max;
            if (s->uprobe) s->uhold = s->uhold * 2 < uhold_max ? s->uhold * 2 : uhold_max;   // a CPU that needs the hold: probed ever more rarely
        } else if (s->probe == 2) s->hold = s->uhold = hold0;   // the probe's run at kmin lasted
        if (held == kmin && s->run >= hold_max) { s->cref = s->crun; s->uhold = hold0; }   // kmin held 8 s at this cost
        s->up_idle = s->late_run < 3;
        s->k = want; s->ups++; s->late_run = s->ilate_run = s->fit_run = s->urun = 0; s->since = 0; s->probe = s->uprobe = 0; s->tpend = 0;
    } else if (s->trial && !grace && !sgrace && !--s->trial) { if (s->probe) { s->probe = 2; s->since = 0; } else { s->tpend = 1; s->tsince = 0; } }   // the trial held: its run is judged by how long it lasts
    // A frame also fits k − q while the running mean of this hold's frames on time does: single frames over the fit among frames under it are
    // what the shorter hold absorbs, and they kept the trial from ever starting (Ocarina of Time on a Mac after a load's late frames: 13.4 ms of GPU work at hold 2 with one
    // frame in 20 over 14.2 ms, 30 fps for the rest of the scene where 60 had held; 22 ms at hold 3 under a lowered GPU clock with frames of 29–44 ms, 20 fps for 25 s)
    double fitc = s->ck == s->k && s->cn && s->ccur < cost ? s->ccur : cost;
    // Late at the longest hold allowed: shown at the next vsync, as the hold cannot rise (fine above)
    if (s->k >= kmax && (s->late_run >= 3 || s->ilate_run >= FSTEP_RUN)) { if (!s->fine) s->fines++; s->fine = 1; s->fine_ok = 0; }
    else if (s->fine) { if (!sgrace) s->fine_ok = late && !idle ? 0 : s->fine_ok + dt; if (s->k < kmax || s->fine_ok >= hold_max) s->fine = 0; }
    if (!late) s->fit_run = s->k > kmin && fitc <= FSTEP_FIT * (s->k - q) ? s->fit_run + 1 : 0;
    if (s->fit_run >= FSTEP_RUN && s->since >= s->hold && !s->trial) { s->k -= q; s->downs++; s->trial = FSTEP_RUN; s->fit_run = 0; }
    else if ((slow || nvn_fstep_same(s) || (nvn_fstep_unpaced(s) && s->since >= s->uhold)) && s->k > kmin && s->since >= s->hold && !s->trial) {   // probe: straight to kmin, so the controller sees the whole deadline
        s->uprobe = !slow && !nvn_fstep_same(s);
        s->k = kmin; s->downs++; s->probes++; s->trial = FSTEP_RUN; s->probe = 1; s->grace = FSTEP_GRACE; s->fit_run = s->late_run = s->ilate_run = 0; s->pa = s->pb = 0; s->plate = s->ptotal = 0;   // urun ends with the next frame at kmin (kept for the caller's log)
    }
    s->step = s->k + (s->debt >= 1 && s->k < smax);
    s->paid += s->step > s->k;
    return s->step;
}

// The GPU clock follows the hold too: Breath of the Wild on an iPhone went from 14.4 to about 25 ms of GPU time a frame at hold 2 with the same 4,900 draws, the CPU still on its
// performance cores, so its cost never fitted hold 1 again. The GPU counts as slowed (another reason for nvn_fstep_next's slow) when its time at a hold above kmin is at least
// FSTEP_GPU_SLOW times its time at kmin and it alone does not fit kmin (a GPU at 3 ms of 17, 6 times its time on a loading screen, is not what holds a frame back). Fitting
// k − 1 is not enough: at hold 3 that sent a slowed GPU on a fit trial to hold 2, whose ratio is not learned yet, and it stayed there where the probe went to 60.
// The reference is not updated during a probe's trial (its frames run while the clock is still rising), so a failed probe leaves it as it was. It is kept for each kmin: one
// learned at a 30 fps cap is the GPU time at the clock that cap left (lowered for 30 fps, and under the thermal cap for the heat), and taken as hold 1's after the cap rose it
// made the same work at hold 2 look not slowed.
// The reference is another scene's time, though: the title screen's, or the light view's before a heavy one stepped up. A scene whose GPU time at kmin's own clock does not fit
// kmin was called slowed against it, and each probe failed after its grace (Ocarina of Time: 22 failed probes a minute in Hyrule Field at night on an M2 Pro, 159 frames at
// 20 fps, with the title screen's reference; Kokiri Forest at every turn of the camera on an Android port). A failed probe measures what the reference cannot: the scene's
// GPU time at kmin once the clock rose for it (post: the frames from the second half of its grace on, which nvn_fstep.ramp keeps while they still get cheaper), against its
// time at the longer hold before the probe and again after it: how much the longer hold slows the GPU here (ratio_at, for kmin and each hold). That time is the smaller of the
// running means of 8 and of 32 frames, and of the two where both are the same hold: a turn of the camera to or from a heavy view makes one of them another scene's (Kokiri
// Forest at 1.6 times the GPU time at hold 3: a ratio of 2.15 where the hold-3 stretch began in the heavy view, and the light view's 2.0 at hold 3 started a probe again). Once FSTEP_GPU_FAILS failed probes have given a hold's ratio, the GPU counts as slowed there when cur over it, the scene's time at kmin, fits kmin, in place
// of the reference: the scene that failed starts no probe while it costs what it did, and a lighter one starts one even when its cur is not FSTEP_GPU_SLOW times a reference
// learned in a heavier scene (a gate on the reference itself, which kmin's frames relearn only after a probe, would keep that lighter scene at the longer hold).
// The ratio is the largest the failed probes gave, and one probe alone does not set it: a probe can fail before the clock is at its top (the iOS thermal cap's end, where it
// took 1 s), which reads as a ratio too small, and one too small keeps a scene that fits kmin at the longer hold, where one too large costs a failed probe now and then, as
// before the ratio, spaced out by the hold's doubling. A probe that holds gives none: it needs no gate, and its frames may be a lighter scene (a menu) than the hold's
#define FSTEP_GPU_SLOW 1.3    // GPU time at the longer hold against its time at kmin
#define FSTEP_GPU_N 30        // frames at the longer hold before the comparison counts
#define FSTEP_GPU_PN 3        // a probe's frames at kmin after the first half of its grace before it gives a ratio
#define FSTEP_GPU_FAILS 2     // failed probes whose ratio a hold needs before the ratio decides
#define FSTEP_GPU_RMIN 0.75   // the smallest ratio a probe gives: hold 1 can take a little longer than the longer hold (Ocarina of Time on an M4 Pro, whose clock barely moves:
                              // 1.03–1.09 vsyncs at hold 1 after the grace, 0.99–1.00 at hold 2; with the ratio taken as 1 the estimate, 1.00, kept starting probes). Below it the
                              // probe's frames were another scene or a stall, and it gives none (a load's stall failed probes there at GPU 4.4 and 5.7 vsyncs, a ratio of 0.15)
typedef struct {
    double ref, cur, avg; int n, k, kmin; double ref_at[FSTEP_KMIN_MAX + 1];   // GPU time at kmin (running mean; of every kmin in ref_at); at the current hold k (of 8 frames, of 32) and its frame count; kmin of the last frame
    double pre, post; int pre_k, pn, was, fail;   // the last probe: the time at the hold pre_k it started from (0: none to compare), its frames' GPU time at kmin and their count; nvn_fstep.probe last
                                                  // frame; it failed, and its ratio waits for FSTEP_GPU_N frames at the hold after it
    double ratio_at[FSTEP_KMIN_MAX + 1][FSTEP_HOLD_TOP + 1]; int ratio_n[FSTEP_KMIN_MAX + 1][FSTEP_HOLD_TOP + 1];   // [kmin][k] (in ticks of 1/120 s with nvn_fstep.t120): GPU time at hold k against kmin's, the largest of the failed probes', and their count
} nvn_fgpu;
// The hold's ratio when it decides (0: the reference does)
static inline double nvn_fgpu_ratio(const nvn_fgpu *g) {
    return g->kmin >= 1 && g->kmin <= FSTEP_KMIN_MAX && g->k >= 1 && g->k <= FSTEP_HOLD_TOP && g->ratio_n[g->kmin][g->k] >= FSTEP_GPU_FAILS ? g->ratio_at[g->kmin][g->k] : 0;
}
static inline int nvn_fgpu_slow(const nvn_fgpu *g) {
    if (g->n < FSTEP_GPU_N || g->cur <= FSTEP_FIT * g->kmin) return 0;
    double r = nvn_fgpu_ratio(g);
    return r > 0 ? g->cur / r <= g->kmin : g->ref > 0 && g->cur >= FSTEP_GPU_SLOW * g->ref;
}
static inline void nvn_fgpu_learn(nvn_fgpu *g, int kmin, int k, double t) {
    double r = t / g->post, *at = &g->ratio_at[kmin][k];
    if (r < FSTEP_GPU_RMIN) return;
    if (r > *at) *at = r;
    g->ratio_n[kmin][k]++;
}
// Once per frame, with the frame's GPU time (vsyncs, or ticks with nvn_fstep.t120; 0 = none finished yet) and the step's state the frame ran under (its hold, probe and grace)
static inline int nvn_fstep_gpu_slowed(nvn_fgpu *g, double gpu, const nvn_fstep *s, int kmin) {
    if (kmin < 1 || kmin > FSTEP_KMIN_MAX) return 0;   // no reference slot for it (hle_nvn.c passes 1 or 2, in ticks of 1/120 s 2 to 4)
    int k = s->k ? s->k : kmin, probe = s->probe;
    if (kmin != g->kmin) g->fail = 0;   // another cap: the clocks the probe measured are not this one's
    if (probe == 1 && g->was != 1) { g->pre_k = g->kmin == kmin && g->k > kmin && g->k <= FSTEP_HOLD_TOP && g->n >= FSTEP_GPU_N ? g->k : 0; g->pre = g->cur < g->avg ? g->cur : g->avg; g->post = 0; g->pn = 0; g->fail = 0; }   // a probe from hold g->k
    else if (probe != 1 && g->was == 1) g->fail = !probe && k > kmin && g->pn >= FSTEP_GPU_PN;   // it ended: failed when the hold went up again
    g->was = probe;
    if (probe == 1 && k <= kmin && gpu > 0 && s->grace <= FSTEP_GRACE / 2) { g->post = g->pn ? g->post + (gpu - g->post) / 4 : gpu; g->pn++; }   // weighted to the last frames: the clock at its highest
    g->ref = g->ref_at[kmin]; g->kmin = kmin;
    if (gpu <= 0) return nvn_fgpu_slow(g);
    if (k <= kmin) { if (probe != 1) g->ref = g->ref_at[kmin] = g->ref > 0 ? g->ref + (gpu - g->ref) / 32 : gpu; g->n = 0; g->k = k; g->fail = 0; return 0; }
    if (k != g->k) { g->k = k; g->n = 0; }
    g->n++; g->cur = g->n > 1 ? g->cur + (gpu - g->cur) / 8 : gpu; g->avg = g->n > 1 ? g->avg + (gpu - g->avg) / (g->n < 32 ? g->n : 32) : gpu;
    if (g->fail && g->n >= FSTEP_GPU_N && k <= FSTEP_HOLD_TOP) {   // the failed probe's ratios: this hold's after it, and the hold it started from (the smaller time where they are one)
        double t = g->cur < g->avg ? g->cur : g->avg;
        if (k == g->pre_k) nvn_fgpu_learn(g, kmin, k, g->pre < t ? g->pre : t);
        else { nvn_fgpu_learn(g, kmin, k, t); if (g->pre_k) nvn_fgpu_learn(g, kmin, g->pre_k, g->pre); }
        g->fail = 0;
    }
    return nvn_fgpu_slow(g);
}

// The frame step state page's target (+0 float fps, 0 = every stub runs the game's own instructions) and step unit (+0x38 u32: 120 = ticks of 1/120 s for FRAME_STEP_120 stubs,
// 0 = vsyncs of 1/60 s; written by loader.c dm_fps_state_apply and fixed for the run)
#define FSTEP_UNIT_OFF 0x38
static inline float nvn_fstep_target(const uint8_t *page) { uint32_t b = __atomic_load_n((const uint32_t *)page, __ATOMIC_RELAXED); float t; memcpy(&t, &b, sizeof t); return t; }
static inline void nvn_fstep_target_put(uint8_t *page, float t) { uint32_t b; memcpy(&b, &t, sizeof b); __atomic_store_n((uint32_t *)page, b, __ATOMIC_RELAXED); }
static inline int nvn_fstep_t120(const uint8_t *page) { return page && __atomic_load_n((const uint32_t *)(page + FSTEP_UNIT_OFF), __ATOMIC_RELAXED) == 120; }
// The cap the frame step holds for a target: 0 (off) for target 0; during a movie its picture rate decides (60 for 50+ fps, else 30, thermal cap aside);
// otherwise 30 for target <= 30 or under the thermal cap, 40 for target <= 40 (caller still checks FRAME_STEP_120 and screen rate), else 60
static inline int nvn_fstep_cap(float target, int movie_hz, int thermal) {
    if (target <= 0) return 0;
    if (movie_hz) return movie_hz >= 50 && !thermal ? 60 : 30;
    return target <= 30 || thermal ? 30 : target <= 40 ? 40 : 60;
}
