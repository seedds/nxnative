// The output limiter (hle_audren.c limit_stereo) on synthetic int32 stereo at 48 kHz: a mix under the ceiling is left bit-identical (a quiet game is untouched), a mix over it ends under it with no help from the
// final clamp, its peaks are turned down before they reach the ceiling (no flat tops: the 1 ms lookahead), both channels are scaled alike and keep their signs, and the gain comes back to exactly 1.0 after the overshoot, from where the output is bit-identical again.
#include "../hle.h"
#include <assert.h>
#include "../hle_audren.c"

void *guest_sym(const char *name) { (void)name; return NULL; }
void *objmap_get(const void *key) { (void)key; return NULL; }
void objmap_put(const void *key, void *value) { (void)key; (void)value; }
void *objmap_del(const void *key) { (void)key; return NULL; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack) { (void)fn; (void)arg; (void)prio; (void)stack; return NULL; }
void dm_guest_thread_join(void *tt) { (void)tt; }
uint64_t now_ns(void) { return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void *dm_pcmout_open(int ch) { (void)ch; return NULL; }
void dm_pcmout_close(void *k) { (void)k; }
void dm_pcmout_start(void *k) { (void)k; }
int dm_pcmout_append(void *k, void *b) { (void)k; (void)b; return 0; }
void *dm_pcmout_released(void *k) { (void)k; return NULL; }
void dm_pcmout_wait(void *k, int ms) { (void)k; (void)ms; }
void hle_bind_system_event(void *ev, int ac) { (void)ev; (void)ac; }
uint32_t hle_dup_readable_handle(uint32_t h) { return h; }

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "t_audren_limit: %s:%d: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)
enum { RATE = 48000, BLOCK = 240, CEIL = 32767 };   // the renderer's 5 ms block
static const double TAU = 6.283185307179586;

// A stereo sine: amplitudes al / ar on the left / right, f Hz, frames from sample index t0
static void sine(int32_t *st, uint32_t n, uint32_t t0, double al, double ar, double f) {
    for (uint32_t s = 0; s < n; s++) { double x = sin(TAU * f * (t0 + s) / RATE); st[2 * s] = (int32_t)lrint(al * x); st[2 * s + 1] = (int32_t)lrint(ar * x); }
}
static double db(double x) { return pow(10.0, x / 20.0); }

// Runs total frames through the limiter in 5 ms blocks as the renderer does; in[] is kept, out[] is the result
static void run(lim_t *lim, const int32_t *in, int32_t *out, uint32_t total) {
    memcpy(out, in, (size_t)total * 8);
    for (uint32_t s = 0; s < total; s += BLOCK) limit_stereo(lim, out + 2 * s, total - s < BLOCK ? total - s : BLOCK, RATE);
}

static int test_under_ceiling(void) {   // gain at 1.0 and no sample frame over the ceiling: the output is the input, whole-scale peaks included
    enum { N = RATE };
    static int32_t in[2 * N], out[2 * N]; lim_t lim = {0};
    sine(in, N, 0, CEIL, CEIL * 0.5, 997);
    in[2 * 100] = CEIL; in[2 * 100 + 1] = -CEIL;
    run(&lim, in, out, N);
    CHECK(!memcmp(in, out, sizeof in), "a mix under the ceiling changed");
    CHECK(lim.dip == 0 && lim.left == 0, "the gain left unity: dip %g, hold %u", lim.dip, lim.left);
    return 0;
}

static int test_burst(void) {   // +4 dB, with the left channel louder than the right: every output under the ceiling, ratio and signs kept, and the limiter works (the peak stays near the ceiling)
    enum { N = RATE / 2 };
    static int32_t in[2 * N], out[2 * N]; lim_t lim = {0};
    sine(in, N, 0, CEIL * db(4), CEIL * db(4) * 0.3, 220);
    run(&lim, in, out, N);
    int32_t peak = 0;
    for (uint32_t s = 0; s < N; s++) {
        int32_t il = in[2 * s], ir = in[2 * s + 1], ol = out[2 * s], or_ = out[2 * s + 1];
        CHECK(abs(ol) <= CEIL && abs(or_) <= CEIL, "frame %u: %d %d is over the ceiling (the clamp would act)", s, ol, or_);
        CHECK((int64_t)ol * il >= 0 && (int64_t)or_ * ir >= 0, "frame %u: sign flip %d -> %d, %d -> %d", s, il, ol, ir, or_);
        // one gain for both channels: out = g * in + a rounding error of at most 0.5 each, so the cross product differs from zero by 0.5 * (|il| + |ir|) at most
        CHECK(llabs((int64_t)ol * ir - (int64_t)or_ * il) <= ((int64_t)abs(il) + abs(ir)) / 2 + 1, "frame %u: ratio %d:%d became %d:%d", s, il, ir, ol, or_);
        if (abs(ol) > peak) peak = abs(ol);
    }
    CHECK(peak >= CEIL - 2, "the limiter over-attenuates: output peak %d", peak);
    // the same burst unlimited would have been clamped: the input is over the ceiling in a good share of the frames
    uint32_t over = 0; for (uint32_t s = 0; s < N; s++) over += abs(in[2 * s]) > CEIL;
    CHECK(over > N / 10, "the burst is not over the ceiling (%u frames)", over);
    return 0;
}

static int test_extremes(void) {   // values far past int16, both channels' signs, silence between: no sign flip, no overflow, and the next quiet frame's gain is still below 1
    static const int32_t cases[][2] = {{2000000000, -1000000000}, {-2147483647 - 1, 5}, {32768, -32768}, {0, 40000}, {-33000, 0}, {0, 0}};
    lim_t lim = {0};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        int32_t st[2] = {cases[i][0], cases[i][1]};
        limit_stereo(&lim, st, 1, RATE);
        CHECK(abs(st[0]) <= CEIL && abs(st[1]) <= CEIL, "case %u: %d %d", i, st[0], st[1]);
        CHECK((int64_t)st[0] * cases[i][0] >= 0 && (int64_t)st[1] * cases[i][1] >= 0, "case %u: sign flip", i);
        CHECK(lim.dip >= 0 && lim.dip < 1, "case %u: dip %g", i, lim.dip);
    }
    return 0;
}

