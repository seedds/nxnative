// Mac device buffer contract: drive the real setup/listener/close paths with Core Audio property notifications,
// including synchronous format notifications from a buffer write and device removal during disposal. No hardware needed.
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <pthread.h>
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
static AudioObjectPropertyListenerProc system_route_listener, device_route_listener, layout_route_listener;
static OSStatus mock_route_add(AudioObjectID id, const AudioObjectPropertyAddress *pa, AudioObjectPropertyListenerProc cb, void *ctx) {
    assert(!ctx);
    if (id == kAudioObjectSystemObject) { assert(pa->mSelector == kAudioHardwarePropertyDefaultOutputDevice); system_route_listener = cb; }
    else if (pa->mSelector == kAudioDevicePropertyPreferredChannelLayout) { assert(pa->mScope == kAudioObjectPropertyScopeOutput); layout_route_listener = cb; }
    else { assert(pa->mSelector == kAudioDevicePropertyStreamFormat); device_route_listener = cb; }
    return noErr;
}
static OSStatus mock_route_remove(AudioObjectID id, const AudioObjectPropertyAddress *pa, AudioObjectPropertyListenerProc cb, void *ctx) {
    (void)id; (void)cb; (void)ctx;
    if (pa->mSelector == kAudioDevicePropertyPreferredChannelLayout) layout_route_listener = NULL; else device_route_listener = NULL;
    return noErr;
}
#define AudioObjectAddPropertyListener mock_route_add
#define AudioObjectRemovePropertyListener mock_route_remove
static int mock_once(pthread_once_t *, void (*)(void));
#define pthread_once mock_once
#include "../hle_audio.c"
#undef pthread_once
__thread _Atomic int dm_host_work;   // defined by the graphics host in a full build

// The fixture steps the watchdog itself. Keep the route listener's normal initialization.
static int mock_once(pthread_once_t *once, void (*init)(void)) {
    return init == au_watchdog_start ? 0 : pthread_once(once, init);
}

