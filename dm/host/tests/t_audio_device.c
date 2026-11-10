// Mac device buffer contract: drive the real setup/listener/close paths with Core Audio property notifications,
// including synchronous format notifications from a buffer write and device removal during disposal. No hardware needed.
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <stddef.h>
#include <time.h>

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
static OSStatus mock_aogpd(AudioObjectID id, const AudioObjectPropertyAddress *pa, UInt32 q, const void *qd, UInt32 *sz, void *data);
#define AudioObjectGetPropertyData mock_aogpd
static OSStatus mock_aogpds(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
#define AudioObjectGetPropertyDataSize mock_aogpds
#include "../hle_audio.c"
__thread _Atomic int dm_host_work;   // defined by the graphics host in a full build

static struct {
    AudioDeviceID device;
    Float64 rate;
    AudioValueRange range;
    UInt32 frames;
    AudioUnitPropertyListenerProc device_listener, format_listener;
    unsigned writes, disposals, input_formats;
    OSStatus init_error;
    int route_ch;                        // the device's output stream format channels (the route)
    int format_ch;                       // channels of the unit's input format, as last set
    AudioChannelLayoutTag layout_tag;    // the unit's input channel layout, as last set (0: none)
} unit;
static AudioUnit fake_au(void) { return (AudioUnit)&unit; }
static int check_open_publication;
static void changed(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el) {
    AudioUnitPropertyListenerProc cb = id == kAudioOutputUnitProperty_CurrentDevice ? unit.device_listener : unit.format_listener;
    if (cb) cb(&unit, fake_au(), id, scope, el);
}
static AudioComponent mock_find(AudioComponent c, const AudioComponentDescription *d) {
    (void)c; assert(d->componentSubType == kAudioUnitSubType_DefaultOutput); return (AudioComponent)&unit;
}
static OSStatus mock_new(AudioComponent c, AudioComponentInstance *au) {
    if (check_open_publication) {
        // Lifecycle/watchdog readers must never find the output while its unit is being constructed.
        aout *pending = (aout *)((char *)au - offsetof(aout, au));
        pthread_mutex_lock(&g_aos_lock);
        for (int i = 0; i < AOMAX; i++) assert(g_aos[i] != pending);
        pthread_mutex_unlock(&g_aos_lock);
    }
    (void)c; assert(!unit.device_listener && !unit.format_listener); *au = fake_au(); return noErr;
}
static OSStatus mock_get(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, void *data, UInt32 *size) {
    assert(au == fake_au() && el == 0);
    if (id == kAudioOutputUnitProperty_CurrentDevice) {
        assert(scope == kAudioUnitScope_Global && *size == sizeof unit.device); memcpy(data, &unit.device, *size);
    } else if (id == kAudioUnitProperty_StreamFormat) {
        assert(scope == kAudioUnitScope_Output && *size == sizeof(AudioStreamBasicDescription));
        AudioStreamBasicDescription f = {.mSampleRate = unit.rate}; memcpy(data, &f, *size);
    } else if (id == kAudioDevicePropertyBufferFrameSizeRange) {
        assert(scope == kAudioUnitScope_Global && *size == sizeof unit.range); memcpy(data, &unit.range, *size);
    } else {
        assert(id == kAudioDevicePropertyBufferFrameSize && scope == kAudioUnitScope_Global && *size == sizeof unit.frames);
        memcpy(data, &unit.frames, *size);
    }
    return noErr;
}
static OSStatus mock_set(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size) {
    assert(au == fake_au() && el == 0);
    if (id == kAudioDevicePropertyBufferFrameSize) {
        assert(unit.device && scope == kAudioUnitScope_Global && size == sizeof unit.frames);
        UInt32 frames; memcpy(&frames, data, size);
        assert(frames >= unit.range.mMinimum && frames <= unit.range.mMaximum);
        unit.frames = frames; assert(++unit.writes < 100);
        changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0); // must not recursively set the same buffer
    } else if (id == kAudioUnitProperty_StreamFormat) {
        const AudioStreamBasicDescription *f = data;
        assert(scope == kAudioUnitScope_Input && size == sizeof *f && f->mSampleRate == RATE && (f->mChannelsPerFrame == 1 || f->mChannelsPerFrame == 2 || f->mChannelsPerFrame == 6));
        assert(f->mBytesPerFrame == 2 * f->mChannelsPerFrame);
        unit.format_ch = (int)f->mChannelsPerFrame;
        unit.input_formats++; changed(id, scope, el); // the 48 kHz guest format is not the hardware rate
    } else if (id == kAudioUnitProperty_AudioChannelLayout) {
        assert(scope == kAudioUnitScope_Input && el == 0 && size == sizeof(AudioChannelLayout));
        unit.layout_tag = ((const AudioChannelLayout *)data)->mChannelLayoutTag;
    } else {
        assert(id == kAudioUnitProperty_SetRenderCallback && scope == kAudioUnitScope_Input && size == sizeof(AURenderCallbackStruct));
    }
    return noErr;
}
static OSStatus mock_add(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc cb, void *ctx) {
    assert(au == fake_au() && !ctx);
    if (id == kAudioOutputUnitProperty_CurrentDevice) unit.device_listener = cb;
    else { assert(id == kAudioUnitProperty_StreamFormat); unit.format_listener = cb; }
    return noErr;
}
static OSStatus mock_remove(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc cb, void *ctx) {
    assert(au == fake_au() && !ctx);
    if (id == kAudioOutputUnitProperty_CurrentDevice) { assert(unit.device_listener == cb); unit.device_listener = NULL; }
    else { assert(id == kAudioUnitProperty_StreamFormat && unit.format_listener == cb); unit.format_listener = NULL; }
    return noErr;
}
// The system's default output and its stream format (its output channels): the route the host reads. A system object, unlike the unit's CurrentDevice, which disposal clears
enum { SYS_OUT = 100 };
static OSStatus mock_aogpd(AudioObjectID id, const AudioObjectPropertyAddress *pa, UInt32 q, const void *qd, UInt32 *sz, void *data) {
    (void)q; (void)qd; (void)sz;
    if (pa->mSelector == kAudioHardwarePropertyDefaultOutputDevice) { assert(id == kAudioObjectSystemObject); *(AudioDeviceID *)data = SYS_OUT; return noErr; }
    if (pa->mSelector == kAudioDevicePropertyStreamFormat) {
        assert(id == SYS_OUT && pa->mScope == kAudioObjectPropertyScopeOutput);
        AudioStreamBasicDescription f = {.mChannelsPerFrame = (UInt32)unit.route_ch}; memcpy(data, &f, sizeof f); return noErr;
    }
    return kAudioHardwareUnknownPropertyError;   // no device name (DM_AUDIO_DEVICE is unset in this test)
}
static OSStatus mock_aogpds(AudioObjectID id, const AudioObjectPropertyAddress *pa, UInt32 q, const void *qd, UInt32 *sz) {
    (void)id; (void)pa; (void)q; (void)qd; (void)sz; return kAudioHardwareUnknownPropertyError;   // device lists: none (DM_AUDIO_DEVICE is unset here)
}
static OSStatus mock_init(AudioUnit au) { assert(au == fake_au()); return unit.init_error; }
static dispatch_semaphore_t start_entered, start_allowed, guest_returned, device_stopped, guest_closed, device_disposed;
static OSStatus start_result;
static OSStatus mock_start(AudioUnit au) {
    assert(au == fake_au());
    dispatch_semaphore_signal(start_entered);
    dispatch_semaphore_wait(start_allowed, DISPATCH_TIME_FOREVER);
    return start_result;
}
static OSStatus mock_stop(AudioUnit au) {
    assert(au == fake_au()); dispatch_semaphore_signal(device_stopped); return noErr;
}
static OSStatus mock_uninit(AudioUnit au) {
    assert(au == fake_au() && !unit.device_listener && !unit.format_listener); return noErr;
}
static OSStatus mock_dispose(AudioComponentInstance au) {
    assert(au == fake_au() && !unit.device_listener && !unit.format_listener);
    unit.device = kAudioObjectUnknown;
    changed(kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0);
    unit.disposals++; if (device_disposed) dispatch_semaphore_signal(device_disposed); return noErr;
}

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static struct { const void *key; void *value; } object;
void *objmap_get(const void *key) { assert(key == object.key); return object.value; }
void objmap_put(const void *key, void *value) { assert(!object.key); object.key = key; object.value = value; }
void *objmap_del(const void *key) { assert(key == object.key); void *value = object.value; object.key = NULL; object.value = NULL; return value; }
void hle_signal_system_event(void *g) { (void)g; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
module_t *module_of(const void *p) { (void)p; return NULL; }
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }   // hle_svc.c: no guest address space in the unit test, so the big-stack take-overs (Start / Stop / CloseAudioOut, hle_bigstack.c) run on the calling host stack
void dm_host_helper_qos(qos_class_t q) { (void)q; }