static int test_release(void) {   // after an overshoot the gain is back above 0.99 within 300 ms of quiet, back at exactly 1.0 within a second, and the output is then the input again
    enum { BURST = RATE / 10, QUIET = RATE + BLOCK, N = BURST + QUIET };   // the last block is the one to compare
    static int32_t in[2 * N], out[2 * N]; lim_t lim = {0};
    sine(in, BURST, 0, CEIL * db(4), CEIL * db(4), 220);
    sine(in + 2 * BURST, QUIET, BURST, 3000, 1500, 440);
    memcpy(out, in, sizeof out);
    limit_stereo(&lim, out, BURST, RATE);
    CHECK(lim.dip > 0.3, "the burst took only %g off the gain", lim.dip);   // 4 dB over at the peak is 0.63
    double before = lim.dip; uint32_t s = BURST;
    for (uint32_t t = BURST + 50 * RATE / 1000; s < t; s += BLOCK) limit_stereo(&lim, out + 2 * s, BLOCK, RATE);
    CHECK(lim.dip > 0.25 * before && lim.dip < 0.5 * before, "after 50 ms of quiet the reduction is %g of %g: not one time constant (1/e) of 50 ms", lim.dip, before);
    for (uint32_t t = BURST + 300 * RATE / 1000; s < t; s += BLOCK) limit_stereo(&lim, out + 2 * s, BLOCK, RATE);
    CHECK(1 - lim.dip > 0.99, "300 ms after the burst the gain is only %g (%g right after it)", 1 - lim.dip, 1 - before);
    for (uint32_t t = BURST + RATE; s < t; s += BLOCK) limit_stereo(&lim, out + 2 * s, BLOCK, RATE);
    CHECK(lim.dip == 0 && lim.left == 0, "a second after the burst the gain is still %g", 1 - lim.dip);
    limit_stereo(&lim, out + 2 * s, BLOCK, RATE);   // one more block, now at unity
    CHECK(s + BLOCK == N && !memcmp(in + 2 * s, out + 2 * s, (size_t)BLOCK * 8), "the output after the release differs from the input");
    return 0;
}

