// Real Core Audio, no mocks: plays a 5.1 test signal (a tone on each of the six channels) through the host's AudioOut on the device named by
// DM_AUDIO_DEVICE (a virtual device such as BlackHole 2ch / 16ch), and reports the route the host reads. Not part of `run`: it needs the device.
// Run: DM_AUDIO_DEVICE="BlackHole 16ch" build/t_audio_live [seconds]; DM_AUDIO_CAPTURE=<file> (DM_AUDIO_CAPTURE_CH, DM_AUDIO_CAPTURE_DEVICE) records a loopback device's input in-process.
// DM_AUDIO_LIVE_PCM=<file> plays a prepared six-channel file instead of the tones (an ear check of a real speaker system).
// Route change: build/t_audio_live 5 default="BlackHole 16ch" switch="BlackHole 2ch"@2 with DM_AUDIO_CAPTURE_DEVICE="BlackHole 2ch" records what the new device gets
#include "../hle_audio.c"
#include <time.h>
#include <math.h>
#include <string.h>
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

// Named device lookup (the same match as DM_AUDIO_DEVICE) and the system default output, for the mid-session switch test
static AudioDeviceID named_device(const char *want) {
    AudioObjectPropertyAddress pa = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, NULL, &sz);
    AudioDeviceID *ids = malloc(sz); AudioDeviceID found = kAudioObjectUnknown;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &sz, ids);
    for (UInt32 i = 0; i < sz / sizeof *ids; i++) {
        AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        CFStringRef n = NULL; UInt32 nsz = sizeof n; char buf[256] = {0};
        if (!AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &nsz, &n) && n) {
            if (CFStringGetCString(n, buf, sizeof buf, kCFStringEncodingUTF8) && strstr(buf, want)) found = ids[i];
            CFRelease(n);
        }
    }
    free(ids); return found;
}
static AudioDeviceID default_output(void) {
    AudioObjectPropertyAddress pa = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID d = kAudioObjectUnknown; UInt32 sz = sizeof d; AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &sz, &d); return d;
}
static void set_default_output(AudioDeviceID d) {
    AudioObjectPropertyAddress pa = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    OSStatus st = AudioObjectSetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, sizeof d, &d);
    printf("default output set (status %d)\n", (int)st);
}
// In-process capture of the device's input (DM_AUDIO_CAPTURE=<file>, channels DM_AUDIO_CAPTURE_CH): the loopback device's input is what its output
// played, read by an input AudioUnit in this process, so no recorder can drop, truncate or resample it. Raw float32, interleaved.
typedef struct { AudioUnit au; float *buf; uint64_t n, cap; UInt32 nch; AudioBufferList *bl; } capture_t;
static OSStatus capture_input(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *unused) {
    (void)unused; capture_t *c = ctx;
    c->bl->mBuffers[0].mDataByteSize = frames * c->nch * sizeof(float);
    OSStatus st = AudioUnitRender(c->au, flags, ts, bus, frames, c->bl);
    if (!st && c->n + frames <= c->cap) { memcpy(c->buf + c->n * c->nch, c->bl->mBuffers[0].mData, frames * c->nch * sizeof(float)); c->n += frames; }
    return st;
}
static capture_t *capture_start(AudioDeviceID dev, UInt32 nch, uint64_t frames) {
    capture_t *c = calloc(1, sizeof *c); c->nch = nch; c->cap = frames;
    c->buf = malloc(frames * nch * sizeof(float));
    c->bl = calloc(1, sizeof(AudioBufferList) + sizeof(AudioBuffer));
    c->bl->mNumberBuffers = 1; c->bl->mBuffers[0].mNumberChannels = nch; c->bl->mBuffers[0].mData = malloc(4096 * nch * sizeof(float));
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple};
    AudioComponent comp = AudioComponentFindNext(NULL, &d); AudioComponentInstanceNew(comp, &c->au);
    UInt32 on = 1, off = 0;
    AudioUnitSetProperty(c->au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof on);
    AudioUnitSetProperty(c->au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof off);
    AudioUnitSetProperty(c->au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev);
    AudioStreamBasicDescription f = {.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
                                     .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, .mBytesPerPacket = 4 * nch,
                                     .mFramesPerPacket = 1, .mBytesPerFrame = 4 * nch, .mChannelsPerFrame = nch, .mBitsPerChannel = 32};
    OSStatus st = AudioUnitSetProperty(c->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f);
    AURenderCallbackStruct cb = {capture_input, c};
    if (!st) st = AudioUnitSetProperty(c->au, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
    if (!st) st = AudioUnitInitialize(c->au);
    if (!st) st = AudioOutputUnitStart(c->au);
    if (st) { printf("capture: cannot start the input (status %d)\n", (int)st); }
    return c;
}
static void capture_stop(capture_t *c, const char *path) {
    AudioOutputUnitStop(c->au); AudioUnitUninitialize(c->au); AudioComponentInstanceDispose(c->au);
    FILE *f = fopen(path, "wb"); if (f) { fwrite(c->buf, sizeof(float), c->n * c->nch, f); fclose(f); }
    printf("capture: %llu frames of %u channels to %s\n", (unsigned long long)c->n, c->nch, path);
}
// usage: t_audio_live [seconds] [default=<name>] [switch=<name>@<seconds>]   (no DM_AUDIO_DEVICE: the unit follows the system default output)
// The system's own notification of a default output change (the AudioUnit's listeners say nothing when it follows the default)
static OSStatus default_changed(AudioObjectID id, UInt32 n, const AudioObjectPropertyAddress *pa, void *ctx) {
    (void)id; (void)n; (void)pa; (void)ctx; printf("[probe] system: default output device changed\n"); fflush(stdout); return noErr;
}
// The unit's formats: what our callback is asked for (input scope) and what the device takes (output scope)
static void unit_formats(aout *a, const char *when) {   // under g_aos_lock: a route change replaces the unit under it
    pthread_mutex_lock(&g_aos_lock); AudioUnit au = a->au;
    AudioStreamBasicDescription in = {0}, outf = {0}; UInt32 sz = sizeof in;
    AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &in, &sz); sz = sizeof outf;
    AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &outf, &sz);
    printf("[probe] %s: unit input %u ch %u-bit flags %#x %.0f Hz; output %u ch %u-bit flags %#x %.0f Hz\n", when,
           (unsigned)in.mChannelsPerFrame, (unsigned)in.mBitsPerChannel, (unsigned)in.mFormatFlags, in.mSampleRate,
           (unsigned)outf.mChannelsPerFrame, (unsigned)outf.mBitsPerChannel, (unsigned)outf.mFormatFlags, outf.mSampleRate);
    pthread_mutex_unlock(&g_aos_lock); fflush(stdout);
}
int main(int argc, char **argv) {
    setenv("DM_AUDIO", "1", 1);
    AudioObjectPropertyAddress dpa = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &dpa, default_changed, NULL);
    int secs = 3; const char *def_name = NULL, *sw_name = NULL; int sw_at = -1;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "default=", 8)) def_name = argv[i] + 8;
        else if (!strncmp(argv[i], "switch=", 7)) { char *at = strrchr(argv[i], '@'); sw_name = argv[i] + 7; if (at) { *at = 0; sw_at = atoi(at + 1); } }
        else secs = atoi(argv[i]);
    }
    if (!getenv("DM_AUDIO_DEVICE") && !def_name) { puts("set DM_AUDIO_DEVICE, or default=<name>"); return 2; }
    AudioDeviceID original = default_output();
    if (def_name) { AudioDeviceID d = named_device(def_name); if (d == kAudioObjectUnknown) { puts("no such default device"); return 2; } set_default_output(d); }
    int route = dm_audio_output_channels();
    printf("route: %d channels\n", route);
    uint32_t out[16] = {0}; uint8_t prm[16]; h_au_InitParam(prm); *(uint16_t *)(prm + 4) = 6;
    if (h_au_OpenDef0(out, prm) != RES_OK) { puts("open failed"); return 1; }
    aout *a = ao(out);
    printf("opened: guest %d channels, unit %d channels, downmix %s\n", a->ch, a->dev_ch, a->dev_ch == a->ch ? "no" : "yes");
    // Frequencies: front-left 440 Hz, front-right 660 Hz, center 880 Hz, LFE 60 Hz, back-left 1320 Hz, back-right 1760 Hz; 0.2 full scale each
    // DM_AUDIO_LIVE_PCM=<file>: play that instead (16-bit 48 kHz, six channels interleaved in the guest's order), e.g. a spoken name on each speaker to check a real system by ear
    static const double hz[6] = {440, 660, 880, 60, 1320, 1760};
    const char *pcm_path = getenv("DM_AUDIO_LIVE_PCM");
    size_t frames = (size_t)secs * RATE;
    int16_t *pcm;
    if (pcm_path) {
        FILE *pf = fopen(pcm_path, "rb"); if (!pf) { printf("cannot open %s\n", pcm_path); return 2; }
        fseek(pf, 0, SEEK_END); frames = (size_t)ftell(pf) / 12; rewind(pf);
        pcm = malloc(frames * 12); if (fread(pcm, 12, frames, pf) != frames) { puts("short read"); return 2; }
        fclose(pf); secs = (int)(frames / RATE) + 1;
        printf("playing %s: %.1f s\n", pcm_path, (double)frames / RATE);
    } else {
        pcm = malloc(frames * 12);
        for (size_t i = 0; i < frames; i++) for (int j = 0; j < 6; j++)
            pcm[6 * i + j] = (int16_t)lrint(0.2 * 32767 * sin(2 * M_PI * hz[j] * (double)i / RATE));
    }
    gbuf b = {pcm, frames * 12, frames * 12};
    if (!h_au_Append(out, &b)) { puts("append refused"); return 1; }
    const char *cap_path = getenv("DM_AUDIO_CAPTURE"); capture_t *cap = NULL;
    if (cap_path) {
        const char *cd = getenv("DM_AUDIO_CAPTURE_DEVICE");   // another device than the one playing at start (the switch target)
        AudioDeviceID dev = cd ? named_device(cd) : out_device();
        UInt32 nch = (UInt32)atoi(getenv("DM_AUDIO_CAPTURE_CH") ? getenv("DM_AUDIO_CAPTURE_CH") : "6");
        cap = capture_start(dev, nch, (uint64_t)(secs + 2) * RATE);
    }
    h_au_Start(out);
    if (sw_name && sw_at >= 0) {
        usleep((useconds_t)sw_at * 1000000);
        AudioDeviceID d = named_device(sw_name);
        printf("switching the default output to %s at %d s\n", sw_name, sw_at);
        unit_formats(a, "before the switch");
        if (d != kAudioObjectUnknown) set_default_output(d);
        usleep(500000);
        unit_formats(a, "0.5 s after the switch");
        usleep((useconds_t)(secs - sw_at) * 1000000 - 500000);
    }
    uint64_t t_start = now_ns(), until = t_start + (uint64_t)(secs + 1) * NSEC_PER_SEC;
    while (!h_au_Released(out) && now_ns() < until) usleep(10000);
    printf("block released %.2f s after start (%d s of audio)\n", (now_ns() - t_start) / 1e9, secs);
    usleep((useconds_t)secs * 1000000);
    printf("state %d, queued %u, unit %d channels\n", atomic_load(&a->state), h_au_BufCount(out), a->dev_ch);
    h_au_Close(out);
    if (cap) capture_stop(cap, cap_path);
    free(pcm);
    if (def_name || (sw_name && sw_at >= 0)) set_default_output(original);   // the system default as it was before the probe
    puts("live probe done");
    return 0;
}