static struct {
    AudioDeviceID device;
    Float64 rate;
    AudioValueRange range;
    UInt32 frames;
    AudioUnitPropertyListenerProc device_listener, format_listener;
    unsigned writes, disposals, input_formats;
    OSStatus init_error;
    _Atomic int route_ch;                // the fixture changes the device's channel count while queued route checks may read it
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
static _Atomic AudioDeviceID default_device = SYS_OUT;
// The device's preferred speaker layout. By default a six-or-more channel route is a labelled 5.1 system, as an HDMI sink reports it; test_route_layouts sets the others
static struct { int custom, error; AudioChannelLayoutTag tag; AudioChannelBitmap bitmap; UInt32 n; AudioChannelLabel labels[22]; } route_layout;
static pthread_mutex_t route_layout_mu = PTHREAD_MUTEX_INITIALIZER;   // a queued route check may read the layout while the fixture changes it
#define SET_LAYOUT(...) do { pthread_mutex_lock(&route_layout_mu); __VA_ARGS__; pthread_mutex_unlock(&route_layout_mu); } while (0)
static UInt32 mock_layout_size(void) {
    UInt32 n = route_layout.custom ? route_layout.n : 6;
    return (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + n * sizeof(AudioChannelDescription));
}
static void mock_layout(AudioChannelLayout *l) {
    static const AudioChannelLabel five_one[6] = {kAudioChannelLabel_Left, kAudioChannelLabel_Right, kAudioChannelLabel_Center, kAudioChannelLabel_LFEScreen, kAudioChannelLabel_LeftSurround, kAudioChannelLabel_RightSurround};
    memset(l, 0, mock_layout_size());
    l->mChannelLayoutTag = route_layout.custom ? route_layout.tag : kAudioChannelLayoutTag_UseChannelDescriptions;
    l->mChannelBitmap = route_layout.custom ? route_layout.bitmap : 0;
    l->mNumberChannelDescriptions = route_layout.custom ? route_layout.n : 6;
    for (UInt32 i = 0; i < l->mNumberChannelDescriptions; i++) l->mChannelDescriptions[i].mChannelLabel = route_layout.custom ? route_layout.labels[i] : five_one[i];
}
static OSStatus mock_aogpd(AudioObjectID id, const AudioObjectPropertyAddress *pa, UInt32 q, const void *qd, UInt32 *sz, void *data) {
    (void)q; (void)qd;
    if (pa->mSelector == kAudioHardwarePropertyDefaultOutputDevice) { assert(id == kAudioObjectSystemObject); *(AudioDeviceID *)data = default_device; return noErr; }
    if (pa->mSelector == kAudioDevicePropertyPreferredChannelLayout) {
        assert(pa->mScope == kAudioObjectPropertyScopeOutput);
        pthread_mutex_lock(&route_layout_mu);
        OSStatus st = route_layout.error ? route_layout.error : *sz < mock_layout_size() ? kAudioHardwareBadPropertySizeError : noErr;
        if (!st) { *sz = mock_layout_size(); mock_layout(data); }
        pthread_mutex_unlock(&route_layout_mu);
        return st;
    }
    if (pa->mSelector == kAudioDevicePropertyStreamFormat) {
        assert((id == SYS_OUT || id == SYS_OUT + 1) && pa->mScope == kAudioObjectPropertyScopeOutput);
        AudioStreamBasicDescription f = {.mChannelsPerFrame = (UInt32)unit.route_ch}; memcpy(data, &f, sizeof f); return noErr;
    }
    return kAudioHardwareUnknownPropertyError;   // no device name (DM_AUDIO_DEVICE is unset in this test)
}
static OSStatus mock_aogpds(AudioObjectID id, const AudioObjectPropertyAddress *pa, UInt32 q, const void *qd, UInt32 *sz) {
    (void)id; (void)q; (void)qd;
    if (pa->mSelector == kAudioDevicePropertyPreferredChannelLayout) {
        pthread_mutex_lock(&route_layout_mu);
        OSStatus st = route_layout.error; if (!st) *sz = mock_layout_size();
        pthread_mutex_unlock(&route_layout_mu);
        return st;
    }
    return kAudioHardwareUnknownPropertyError;   // device lists: none (DM_AUDIO_DEVICE is unset here)
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
static void wait_for_phase(aout *a, int phase) {
    uint64_t until = now_ns() + 3 * NSEC_PER_SEC;
    for (;;) {
        pthread_mutex_lock(&g_aos_lock);
        int ready = a->start_phase == phase;
        pthread_mutex_unlock(&g_aos_lock);
        if (ready) return;
        assert(now_ns() < until);
        usleep(1000);
    }
}
static void watchdog_retry(aout *a) {
    wait_for_phase(a, AU_IDLE);
    pthread_mutex_lock(&g_aos_lock);
    uint64_t t = now_ns();
    if (t < a->retry_at) t = a->retry_at;
    au_watchdog_output(a, t, WD_TICK_MS * 1000000ull);
    pthread_mutex_unlock(&g_aos_lock);
}
static void blocked_start(void *out) {
    const uint64_t tick = WD_TICK_MS * 1000000ull;
    enum { callback_frames = RATE / 100 };
    start_entered = dispatch_semaphore_create(0);
    start_allowed = dispatch_semaphore_create(0);
    guest_returned = dispatch_semaphore_create(0);
    device_stopped = dispatch_semaphore_create(0);
    guest_closed = dispatch_semaphore_create(0);
    static int16_t pcm[callback_frames * 2];
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
    int16_t samples[callback_frames * 2] = {-1, -1};
    AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof samples, samples}}};
    aout *a = ao(out);
    au_render(a, NULL, NULL, 0, callback_frames, &bl);
    assert(!samples[0] && !samples[1]);
    dispatch_semaphore_signal(start_allowed);
    pthread_join(guest, NULL);
    wait_for_phase(a, AU_WAIT_CALLBACK);
    au_render(a, NULL, NULL, 0, callback_frames, &bl);
    assert(!samples[0] && !samples[1]);
    pthread_mutex_lock(&g_aos_lock);
    au_watchdog_output(a, a->wd_t + tick, tick);
    pthread_mutex_unlock(&g_aos_lock);
    au_render(a, NULL, NULL, 0, callback_frames, &bl);
    assert(samples[0] == 1234 && samples[1] == 1234);
    for (unsigned frame = callback_frames; frame < RATE; frame += callback_frames)
        au_render(a, NULL, NULL, 0, callback_frames, &bl);
    assert(h_au_Released(out) == &next);

    dm_audio_suspend(1);
    assert(signaled(device_stopped, 1));
    dm_audio_suspend(0);
    assert(signaled(start_entered, 2));
    // A successful start with no callbacks keeps releasing buffers and is retried.
    dispatch_semaphore_signal(start_allowed);
    wait_for_phase(a, AU_WAIT_CALLBACK);
    pthread_mutex_lock(&g_aos_lock);
    uint64_t deadline = a->wd_t + NSEC_PER_SEC;
    au_watchdog_output(a, deadline - 1, tick);
    assert(!signaled(device_stopped, 0));
    au_watchdog_output(a, deadline, 1);
    assert(signaled(device_stopped, 0));
    pthread_mutex_unlock(&g_aos_lock);
    assert(h_au_Append(out, &b));
    until = now_ns() + NSEC_PER_SEC;
    while (!(released = h_au_Released(out)) && now_ns() < until) usleep(1000);
    assert(released == &b);
    watchdog_retry(a);
    assert(signaled(start_entered, 3));
    start_result = kAudioUnitErr_FailedInitialization;
    dispatch_semaphore_signal(start_allowed);
    watchdog_retry(a);
    assert(signaled(start_entered, 4));
    // Reset while the retry blocks. The old unit stays alive until Start returns, then its replacement starts.
    start_result = noErr;
    dm_audio_reset(); assert(unit.disposals == 0);
    dispatch_semaphore_signal(start_allowed);
    assert(signaled(device_stopped, 1));
    watchdog_retry(a);
    assert(signaled(start_entered, 2));
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
// The route is 5.1 only when the device's speaker layout says so, whatever its channel count (issue 3263)
static int route_with_labels(int channels, UInt32 n, const AudioChannelLabel *labels) {
    SET_LAYOUT(memset(&route_layout, 0, sizeof route_layout);
               route_layout.custom = 1; route_layout.tag = kAudioChannelLayoutTag_UseChannelDescriptions; route_layout.n = n;
               for (UInt32 i = 0; i < n; i++) route_layout.labels[i] = labels ? labels[i] : kAudioChannelLabel_Unknown);
    unit.route_ch = channels;
    return dm_audio_output_channels();
}
static void test_route_layouts(void) {
    enum { L = kAudioChannelLabel_Left, R = kAudioChannelLabel_Right, C = kAudioChannelLabel_Center, LFE = kAudioChannelLabel_LFEScreen,
           LS = kAudioChannelLabel_LeftSurround, RS = kAudioChannelLabel_RightSurround,
           LSD = kAudioChannelLabel_LeftSurroundDirect, RSD = kAudioChannelLabel_RightSurroundDirect,
           RL = kAudioChannelLabel_RearSurroundLeft, RR = kAudioChannelLabel_RearSurroundRight,
           U = (int)kAudioChannelLabel_Unknown };
    assert(layout_route_listener);
    // An audio interface: discrete outputs, unlabelled or with stereo monitors on the first two
    assert(route_with_labels(22, 22, NULL) == 2);
    AudioChannelLabel monitors[22] = {L, R}; for (int i = 2; i < 22; i++) monitors[i] = (AudioChannelLabel)U;
    assert(route_with_labels(22, 22, monitors) == 2);
    assert(route_with_labels(6, 6, NULL) == 2);
    const AudioChannelLabel no_center[6] = {L, R, U, LFE, LS, RS}, no_back[6] = {L, R, C, LFE, U, U}, one_back[6] = {L, R, C, LFE, LS, U};
    assert(route_with_labels(6, 6, no_center) == 2 && route_with_labels(6, 6, no_back) == 2 && route_with_labels(6, 6, one_back) == 2);
    // Layouts whose surround labels Core Audio's ChannelMap leaves unmapped stay stereo
    const AudioChannelLabel mixed_back[6] = {L, R, C, LFE, LS, RR};
    const AudioChannelLabel unmapped_71[8] = {L, R, C, LFE, LSD, RSD, RL, RR};
    assert(route_with_labels(6, 6, mixed_back) == 2 && route_with_labels(8, 8, unmapped_71) == 2);
    // Surround systems: HDMI's order, rear speakers, 5.0 (five or six channels), and 5.1 on non-front outputs of an interface
    const AudioChannelLabel hdmi[8] = {L, R, LFE, C, LS, RS, RL, RR}, rear[6] = {L, R, C, LFE, RL, RR}, five[5] = {L, R, C, LS, RS};
    assert(route_with_labels(8, 8, hdmi) == 6 && route_with_labels(6, 6, rear) == 6);
    assert(route_with_labels(6, 5, five) == 6 && route_with_labels(5, 5, five) == 6);
    AudioChannelLabel offset_51[22]; for (int i = 0; i < 22; i++) offset_51[i] = (AudioChannelLabel)U;
    offset_51[2] = L; offset_51[3] = R; offset_51[4] = C; offset_51[5] = LFE; offset_51[6] = LS; offset_51[7] = RS;
    assert(route_with_labels(22, 22, offset_51) == 6);
    // Mapped speakers must fall within the stream's output channels
    assert(route_with_labels(2, 8, hdmi) == 2 && route_with_labels(4, 8, hdmi) == 2);
    const AudioChannelLabel past_six[8] = {L, R, C, LFE, U, U, LS, RS};
    assert(route_with_labels(6, 8, past_six) == 2 && route_with_labels(8, 8, past_six) == 6);
    // Standard layouts and bitmaps
    route_with_labels(6, 0, NULL); SET_LAYOUT(route_layout.tag = kAudioChannelLayoutTag_MPEG_5_1_A); assert(dm_audio_output_channels() == 6);
    route_with_labels(5, 0, NULL); SET_LAYOUT(route_layout.tag = kAudioChannelLayoutTag_MPEG_5_0_A); assert(dm_audio_output_channels() == 6);
    route_with_labels(6, 0, NULL); SET_LAYOUT(route_layout.tag = kAudioChannelLayoutTag_Stereo); assert(dm_audio_output_channels() == 2);
    route_with_labels(22, 0, NULL); SET_LAYOUT(route_layout.tag = kAudioChannelLayoutTag_DiscreteInOrder | 22); assert(dm_audio_output_channels() == 2);
    route_with_labels(7, 0, NULL); SET_LAYOUT(route_layout.tag = kAudioChannelLayoutTag_DTS_6_1_B); assert(dm_audio_output_channels() == 2);
    route_with_labels(6, 0, NULL); SET_LAYOUT(route_layout.tag = kAudioChannelLayoutTag_UseChannelBitmap;
               route_layout.bitmap = kAudioChannelBit_Left | kAudioChannelBit_Right | kAudioChannelBit_Center | kAudioChannelBit_LFEScreen | kAudioChannelBit_LeftSurround | kAudioChannelBit_RightSurround);
    assert(dm_audio_output_channels() == 6);
    SET_LAYOUT(route_layout.bitmap = kAudioChannelBit_Left | kAudioChannelBit_Right); assert(dm_audio_output_channels() == 2);
    // A device with no readable layout stays audible as stereo
    route_with_labels(8, 8, hdmi); SET_LAYOUT(route_layout.error = kAudioHardwareUnknownPropertyError);
    assert(dm_audio_output_channels() == 2);
    // A speaker change reaches the route through the layout listener
    route_with_labels(8, 8, hdmi); assert(dm_audio_output_channels() == 6 && dm_audio_route() == 6);
    SET_LAYOUT(route_layout.labels[3] = kAudioChannelLabel_Unknown);
    AudioObjectPropertyAddress pa = {kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    layout_route_listener(SYS_OUT, 1, &pa, NULL);
    uint64_t until = now_ns() + NSEC_PER_SEC;
    while (dm_audio_route() != 2 && now_ns() < until) usleep(1000);
    assert(dm_audio_route() == 2);
    SET_LAYOUT(memset(&route_layout, 0, sizeof route_layout)); unit.route_ch = 2; dm_audio_route_changed();
}
// Drive the watchdog's time explicitly. The wall-clock limits synchronize with the asynchronous start worker.
static void test_same_channel_route(void) {
    aout a = {.au = fake_au(), .ch = 2, .dev_ch = 2, .state = STATE_STARTED, .backend = BK_AU, .vol = 0x3f800000u};
    int16_t pcm[2] = {1234, 1234};
    aout_buf b = {.ptr = pcm, .size = sizeof pcm};
    a.q[0] = &b;
    atomic_store(&a.q_tail, 1);
    start_entered = dispatch_semaphore_create(0);
    start_allowed = dispatch_semaphore_create(0);
    device_stopped = dispatch_semaphore_create(0);
    pthread_mutex_lock(&g_aos_lock);
    g_aos[0] = &a;
    pthread_mutex_unlock(&g_aos_lock);
    default_device = SYS_OUT + 1;
    assert(dm_audio_output_channels() == 2);   // an open can refresh the cache before the route worker sees the move
    dm_audio_route_changed();
    pthread_mutex_lock(&g_aos_lock);
    uint64_t deadline = a.route_until;
    assert(deadline && !unit.disposals);
    a.wd_t = deadline - WD_ROUTE_SETTLE_MS * 1000000ull;
    au_watchdog_output(&a, deadline - 1, WD_TICK_MS * 1000000ull);
    assert(a.backend == BK_AU && a.start_phase == AU_IDLE && atomic_load(&a.q_head) == 0);
    assert(!a.sink_pcm && a.ch == 2 && a.dev_ch == 2);
    int16_t samples[2] = {-1, -1};
    AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof samples, samples}}};
    au_render(&a, NULL, NULL, 0, 1, &bl);
    assert(samples[0] == 1234 && samples[1] == 1234);
    au_watchdog_output(&a, deadline, WD_TICK_MS * 1000000ull);
    assert(a.backend == BK_AU && !a.route_until);
    pthread_mutex_unlock(&g_aos_lock);

    default_device = SYS_OUT;
    dm_audio_route_changed();
    pthread_mutex_lock(&g_aos_lock);
    deadline = a.route_until;
    a.wd_t = deadline - WD_ROUTE_SETTLE_MS * 1000000ull;
    au_watchdog_output(&a, deadline, WD_TICK_MS * 1000000ull);
    assert(a.backend == BK_NULL && a.start_phase == AU_STARTING && !a.route_until && !a.retry_at);
    pthread_mutex_unlock(&g_aos_lock);
    assert(signaled(device_stopped, 3) && signaled(start_entered, 3));

    // A switch during a blocked Start still gets a prompt attempt if the old Start returns without callbacks.
    default_device = SYS_OUT + 1;
    dm_audio_route_changed();
    dispatch_semaphore_signal(start_allowed);
    wait_for_phase(&a, AU_WAIT_CALLBACK);
    pthread_mutex_lock(&g_aos_lock);
    assert(a.route_until);
    uint64_t stalled_at = a.wd_t + (WD_STALL_MS + 1) * 1000000ull;
    au_watchdog_output(&a, stalled_at > a.route_until ? stalled_at : a.route_until, WD_TICK_MS * 1000000ull);
    assert(a.start_phase == AU_STARTING && !a.route_until && !a.retry_at);
    pthread_mutex_unlock(&g_aos_lock);
    assert(signaled(device_stopped, 3) && signaled(start_entered, 3));
    start_result = kAudioUnitErr_FailedInitialization;
    dispatch_semaphore_signal(start_allowed);
    wait_for_phase(&a, AU_IDLE);
    pthread_mutex_lock(&g_aos_lock);
    au_watchdog_output(&a, a.retry_at - 1, WD_TICK_MS * 1000000ull);
    assert(a.backend == BK_NULL && a.start_phase == AU_IDLE && a.retry_ms == 1000 && !unit.disposals);
    pthread_mutex_unlock(&g_aos_lock);

    // A route notification received after the silent sink has taken over clears the old backoff.
    default_device = SYS_OUT;
    dm_audio_route_changed();
    pthread_mutex_lock(&g_aos_lock);
    au_watchdog_output(&a, a.retry_at - 1, WD_TICK_MS * 1000000ull);
    assert(a.backend == BK_NULL && a.start_phase == AU_STARTING && !a.retry_at);
    pthread_mutex_unlock(&g_aos_lock);
    assert(signaled(start_entered, 3));
    dispatch_semaphore_signal(start_allowed);
    wait_for_phase(&a, AU_IDLE);
    pthread_mutex_lock(&g_aos_lock);
    null_stop(&a);
    g_aos[0] = NULL;
    pthread_mutex_unlock(&g_aos_lock);
    start_result = noErr;
    dispatch_release(device_stopped);
    dispatch_release(start_allowed);
    dispatch_release(start_entered);
}
static void test_sink_route(void) {
    unit.route_ch = 2; unit.device = 1; unit.rate = 48000; unit.range = (AudioValueRange){15, 4096};
    void *key = dm_pcmout_open_sink(); aout *a = ao(key);
    assert(a->sink_pcm && a->ch == DM_AUDIO_SINK_CHANNELS && a->dev_ch == 2);
    int16_t pcm[257 * DM_AUDIO_SINK_CHANNELS];
    for (int i = 0; i < 257; i++) for (int j = 0; j < DM_AUDIO_SINK_CHANNELS; j++) pcm[i * DM_AUDIO_SINK_CHANNELS + j] = (int16_t)(1000 * j + i);
    aout_buf b = {.ptr = pcm, .cap = sizeof pcm, .size = sizeof pcm}; assert(dm_pcmout_append(key, &b));
    int16_t stereo[129 * 2], six[127 * 6];
    atomic_store(&a->state, STATE_STARTED);
    pull_device(a, (uint8_t *)stereo, sizeof stereo);
    atomic_store(&a->state, STATE_STOPPED);
    for (int i = 0; i < 129; i++) for (int j = 0; j < 2; j++) assert(stereo[2 * i + j] == 1000 * (j + 6) + i);
    unit.route_ch = 6; dm_audio_route_changed(); assert(a->dev_ch == 6 && unit.format_ch == 6);
    atomic_store(&a->state, STATE_STARTED);
    pull_device(a, (uint8_t *)six, sizeof six);
    atomic_store(&a->state, STATE_STOPPED);
    for (int i = 0; i < 127; i++) for (int j = 0; j < 6; j++) assert(six[6 * i + j] == 1000 * j + i + 129);
    dm_audio_suspend(1);
    unit.route_ch = 2; dm_audio_route_changed(); assert(a->dev_ch == 6);   // the cache changes in the background; the unit waits for resume
    dm_audio_suspend(0); assert(a->dev_ch == 2 && unit.format_ch == 2);
    atomic_store(&a->state, STATE_STARTED);
    pull_device(a, (uint8_t *)stereo, sizeof(int16_t) * 2);
    atomic_store(&a->state, STATE_STOPPED);
    assert(stereo[0] == 6256 && stereo[1] == 7256 && dm_pcmout_released(key) == &b);
    dm_pcmout_close(key);
    assert(!object.key);

    // DeviceSink dump on a 5.1 route latches 6-channel surround [0..5] and keeps that framing even after a mid-run route change to stereo.
    char path[512]; snprintf(path, sizeof path, "%s/t_audio_dev_dump_%d.raw", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    setenv("DM_AUDIO_DUMP", path, 1);
    unit.route_ch = 6;
    void *dkey = dm_pcmout_open_sink(); aout *da = ao(dkey);
    int did = da->id;
    assert(da->sink_pcm && da->dev_ch == 6 && da->dump_ch == 6);
    aout_buf db1 = {.ptr = pcm, .cap = sizeof pcm, .size = sizeof pcm};
    assert(dm_pcmout_append(dkey, &db1));
    unit.route_ch = 2; dm_audio_route_changed(); assert(da->dev_ch == 2 && da->dump_ch == 6);
    aout_buf db2 = {.ptr = pcm, .cap = sizeof pcm, .size = sizeof pcm};
    assert(dm_pcmout_append(dkey, &db2));
    dm_pcmout_close(dkey); unsetenv("DM_AUDIO_DUMP");
    assert(!object.key);
    char dp[600];
    if (did) snprintf(dp, sizeof dp, "%s.%d", path, did); else snprintf(dp, sizeof dp, "%s", path);
    FILE *df = fopen(dp, "rb"); assert(df);
    int16_t got[2 * 257 * 6 + 1]; size_t dn = fread(got, sizeof *got, sizeof got / sizeof *got, df);
    fclose(df); unlink(dp);
    assert(dn == 2 * 257 * 6);
    for (int blk = 0; blk < 2; blk++)
        for (int i = 0; i < 257; i++)
            for (int j = 0; j < 6; j++)
                assert(got[(blk * 257 + i) * 6 + j] == 1000 * j + i);
}
int main(void) {
    setenv("DM_AUDIO", "1", 1); unsetenv("DM_DBG_AU_FRAMES");
    test_route_without_output();
    test_same_channel_route();
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
    assert(au_create(&failed, 2) == kAudioUnitErr_FailedInitialization && !failed.au && unit.disposals == 13);
    unit.init_error = 0; test_sink_route();
    test_route_layouts();
    puts("audio device: buffer configuration, blocked start, callback handoff, retry and lifecycle passed");
    return 0;
}

void dm_audio_device_changed(void) { atomic_fetch_add(&route_notifications, 1); }   // renderer notification delivery is checked by t_audren_work
