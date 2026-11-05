// Under Mac spatial audio (DM_AUDIO_SPATIAL) capture hands the DeviceSink's six channels over as they are, interleaved FL FR C LFE BL BR, for hle_audio.c's 5.1 bed:
// each output channel is the mix buffer its sink input names (in the sink's order, not the buffers'), a sink with two inputs fills the fronts and leaves the rest silent,
// values are clamped to int16, and a 32 kHz renderer goes through the 3:2 resampler on all six channels (a constant level per channel stays that level, no channel leaks into another)
#include "../hle.h"
#include <assert.h>
#include <sys/wait.h>
#include "../hle_audren.c"

void *guest_sym(const char *name) { (void)name; return NULL; }
void *objmap_get(const void *key) { (void)key; return NULL; }
void objmap_put(const void *key, void *value) { (void)key; (void)value; }
void *objmap_del(const void *key) { (void)key; return NULL; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack) { (void)fn; (void)arg; (void)prio; (void)stack; return NULL; }
void dm_guest_thread_join(void *tt) { (void)tt; }
uint64_t now_ns(void) { return 0; }
void *dm_pcmout_open(int ch) { (void)ch; return NULL; }
void dm_pcmout_close(void *k) { (void)k; }
void dm_pcmout_start(void *k) { (void)k; }
int dm_pcmout_append(void *k, void *b) { (void)k; (void)b; return 0; }
void *dm_pcmout_released(void *k) { (void)k; return NULL; }
void dm_pcmout_wait(void *k, int ms) { (void)k; (void)ms; }
int dm_audio_spatial(void) { return 1; }   // spatial audio on: six output channels
void hle_bind_system_event(void *ev, int ac) { (void)ev; (void)ac; }
uint32_t hle_dup_readable_handle(uint32_t h) { return h; }

enum { SAMPLES = 240, NMIX = 8, NCMD = 6 };
static int32_t mix[NMIX * SAMPLES];
_Alignas(8) static uint8_t list[0x20 + NCMD * 0x148];
static const int16_t sink_in[6] = {7, 2, 5, 0, 3, 6};   // sink input j reads mix buffer sink_in[j]
static const int32_t level[6] = {1000, -2000, 3000, -4000, 5000, 40000};   // per output channel; the last one past int16 (clamped)

// An SDK 16 command list: NCMD - 1 filler commands, then a DeviceSink (ID 0x13, 0x148 bytes, input count +0x130, inputs +0x134)
static size_t build(uint32_t rate, uint32_t samples, uint32_t nin) {
    memset(list, 0, sizeof list);
    *(uint32_t *)(list + 4) = NCMD; *(const int32_t **)(list + 8) = mix; *(uint32_t *)(list + 0x14) = samples; *(uint32_t *)(list + 0x18) = rate;
    uint8_t *q = list + 0x20;
    for (int i = 0; i < NCMD; i++) {
        int sink = i == NCMD - 1, size = sink ? 0x148 : 0x40;
        *(uint32_t *)q = 0xcafebabeu; q[4] = 1; q[5] = (uint8_t)(sink ? 0x13 : 6); *(int16_t *)(q + 6) = (int16_t)size;
        if (sink) { *(uint32_t *)(q + 0x130) = nin; for (uint32_t j = 0; j < nin; j++) *(int16_t *)(q + 0x134 + 2 * j) = sink_in[j]; }
        q += size;
    }
    return (size_t)(q - list);
}
static void fill(uint32_t samples) {
    for (int j = 0; j < 6; j++) for (uint32_t s = 0; s < samples; s++) mix[(size_t)sink_in[j] * samples + s] = level[j];
}
static int16_t expect(int j) { return (int16_t)(level[j] > 32767 ? 32767 : level[j] < -32768 ? -32768 : level[j]); }

static void test_48k_six(void) {
    int16_t out[FRAME_MAX * 6];
    fill(SAMPLES);
    assert(capture(&g_ar[0], list, build(48000, SAMPLES, 6), out) == SAMPLES);
    for (int s = 0; s < SAMPLES; s++) for (int j = 0; j < 6; j++) assert(out[6 * s + j] == expect(j));
}
static void test_48k_two(void) {
    int16_t out[FRAME_MAX * 6];
    fill(SAMPLES);
    assert(capture(&g_ar[0], list, build(48000, SAMPLES, 2), out) == SAMPLES);
    for (int s = 0; s < SAMPLES; s++) for (int j = 0; j < 6; j++) assert(out[6 * s + j] == (j < 2 ? expect(j) : 0));
}
static void test_32k_six(void) {
    enum { S32 = 160 };   // 5 ms at 32 kHz: 240 frames at 48 kHz
    int16_t out[FRAME_MAX * 6];
    fill(S32);
    size_t n = build(32000, S32, 6);
    for (int frame = 0; frame < 4; frame++) {
        int got = capture(&g_ar[0], list, n, out);
        assert(got == 240);
        if (frame == 0) continue;   // the filter's history starts at zero: the first frame ramps in
        for (int s = 0; s < got; s++) for (int j = 0; j < 6; j++) {
            int d = out[6 * s + j] - expect(j);
            assert(d >= -1 && d <= 1);
        }
    }
}

int main(void) {
    void (*const tests[])(void) = {test_48k_six, test_48k_two, test_32k_six};
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++) {
        // A fresh process each: capture's layout detection and the resampler's history are per process
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) { tests[i](); _exit(0); }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("audren spatial: six channels in sink order, two-input sink on the fronts, int16 clamp and the 32 kHz resampler on six channels passed");
    return 0;
}
