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
static AudioObjectPropertyListenerProc system_route_listener, device_route_listener;
static OSStatus mock_route_add(AudioObjectID id, const AudioObjectPropertyAddress *pa, AudioObjectPropertyListenerProc cb, void *ctx) {
    assert(!ctx);
    if (id == kAudioObjectSystemObject) { assert(pa->mSelector == kAudioHardwarePropertyDefaultOutputDevice); system_route_listener = cb; }
    else { assert(pa->mSelector == kAudioDevicePropertyStreamFormat || pa->mSelector == kAudioDevicePropertyDataSource); device_route_listener = cb; }   // the same listener for both
    return noErr;
}
static OSStatus mock_route_remove(AudioObjectID id, const AudioObjectPropertyAddress *pa, AudioObjectPropertyListenerProc cb, void *ctx) {
    (void)id; (void)pa; (void)cb; (void)ctx; device_route_listener = NULL; return noErr;
}
#define AudioObjectAddPropertyListener mock_route_add
#define AudioObjectRemovePropertyListener mock_route_remove
#define DM_SBAR_EXTERNAL 1   // the system renderer is mocked below (test_system_route)
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
    _Atomic int route_ch;                // the fixture changes the device's channel count while queued route checks may read it
    _Atomic UInt32 transport, data_source;   // the device's transport type and data source (0: the device has none, the property read fails); atomic as route_ch
    _Atomic AudioDeviceID sys_out;           // the system default output (0: SYS_OUT)
    int format_ch;                       // channels of the unit's input format, as last set
    AudioChannelLayoutTag layout_tag;    // the unit's input channel layout, as last set (0: none)
    int format_float;                    // the unit's input format is 32-bit float, as last set
} unit;
static AudioUnit fake_au(void) { return (AudioUnit)&unit; }
static int check_open_publication;
static void changed(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el) {
    AudioUnitPropertyListenerProc cb = id == kAudioOutputUnitProperty_CurrentDevice ? unit.device_listener : unit.format_listener;
    if (cb) cb(&unit, fake_au(), id, scope, el);
}
static AudioComponent mock_find(AudioComponent c, const AudioComponentDescription *d) {
    (void)c; if (d->componentType == kAudioUnitType_Mixer) return NULL;   // no AUSpatialMixer here: t_audio_spatial makes the real one offline
    assert(d->componentSubType == kAudioUnitSubType_DefaultOutput); return (AudioComponent)&unit;
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
        unsigned bps = f->mFormatFlags & kAudioFormatFlagIsFloat ? 4 : 2;   // the host renderer's DeviceSink output is float, a guest's PCM16
        assert(f->mBytesPerFrame == bps * f->mChannelsPerFrame && f->mBitsPerChannel == 8 * bps);
        unit.format_float = bps == 4;
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
    AudioDeviceID out = unit.sys_out ? unit.sys_out : SYS_OUT;
    if (pa->mSelector == kAudioHardwarePropertyDefaultOutputDevice) { assert(id == kAudioObjectSystemObject); *(AudioDeviceID *)data = out; return noErr; }
    if (pa->mSelector == kAudioDevicePropertyTransportType && unit.transport) { assert(id == out); *(UInt32 *)data = unit.transport; return noErr; }
    if (pa->mSelector == kAudioDevicePropertyDataSource && unit.data_source) { assert(id == out); *(UInt32 *)data = unit.data_source; return noErr; }
    if (pa->mSelector == kAudioDevicePropertyStreamFormat) {
        assert(id == out && pa->mScope == kAudioObjectPropertyScopeOutput);
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
    if (!start_entered) return noErr;   // after blocked_start: a start that returns at once (test_system_route's fold unit)
    dispatch_semaphore_signal(start_entered);
    dispatch_semaphore_wait(start_allowed, DISPATCH_TIME_FOREVER);
    return start_result;
}
static OSStatus mock_stop(AudioUnit au) {
    assert(au == fake_au()); if (device_stopped) dispatch_semaphore_signal(device_stopped); return noErr;
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
    aout_buf b = {.ptr = pcm, .cap = sizeof pcm, .size = sizeof pcm};
    assert(h_au_Append(out, &b));
    pthread_t guest;
    assert(!pthread_create(&guest, NULL, guest_start, out));
    assert(signaled(start_entered, 2));
    int returned = signaled(guest_returned, 1);
    uint64_t until = now_ns() + NSEC_PER_SEC;
    aout_buf *released;
    while (!(released = h_au_Released(out)) && now_ns() < until) usleep(1000);
    until = now_ns() + NSEC_PER_SEC;
    int lock_free;
    while (!(lock_free = !pthread_mutex_trylock(&g_aos_lock)) && now_ns() < until) usleep(1000);
    assert(returned && released == &b && lock_free);
    pthread_mutex_unlock(&g_aos_lock);

    // A callback may arrive before Start returns. Only the silent sink may touch the PCM then.
    static int16_t music[RATE * 2];
    for (unsigned i = 0; i < sizeof music / sizeof *music; i++) music[i] = 1234;
    aout_buf next = {.ptr = music, .cap = sizeof music, .size = sizeof music};
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
    dispatch_release(device_stopped); device_stopped = NULL;
    dispatch_release(guest_returned);
    dispatch_release(start_allowed); start_allowed = NULL;
    dispatch_release(start_entered); start_entered = NULL;
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
static _Atomic int route_notifications;
static void test_route_without_output(void) {
    unit.route_ch = 2;
    assert(dm_audio_route() == 2 && system_route_listener && device_route_listener);
    unit.route_ch = 6;
    AudioObjectPropertyAddress pa = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    system_route_listener(kAudioObjectSystemObject, 1, &pa, NULL);
    uint64_t until = now_ns() + NSEC_PER_SEC;
    while ((dm_audio_route() != 6 || !atomic_load(&route_notifications)) && now_ns() < until) usleep(1000);
    assert(dm_audio_route() == 6 && atomic_load(&route_notifications));
    unit.route_ch = 2;
    pa.mSelector = kAudioDevicePropertyStreamFormat; pa.mScope = kAudioObjectPropertyScopeOutput;
    device_route_listener(SYS_OUT, 1, &pa, NULL);
    until = now_ns() + NSEC_PER_SEC;
    while (dm_audio_route() != 2 && now_ns() < until) usleep(1000);
    assert(dm_audio_route() == 2);
}
static void test_sink_route(void) {
    unit.route_ch = 2; unit.device = 1; unit.rate = 48000; unit.range = (AudioValueRange){15, 4096};
    void *key = dm_pcmout_open_sink(); aout *a = ao(key);
    assert(a->sink_pcm && a->ch == DM_AUDIO_SINK_CHANNELS && a->dev_ch == 2 && a->bps == 4 && unit.format_float && unit.format_ch == 2);
    // Float blocks (hle_audren.c), values above full scale included: they reach the device unclamped
    float pcm[257 * DM_AUDIO_SINK_CHANNELS];
    for (int i = 0; i < 257; i++) for (int j = 0; j < DM_AUDIO_SINK_CHANNELS; j++) pcm[i * DM_AUDIO_SINK_CHANNELS + j] = 0.25f * (float)j + (float)i / 1024.0f;
    aout_buf b = {.ptr = pcm, .cap = sizeof pcm, .size = sizeof pcm}; assert(dm_pcmout_append(key, &b));
    float stereo[129 * 2], six[127 * 6];
    atomic_store(&a->state, STATE_STARTED);
    pull_device(a, (uint8_t *)stereo, sizeof stereo);
    atomic_store(&a->state, STATE_STOPPED);
    for (int i = 0; i < 129; i++) for (int j = 0; j < 2; j++) assert(stereo[2 * i + j] == 0.25f * (float)(j + 6) + (float)i / 1024.0f);
    assert(stereo[2 * 128 + 1] > 1.0f);   // 1.75 + 128/1024: above full scale, not clamped
    unit.route_ch = 6; dm_audio_route_changed(); assert(a->dev_ch == 6 && unit.format_ch == 6 && unit.format_float && unit.layout_tag == kAudioChannelLayoutTag_MPEG_5_1_A);
    atomic_store(&a->state, STATE_STARTED);
    pull_device(a, (uint8_t *)six, sizeof six);
    atomic_store(&a->state, STATE_STOPPED);
    for (int i = 0; i < 127; i++) for (int j = 0; j < 6; j++) assert(six[6 * i + j] == 0.25f * (float)j + (float)(i + 129) / 1024.0f);
    dm_audio_suspend(1);
    unit.route_ch = 2; dm_audio_route_changed(); assert(a->dev_ch == 6);   // the cache changes in the background; the unit waits for resume
    dm_audio_suspend(0); assert(a->dev_ch == 2 && unit.format_ch == 2);
    atomic_store(&a->state, STATE_STARTED);
    pull_device(a, (uint8_t *)stereo, sizeof(float) * 2);
    atomic_store(&a->state, STATE_STOPPED);
    assert(stereo[0] == 1.5f + 256.0f / 1024.0f && stereo[1] == 1.75f + 256.0f / 1024.0f && dm_pcmout_released(key) == &b);
    dm_pcmout_close(key);
    assert(!object.key);
}
// The system renderer, mocked (audio_sbar.h): a clock that runs from dm_sbar_play in real time, so the pump paces itself against it as against the real synchronizer
static struct {
    _Atomic int available, made, destroyed, plays, stops, ch, status, enqueued, nonzero, refuse;
    _Atomic int64_t pts; _Atomic uint64_t t0; _Atomic int playing;
    float first[6];                      // the first enqueued frame that is not silence (the pump's pre-roll and hold are)
    _Atomic double max_lead;             // the largest lead seen at an enqueue once playing
} sbm;
int dm_sbar_available(void) { return atomic_load(&sbm.available); }
dm_sbar *dm_sbar_create(int ch, double rate, char *why, size_t whyn) {
    if (atomic_load(&sbm.refuse)) { snprintf(why, whyn, "refused by the test"); return NULL; }
    (void)why; (void)whyn; assert(rate == 48000); atomic_store(&sbm.ch, ch); atomic_fetch_add(&sbm.made, 1); atomic_store(&sbm.pts, 0); atomic_store(&sbm.playing, 0); return (dm_sbar *)&sbm;
}
double dm_sbar_lead(dm_sbar *s) {
    assert(s == (dm_sbar *)&sbm);
    double played = atomic_load(&sbm.playing) ? (now_ns() - atomic_load(&sbm.t0)) / 1e9 : 0;
    return atomic_load(&sbm.pts) / 48000.0 - played;
}
int dm_sbar_enqueue(dm_sbar *s, const float *x, int n) {
    assert(s == (dm_sbar *)&sbm && n == 240);
    if (atomic_load(&sbm.playing)) { double l = dm_sbar_lead(s); if (l > atomic_load(&sbm.max_lead)) atomic_store(&sbm.max_lead, l); }
    for (int i = 0; i < n && !atomic_load(&sbm.nonzero); i++) if (x[6 * i]) { memcpy(sbm.first, x + 6 * i, sizeof sbm.first); atomic_store(&sbm.nonzero, 1); }
    atomic_fetch_add(&sbm.pts, n); atomic_fetch_add(&sbm.enqueued, 1); return 0;
}
int dm_sbar_play(dm_sbar *s) { assert(s == (dm_sbar *)&sbm); atomic_store(&sbm.t0, now_ns()); atomic_store(&sbm.playing, 1); atomic_fetch_add(&sbm.plays, 1); return 0; }
void dm_sbar_stop(dm_sbar *s) { assert(s == (dm_sbar *)&sbm); atomic_store(&sbm.playing, 0); atomic_store(&sbm.pts, 0); atomic_fetch_add(&sbm.stops, 1); }
int dm_sbar_status(dm_sbar *s, char *why, size_t whyn) { assert(s == (dm_sbar *)&sbm); snprintf(why, whyn, "mock"); return atomic_exchange(&sbm.status, SBAR_OK); }
void dm_sbar_destroy(dm_sbar *s) { assert(s == (dm_sbar *)&sbm && !atomic_load(&sbm.playing)); atomic_fetch_add(&sbm.destroyed, 1); }
static int wait_for(_Atomic int *v, int want, int ms) {
    uint64_t until = now_ns() + (uint64_t)ms * 1000000;
    while (atomic_load(v) < want) { if (now_ns() > until) return 0; usleep(1000); }
    return 1;
}
// AirPods (Bluetooth, two channels) with DM_SPATIAL_AUDIO=auto: the game is told 6 and the renderer's six go to the system renderer, pumped from the queue at its clock with
// 150 ms of lead; the route then moves to the built-in speakers (no AUSpatialMixer in this mock: the fold, told 2), back to Bluetooth, to another Bluetooth device (a new
// renderer), and the renderer fails (the silent sink keeps the game's blocks moving). Close stops the pump before the renderer goes
static void test_system_route(void) {
    if (spatial_pref() != SPATIAL_PREF_AUTO) { puts("system route: skipped (the profile's DM_SPATIAL_AUDIO default is 0)"); return; }
    atomic_store(&sbm.available, 1);
    unit.route_ch = 2; unit.transport = 'blue'; unit.data_source = 0; unit.device = 1; unit.rate = 48000; unit.range = (AudioValueRange){15, 4096};
    dm_audio_route_changed();
    assert(dm_audio_route() == 6);   // the game mixes 5.1 on AirPods
    unsigned disposals = unit.disposals;
    void *key = dm_pcmout_open_sink(); aout *a = ao(key);
    assert(a->sb && !a->au && a->path == AP_SYSTEM && a->dev_ch == 6 && atomic_load(&sbm.made) == 1 && atomic_load(&sbm.ch) == 6);
    // Float blocks, each channel its own value: the six reach the renderer as they are (MPEG_5_1_A order), the fold is not used. Each goes back in the queue as it returns
    enum { NB = QMAX - 1 };
    static float pcm[NB][240 * DM_AUDIO_SINK_CHANNELS]; static aout_buf bb[NB];
    for (int b = 0; b < NB; b++) for (int i = 0; i < 240; i++) for (int j = 0; j < DM_AUDIO_SINK_CHANNELS; j++) pcm[b][i * DM_AUDIO_SINK_CHANNELS + j] = 0.1f * (float)(j + 1);
    for (int b = 0; b < NB; b++) { bb[b] = (aout_buf){.ptr = pcm[b], .cap = sizeof pcm[b], .size = sizeof pcm[b]}; assert(dm_pcmout_append(key, &bb[b])); }
    uint64_t t0 = now_ns();
    dm_pcmout_start(key);
    assert(wait_for(&sbm.plays, 1, 2000) && wait_for(&sbm.nonzero, 1, 3000));
    for (int j = 0; j < 6; j++) assert(sbm.first[j] == 0.1f * (float)(j + 1));
    // Steady state: the game's blocks come back at the renderer's real-time pace, the lead stays near 150 ms (the blocks the silent sink released during the start go first)
    usleep(50000);
    for (aout_buf *r; (r = dm_pcmout_released(key));) assert(dm_pcmout_append(key, r));   // every block back in the queue as it returns: it never runs dry
    int released = 0; uint64_t t1 = now_ns();
    while (released < 100 && now_ns() - t1 < 3000000000ull) {
        aout_buf *r = dm_pcmout_released(key);
        if (r) { released++; assert(dm_pcmout_append(key, r)); } else usleep(500);
    }
    double secs = (now_ns() - t1) / 1e9;
    printf("system route: first sound %.0f ms after start, 100 blocks (500 ms) released in %.0f ms, the largest lead %.1f ms\n", (now_ns() - t0) / 1e6 - secs * 1000, secs * 1000, atomic_load(&sbm.max_lead) * 1000);
    assert(released == 100 && secs > 0.45 && secs < 0.60);
    assert(atomic_load(&sbm.max_lead) >= 0.140 && atomic_load(&sbm.max_lead) < 0.165);
    pthread_mutex_lock(&g_aos_lock); assert(a->backend == BK_AU); pthread_mutex_unlock(&g_aos_lock);   // the watchdog took the pump's pulls as callbacks
    // To the built-in speakers: no mixer component in this mock, so the fold on a unit, told 2; the pump stopped before its renderer went
    unit.transport = 'bltn'; unit.data_source = 'ispk'; unit.device = 1;
    dm_audio_route_changed();
    assert(dm_audio_route() == 2 && wait_dev_ch(a, 2));
    pthread_mutex_lock(&g_aos_lock); assert(!a->sb && a->au && a->path == AP_FOLD && unit.format_float && unit.format_ch == 2); pthread_mutex_unlock(&g_aos_lock);
    assert(atomic_load(&sbm.destroyed) == 1 && atomic_load(&sbm.stops) >= 1);
    // Back to Bluetooth: a new renderer, told 6 again
    unit.transport = 'blue'; unit.data_source = 0;
    dm_audio_route_changed();
    assert(dm_audio_route() == 6 && wait_dev_ch(a, 6) && atomic_load(&sbm.made) == 2);
    pthread_mutex_lock(&g_aos_lock); assert(a->sb && !a->au); pthread_mutex_unlock(&g_aos_lock);
    // Another Bluetooth device: same path and count, but the renderer follows the device it was made on, so a new one
    unit.sys_out = SYS_OUT + 1;
    dm_audio_route_changed();
    assert(atomic_load(&sbm.made) == 3 && atomic_load(&sbm.destroyed) == 2);
    // The renderer fails: the pump stops pulling, the watchdog stands the silent sink in, the game's blocks keep coming back
    assert(wait_for(&sbm.playing, 1, 3000));   // the new renderer pre-rolled and runs (one stopped before its pump's first pass never plays)
    atomic_store(&sbm.status, SBAR_FAILED);
    for (uint64_t t = now_ns(); now_ns() - t < 300000000ull;) { aout_buf *r = dm_pcmout_released(key); if (r) assert(dm_pcmout_append(key, r)); else usleep(500); }
    int after = 0; uint64_t t2 = now_ns();
    while (after < 20 && now_ns() - t2 < 1000000000ull) { aout_buf *r = dm_pcmout_released(key); if (r) { after++; assert(dm_pcmout_append(key, r)); } else usleep(500); }
    assert(after == 20);
    // The watchdog stands the silent sink in on its own schedule (a stall, or the 1 s start timeout), and its stop makes a new renderer for the retry, which plays again
    assert(wait_for(&sbm.made, 4, 5000));
    for (uint64_t t = now_ns(); !atomic_load(&sbm.playing) && now_ns() - t < 5000000000ull;) { aout_buf *r = dm_pcmout_released(key); if (r) assert(dm_pcmout_append(key, r)); else usleep(500); }
    assert(atomic_load(&sbm.playing));
    dm_pcmout_close(key);
    assert(atomic_load(&sbm.destroyed) == atomic_load(&sbm.made) && unit.disposals == disposals + 1);   // every renderer released; the one fold unit, between
    // A renderer that cannot be made: the fallback (AUSpatialMixer, none in this mock, so the fold) plays, but the output keeps the decided path, so the notifications its own
    // unit sends (the buffer write) do not remake it every 80 ms; only a new decision does
    atomic_store(&sbm.refuse, 1); unit.sys_out = SYS_OUT + 2;
    void *kf = dm_pcmout_open_sink(); aout *f = ao(kf);
    pthread_mutex_lock(&g_aos_lock); assert(!f->sb && f->au && f->path == AP_SYSTEM && f->dev_ch == 2 && f->want_ch == 6); pthread_mutex_unlock(&g_aos_lock);
    unsigned d0 = unit.disposals;
    changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    usleep(ROUTE_SETTLE_MS * 1000 + 120000); dm_audio_route_changed(); dm_audio_route_changed();
    assert(unit.disposals == d0);
    dm_pcmout_close(kf); atomic_store(&sbm.refuse, 0);
    unit.transport = 0; unit.data_source = 0; unit.sys_out = 0; atomic_store(&sbm.available, 0);
    dm_audio_route_changed(); assert(dm_audio_route() == 2);
    puts("system route: the six to the system renderer at its clock, live to the fold and back, a new renderer per device, a failed renderer covered by the silent sink");
}
int main(void) {
    setenv("DM_AUDIO", "1", 1); unsetenv("DM_DBG_AU_FRAMES");
    test_route_without_output();
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
    assert(six->ch == 6 && six->dev_ch == 6 && unit.format_ch == 6 && !unit.format_float && unit.layout_tag == kAudioChannelLayoutTag_MPEG_5_1_A);   // a guest's PCM16 stays PCM16
    static int16_t six_pcm[240 * 6];
    for (int i = 0; i < 240; i++) for (int j = 0; j < 6; j++) six_pcm[6 * i + j] = (int16_t)(1000 * (j + 1) + i);
    aout_buf blk = {.ptr = six_pcm, .cap = sizeof six_pcm, .size = sizeof six_pcm};
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
    aout_buf fb = {.ptr = fr_pcm, .cap = sizeof fr_pcm, .size = sizeof fr_pcm};
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
    usleep(ROUTE_SETTLE_MS * 1000 + 120000); dm_audio_route_changed(); assert(unit.disposals == 6);
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
    usleep(ROUTE_SETTLE_MS * 1000 + 120000);   // the route check the new unit's own format notification queued has run (it reads unit.route_ch, written next)
    unit.route_ch = 2; changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    usleep(ROUTE_SETTLE_MS * 1000 + 120000); dm_audio_route_changed(); unit.route_ch = 6; changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
    usleep(ROUTE_SETTLE_MS * 1000 + 120000); dm_audio_route_changed(); assert(ao(outs)->dev_ch == 2 && unit.disposals == 10);
    h_au_Close(outs);
    assert(!object.key && unit.disposals == 11);
    // A mono output goes to a mono unit as the guest writes it, on either route (it is not downmixed as six channels)
    unit.layout_tag = 0; uint32_t outm[16] = {0}; uint8_t prm1[16]; h_au_InitParam(prm1); *(uint16_t *)(prm1 + 4) = 1;
    assert(h_au_OpenDef0(outm, prm1) == RES_OK && ao(outm)->dev_ch == 1 && unit.format_ch == 1 && unit.layout_tag == 0);
    static int16_t mono_pcm[240];
    for (int i = 0; i < 240; i++) mono_pcm[i] = (int16_t)(100 + i);
    aout_buf mb = {.ptr = mono_pcm, .cap = sizeof mono_pcm, .size = sizeof mono_pcm};
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
    audio_route_decision stereo = audio_route_decide(SPATIAL_PREF_OFF, AT_UNKNOWN, 0, 2, 0);
    assert(au_create(&failed, &stereo) == kAudioUnitErr_FailedInitialization && !failed.au && unit.disposals == 13);
    unit.init_error = 0; test_sink_route();
    // DM_SPATIAL_AUDIO is read once (the profile's default here) and never written: a later environment change and every route read above leave it
    int pref = spatial_pref(); assert(pref == (DM_GAME_SPATIAL_AUDIO ? SPATIAL_PREF_AUTO : SPATIAL_PREF_OFF));
    setenv("DM_SPATIAL_AUDIO", pref ? "0" : "auto", 1); unit.route_ch = 2; dm_audio_route_changed(); assert(spatial_pref() == pref); unsetenv("DM_SPATIAL_AUDIO");
    test_system_route();
    puts("audio device: buffer configuration, blocked start, callback handoff, retry and lifecycle passed");
    return 0;
}

void dm_audio_device_changed(void) { atomic_fetch_add(&route_notifications, 1); }   // renderer notification delivery is checked by t_audren_work