static int test_blocks(void) {   // at any block size: nothing over the ceiling, no sign flip, and the gain carried from one call to the next
    enum { N = RATE / 20 };
    static const uint32_t sizes[] = {1, 7, 240, 1000, FRAME_MAX, 1500};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        static int32_t in[2 * N], out[2 * N]; lim_t lim = {0};
        sine(in, N, 0, CEIL * db(5), CEIL * db(2), 330);
        memcpy(out, in, sizeof in);
        for (uint32_t s = 0; s < N; s += sizes[i]) limit_stereo(&lim, out + 2 * s, N - s < sizes[i] ? N - s : sizes[i], RATE);
        for (uint32_t s = 0; s < N; s++) {
            CHECK(abs(out[2 * s]) <= CEIL && abs(out[2 * s + 1]) <= CEIL, "blocks of %u, frame %u: %d %d", sizes[i], s, out[2 * s], out[2 * s + 1]);
            CHECK((int64_t)out[2 * s] * in[2 * s] >= 0 && (int64_t)out[2 * s + 1] * in[2 * s + 1] >= 0, "blocks of %u, frame %u: sign flip", sizes[i], s);
        }
        CHECK(lim.dip > 0 && lim.dip < 1, "blocks of %u: dip %g", sizes[i], lim.dip);
    }
    return 0;
}

static int test_flat_tops(void) {   // a +4 dB sine in the renderer's 5 ms blocks is turned down before its peaks reach the ceiling, not cut flat at it: no run of more than 2 frames at the ceiling
    enum { N = RATE / 2 };
    static const double freqs[] = {110, 220, 331, 440, 1000};   // the peaks meet the blocks' edges at every phase
    for (unsigned i = 0; i < sizeof freqs / sizeof *freqs; i++) {
        static int32_t in[2 * N], out[2 * N]; lim_t lim = {0};
        sine(in, N, 0, CEIL * db(4), CEIL * db(4) * 0.5, freqs[i]);
        run(&lim, in, out, N);
        uint32_t longest = 0, at = 0;
        for (uint32_t s = 0, len = 0; s < N; s++) {
            len = abs(out[2 * s]) == CEIL ? len + 1 : 0;
            if (len > longest) { longest = len; at = s; }
        }
        CHECK(longest <= 2, "%g Hz: a run of %u frames at the ceiling ends at frame %u (offset %u in its block)", freqs[i], longest, at, at % BLOCK);
    }
    return 0;
}

static int test_hold_runs_on(void) {   // the gain a peak needs is held for the L - 1 frames after it, also when they are in the next call or past the end of the chunk the peak is in: not just the release
    enum { L = RATE / 1000, AFTER = 100 };
    static const struct { uint32_t peak, calls; const char *what; } cases[] = {{BLOCK - 1, 2, "a peak in the last frame of a block, the next block in the next call"},
                                                                                 {FRAME_MAX - 1, 1, "a peak in the last frame of a chunk of one long call"}};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        enum { N = FRAME_MAX + AFTER };
        static int32_t in[2 * N], out[2 * N]; lim_t lim = {0}; uint32_t p = cases[i].peak, n = p + 1 + AFTER;
        for (uint32_t s = 0; s < n; s++) in[2 * s] = in[2 * s + 1] = 10000;   // quiet: under the ceiling, and half of it is what a peak of 65534 leaves
        in[2 * p] = 65534; in[2 * p + 1] = 0;   // an isolated peak: it needs a gain of 0.5
        memcpy(out, in, (size_t)n * 8);
        if (cases[i].calls == 2) { limit_stereo(&lim, out, p + 1, RATE); limit_stereo(&lim, out + 2 * (p + 1), AFTER, RATE); }
        else limit_stereo(&lim, out, n, RATE);
        CHECK(abs(out[2 * p]) <= CEIL, "%s: the peak frame is %d", cases[i].what, out[2 * p]);
        for (uint32_t s = p + 1; s < p + L; s++) CHECK(out[2 * s] == 5000 && out[2 * s + 1] == 5000, "%s: frame %u after the peak is %d %d, not held at 5000", cases[i].what, s - p, out[2 * s], out[2 * s + 1]);
        CHECK(out[2 * (p + L)] > 5000, "%s: the hold goes on past its %d frames (frame %d is %d)", cases[i].what, L - 1, L, out[2 * (p + L)]);   // the release takes over
    }
    return 0;
}

int main(void) {
    int r = test_under_ceiling() | test_burst() | test_extremes() | test_release() | test_blocks() | test_flat_tops() | test_hold_runs_on();
    if (!r) puts("t_audren_limit: ok");
    return r;
}
