// The spatial stages, offline: real AudioToolbox, no output unit, nothing played. hle_audio.c's AUSpatialMixer (spatial_mixer_make) is made for the built-in speakers and for
// headphones exactly as the host makes it, its formats read back (six interleaved float in with MPEG_5_1_A, stereo non-interleaved float out, the output type, the ambience bed),
// then rendered into memory with AudioUnitRender from a DeviceSink queue of float blocks, as the output unit's callback would pull it. Checks: the queue is consumed and released,
// the level against the stereo fold of the same blocks (the make-up gain question: none needed here), every channel heard (the LFE too, on the speakers through the
// host's redirect). In `run`: it needs no device, only the AudioToolbox component
#include "../hle_audio.c"
#include <math.h>
#include <time.h>
__thread _Atomic int dm_host_work;
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
void dm_audio_device_changed(void) {}

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

enum { FRAMES = 240, BLOCKS = 40, SLICE = 240 };
static float blocks[BLOCKS][FRAMES * DM_AUDIO_SINK_CHANNELS];
static aout_buf bufs[BLOCKS];
static unsigned seed[6];
static const float FOLD_C_ = 23167.0f / 32768.0f, FOLD_LFE_ = 8224.0f / 32768.0f;   // hle_audren.c's fold
static float noise(int c) { seed[c] = seed[c] * 1664525u + 1013904223u; return ((float)(seed[c] >> 8) / 16777216.0f) * 2 - 1; }
// Fill the blocks: decorrelated noise at 0.25 on the channels in mask (bit j: channel j), the fold beside them as hle_audren.c computes it
static float fold_peak;
static double fill(unsigned mask) {
    double fold_e = 0; fold_peak = 0;
    for (int c = 0; c < 6; c++) seed[c] = 1000u + 77u * (unsigned)c;
    for (int b = 0; b < BLOCKS; b++) for (int i = 0; i < FRAMES; i++) {
        float *f = blocks[b] + i * DM_AUDIO_SINK_CHANNELS;
        for (int c = 0; c < 6; c++) f[c] = mask >> c & 1 ? 0.25f * noise(c) : 0;
        float ce = f[2] * FOLD_C_ + f[3] * FOLD_LFE_; f[6] = f[0] + ce + f[4] * FOLD_C_; f[7] = f[1] + ce + f[5] * FOLD_C_;
        if (b >= 4) { fold_e += (double)f[6] * f[6] + (double)f[7] * f[7]; fold_peak = fmaxf(fold_peak, fmaxf(fabsf(f[6]), fabsf(f[7]))); }
    }
    return fold_e;
}
// Render the queue through the mixer, SLICE frames at a time, into non-interleaved stereo; energy after the first 4 blocks (the mixer's own short latency settles)
static double render(aout *a, float *peak) {
    static float L[SLICE], R[SLICE];
    AudioBufferList *bl = calloc(1, sizeof(AudioBufferList) + sizeof(AudioBuffer));
    bl->mNumberBuffers = 2;
    AudioTimeStamp ts = {.mFlags = kAudioTimeStampSampleTimeValid};
    double e = 0; *peak = 0;
    for (int s = 0; s < BLOCKS * FRAMES / SLICE; s++) {
        bl->mBuffers[0] = (AudioBuffer){1, sizeof L, L}; bl->mBuffers[1] = (AudioBuffer){1, sizeof R, R};
        AudioUnitRenderActionFlags fl = 0;
        OSStatus st = AudioUnitRender(a->mixer, &fl, &ts, 0, SLICE, bl);
        CHECK(st == noErr);
        ts.mSampleTime += SLICE;
        if (s >= 4 * FRAMES / SLICE) for (int i = 0; i < SLICE; i++) { e += (double)L[i] * L[i] + (double)R[i] * R[i]; *peak = fmaxf(*peak, fmaxf(fabsf(L[i]), fabsf(R[i]))); }
    }
    free(bl);
    return e;
}

