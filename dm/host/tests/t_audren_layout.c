// The command header layout is taken from the first command list that walks in one, not from the first list generated.
// Mario Kart 8 Deluxe (SDK 4.4.0): the render thread generated a list before the game's first RequestUpdate whose third command was zeroed; taking the layout from it
// left the SDK 6 headers read in the newer layout, no DeviceSink was found and the game was silent. Also check recovery with the other supported headers and sinks.
// Bastion (SDK 5.4.0): its first list, two commands with the SDK 6 header whose first has ID 0x14 and 0x14 bytes, walks in the SDK 8–20 layout too and must decide nothing.
#include "../hle.h"
#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <sys/wait.h>
#include <unistd.h>
static int wait_for_render(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
static kern_return_t render_policy_set(thread_t, thread_policy_flavor_t, thread_policy_t, mach_msg_type_number_t);
#define pthread_cond_timedwait_relative_np wait_for_render
#define thread_policy_set render_policy_set
#include "../hle_audren.c"
#undef thread_policy_set
#undef pthread_cond_timedwait_relative_np

// Synthetic render loops must not promote the test runner. One short-lived pthread below exercises the real Mach boundary.
static struct { int live, started, active, fail, sets, resets; } policy;
static kern_return_t render_policy_set(thread_t thread, thread_policy_flavor_t flavor, thread_policy_t p, mach_msg_type_number_t n) {
    if (policy.live) return thread_policy_set(thread, flavor, p, n);
    assert(policy.started);
    if (flavor == THREAD_TIME_CONSTRAINT_POLICY) {
        assert(!policy.active); policy.sets++;
        if (policy.fail) return KERN_FAILURE;
        policy.active = 1;
    } else {
        assert(flavor == THREAD_STANDARD_POLICY && policy.active);
        policy.active = 0; policy.resets++;
    }
    return KERN_SUCCESS;
}

enum { SAMPLES = 240, NMIX = 8, NCMD = 6 };
enum { PACE_BLOCKS = 2000 };
static struct {
    int rate, device_frames, callbacks, played, released, appended, rendered, bursts, waits, target_blocks;
    Result update_rc;
    int ev_clears;
    void *last_cleared_ev;
    uint64_t now, last_exec, min_spacing;
    aout_buf *output[NBUF];
    pthread_t concurrent_upd;
} pace;
static struct {
    int on, queued, released, signaled, sleeping, wait_sleep, fresh_execs, ev_waits;
    pthread_mutex_t mu;
    pthread_cond_t ev_cv, sleep_cv;
} dq;
static _Alignas(8) uint8_t pace_sys[0x300];
static void *pace_sym(const char *name);
// test_renderer_blocks: the renderer each appended block came from; the output holds what it is given and plays none of it
static int blocks_on; static aout_buf *blocks[AR_MAX][NBUF]; static int blocks_n[AR_MAX];
static void blocks_exec(void *obj, void *state);
// test_update_lock: the module's functions by name
static struct { const char *name; void *p; } lock_syms[4];
void *guest_sym(const char *name) {
    for (size_t i = 0; i < sizeof lock_syms / sizeof *lock_syms; i++) if (lock_syms[i].name && !strcmp(lock_syms[i].name, name)) return lock_syms[i].p;
    if (pace.rate) return pace_sym(name);
    return blocks_on && strstr(name, "ExecuteAudioRendererRendering") ? (void *)blocks_exec : NULL;
}
void *objmap_get(const void *key) { (void)key; return NULL; }
void objmap_put(const void *key, void *value) { (void)key; (void)value; }
void *objmap_del(const void *key) { (void)key; return NULL; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack) { (void)fn; (void)arg; (void)prio; (void)stack; return NULL; }
void dm_guest_thread_join(void *tt) { (void)tt; }
void dm_host_lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
uint64_t now_ns(void) { return pace.now; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void *dm_pcmout_open_sink(void) { return NULL; }
int dm_audio_output_channels(void) { return 2; }   // the stereo route; the six-channel resampler is checked directly below
int dm_audio_route(void) { return 2; }
void dm_pcmout_close(void *k) { (void)k; assert(!policy.active); policy.started = 0; }
void dm_pcmout_start(void *k) { (void)k; assert(!policy.active); policy.started = 1; }
int dm_pcmout_append(void *k, void *b) {
    (void)k;
    if (pace.rate) {
        const aout_buf *block = b;
        const int16_t *pcm = block->ptr;
        assert(block->size == SAMPLES * DM_AUDIO_SINK_CHANNELS * sizeof *pcm);
        int expected = pace.appended + 1;
        for (int i = 0; i < SAMPLES; i++, pcm += DM_AUDIO_SINK_CHANNELS)
            assert(pcm[0] == expected && pcm[1] == -expected && pcm[6] == expected && pcm[7] == -expected);
        assert(pace.appended - pace.released < NBUF);
        pace.output[pace.appended++ % NBUF] = b;
        int limit = pace.target_blocks ? pace.target_blocks : PACE_BLOCKS;
        if (pace.appended == limit) {
            atomic_store(&t_ar->open, 0);
            if (dq.on) {
                pthread_mutex_lock(&dq.mu);
                pthread_cond_broadcast(&dq.ev_cv);
                pthread_mutex_unlock(&dq.mu);
            }
        } else if (dq.on) {
            pthread_mutex_lock(&dq.mu);
            while (dq.wait_sleep && !dq.sleeping) pthread_cond_wait(&dq.sleep_cv, &dq.mu);
            dq.wait_sleep = 0;
            pthread_mutex_unlock(&dq.mu);
        }
        return 1;
    }
    if (!blocks_on) return 0;
    int r = ar_index(t_ar); blocks[r][blocks_n[r]++] = b;
    if (blocks_n[r] == NBUF) atomic_store(&t_ar->open, 0);
    return 1;
}
// Advance by whole device callbacks. Fractional resampling is carried across callbacks,
// so 44.1 kHz consumes alternating counts of 48 kHz samples as CoreAudio does.
static uint64_t next_callback(void) { return (uint64_t)(pace.callbacks + 1) * pace.device_frames * 1000000000ull / pace.rate; }
static void pace_advance(uint64_t until) {
    while (next_callback() <= until) {
        int before = pace.played / SAMPLES;
        pace.played = (int)((uint64_t)++pace.callbacks * pace.device_frames * 48000 / pace.rate);
        assert(pace.played <= pace.appended * SAMPLES);   // no zero-fill needed
        if (pace.played / SAMPLES - before > 1) pace.bursts++;
    }
    pace.now = until;
}
void *dm_pcmout_released(void *k) {
    (void)k;
    return pace.released < pace.played / SAMPLES ? pace.output[pace.released++ % NBUF] : NULL;
}
void dm_pcmout_wait(void *k, int ms) {
    (void)k;
    if (pace.rate) {
        uint64_t timeout = pace.now + (uint64_t)ms * 1000000;
        uint64_t callback = next_callback();
        pace_advance(callback < timeout ? callback : timeout);
    }
}
void hle_bind_system_event(void *ev, int ac) { (void)ev; (void)ac; }
void hle_clear_system_event(void *ev) {
    pace.ev_clears++;
    pace.last_cleared_ev = ev;
    if (dq.on) {
        pthread_mutex_lock(&dq.mu);
        dq.signaled = 0;
        pthread_mutex_unlock(&dq.mu);
    }
}
uint32_t hle_dup_readable_handle(uint32_t h) { return h; }

static int32_t mix[NMIX * SAMPLES];
_Alignas(8) static uint8_t list[0x20 + NCMD * 0x148];
static uint8_t *put(uint8_t *q, int sdk, int id, int size) {
    memset(q, 0, (size_t)size);
    if (sdk == 21) { q[0] = 1; q[1] = (uint8_t)id; *(int16_t *)(q + 2) = (int16_t)size; }
    else {
        *(uint32_t *)q = 0xcafebabeu;
        if (sdk <= 6) { *(int16_t *)(q + 4) = (int16_t)size; *(int16_t *)(q + 6) = (int16_t)id; q[0x10] = 1; }
        else { q[4] = 1; q[5] = (uint8_t)id; *(int16_t *)(q + 6) = (int16_t)size; }
    }
    return q + size;
}
static size_t build(int sdk, int ncmd) {
    memset(list, 0, sizeof list);
    *(uint32_t *)(list + 4) = (uint32_t)ncmd; *(const int32_t **)(list + 8) = mix; *(uint32_t *)(list + 0x14) = SAMPLES; *(uint32_t *)(list + 0x18) = 48000;
    uint8_t *q = list + 0x20;
    for (int i = 0; i < ncmd - 1; i++) q = put(q, sdk, 6, 0x40);
    int id = sdk <= 5 ? 0xf : sdk == 6 ? 0x10 : sdk >= 20 ? 0x14 : 0x13;
    uint8_t *sink = q; q = put(q, sdk, id, sdk == 4 ? 0x130 : sdk == 5 ? 0x138 : sdk == 21 ? 0x140 : 0x148);
    int count = sdk <= 5 ? 0x120 : sdk == 21 ? 0x128 : 0x130;
    *(uint32_t *)(sink + count) = 2;
    if (sdk == 4) { sink[0x124] = 0; sink[0x125] = 1; }
    else { *(int16_t *)(sink + count + 4) = 0; *(int16_t *)(sink + count + 6) = 1; }
    return (size_t)(q - list);
}

static void test_headers(void) {
    const int sdks[] = {4, 5, 6, 16, 21};
    for (size_t i = 0; i < sizeof sdks / sizeof *sdks; i++) {
        _Alignas(8) uint8_t aligned[0x40], wire[0x41]; uint8_t *q = wire + 1;
        int sdk = sdks[i], id = sdk == 21 ? 0x14 : 0x10;
        g_hdr6 = sdk <= 6; g_hdr21 = sdk == 21;
        put(aligned, sdk, id, sizeof aligned); memcpy(q, aligned, sizeof aligned);
        assert(cmd_size(q) == sizeof aligned && cmd_id(q) == id && cmd_on(q));
        assert(cmd_ok(q, q + sizeof aligned));
        // Header and body truncation must fail before any field or next-command read.
        for (size_t n = 0; n < sizeof aligned; n++) assert(!cmd_ok(q, q + n));
        const int16_t invalid[] = {0, -1, 4, 0x41};
        for (size_t n = 0; n < sizeof invalid / sizeof *invalid; n++) {
            memcpy(q + (sdk == 21 ? 2 : sdk <= 6 ? 4 : 6), invalid + n, sizeof *invalid);
            assert(!cmd_ok(q, q + sizeof aligned));
        }
    }
    g_hdr6 = g_hdr21 = 0;
    int16_t out[FRAME_MAX * DM_AUDIO_SINK_CHANNELS];
    for (size_t n = 0; n < 0x20; n++) assert(capture(&g_ar[0], list, n, out) == 0);
}

static void test(int sdk, int incomplete) {
    // render_once starts with ID 0x14 for SDK 20+ (the Span version of GenerateCommand), 0x13 otherwise.
    g_sink_id = sdk == 21 ? 0x14 : 0x13;
    for (int s = 0; s < SAMPLES; s++) { mix[s] = 1000; mix[SAMPLES + s] = -1000; }
    int16_t out[FRAME_MAX * DM_AUDIO_SINK_CHANNELS];

    // An empty startup list must not choose a layout either.
    size_t n = build(sdk, NCMD);
    *(uint32_t *)(list + 4) = 0;
    assert(capture(&g_ar[0], list, 0x20, out) == 0);
    if (incomplete) {
        // The third command is zeroed, as Mario Kart 8 Deluxe's first list was. Several such frames must leave detection open.
        n = build(sdk, NCMD);
        memset(list + 0x20 + 2 * 0x40, 0, 0x40);
        for (int i = 0; i < 3; i++) assert(capture(&g_ar[0], list, n, out) == 0);
        assert(!g_hdr6 && !g_hdr21 && !g_sink4);
    }
    if (sdk == 5) {
        // Bastion's first list: a 0x14-byte command of ID 0x14, then a 0x28-byte one of ID 0xa: in the SDK 8–20 layout the IDs read as sizes, and both still walk
        memset(list, 0, sizeof list);
        *(uint32_t *)(list + 4) = 2; *(const int32_t **)(list + 8) = mix; *(uint32_t *)(list + 0x14) = SAMPLES; *(uint32_t *)(list + 0x18) = 48000;
        uint8_t *q = put(list + 0x20, 5, 0x14, 0x14); q = put(q, 5, 0xa, 0x28);
        assert(capture(&g_ar[0], list, (size_t)(q - list), out) == 0);
        assert(!g_hdr6 && !g_hdr21 && !g_sink4);
    }

    // SDK 21 starts with short valid lists: detect its header before the first full list's sink scan.
    n = build(sdk, sdk == 21 ? 2 : NCMD);
    int got = capture(&g_ar[0], list, n, out);
    assert(g_hdr6 == (sdk <= 6) && g_hdr21 == (sdk == 21) && g_sink4 == (sdk <= 5) && g_sink5 == (sdk == 5));
    assert(g_sink_id == (sdk <= 5 ? 0xf : sdk == 6 ? 0x10 : sdk == 21 ? 0x14 : 0x13));
    assert(got == SAMPLES);
    for (int s = 0; s < SAMPLES; s++) assert(out[DM_AUDIO_SINK_CHANNELS * s + 6] == 1000 && out[DM_AUDIO_SINK_CHANNELS * s + 7] == -1000);
    n = build(sdk, NCMD);
    assert(capture(&g_ar[0], list, n, out) == SAMPLES);   // detection stays valid for later full frames
}

// Malformed lists after layout selection must leave the histogram/sink scan pending
// and must never produce PCM, even when a valid sink precedes the malformed tail.
static void test_malformed(int sdk, int bad) {
    g_sink_id = sdk == 21 ? 0x14 : 0x13;
    int16_t out[FRAME_MAX * DM_AUDIO_SINK_CHANNELS];
    size_t n = build(sdk, NCMD);
    *(uint32_t *)(list + 4) = 1;
    assert(capture(&g_ar[0], list, 0x20 + 0x40, out) == 0);   // select the layout without a sink or histogram
    int sink_id = g_sink_id;
    n = build(sdk, NCMD);
    if (bad < 2) memset(list + 0x20 + (size_t)bad * 0x40, 0, 0x40);
    else { *(uint32_t *)(list + 4) = NCMD + 1; memset(list + n, 0, 0x40); n += 0x40; }
    for (size_t i = 0; i < sizeof out / sizeof *out; i++) out[i] = 1234;

    FILE *log = tmpfile(); assert(log);
    fflush(stderr); int saved = dup(STDERR_FILENO); assert(saved >= 0);
    assert(dup2(fileno(log), STDERR_FILENO) >= 0);
    int got = capture(&g_ar[0], list, n, out);
    fflush(stderr); assert(dup2(saved, STDERR_FILENO) >= 0); close(saved);
    char text[2048]; assert(fseek(log, 0, SEEK_SET) == 0); size_t bytes = fread(text, 1, sizeof text - 1, log); text[bytes] = 0; fclose(log);
    assert(got == 0);
    assert(!strstr(text, "by command ID:"));
    assert(strstr(text, "malformed"));
    assert(g_sink_id == sink_id && !g_sink4 && !g_sink5);
    for (size_t i = 0; i < sizeof out / sizeof *out; i++) assert(out[i] == 1234);

    // A subsequent valid full list still discovers the SDK 4/5 sink and yields stereo.
    n = build(sdk, NCMD);
    for (int i = 0; i < SAMPLES; i++) { mix[i] = 700; mix[SAMPLES + i] = -900; }
    assert(capture(&g_ar[0], list, n, out) == SAMPLES);
    for (int i = 0; i < SAMPLES; i++) assert(out[DM_AUDIO_SINK_CHANNELS * i + 6] == 700 && out[DM_AUDIO_SINK_CHANNELS * i + 7] == -900);
    // Validation also applies after the histogram has already been logged.
    *(uint32_t *)(list + 4) = NCMD + 1; memset(list + n, 0, 0x40);
    assert(capture(&g_ar[0], list, n + 0x40, out) == 0);
}

static void test_histogram(void) {
    // 64 IDs x 1000 commands need 566 bytes of histogram text, more than its 512-byte buffer.
    // Disabled commands keep this a formatter test without dereferencing sink payloads.
    enum { COUNT = 64000, SIZE = 8 };
    size_t len = 0x20 + COUNT * SIZE;
    uint8_t *cl = calloc(1, len); assert(cl);
    *(uint32_t *)(cl + 4) = COUNT;
    for (int i = 0; i < COUNT; i++) { uint8_t *q = cl + 0x20 + i * SIZE; put(q, 16, i / 1000, SIZE); q[4] = 0; }
    FILE *log = tmpfile(); assert(log);
    fflush(stderr); int saved = dup(STDERR_FILENO); assert(saved >= 0);
    assert(dup2(fileno(log), STDERR_FILENO) >= 0);
    int16_t out[FRAME_MAX * DM_AUDIO_SINK_CHANNELS]; assert(capture(&g_ar[0], cl, len, out) == 0);
    fflush(stderr); assert(dup2(saved, STDERR_FILENO) >= 0); close(saved);
    char text[2048]; assert(fseek(log, 0, SEEK_SET) == 0); size_t bytes = fread(text, 1, sizeof text - 1, log); text[bytes] = 0; fclose(log);
    const char *hist = strstr(text, "by command ID:"); assert(hist);
    hist += strlen("by command ID:");
    const char *end = strchr(hist, '\n'); assert(end && end - hist <= 512);
    assert(strstr(hist, " 0×1000 1×1000"));
    free(cl);
}

// Two renderers open at once: an SDK call that names only the system finds its own renderer, and a system not seen yet is the one open renderer whose system is not known
static void test_two_renderers(void) {
    uint8_t x, y, z; memset(g_ar, 0, sizeof g_ar);
    g_ar[0].handle[0] = &x; g_ar[1].handle[0] = &y;
    assert(ar_of_sys(&z) == NULL);   // two unknown: not a guess
    g_ar[0].sys = &x;
    assert(ar_of_sys(&x) == &g_ar[0]);
    assert(ar_of_sys(&y) == &g_ar[1] && g_ar[1].sys == &y);
    assert(ar_of_sys(&y) == &g_ar[1]);
    assert(ar_of_sys(&z) == NULL);   // no renderer left to be it
    // The 3:2 upsampler's history is each renderer's own: frames of one do not reach the other's output
    int32_t quiet[2 * 160] = {0}, loud[2 * 160], out[2 * 240];
    for (int i = 0; i < 2 * 160; i++) loud[i] = 20000;
    memset(g_ar, 0, sizeof g_ar);
    assert(up_3_2(&g_ar[0], loud, 160, out, 2) == 240);
    assert(up_3_2(&g_ar[1], quiet, 160, out, 2) == 240);
    for (int i = 0; i < 2 * 240; i++) assert(out[i] == 0);
    // Six channels (the 5.1 route): each channel resamples on its own; an LFE-only block stays on the LFE
    int32_t six_in[6 * 160] = {0}, six_out[6 * 240];
    for (int i = 0; i < 160; i++) six_in[6 * i + 3] = 20000;
    memset(g_ar, 0, sizeof g_ar);
    assert(up_3_2(&g_ar[2], six_in, 160, six_out, 6) == 240);
    for (int m = 0; m < 240; m++) for (int j = 0; j < 6; j++) if (j != 3) assert(six_out[6 * m + j] == 0);
    assert(six_out[6 * 100 + 3] == 20000);
    memset(g_ar, 0, sizeof g_ar);
}

// Six channels (the 5.1 route, SDK 16's layout): the sink's six inputs come out in order, six int16 per sample; an output of 2 keeps the stereo downmix
static void test_stereo_unchanged(void);
static void test_six_channels(void) {
    g_sink_id = 0x13;
    for (int j = 0; j < 6; j++) for (int s = 0; s < SAMPLES; s++) mix[j * SAMPLES + s] = 1000 * (j + 1);
    size_t n = build(16, NCMD);
    uint8_t *sink = list + 0x20 + (NCMD - 1) * 0x40;   // build() puts the sink after NCMD - 1 filler commands
    *(uint32_t *)(sink + 0x130) = 6;
    for (int j = 0; j < 6; j++) ((int16_t *)(sink + 0x134))[j] = (int16_t)j;
    static int16_t out[FRAME_MAX * DM_AUDIO_SINK_CHANNELS];
    assert(capture(&g_ar[0], list, n, out) == SAMPLES);
    assert(!g_hdr6 && !g_hdr21 && !g_sink4);
    for (int s = 0; s < SAMPLES; s++) for (int j = 0; j < 6; j++) assert(out[DM_AUDIO_SINK_CHANNELS * s + j] == 1000 * (j + 1));
    // A mono sink on the six-channel output: on the front pair, as on stereo; the other four silent
    *(uint32_t *)(sink + 0x130) = 1;
    assert(capture(&g_ar[0], list, n, out) == SAMPLES);
    for (int s = 0; s < SAMPLES; s++) for (int j = 0; j < 6; j++) assert(out[DM_AUDIO_SINK_CHANNELS * s + j] == (j < 2 ? 1000 : 0));
    // A center above PCM16 range must cancel the front before stereo clipping.
    for (int j = 0; j < 6; j++) for (int i = 0; i < SAMPLES; i++) mix[j * SAMPLES + i] = j == 0 ? -30000 : j == 2 ? 60000 : 0;
    *(uint32_t *)(sink + 0x130) = 6;
    assert(capture(&g_ar[0], list, n, out) == SAMPLES);
    assert(out[0] == -30000 && out[2] == 32767 && out[6] == 12420 && out[7] == 32767);
    test_stereo_unchanged();
    puts("audren six channels: the sink's six inputs in order, six per sample; a mono sink on the front pair");
}

// Golden PCM from current main's stereo-only capture, over three frames with cancellation and clipping.
// This locks down mixing, 32 kHz resampling history and final clamp order independently of the new surround path.
static void test_stereo_unchanged(void) {
    const uint64_t golden[] = {0xb2c91fe377e1ac44ull, 0x9dfef1ca9df24ea6ull};
    for (int r = 0; r < 2; r++) {
        int rate = r ? 32000 : 48000, count = r ? 160 : 240;
        ar_t a = {0}; uint64_t hash = 14695981039346656037ull;
        for (int frame = 0; frame < 3; frame++) {
            g_sink_id = 0x13; size_t n = build(16, NCMD);
            *(uint32_t *)(list + 0x14) = (uint32_t)count; *(uint32_t *)(list + 0x18) = (uint32_t)rate;
            uint8_t *sink = list + 0x20 + (NCMD - 1) * 0x40; *(uint32_t *)(sink + 0x130) = 6;
            for (int c = 0; c < 6; c++) {
                ((int16_t *)(sink + 0x134))[c] = (int16_t)c;
                for (int i = 0; i < count; i++) mix[c * count + i] = ((frame * 977 + c * 7919 + i * 613) % 120001) - 60000;
            }
            int16_t out[FRAME_MAX * DM_AUDIO_SINK_CHANNELS];
            assert(capture(&a, list, n, out) == 240);
            for (int i = 0; i < 240; i++) for (int j = 6; j < 8; j++) {
                uint16_t v = (uint16_t)out[i * DM_AUDIO_SINK_CHANNELS + j];
                hash = (hash ^ (v & 255)) * 1099511628211ull; hash = (hash ^ (v >> 8)) * 1099511628211ull;
            }
        }
        assert(hash == golden[r]);
    }
    puts("audren stereo: 48 kHz and 32 kHz PCM match the pre-surround golden output, including clipping and resampler history");
}

// Issue #1336: AudioOut queues a block's pointer and copies its samples when the sound card asks, so a block is the renderer's own until it comes back released.
// Renderer 0 queues all its blocks and none plays; renderer 1 then renders (blocks_exec marks each frame with its renderer) and renderer 0's blocks must still hold its samples
static void blocks_exec(void *obj, void *state) {
    (void)obj; (void)state;
    for (int i = 0; i < SAMPLES * DM_AUDIO_SINK_CHANNELS; i++) t_ar->frame[i] = (int16_t)(ar_index(t_ar) + 1);
    t_ar->frame_n = SAMPLES;
}
static void test_renderer_blocks(void) {
    memset(blocks_n, 0, sizeof blocks_n);
    blocks_on = 1;
    for (int r = 0; r < 2; r++) {
        ar_t *a = &g_ar[r];
        pthread_mutex_init(&a->mu, NULL); pthread_cond_init(&a->cv, NULL);
        atomic_store(&a->open, 1); render_main(a);
        pthread_cond_destroy(&a->cv); pthread_mutex_destroy(&a->mu);
    }
    blocks_on = 0;
    for (int r = 0; r < 2; r++) {
        assert(blocks_n[r] == NBUF);
        for (int i = 0; i < NBUF; i++) {
            const int16_t *s = blocks[r][i]->ptr;
            assert(blocks[r][i]->size == SAMPLES * DM_AUDIO_SINK_CHANNELS * sizeof(int16_t));
            for (int j = 0; j < SAMPLES * DM_AUDIO_SINK_CHANNELS; j++) assert(s[j] == r + 1);
        }
    }
    memset(g_ar, 0, sizeof g_ar);
}

static void *policy_thread(void *unused) {
    (void)unused;
    assert(!pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0));
    assert(render_rt());
    thread_time_constraint_policy_data_t p = {0};
    mach_msg_type_number_t n = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t def = FALSE;
    thread_t self = pthread_mach_thread_np(pthread_self());
    assert(thread_policy_get(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, &n, &def) == KERN_SUCCESS && !def);
    mach_timebase_info_data_t tb; assert(mach_timebase_info(&tb) == KERN_SUCCESS);
    fprintf(stderr, "Mach audio policy readback: period %u computation %u constraint %u, timebase %u/%u\n", p.period, p.computation, p.constraint, tb.numer, tb.denom);
    assert((uint64_t)p.period * tb.numer / tb.denom == 5000000);
    // XNU may raise computation to half the constraint; the accepted budget is not guaranteed to equal the request.
    assert((uint64_t)p.computation * tb.numer / tb.denom >= 1000000 && p.computation <= p.constraint);
    assert(p.constraint == p.period && p.preemptible);
    render_rt_end();
    thread_extended_policy_data_t extended = {0}; n = THREAD_EXTENDED_POLICY_COUNT; def = FALSE;
    assert(thread_policy_get(self, THREAD_EXTENDED_POLICY, (thread_policy_t)&extended, &n, &def) == KERN_SUCCESS && !def && extended.timeshare);
    return NULL;
}
static void test_render_policy(void) {
    // Failure and the diagnostic opt-out must still publish every renderer's PCM, without an unmatched policy reset.
    unsetenv("DM_AUDREN_RT");
    test_renderer_blocks(); assert(policy.sets == 2 && policy.resets == 2);
    policy.fail = 1;
    test_renderer_blocks(); assert(policy.sets == 4 && policy.resets == 2);
    setenv("DM_AUDREN_RT", "0", 1);
    test_renderer_blocks(); assert(policy.sets == 4 && policy.resets == 2);
    unsetenv("DM_AUDREN_RT"); policy.fail = 0;
    policy.live = 1;
    pthread_t thread; assert(!pthread_create(&thread, NULL, policy_thread, NULL));
    assert(!pthread_join(thread, NULL)); policy.live = 0;
    puts("audren scheduling: real worker policy set/read/reset; setup/teardown outside real-time; PCM preserved on rejection and opt-out");
}

// The update lock is read off Update, else off UpdateImpl; a miss keeps the caller's offset and is a red line. Built from prologues of the forms the SDKs have (`bl` into a PLT stub or straight to the function)
enum { F_UPDATE, F_IMPL = 16, F_STUB = 32, F_TICK = 36, F_SDK_LOCK, F_OS_LOCK, F_CODE };
static uint32_t lock_code[F_CODE];
static void *lock_got __attribute__((aligned(8)));
static void lock_fn(int at, int off, int target) {   // pacibsp; add x19, x0, #off; mov x20, x0; mov x0, x19; bl target (no add: bl target first, as the tick call of SDK 19's Update)
    uint32_t *f = lock_code + at; int n = 0;
    f[n++] = 0xd503237fu;
    if (off >= 0) { f[n++] = 0x91000000u | (uint32_t)off << 10 | 19; f[n++] = 0xaa0003e0u | 20; f[n++] = 0xaa0003e0u | 19u << 16; }
    f[n] = 0x94000000u | ((uint32_t)(target - at - n) & 0x3ffffffu);
}
static int lock_read(size_t *off, int *os_mutex, const char **red) {   // update_lock with its log caught in *red: NULL when it printed none
    static char log[256]; memset(log, 0, sizeof log);
    fflush(stderr); int saved = dup(2); FILE *t = tmpfile(); assert(saved >= 0 && t); dup2(fileno(t), 2);
    int r = update_lock(off, os_mutex);
    fflush(stderr); dup2(saved, 2); close(saved); assert(!fseek(t, 0, SEEK_SET)); assert(fread(log, 1, sizeof log - 1, t) < sizeof log); fclose(t);
    *red = strstr(log, "[RED] audren: no update lock"); return r;
}
static void lock_plt_stub(void) {
    const uint64_t got = (uint64_t)&lock_got, stub = (uint64_t)(lock_code + F_STUB);
    const uint64_t pages = ((got & ~0xfffull) - (stub & ~0xfffull)) >> 12;
    lock_code[F_STUB] = 0x90000010u | (uint32_t)(pages & 3) << 29 | (uint32_t)((pages >> 2) & 0x7ffff) << 5;   // adrp x16, the GOT's page
    lock_code[F_STUB + 1] = 0xf9400211u | (uint32_t)(got & 0xfff) / 8 << 10;   // ldr x17, [x16, #the slot]
}
static void test_update_lock(void) {
    lock_plt_stub();
    lock_got = lock_code + F_SDK_LOCK;
    lock_syms[0].name = "_ZN2nn2os12SdkMutexType4LockEv"; lock_syms[0].p = lock_code + F_SDK_LOCK;
    lock_syms[1].name = "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"; lock_syms[1].p = lock_code + F_OS_LOCK;
    lock_syms[2].name = "_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm"; lock_syms[2].p = lock_code + F_UPDATE;
    lock_syms[3].name = "_ZN2nn5audio6server17AudioRenderSystem10UpdateImplEPvmS3_mPKvm"; lock_syms[3].p = lock_code + F_IMPL;
    size_t off; int os; const char *red;
    // before SDK 19: Update locks, through the PLT, and it wins over a lock in UpdateImpl
    lock_fn(F_UPDATE, 0x1f0, F_STUB); lock_fn(F_IMPL, 0x2d8, F_STUB);
    off = 1; os = 1; assert(lock_read(&off, &os, &red) && off == 0x1f0 && !os && !red);
    lock_got = lock_code + F_OS_LOCK;   // nn::os::LockMutex
    off = 1; os = 0; assert(lock_read(&off, &os, &red) && off == 0x1f0 && os == 1 && !red);
    lock_got = lock_code + F_SDK_LOCK;
    // from SDK 19: Update's first call is the tick, and UpdateImpl has the lock: +0x2d8 in SDK 22, whatever the caller guessed
    lock_fn(F_UPDATE, -1, F_TICK);
    off = 0x2d0; os = 0; assert(lock_read(&off, &os, &red) && off == 0x2d8 && !os && !red);
    lock_fn(F_IMPL, 0x2d0, F_SDK_LOCK);   // SDK 19–21, a straight bl
    off = 1; os = 0; assert(lock_read(&off, &os, &red) && off == 0x2d0 && !os && !red);
    // neither locks first, or an SDK without UpdateImpl: the guess stays and the log says so
    lock_fn(F_IMPL, 0x2d8, F_TICK);
    off = 0x2d0; os = 0; assert(!lock_read(&off, &os, &red) && off == 0x2d0 && !os && red);
    memset(&lock_syms[3], 0, sizeof lock_syms[3]);
    off = 0x1f0; os = 0; assert(!lock_read(&off, &os, &red) && off == 0x1f0 && !os && red);
    memset(lock_syms, 0, sizeof lock_syms);
}

static span_t pace_generate(void *sys, void *cl, size_t n) {
    (void)sys; (void)cl; (void)n;
    if (dq.on) {
        pthread_mutex_lock(&dq.mu);
        assert(dq.queued > 0);
        dq.queued--;
        dq.released++;
        pthread_mutex_unlock(&dq.mu);
    }
    int frame = ++pace.rendered;
    for (int i = 0; i < SAMPLES; i++) { mix[i] = frame; mix[SAMPLES + i] = -frame; }
    return (span_t){list, build(20, NCMD)};
}
static void pace_noop(void *p) { (void)p; }
static void pace_tmax(void *p, uint32_t n) { (void)p; (void)n; }
static void pace_setup(void *p, const void *cl, size_t n) { (void)p; (void)cl; (void)n; }
static Result pace_update(void *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn) {
    (void)sys; (void)on; (void)p; (void)pn; (void)inn;
    if (pace.update_rc) return pace.update_rc;
    if (dq.on) {
        pthread_mutex_lock(&dq.mu);
        if (in) dq.queued++;
        if (o) *(int *)o = dq.released;
        dq.released = 0;
        pthread_mutex_unlock(&dq.mu);
    }
    return RES_OK;
}
static void pace_exec(void *sys, void *state) {
    (void)state;
    if (pace.appended >= NBUF) {
        uint64_t gap = pace.now - pace.last_exec;
        assert(gap >= 4500000);
        if (gap < pace.min_spacing) pace.min_spacing = gap;
    }
    pace.last_exec = pace.now;
    assert(h_ars_Exec(sys) == RES_OK);
    if (dq.on) {
        pthread_mutex_lock(&dq.mu);
        dq.fresh_execs++;
        dq.signaled = 1;
        dq.sleeping = 0;
        dq.wait_sleep = 1;
        pthread_cond_signal(&dq.ev_cv);
        pthread_mutex_unlock(&dq.mu);
    }
}
static void *pace_sym(const char *name) {
    if (strstr(name, "ExecuteAudioRendererRendering")) return pace_exec;
    if (strstr(name, "AudioRenderSystem6Update")) return pace_update;
    if (strstr(name, "GenerateCommandENS_4Span")) return pace_generate;
    if (strstr(name, "SdkMutexType6Unlock") || strstr(name, "Processor7Process")) return pace_noop;
    if (strstr(name, "Processor17SetProcessTimeMax")) return pace_tmax;
    if (strstr(name, "Processor5Setup")) return pace_setup;
    return NULL;
}
static void *pace_concurrent_update(void *arg) {
    assert(h_ars_Update(arg, NULL, 0, NULL, 0, NULL, 0) == RES_OK);
    return NULL;
}
static int wait_for_render(pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *delay) {
    if (!pace.rate) return pthread_cond_timedwait_relative_np(cv, mu, delay);
    if (!dq.on && !pace.waits++) {
        // Exercise a concurrent RequestUpdate while render_main is inside its paced wait (a->mu released).
        pthread_mutex_unlock(mu);
        assert(!pthread_create(&pace.concurrent_upd, NULL, pace_concurrent_update, pace_sys));
        assert(!pthread_join(pace.concurrent_upd, NULL));
        pthread_mutex_lock(mu);
    } else {
        pace.waits++;
    }
    pace_advance(pace.now + (uint64_t)delay->tv_nsec);
    return ETIMEDOUT;
}
static void test_update_clears_event(void) {
    // RequestUpdate clears the renderer event on RES_OK (matching Horizon's AudioRenderSystem::Update),
    // leaves it untouched on failure or when no event is attached, and never renders or blocks on repeated polls.
    ar_t *a = &g_ar[0];
    uint8_t ev_storage = 0;
    memset(a, 0, sizeof *a);
    memset(&pace, 0, sizeof pace);
    pace.rate = 48000;
    *(int32_t *)(pace_sys + 0x48) = *(int32_t *)(pace_sys + 0x4c) = 1;
    a->sys = pace_sys; a->handle[0] = pace_sys; a->ev = &ev_storage; a->ev_kernel = 1;
    pthread_mutex_init(&a->mu, NULL); pthread_cond_init(&a->cv, NULL);
    atomic_store(&a->open, 1);

    for (int i = 0; i < 12; i++) assert(h_ars_Update(pace_sys, NULL, 0, NULL, 0, NULL, 0) == RES_OK);
    assert(pace.rendered == 0 && pace.ev_clears == 12 && pace.last_cleared_ev == &ev_storage);

    pace.update_rc = RES_FAIL;
    assert(h_ars_Update(pace_sys, NULL, 0, NULL, 0, NULL, 0) == RES_FAIL);
    assert(pace.ev_clears == 12 && pace.rendered == 0);
    pace.update_rc = RES_OK;

    a->ev = NULL;
    assert(h_ars_Update(pace_sys, NULL, 0, NULL, 0, NULL, 0) == RES_OK);
    assert(pace.ev_clears == 12 && pace.rendered == 0);

    pthread_cond_destroy(&a->cv); pthread_mutex_destroy(&a->mu);
    memset(&pace, 0, sizeof pace);
    memset(a, 0, sizeof *a);
    puts("audren update: clears renderer event on RES_OK, preserves on error, never renders or blocks on polls");
}
static void *dq_driver_thread(void *arg) {
    // Models SQEX::Sd::Driver::Core::CoreAudioOut (DRAGON QUEST XI S) and event-paced drivers (Wonder):
    // append while wave buffers are free, RequestUpdate, take released buffers, and wait on the renderer event
    // only when none is released. Because Update clears any stale pre-update signal, the driver always waits for a fresh render.
    ar_t *a = arg;
    int bufs_free = 2;
    while (atomic_load(&a->open)) {
        int released = 0;
        const void *append = NULL;
        if (bufs_free > 0) { bufs_free--; append = (const void *)1; }
        assert(h_ars_Update(pace_sys, &released, sizeof released, NULL, 0, append, 0) == RES_OK);
        bufs_free += released;
        if (bufs_free > 0) continue;
        pthread_mutex_lock(&dq.mu);
        dq.ev_waits++;
        dq.sleeping = 1;
        pthread_cond_signal(&dq.sleep_cv);
        while (!dq.signaled && atomic_load(&a->open)) pthread_cond_wait(&dq.ev_cv, &dq.mu);
        dq.signaled = 0;
        dq.sleeping = 0;
        pthread_mutex_unlock(&dq.mu);
    }
    return NULL;
}
static void test_event_paced_driver(int rate, int frames) {
    ar_t *a = &g_ar[0];
    uint8_t ev_storage = 0;
    memset(a, 0, sizeof *a);
    memset(&pace, 0, sizeof pace);
    memset(&dq, 0, sizeof dq);
    pace.rate = rate; pace.device_frames = frames; pace.min_spacing = UINT64_MAX; pace.target_blocks = 250;
    dq.on = 1;
    dq.signaled = 1;   // stale pre-update signal (as from startup prefill or a slow audio thread) must be cleared by Update
    pthread_mutex_init(&dq.mu, NULL); pthread_cond_init(&dq.ev_cv, NULL); pthread_cond_init(&dq.sleep_cv, NULL);
    *(int32_t *)(pace_sys + 0x48) = *(int32_t *)(pace_sys + 0x4c) = 1;
    a->sys = pace_sys; a->handle[0] = pace_sys; a->ev = &ev_storage; a->ev_kernel = 1;
    pthread_mutex_init(&a->mu, NULL); pthread_cond_init(&a->cv, NULL);
    atomic_store(&a->open, 1);

    pthread_t driver;
    assert(!pthread_create(&driver, NULL, dq_driver_thread, a));
    pthread_mutex_lock(&dq.mu);
    while (!dq.sleeping) pthread_cond_wait(&dq.sleep_cv, &dq.mu);
    pthread_mutex_unlock(&dq.mu);
    // Update cleared the stale signal without rendering: voice holds 2 buffers, driver is sleeping on the event.
    assert(pace.rendered == 0 && dq.queued == 2 && dq.ev_waits == 1 && !dq.signaled);

    render_main(a);
    assert(!pthread_join(driver, NULL));
    assert(pace.appended == pace.target_blocks && pace.rendered == pace.target_blocks);
    assert(dq.fresh_execs == pace.target_blocks && dq.ev_waits >= dq.fresh_execs);
    printf("audren event-paced driver: %d Hz/%d device frames, %d PCM blocks in order, %d fresh exec wakes, no underruns\n",
           rate, frames, pace.appended, dq.fresh_execs);
    pthread_cond_destroy(&dq.sleep_cv); pthread_cond_destroy(&dq.ev_cv); pthread_mutex_destroy(&dq.mu);
    pthread_cond_destroy(&a->cv); pthread_mutex_destroy(&a->mu);
    memset(&dq, 0, sizeof dq);
    memset(&pace, 0, sizeof pace);
    memset(a, 0, sizeof *a);
}
static void test_render_cadence(int rate, int frames) {
    ar_t *a = &g_ar[0];
    uint8_t ev_storage = 0;
    memset(a, 0, sizeof *a);
    pace.rate = rate; pace.device_frames = frames; pace.min_spacing = UINT64_MAX;
    *(int32_t *)(pace_sys + 0x48) = *(int32_t *)(pace_sys + 0x4c) = 1;
    a->sys = pace_sys; a->handle[0] = pace_sys; a->ev = &ev_storage; a->ev_kernel = 1;
    lock_plt_stub();
    lock_got = (void *)pace_noop;
    lock_fn(F_IMPL, 0x2d0, F_STUB);
    lock_syms[0].name = "_ZN2nn2os12SdkMutexType4LockEv"; lock_syms[0].p = (void *)pace_noop;
    lock_syms[1].name = "_ZN2nn5audio6server17AudioRenderSystem10UpdateImplEPvmS3_mPKvm"; lock_syms[1].p = lock_code + F_IMPL;
    pthread_mutex_init(&a->mu, NULL); pthread_cond_init(&a->cv, NULL);
    atomic_store(&a->open, 1);
    render_main(a);
    assert(pace.waits > 1 && pace.bursts > 1 && pace.ev_clears == 1);
    assert(pace.appended == PACE_BLOCKS && pace.rendered == PACE_BLOCKS);
    printf("audren cadence: %d Hz/%d device frames, %d callbacks (%d burst releases), %d PCM blocks in order, min exec spacing %.2f ms, no underruns\n",
           rate, frames, pace.callbacks, pace.bursts, pace.appended, pace.min_spacing / 1e6);
    pthread_cond_destroy(&a->cv); pthread_mutex_destroy(&a->mu);
    memset(&pace, 0, sizeof pace);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--cadence")) {
        test_render_cadence(44100, 256);
        test_render_cadence(48000, 512);
        test_update_clears_event();
        test_event_paced_driver(48000, 240);
        test_event_paced_driver(48000, 512);
        return 0;
    }
    test_update_lock();
    test_two_renderers();
    test_render_policy();
    if (argc == 2 && !strcmp(argv[1], "--histogram")) { test_histogram(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--malformed")) { test_malformed(16, 0); return 0; }
    test_headers();
    const int sdks[] = {4, 5, 6, 16, 21};
    for (size_t i = 0; i < sizeof sdks / sizeof *sdks; i++) for (int scenario = 0; scenario <= 4; scenario++) {
        // Each renderer format starts in a fresh process, including capture's one-time layout and sink detection.
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            if (scenario <= 1) test(sdks[i], scenario);
            else test_malformed(sdks[i], scenario - 2);
            _exit(0);
        }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    pid_t six = fork(); assert(six >= 0);
    if (!six) { test_six_channels(); fflush(stdout); _exit(0); }
    int status6; assert(waitpid(six, &status6, 0) == six && WIFEXITED(status6) && WEXITSTATUS(status6) == 0);
    test_histogram();
    test_render_cadence(44100, 256);
    test_render_cadence(48000, 512);
    test_update_clears_event();
    test_event_paced_driver(48000, 240);
    test_event_paced_driver(48000, 512);
    puts("audren layout: update lock read off Update or UpdateImpl, renderers' own blocks, bounded histograms, malformed-list rejection, unaligned and truncated headers, SDK 4/5/6/16/21 stereo PCM and empty/incomplete startup recovery passed");
    return 0;
}

uint64_t hle_system_event_binding(void *ev) { return (uint64_t)(uintptr_t)ev; }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; return 0; }
