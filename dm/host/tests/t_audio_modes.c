// Output-mode unit test: the mode picker, the fallbacks and the banked-slice delivery of the three Apple-side
// devices (audio_sys.h), with the devices and the AudioUnit mocked out - none of this needs hardware, and a
// device that cannot build on some machine must look exactly like the fallback path exercised here. Mode 0
// replays t_audio.c's expected vectors byte for byte: the host's own Q15 fold is the tested default and the
// mode plumbing around it must not move it.
// The real AudioToolbox headers come first: declared before the renames below, they keep the API names, and the
// mocks stay static under their own names - the order that t_audio_device.c already proves on this SDK.
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
static AudioComponent mock_find(AudioComponent, const AudioComponentDescription *);
static OSStatus mock_new(AudioComponent, AudioComponentInstance *);
static OSStatus mock_get(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, void *, UInt32 *);
static OSStatus mock_set(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *, UInt32);
static OSStatus mock_add(AudioUnit, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void *);
static OSStatus mock_remove(AudioUnit, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void *);
static OSStatus mock_init(AudioUnit);
static OSStatus mock_start(AudioUnit);
static OSStatus mock_stop(AudioUnit);
static OSStatus mock_uninit(AudioUnit);
static OSStatus mock_dispose(AudioComponentInstance);
#define AudioComponentFindNext mock_find
#define AudioComponentInstanceNew mock_new
#define AudioUnitGetProperty mock_get
#define AudioUnitSetProperty mock_set
#define AudioUnitAddPropertyListener mock_add
#define AudioUnitRemovePropertyListenerWithUserData mock_remove
#define AudioUnitInitialize mock_init
#define AudioOutputUnitStart mock_start
#define AudioOutputUnitStop mock_stop
#define AudioUnitUninitialize mock_uninit
#define AudioComponentInstanceDispose mock_dispose
#include "../hle_audio.c"
#include <time.h>
__thread _Atomic int dm_host_work;   // defined by the graphics host in a full build
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static struct { const void *k; void *v; } om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) return om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; return; } }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) { om[i].k = 0; return om[i].v; } return NULL; }
void hle_signal_system_event(void *g) { (void)g; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
module_t *module_of(const void *p) { (void)p; return NULL; }
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }

static int checks, failures;
#define CHECK(v, why) do { checks++; if (!(v)) { failures++; printf("FAIL: %s\n", why); } } while (0)

// ---- the mode devices (audio_sys.h), mocked: opens and closes counted, failures on demand ----
static int conv_fail, ausm_fail_open;
static unsigned conv_open_n, conv_close_n, ausm_open_n, ausm_close_n;
static int mode_at_ausm_open = -1;   // dm_audio_mode() as seen by the last dm_ausm_open: the switch must land after the rebuild, not before it
static int conv_handle, ausm_handle;
void *dm_conv_open(const int *role, int tag, int rate) { (void)role; (void)tag; (void)rate; conv_open_n++; return &conv_handle; }
int dm_conv_run(void *h, const int16_t *in, unsigned frames, int16_t *out) {
    (void)h;
    if (conv_fail) return 0;   // the device gave nothing this block: the caller must fold the block itself
    for (unsigned f = 0; f < frames; f++) { out[f * 2] = in[f * 6]; out[f * 2 + 1] = in[f * 6 + 1]; }   // the converter's own downmix, mocked as the front pair through
    return (int)frames;
}
void dm_conv_close(void *h) { (void)h; conv_close_n++; }
const char *dm_arm_last_err(void) { return "mocked device error"; }
void *dm_ausm_open(const int *role, int tag, unsigned slice) {
    (void)role; (void)tag; (void)slice;
    ausm_open_n++; mode_at_ausm_open = atomic_load_explicit(&g_mode, memory_order_relaxed);
    return ausm_fail_open ? NULL : &ausm_handle;
}
int dm_ausm_run(void *h, const int16_t *in6, unsigned frames, int16_t *out2) {
    (void)h;   // pass the front pair through: the delivered stream then names every frame in order
    for (unsigned f = 0; f < frames; f++) { out2[f * 2] = in6[f * 6]; out2[f * 2 + 1] = in6[f * 6 + 1]; }
    return (int)frames;
}
void dm_ausm_close(void *h) { (void)h; ausm_close_n++; }