static void test_mixer(int type, const char *name) {
    void *key = dm_pcmout_open_sink(); aout *a = ao(key);
    CHECK(a->sink_pcm && a->bps == 4 && !a->au);   // DM_AUDIO=0: no output unit at all
    a->dev_ch = 6;
    a->mixer = spatial_mixer_make(a, type); a->mixer_type = type;
    CHECK(a->mixer != NULL);
    if (!a->mixer) { dm_pcmout_close(key); return; }
    // The formats and properties as the host set them, read back
    AudioStreamBasicDescription in, out; UInt32 sz = sizeof in;
    CHECK(!AudioUnitGetProperty(a->mixer, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &in, &sz));
    CHECK(in.mSampleRate == 48000 && in.mChannelsPerFrame == 6 && in.mBitsPerChannel == 32 && (in.mFormatFlags & kAudioFormatFlagIsFloat) && !(in.mFormatFlags & kAudioFormatFlagIsNonInterleaved));
    sz = sizeof out;
    CHECK(!AudioUnitGetProperty(a->mixer, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &out, &sz));
    CHECK(out.mSampleRate == 48000 && out.mChannelsPerFrame == 2 && (out.mFormatFlags & kAudioFormatFlagIsFloat) && (out.mFormatFlags & kAudioFormatFlagIsNonInterleaved));
    struct { AudioChannelLayout l; AudioChannelDescription more[8]; } lay; sz = sizeof lay;
    CHECK(!AudioUnitGetProperty(a->mixer, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, &lay, &sz) && lay.l.mChannelLayoutTag == kAudioChannelLayoutTag_MPEG_5_1_A);
    UInt32 v = 0; sz = sizeof v;
    CHECK(!AudioUnitGetProperty(a->mixer, kAudioUnitProperty_SpatialMixerOutputType, kAudioUnitScope_Global, 0, &v, &sz) && v == (UInt32)type);
    sz = sizeof v; CHECK(!AudioUnitGetProperty(a->mixer, kAudioUnitProperty_SpatializationAlgorithm, kAudioUnitScope_Input, 0, &v, &sz) && v == kSpatializationAlgorithm_UseOutputType);
    sz = sizeof v; CHECK(!AudioUnitGetProperty(a->mixer, kAudioUnitProperty_SpatialMixerSourceMode, kAudioUnitScope_Input, 0, &v, &sz) && v == kSpatialMixerSourceMode_AmbienceBed);
    Float64 lat = -1; sz = sizeof lat; AudioUnitGetProperty(a->mixer, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &lat, &sz);
    UInt32 personal = 0; sz = sizeof personal;
    OSStatus pst = AudioUnitGetProperty(a->mixer, kAudioUnitProperty_SpatialMixerAnyInputIsUsingPersonalizedHRTF, kAudioUnitScope_Global, 0, &personal, &sz);
    printf("AUSpatialMixer for %s: latency %.2f ms, personalized HRTF %s\n", name, lat * 1000, pst ? "(not answered)" : personal ? "in use" : "not in use");
    CHECK(lat >= 0 && lat < 0.005);
    // Level against the fold: all six together, then each channel alone (the LFE included)
    const unsigned masks[] = {0x3f, 1, 2, 4, 8, 16, 32};
    const char *what[] = {"all six", "front-left", "front-right", "center", "LFE", "back-left", "back-right"};
    for (unsigned m = 0; m < sizeof masks / sizeof *masks; m++) {
        double fold_e = fill(masks[m]);
        atomic_store(&a->state, STATE_STARTED);
        for (int b = 0; b < BLOCKS; b++) { bufs[b] = (aout_buf){.ptr = blocks[b], .cap = sizeof blocks[b], .size = sizeof blocks[b]}; CHECK(dm_pcmout_append(key, &bufs[b])); }
        float peak; double e = render(a, &peak);
        atomic_store(&a->state, STATE_STOPPED);
        int released = 0; while (dm_pcmout_released(key)) released++;
        CHECK(released == BLOCKS);   // the mixer pulled every block through the host's queue
        double db = 10 * log10(e / fold_e);
        printf("  %-11s output vs the fold %+6.2f dB, peak %.3f (the fold's %.3f)\n", what[m], db, peak, fold_peak);
        CHECK(e > 0);                        // every channel is heard (on the speakers the LFE through the front pair)
        CHECK(db > -3.0 && db < 9.0);        // no make-up gain needed: never more than 3 dB under the fold (headphones: louder with AirPods the default output, up to +6.9 dB on a back channel)
        CHECK(peak < 4.0f * fold_peak);      // a sanity bound only: the HRTF filters raise white noise's crest factor (a lone back channel peaks 2.7x the fold's), the RMS above is the level
    }
    out_dispose(a);
    CHECK(!a->mixer && !a->au);
    dm_pcmout_close(key);
}

int main(void) {
    setenv("DM_AUDIO", "0", 1);   // outputs on the silent sink path: no output unit is ever made or started
    CHECK(route_can() & AR_CAN_MIXER);
    test_mixer(AR_MIXER_BUILTIN_SPEAKERS, "the built-in speakers");
    test_mixer(AR_MIXER_HEADPHONES, "headphones");
    printf("audio spatial: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