static void *guest_start(void *out) {
    assert(h_au_Start(out) == RES_OK);
    dispatch_semaphore_signal(guest_returned);
    return NULL;
}
static void *guest_close(void *out) { h_au_Close(out); dispatch_semaphore_signal(guest_closed); return NULL; }
static int signaled(dispatch_semaphore_t s, int seconds) {
    return dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, seconds * (int64_t)NSEC_PER_SEC)) == 0;
}
static void blocked_start(void *out) {
    start_entered = dispatch_semaphore_create(0);
    start_allowed = dispatch_semaphore_create(0);
    guest_returned = dispatch_semaphore_create(0);
    device_stopped = dispatch_semaphore_create(0);
    guest_closed = dispatch_semaphore_create(0);
    static int16_t pcm[480 * 2];
    gbuf b = {pcm, sizeof pcm, sizeof pcm};
    assert(h_au_Append(out, &b));
    pthread_t guest;
    assert(!pthread_create(&guest, NULL, guest_start, out));
    assert(signaled(start_entered, 2));
    int returned = signaled(guest_returned, 1);
    uint64_t until = now_ns() + NSEC_PER_SEC;
    gbuf *released;
    while (!(released = h_au_Released(out)) && now_ns() < until) usleep(1000);
    until = now_ns() + NSEC_PER_SEC;
    int lock_free;
    while (!(lock_free = !pthread_mutex_trylock(&g_aos_lock)) && now_ns() < until) usleep(1000);
    assert(returned && released == &b && lock_free);
    pthread_mutex_unlock(&g_aos_lock);

    // A callback may arrive before Start returns. Only the silent sink may touch the PCM then.
    static int16_t music[RATE * 2];
    for (unsigned i = 0; i < sizeof music / sizeof *music; i++) music[i] = 1234;
    gbuf next = {music, sizeof music, sizeof music};
    assert(h_au_Append(out, &next));
    int16_t samples[2] = {-1, -1};
    AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof samples, samples}}};
    aout *a = ao(out);
    au_render(a, NULL, NULL, 0, 1, &bl);
    assert(!samples[0] && !samples[1]);
    dispatch_semaphore_signal(start_allowed);
    pthread_join(guest, NULL);
    until = now_ns() + NSEC_PER_SEC;
    do { au_render(a, NULL, NULL, 0, 1, &bl); if (samples[0]) break; usleep(1000); } while (now_ns() < until);
    assert(samples[0] == 1234 && samples[1] == 1234);

    dm_audio_suspend(1);
    assert(signaled(device_stopped, 1));
    dm_audio_suspend(0);
    assert(signaled(start_entered, 2));
    // A successful start with no callbacks keeps releasing buffers and is retried.
    dispatch_semaphore_signal(start_allowed);
    assert(signaled(device_stopped, 3));
    until = now_ns() + NSEC_PER_SEC;
    while (!(released = h_au_Released(out)) && now_ns() < until) usleep(1000);
    assert(released == &next);
    assert(signaled(start_entered, 3));
    start_result = kAudioUnitErr_FailedInitialization;
    dispatch_semaphore_signal(start_allowed);
    assert(signaled(start_entered, 4));
    // Reset while the retry blocks. The old unit stays alive until Start returns, then its replacement starts.
    start_result = noErr;
    dm_audio_reset(); assert(unit.disposals == 0);
    dispatch_semaphore_signal(start_allowed);
    assert(signaled(device_stopped, 1) && signaled(start_entered, 2));
    assert(unit.disposals == 1);
    // Close returns while the driver still blocks Start: it unregisters the output and leaves the unit to the start's worker,
    // which stops and disposes it when Start returns.
    device_disposed = dispatch_semaphore_create(0);
    assert(!pthread_create(&guest, NULL, guest_close, out));
    assert(signaled(guest_closed, 2));
    pthread_join(guest, NULL);
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) assert(g_aos[i] != a);
    pthread_mutex_unlock(&g_aos_lock);
    assert(unit.disposals == 1);
    dispatch_semaphore_signal(start_allowed);
    assert(signaled(device_stopped, 1) && signaled(device_disposed, 2) && unit.disposals == 2);
    dispatch_release(device_disposed); device_disposed = NULL;
    dispatch_release(guest_closed);
    dispatch_release(device_stopped);
    dispatch_release(guest_returned);
    dispatch_release(start_allowed);
    dispatch_release(start_entered);
}