// ---- the AudioUnit, mocked closely after t_audio_device.c: buffer read-back, listeners, start/stop counts ----
static struct {
    AudioDeviceID device;
    Float64 rate;
    AudioValueRange range;
    UInt32 frames;
    AudioUnitPropertyListenerProc device_listener, format_listener;
    unsigned writes, disposals, starts, stops;
} unit;
static AudioUnit fake_au(void) { return (AudioUnit)&unit; }
static void changed(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el) {
    AudioUnitPropertyListenerProc cb = id == kAudioOutputUnitProperty_CurrentDevice ? unit.device_listener : unit.format_listener;
    if (cb) cb(&unit, fake_au(), id, scope, el);
}
static AudioComponent mock_find(AudioComponent c, const AudioComponentDescription *d) {
    (void)c; assert(d->componentSubType == kAudioUnitSubType_DefaultOutput); return (AudioComponent)&unit;
}
static OSStatus mock_new(AudioComponent c, AudioComponentInstance *au) { (void)c; *au = fake_au(); return noErr; }
static OSStatus mock_get(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, void *data, UInt32 *size) {
    assert(au == fake_au() && el == 0);
    if (id == kAudioOutputUnitProperty_CurrentDevice) memcpy(data, &unit.device, *size);
    else if (id == kAudioUnitProperty_StreamFormat) { AudioStreamBasicDescription f = {.mSampleRate = unit.rate}; memcpy(data, &f, *size); }
    else if (id == kAudioDevicePropertyBufferFrameSizeRange) memcpy(data, &unit.range, *size);
    else memcpy(data, &unit.frames, *size);
    return noErr;
}
static OSStatus mock_set(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size) {
    assert(au == fake_au() && el == 0);
    if (id == kAudioDevicePropertyBufferFrameSize) {
        assert(unit.device && size == sizeof unit.frames);
        UInt32 frames; memcpy(&frames, data, size);
        assert(frames >= unit.range.mMinimum && frames <= unit.range.mMaximum);
        unit.frames = frames; unit.writes++;
        changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);   // as the HAL does; must not re-write the same buffer
    }
    return noErr;
}
static OSStatus mock_add(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc cb, void *ctx) {
    (void)au; (void)ctx;
    if (id == kAudioOutputUnitProperty_CurrentDevice) unit.device_listener = cb;
    else unit.format_listener = cb;
    return noErr;
}
static OSStatus mock_remove(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc cb, void *ctx) {
    (void)au; (void)cb; (void)ctx;
    if (id == kAudioOutputUnitProperty_CurrentDevice) unit.device_listener = NULL;
    else unit.format_listener = NULL;
    return noErr;
}
static OSStatus mock_init(AudioUnit au) { (void)au; return noErr; }
static OSStatus mock_start(AudioUnit au) { (void)au; unit.starts++; return noErr; }
static OSStatus mock_stop(AudioUnit au) { (void)au; unit.stops++; return noErr; }
static OSStatus mock_uninit(AudioUnit au) { (void)au; return noErr; }
static OSStatus mock_dispose(AudioComponentInstance au) { (void)au; unit.device = kAudioObjectUnknown; unit.disposals++; return noErr; }

// t_audio.c's expected vectors, fed through a 273-frame callback that splits a block mid-way
static int surround_vectors(int build_mode, int conv_fails) {
    enum { FRAMES = 273, SPLIT = 17 };
    static const int16_t expected[8][2] = {
        {10000, 0}, {0, 10000}, {7070, 7070}, {2509, 2509},
        {7070, 0}, {0, 7070}, {32767, 32767}, {-32768, -32768}
    };
    int ok = 1;
    for (int test = 0; test < 8; test++) {
        int16_t in[FRAMES * 6] = {0}, out[FRAMES * 2];
        for (int f = 0; f < FRAMES; f++) for (int ch = 0; ch < 6; ch++)
            in[f * 6 + ch] = test < 6 ? (test == ch ? 10000 : 0) : test == 6 ? 32767 : -32768;
        memset(out, 0xa5, sizeof out);
        gbuf b[2] = {{in, SPLIT * 12, SPLIT * 12}, {in + SPLIT * 6, (FRAMES - SPLIT) * 12, (FRAMES - SPLIT) * 12}};
        aout a = {.ch = 6};
        atomic_init(&a.state, STATE_STARTED); atomic_init(&a.vol, 0x3f800000u);
        a.q[0] = b; a.q[1] = b + 1; atomic_init(&a.q_tail, 2);
        arm_build(&a, build_mode);
        conv_fail = conv_fails;
        AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof out, out}}};
        au_render(&a, NULL, NULL, 0, FRAMES, &bl);
        conv_fail = 0;
        for (int f = 0; f < FRAMES && ok; f++) for (int ch = 0; ch < 2; ch++)
            if (out[f * 2 + ch] != expected[test][ch]) { printf("FAIL mode %d fallback: case %d frame %d channel %d expected %d got %d\n", build_mode, test, f, ch, expected[test][ch], out[f * 2 + ch]); ok = 0; }
        if (atomic_load(&a.arm_fail) != (build_mode == AM_MATRIX ? 0 : 3)) { printf("FAIL mode %d: fallback blocks counted %llu\n", build_mode, (unsigned long long)atomic_load(&a.arm_fail)); ok = 0; }
    }
    return ok;
}