// The route check runs on a queue and replaces the unit under g_aos_lock: read the output's unit count there
static int wait_dev_ch(aout *a, int want) {
    uint64_t until = now_ns() + 2 * NSEC_PER_SEC;
    for (;;) {
        pthread_mutex_lock(&g_aos_lock); int got = a->dev_ch; pthread_mutex_unlock(&g_aos_lock);
        if (got == want) return 1;
        if (now_ns() > until) return 0;
        usleep(1000);
    }
}
int main(void) {
    setenv("DM_AUDIO", "1", 1); unsetenv("DM_DBG_AU_FRAMES");
    unit.device = 1; unit.rate = 48000; unit.range = (AudioValueRange){15, 4096}; unit.frames = 512;
    uint32_t out[16] = {0}; uint8_t prm[16]; h_au_InitParam(prm);
    check_open_publication = 1;
    assert(h_au_OpenDef0(out, prm) == RES_OK);
    check_open_publication = 0;
    assert(unit.frames == 240 && unit.writes == 1 && unit.input_formats == 1 && unit.device_listener && unit.format_listener);

    unit.device = 2; unit.rate = 44100; unit.frames = 512;
    changed(kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0);
    assert(unit.frames == 221 && unit.writes == 2);
    // A device's rate can change without a CurrentDevice notification.
    unit.rate = 96000; changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    assert(unit.frames == 480 && unit.writes == 3);
    unit.rate = 48000; changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    assert(unit.frames == 240 && unit.writes == 4);
    unit.rate = 44100;
    changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0);
    changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1);
    assert(unit.frames == 240 && unit.writes == 4);
    changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    assert(unit.frames == 221 && unit.writes == 5);

    // Device limits are part of the HAL contract. Request the nearest supported frame count.
    unit.device = 3; unit.rate = 48000; unit.range.mMinimum = 256;
    changed(kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0);
    assert(unit.frames == 256);
    unit.range = (AudioValueRange){15, 128}; changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    assert(unit.frames == 128);
    unit.range.mMaximum = 4096;
    setenv("DM_DBG_AU_FRAMES", "1024", 1);
    changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0); assert(unit.frames == 1024);
    unit.device = 4; unit.rate = 44100; changed(kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0); assert(unit.frames == 1024);
    unsetenv("DM_DBG_AU_FRAMES");
    unsigned before = unit.writes; unit.device = kAudioObjectUnknown;
    changed(kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0); assert(unit.writes == before);
    unit.device = 1;
    blocked_start(out);
    assert(!object.key);

    // Six-channel route: the unit takes six channels with the 5.1 layout, and a six-channel output reaches the device as it is.
    unit.route_ch = 6; unit.device = 5; unit.rate = 48000; unit.frames = 512; unit.format_ch = 0; unit.layout_tag = 0;
    assert(dm_audio_output_channels() == 6);
    assert(dm_audio_route() == 6);   // the game threads' read: the last route a host thread read
    uint32_t out6[16] = {0}; uint8_t prm6[16]; h_au_InitParam(prm6); *(uint16_t *)(prm6 + 4) = 6;
    assert(h_au_OpenDef0(out6, prm6) == RES_OK);
    aout *six = ao(out6);
    assert(six->ch == 6 && six->dev_ch == 6 && unit.format_ch == 6 && unit.layout_tag == kAudioChannelLayoutTag_MPEG_5_1_A);
    static int16_t six_pcm[240 * 6];
    for (int i = 0; i < 240; i++) for (int j = 0; j < 6; j++) six_pcm[6 * i + j] = (int16_t)(1000 * (j + 1) + i);
    gbuf blk = {six_pcm, sizeof six_pcm, sizeof six_pcm};
    assert(h_au_Append(out6, &blk));
    atomic_store(&six->state, STATE_STARTED);
    int16_t got6[240 * 6];
    AudioBufferList bl6 = {.mNumberBuffers = 1, .mBuffers = {{6, sizeof got6, got6}}};
    au_render(six, NULL, NULL, 0, 240, &bl6);
    assert(memcmp(got6, six_pcm, sizeof got6) == 0);
    assert(h_au_Released(out6) == &blk);
    h_au_Close(out6);
    assert(!object.key && unit.disposals == 3);

    // Six-channel output on a stereo route: the unit takes stereo and the host downmixes (front 1.0, center and back 0.707, LFE 0.251, Q15).
    unit.route_ch = 2; unit.device = 6; unit.format_ch = 0; unit.layout_tag = 0;
    assert(dm_audio_output_channels() == 2);
    uint32_t out2[16] = {0};
    assert(h_au_OpenDef0(out2, prm6) == RES_OK);
    aout *dn = ao(out2);
    assert(dn->ch == 6 && dn->dev_ch == 2 && unit.format_ch == 2 && unit.layout_tag == 0);
    static int16_t fr_pcm[240 * 6];
    for (int i = 0; i < 240; i++) {   // every channel in its own value; the last two frames at full scale either way, to check the clamp
        int16_t f[6] = {1000, 2000, 3000, 4000, 5000, 6000}, hi[6] = {30000, 30000, 30000, 30000, 30000, 30000}, lo[6] = {-30000, -30000, -30000, -30000, -30000, -30000};
        memcpy(fr_pcm + 6 * i, i == 238 ? hi : i == 239 ? lo : f, sizeof f);
    }
    gbuf fb = {fr_pcm, sizeof fr_pcm, sizeof fr_pcm};
    assert(h_au_Append(out2, &fb));
    atomic_store(&dn->state, STATE_STARTED);
    static int16_t st2[2 * 240];
    AudioBufferList bl2 = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof st2, st2}}};
    au_render(dn, NULL, NULL, 0, 240, &bl2);
    assert(st2[0] == 7659 && st2[1] == 9366);   // L = 1000 + 0.707 × 3000 + 0.251 × 4000 + 0.707 × 5000, R = 2000 + 0.707 × 3000 + 0.251 × 4000 + 0.707 × 6000
    assert(st2[2 * 238] == 32767 && st2[2 * 238 + 1] == 32767 && st2[2 * 239] == -32768 && st2[2 * 239 + 1] == -32768);
    h_au_Close(out2);
    assert(!object.key && unit.disposals == 4);
    unit.route_ch = 2; unit.device = 1;

    // Route change mid-session (HDMI 5.1 to the speakers and back): the unit's own notification leads to a new unit at the route's count, off the notifying thread.
    unit.route_ch = 6; unit.device = 7; unit.format_ch = 0; unit.layout_tag = 0;
    uint32_t outr[16] = {0};
    assert(h_au_OpenDef0(outr, prm6) == RES_OK);
    aout *rt = ao(outr);
    assert(rt->dev_ch == 6 && unit.format_ch == 6 && unit.layout_tag == kAudioChannelLayoutTag_MPEG_5_1_A && unit.disposals == 4);
    unit.route_ch = 2; unit.layout_tag = 0;
    changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    assert(wait_dev_ch(rt, 2) && unit.format_ch == 2 && unit.layout_tag == 0 && unit.disposals == 5);
    unit.route_ch = 6; unit.device = 7;   // disposal cleared the unit's device; the new unit is on the default output again
    changed(kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0);
    assert(wait_dev_ch(rt, 6) && unit.format_ch == 6 && unit.layout_tag == kAudioChannelLayoutTag_MPEG_5_1_A && unit.disposals == 6);
    // A notification that leaves the route's count alone keeps the unit (the queued check, then the same check run here)
    changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    usleep(100000); dm_audio_route_changed(); assert(unit.disposals == 6);
    // A new unit that cannot be made leaves the output silent until the next route change, which tries again
    unit.route_ch = 2; unit.init_error = kAudioUnitErr_FailedInitialization;
    dm_audio_route_changed();
    pthread_mutex_lock(&g_aos_lock); int lost = !rt->au && rt->unit_lost; pthread_mutex_unlock(&g_aos_lock);
    assert(lost && unit.disposals == 8);   // the old unit and the failed new one
    unit.init_error = 0; unit.device = 7;
    dm_audio_route_changed();
    assert(wait_dev_ch(rt, 2) && rt->au && !rt->unit_lost && unit.format_ch == 2);
    unit.route_ch = 6; dm_audio_route_changed();
    assert(wait_dev_ch(rt, 6) && unit.disposals == 9);
    h_au_Close(outr);
    assert(!object.key && unit.disposals == 10);
    // A stereo output stays stereo on a six-channel route (the guest writes two channels): a route change leaves its unit alone
    uint32_t outs[16] = {0};
    assert(h_au_OpenDef0(outs, prm) == RES_OK && ao(outs)->dev_ch == 2 && unit.format_ch == 2);
    unit.route_ch = 2; changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    usleep(100000); dm_audio_route_changed(); unit.route_ch = 6; changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    usleep(100000); dm_audio_route_changed(); assert(ao(outs)->dev_ch == 2 && unit.disposals == 10);
    h_au_Close(outs);
    assert(!object.key && unit.disposals == 11);
    // A mono output goes to a mono unit as the guest writes it, on either route (it is not downmixed as six channels)
    unit.layout_tag = 0; uint32_t outm[16] = {0}; uint8_t prm1[16]; h_au_InitParam(prm1); *(uint16_t *)(prm1 + 4) = 1;
    assert(h_au_OpenDef0(outm, prm1) == RES_OK && ao(outm)->dev_ch == 1 && unit.format_ch == 1 && unit.layout_tag == 0);
    static int16_t mono_pcm[240];
    for (int i = 0; i < 240; i++) mono_pcm[i] = (int16_t)(100 + i);
    gbuf mb = {mono_pcm, sizeof mono_pcm, sizeof mono_pcm};
    assert(h_au_Append(outm, &mb));
    atomic_store(&ao(outm)->state, STATE_STARTED);
    int16_t m3[3]; AudioBufferList blm = {.mNumberBuffers = 1, .mBuffers = {{1, sizeof m3, m3}}};
    au_render(ao(outm), NULL, NULL, 0, 3, &blm);
    assert(m3[0] == 100 && m3[1] == 101 && m3[2] == 102);
    unit.route_ch = 2; dm_audio_route_changed(); assert(ao(outm)->dev_ch == 1 && unit.disposals == 11);
    h_au_Close(outm);
    assert(!object.key && unit.disposals == 12);
    unit.route_ch = 2; unit.device = 1;

    // Failure after listener registration follows the same cleanup path as Close.
    unit.device = 1; unit.rate = 48000; unit.init_error = kAudioUnitErr_FailedInitialization;
    aout failed = {.ch = 2};
    assert(au_create(&failed, 2) == kAudioUnitErr_FailedInitialization && !failed.au && unit.disposals == 13);
    puts("audio device: buffer configuration, blocked start, callback handoff, retry and lifecycle passed");
    return 0;
}