int main(void) {
    setenv("DM_AUDIO", "1", 1);
    unsetenv("DM_DBG_AU_FRAMES"); unsetenv("DM_AUDIO_LATENCY"); unsetenv("DM_DBG_AUDIO_MODE"); unsetenv("DM_AUDIO_OUT");
    unit.device = 1; unit.rate = 48000; unit.range = (AudioValueRange){15, 4096}; unit.frames = 512;

    // 1) The picker: setting words, the diagnostic number, the override and both fallbacks.
    CHECK(mode_pick() == AM_MATRIX, "no setting: the host's own fold");
    setenv("DM_AUDIO_OUT", "spatial", 1); CHECK(mode_pick() == AM_SPATIAL, "Audio output spatial");
    setenv("DM_AUDIO_OUT", "wide", 1); CHECK(mode_pick() == AM_SPATIAL, "Audio output wide, a name the one spatial mode still accepts");
    setenv("DM_AUDIO_OUT", "binaural", 1); CHECK(mode_pick() == AM_SPATIAL, "Audio output binaural, likewise");
    setenv("DM_AUDIO_OUT", "sys", 1); CHECK(mode_pick() == AM_SYSCONV, "Audio output sys");
    setenv("DM_AUDIO_OUT", "matrix", 1); CHECK(mode_pick() == AM_MATRIX, "Audio output matrix");
    setenv("DM_AUDIO_OUT", "louder", 1); CHECK(mode_pick() == AM_MATRIX, "an unknown Audio output word falls back with a [RED] line");
    unsetenv("DM_AUDIO_OUT");
    setenv("DM_DBG_AUDIO_MODE", "2", 1); CHECK(mode_pick() == AM_SPATIAL, "the diagnostic mode number");
    setenv("DM_DBG_AUDIO_MODE", "9", 1); CHECK(mode_pick() == AM_MATRIX, "an unknown diagnostic number falls back");
    setenv("DM_DBG_AUDIO_MODE", "-1", 1); CHECK(mode_pick() == AM_MATRIX, "a negative diagnostic number falls back");
    setenv("DM_DBG_AUDIO_MODE", "0", 1); setenv("DM_AUDIO_OUT", "spatial", 1);
    CHECK(mode_pick() == AM_MATRIX, "the diagnostic arm overrides the Audio output setting");
    unsetenv("DM_DBG_AUDIO_MODE"); unsetenv("DM_AUDIO_OUT");

    // 2) Mode 0 is byte-exact, with no device and with a device that gives nothing; mode 1's healthy delivery
    //    ships the converter's own downmix verbatim.
    CHECK(surround_vectors(AM_MATRIX, 0), "mode 0 stays byte-exact");
    CHECK(surround_vectors(AM_SYSCONV, 1), "a downmix shortfall folds the block with the host's own coefficients");
    { aout a = {.ch = 6};
      enum { FN = 273 };
      static int16_t in1[FN * 6], out1[FN * 2];
      for (int f = 0; f < FN; f++) for (int c = 0; c < 6; c++) in1[f * 6 + c] = (int16_t)(f + c);   // every channel non-zero: a fold would not pass for the front pair
      memset(out1, 0xa5, sizeof out1);
      gbuf b1 = {in1, sizeof in1, sizeof in1};
      atomic_init(&a.state, STATE_STARTED); atomic_init(&a.vol, 0x3f800000u);
      a.q[0] = &b1; atomic_init(&a.q_tail, 1);
      arm_build(&a, AM_SYSCONV);
      static const unsigned ticks1[] = {100, 128, 45};   // straddles the 128-frame hand-off blocks
      unsigned done = 0;
      for (unsigned t = 0; t < sizeof ticks1 / sizeof *ticks1; t++) {
          AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, ticks1[t] * 4, out1 + done * 2}}};
          au_render(&a, NULL, NULL, 0, ticks1[t], &bl);
          done += ticks1[t];
      }
      CHECK(done == FN, "the mode 1 callbacks cover the whole feed");
      int ok1 = 1;
      for (int f = 0; f < FN && ok1; f++)
          if (out1[f * 2] != in1[f * 6] || out1[f * 2 + 1] != in1[f * 6 + 1]) {
              printf("FAIL mode 1 delivery: frame %d delivered as %d/%d\n", f, out1[f * 2], out1[f * 2 + 1]); ok1 = 0; }
      CHECK(ok1, "mode 1 delivers the converter's own downmix once, in order, across callback sizes");
      CHECK(atomic_load(&a.arm_fail) == 0, "a healthy converter never counts fallbacks");
      arm_free(&a); }
    ausm_fail_open = 1;
    { aout a = {.ch = 6}; arm_build(&a, AM_SPATIAL);
      CHECK(!a.arm && a.armMode == AM_MATRIX && atomic_load(&a.arm_fail) == 0, "a spatial mixer that cannot build leaves the host's own fold"); }
    ausm_fail_open = 0;

    // 3) The slice: the device's own tick, floored at 16, never the last callback's size.
    atomic_store(&g_au_frames, 16);
    { aout a = {.ch = 6}; arm_build(&a, AM_SPATIAL); CHECK(a.slice == 16, "the slice follows the granted tick (16 under the low-latency tier)"); }
    atomic_store(&g_au_frames, 512);
    { aout a = {.ch = 6}; arm_build(&a, AM_SPATIAL);
      CHECK(a.slice == 512, "the slice follows the granted tick");
      static int16_t in[273 * 6], out[273 * 2];
      gbuf b = {in, sizeof in, sizeof in};
      atomic_init(&a.state, STATE_STARTED); atomic_init(&a.vol, 0x3f800000u);
      a.q[0] = &b; atomic_init(&a.q_tail, 1);
      AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof out, out}}};
      au_render(&a, NULL, NULL, 0, 273, &bl);
      CHECK(a.slice == 512, "an oversized transient callback never becomes the slice"); }
    atomic_store(&g_au_frames, 8);
    { aout a = {.ch = 6}; arm_build(&a, AM_SPATIAL); CHECK(a.slice == 16, "the slice floors at 16"); }
    atomic_store(&g_au_frames, 0);
    { aout a = {.ch = 6}; arm_build(&a, AM_SPATIAL); CHECK(a.slice == RATE / 200, "without a granted tick the slice is the standard 5 ms"); }

    // 4) The banked slice: whole 16-frame slices delivered across callback sizes 100/16/5/240/273 with nothing
    //    repeated, dropped or reordered - the arbitrary-callback-size case the fixed-tick devices must survive,
    //    and the low-latency tier x mode combination in one.
    atomic_store(&g_au_frames, 16);
    enum { N = 100 + 16 + 5 + 240 + 273 };
    static int16_t big_in[N * 6], big_out[N * 2];
    for (int f = 0; f < N; f++) { big_in[f * 6] = (int16_t)f; big_in[f * 6 + 1] = (int16_t)(f + 1); }
    { aout a = {.ch = 6};
      gbuf b = {big_in, sizeof big_in, sizeof big_in};
      atomic_init(&a.state, STATE_STARTED); atomic_init(&a.vol, 0x3f800000u);
      a.q[0] = &b; atomic_init(&a.q_tail, 1);
      arm_build(&a, AM_SPATIAL);
      CHECK(a.slice == 16, "the banked run builds on the 16-frame slice");
      static const unsigned ticks[] = {100, 16, 5, 240, 273};
      unsigned done = 0;
      for (unsigned t = 0; t < sizeof ticks / sizeof *ticks; t++) {
          int16_t out[273 * 2];
          AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, ticks[t] * 4, out}}};
          au_render(&a, NULL, NULL, 0, ticks[t], &bl);
          memcpy(big_out + done * 2, out, (size_t)ticks[t] * 4);
          done += ticks[t];
      }
      CHECK(done == N, "the tick sizes cover the whole feed");
      int ok = 1;
      for (int f = 0; f < N && ok; f++) if (big_out[f * 2] != (int16_t)f || big_out[f * 2 + 1] != (int16_t)(f + 1)) {
          printf("FAIL banked slice: frame %d delivered as %d/%d\n", f, big_out[f * 2], big_out[f * 2 + 1]); ok = 0; }
      CHECK(ok, "every rendered frame is delivered exactly once, in order, across callback sizes");
      CHECK(atomic_load(&a.arm_fail) == 0, "a healthy device never counts fallbacks");
      CHECK(atomic_load(&a.q_head) == 1 && a.ringEnd - a.ringPos <= a.slice, "the whole feed is consumed and the bank never holds more than one slice");
      arm_free(&a); }
    atomic_store(&g_au_frames, 0);

    // 5) dm_audio_set_mode on the real lifecycle: Start hands the output to the silent sink and the start runs
    //    on the host worker, the watchdog joins the unit once it calls back; each switch then stops the unit,
    //    rebuilds the device, publishes the mode and restarts it the same way. Stereo outputs and outputs
    //    already carrying the mode are left alone; the previous device is closed, not leaked. One rendered
    //    callback is what lets the watchdog join, so every step drives one.
    uint32_t out51[16] = {0}, out2ch[16] = {0};
    uint8_t prm51[16], prm2[16]; h_au_InitParam(prm51); h_au_InitParam(prm2);
    *(uint16_t *)(prm51 + 4) = 6;
    CHECK(!h_au_OpenDef0(out51, prm51), "open a 5.1 output");
    CHECK(!h_au_OpenDef0(out2ch, prm2), "open a stereo output");
    aout *a51 = ao(out51), *a2 = ao(out2ch);
    static int16_t joinbuf[64];
    AudioBufferList jbl = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof joinbuf, joinbuf}}};
    atomic_store(&a2->state, STATE_STARTED);   // a stereo output that reads as playing; Start itself sets the 5.1 output's state
    CHECK(!h_au_Start(out51), "start the 5.1 output");
    for (int i = 0; i < 2000 && a51->start_phase != AU_WAIT_CALLBACK; i++) usleep(1000);
    CHECK(a51->start_phase == AU_WAIT_CALLBACK && a51->backend == BK_NULL, "the start is in flight on the host worker, the silent sink consuming");
    au_render(a51, NULL, NULL, 0, 16, &jbl);
    for (int i = 0; i < 2000 && a51->backend != BK_AU; i++) usleep(1000);
    CHECK(a51->backend == BK_AU && !atomic_load(&a51->hold_device), "the watchdog joined the real unit off the silent sink");
    unsigned stops0 = unit.stops, starts0 = unit.starts;
    ausm_open_n = 0; ausm_close_n = 0; mode_at_ausm_open = -1;
    static const int modes[] = {AM_SPATIAL, AM_MATRIX};
    for (unsigned s = 0; s < sizeof modes / sizeof *modes; s++) {
        dm_audio_set_mode(modes[s]);
        // Wait for the worker's WAIT_CALLBACK (which stamps the watchdog's callback baseline in the same
        // locked section), not for the mock's start count: a callback rendered between the two would equal
        // the baseline and the join would never fire.
        for (int i = 0; i < 2000 && a51->start_phase != AU_WAIT_CALLBACK; i++) usleep(1000);
        CHECK(unit.stops == stops0 + s + 1 && unit.starts == starts0 + s + 1, "every switch stops and starts the playing output once, the start on the host worker");
        if (s == 0) {
            CHECK(ausm_open_n == 1 && mode_at_ausm_open == AM_MATRIX, "the device builds while the old mode is still published: the switch lands after the rebuild");
            CHECK(atomic_load(&g_mode) == AM_SPATIAL && !strcmp(dm_audio_mode_name(), "spatial (auto)"), "the new mode is published, with its name");
            CHECK(a51->armMode == AM_SPATIAL && a51->arm && a51->slice == 240, "the 5.1 output carries the new device at the granted tick");
            CHECK(a51->backend == BK_NULL && atomic_load(&a51->hold_device), "the silent sink covers the restart gap");
            CHECK(a2->armMode == AM_MATRIX && !a2->arm, "a stereo output is neither stopped nor given a device");
        }
        if (s == 1) CHECK(a51->armMode == AM_MATRIX && !a51->arm && ausm_close_n == 1, "back to the host's own fold frees the device");
        au_render(a51, NULL, NULL, 0, 16, &jbl);
        for (int i = 0; i < 2000 && a51->backend != BK_AU; i++) usleep(1000);
        CHECK(a51->backend == BK_AU, "the restarted unit takes the output back");
    }
    unsigned stops_before = unit.stops;
    dm_audio_set_mode(AM_MATRIX);
    CHECK(unit.stops == stops_before && ausm_close_n == 1, "a mode the output already carries changes nothing");
    // Suspended: the silent sink is the consumer, so the arm is rebuilt and no start runs until the resume.
    dm_audio_suspend(1);
    CHECK(unit.stops == stops_before + 1 && a51->backend == BK_NULL && a2->backend == BK_NULL, "suspend stops the real unit and hands both outputs to the silent sink");
    int opens_at = ausm_open_n; unsigned starts_susp = unit.starts;
    dm_audio_set_mode(AM_SPATIAL);
    usleep(50000);
    CHECK(unit.starts == starts_susp && ausm_open_n == opens_at + 1 && a51->armMode == AM_SPATIAL, "a suspended output's arm is rebuilt, and no start runs until the resume");
    dm_audio_suspend(0);
    for (int i = 0; i < 2000 && (a51->start_phase != AU_WAIT_CALLBACK || a2->start_phase != AU_WAIT_CALLBACK); i++) usleep(1000);
    CHECK(unit.starts == starts_susp + 2, "the resume restarts both outputs' units on the host worker");
    au_render(a51, NULL, NULL, 0, 16, &jbl); au_render(a2, NULL, NULL, 0, 16, &jbl);
    for (int i = 0; i < 2000 && (a51->backend != BK_AU || a2->backend != BK_AU); i++) usleep(1000);
    CHECK(a51->backend == BK_AU && a2->backend == BK_AU, "the watchdog joins the resumed units");
    // Stopped: the arm is rebuilt without a stop or a start; Start picks it up.
    h_au_Stop(out51);
    for (int i = 0; i < 2000 && a51->backend != BK_NONE; i++) usleep(1000);
    CHECK(a51->backend == BK_NONE, "stop leaves no consumer on the output");
    dm_audio_set_mode(AM_MATRIX);
    CHECK(unit.stops == stops_before + 2 && unit.starts == starts_susp + 2 && ausm_open_n == opens_at + 1 && ausm_close_n == 2, "a stopped output's arm is rebuilt without a stop or a start");
    dm_audio_set_mode(-1);
    CHECK(atomic_load(&g_mode) == AM_MATRIX && a51->armMode == AM_MATRIX, "an unknown mode number changes nothing");
    CHECK(!strcmp(dm_audio_mode_name(), "matrix"), "the name follows the mode in effect");

    printf("audio output modes: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
