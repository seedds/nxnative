// Hardwareless recovery regression: no AudioUnit, speaker or system session is
// opened. The real lifecycle/queue code runs against injected CoreAudio calls.
// clang -O1 -g -Wall -Wno-unused-function -I dm/host -I <generated profile dir>
//   dm/host/tests/t_audio_recovery.c -framework AudioToolbox -o /tmp/t_audio_recovery
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <time.h>

static AudioComponent mock_AudioComponentFindNext(AudioComponent, const AudioComponentDescription *);
static OSStatus mock_AudioComponentInstanceNew(AudioComponent, AudioComponentInstance *);
static OSStatus mock_AudioComponentInstanceDispose(AudioComponentInstance);
static OSStatus mock_AudioUnitSetProperty(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *, UInt32);
static OSStatus mock_AudioUnitInitialize(AudioUnit);
static OSStatus mock_AudioUnitUninitialize(AudioUnit);
static OSStatus mock_AudioOutputUnitStart(AudioUnit);
static OSStatus mock_AudioOutputUnitStop(AudioUnit);

#define AudioComponentFindNext mock_AudioComponentFindNext
#define AudioComponentInstanceNew mock_AudioComponentInstanceNew
#define AudioComponentInstanceDispose mock_AudioComponentInstanceDispose
#define AudioUnitSetProperty mock_AudioUnitSetProperty
#define AudioUnitInitialize mock_AudioUnitInitialize
#define AudioUnitUninitialize mock_AudioUnitUninitialize
#define AudioOutputUnitStart mock_AudioOutputUnitStart
#define AudioOutputUnitStop mock_AudioOutputUnitStop
#include "../hle_audio.c"
#undef AudioComponentFindNext
#undef AudioComponentInstanceNew
#undef AudioComponentInstanceDispose
#undef AudioUnitSetProperty
#undef AudioUnitInitialize
#undef AudioUnitUninitialize
#undef AudioOutputUnitStart
#undef AudioOutputUnitStop

typedef struct { int initialized, running, disposed; } fake_unit;
static fake_unit g_units[64];
static int g_nunit, g_fail_new, g_fail_start, g_create_calls, g_start_calls, g_stop_calls, g_dispose_calls, g_running;
static aout *g_checked;
static int g_component;
static fake_unit *unit(AudioUnit au) { assert(au); return (fake_unit *)(void *)au; }
static AudioComponent mock_AudioComponentFindNext(AudioComponent previous, const AudioComponentDescription *description) {
    (void)previous; assert(description->componentType == kAudioUnitType_Output);
    return (AudioComponent)(void *)&g_component;
}
static OSStatus mock_AudioComponentInstanceNew(AudioComponent component, AudioComponentInstance *result) {
    assert(component); g_create_calls++;
    if (g_fail_new) { g_fail_new--; *result = NULL; return -10863; }
    assert(g_nunit < (int)(sizeof g_units / sizeof *g_units));
    fake_unit *u = &g_units[g_nunit++]; memset(u, 0, sizeof *u);
    *result = (AudioComponentInstance)(void *)u;
    return noErr;
}
static OSStatus mock_AudioComponentInstanceDispose(AudioComponentInstance instance) {
    fake_unit *u = unit(instance); assert(!u->running && !u->disposed);
    u->disposed = 1; g_dispose_calls++; return noErr;
}
static OSStatus mock_AudioUnitSetProperty(AudioUnit au, AudioUnitPropertyID property, AudioUnitScope scope, AudioUnitElement element, const void *data, UInt32 size) {
    (void)property; (void)scope; (void)element; (void)data; (void)size;
    assert(!unit(au)->disposed); return noErr;
}
static OSStatus mock_AudioUnitInitialize(AudioUnit au) { assert(!unit(au)->disposed); unit(au)->initialized = 1; return noErr; }
static OSStatus mock_AudioUnitUninitialize(AudioUnit au) { assert(!unit(au)->running); unit(au)->initialized = 0; return noErr; }
static OSStatus mock_AudioOutputUnitStart(AudioUnit au) {
    fake_unit *u = unit(au);
    assert(u->initialized && !u->disposed && !u->running);
    // A recreated unit must never play concurrently with the silent consumer.
    assert(!g_running && g_checked && !atomic_load(&g_checked->null_run));
    g_start_calls++;
    if (g_fail_start) { g_fail_start--; return -10863; }
    u->running = 1; g_running++; return noErr;
}
static OSStatus mock_AudioOutputUnitStop(AudioUnit au) {
    fake_unit *u = unit(au); assert(!u->disposed);
    if (u->running) { u->running = 0; g_running--; }
    g_stop_calls++; return noErr;
}

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static const void *g_key; static void *g_value;
void *objmap_get(const void *key) { return key == g_key ? g_value : NULL; }
void objmap_put(const void *key, void *value) { g_key = key; g_value = value; }
void *objmap_del(const void *key) { void *value = objmap_get(key); if (value) { g_key = NULL; g_value = NULL; } return value; }
void hle_signal_system_event(void *event) { (void)event; }
void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; }
module_t *module_of(const void *address) { (void)address; return NULL; }
void dm_host_helper_qos(qos_class_t qos) { (void)qos; }

static void begin(aout *a, int playing) {
    assert(!g_running); memset(a, 0, sizeof *a); g_checked = a;
    pthread_mutex_init(&a->m, NULL); a->ch = 2; a->id = 42;
    a->real_requested = 1;
    atomic_init(&a->state, playing ? STATE_STARTED : STATE_STOPPED);
    atomic_init(&a->vol, 0x3f800000u);
    g_fail_new = g_fail_start = 0; g_create_calls = g_start_calls = g_stop_calls = g_dispose_calls = 0;
    g_aos[0] = a; g_suspended = 0; objmap_put(a, a);
    assert(!au_create(a));
    if (playing) { au_go(a, now_ns()); assert(a->backend == BK_AU); }
}
static void finish(aout *a) {
    pthread_mutex_lock(&g_aos_lock); backend_stop(a); g_aos[0] = NULL; g_suspended = 0; pthread_mutex_unlock(&g_aos_lock);
    if (a->au) { mock_AudioUnitUninitialize(a->au); mock_AudioComponentInstanceDispose(a->au); a->au = NULL; }
    assert(!g_running && !atomic_load(&a->null_run));
    objmap_del(a); pthread_mutex_destroy(&a->m); g_checked = NULL;
}
static void retry_due(aout *a) {
    pthread_mutex_lock(&g_aos_lock);
    au_watchdog_step_locked(a->retry_at + 1, WD_TICK_MS * 1000000ull);
    pthread_mutex_unlock(&g_aos_lock);
}
static void expect_meter(aout *a, uint32_t peak, uint32_t mean, uint32_t zeros, uint32_t samples) {
    uint64_t levels = atomic_load(&a->pcm_levels), counts = atomic_load(&a->pcm_counts);
    assert((uint32_t)(levels >> 32) == peak && (uint32_t)levels == mean);
    assert((uint32_t)(counts >> 32) == zeros && (uint32_t)counts == samples);
}
static void meter_boundaries(void) {
    aout a = {0};
    const int16_t zero[] = {0, 0, 0, 0}; au_meter_publish(&a, zero, 4); expect_meter(&a, 0, 0, 4, 4);
    const int16_t mixed[] = {0, 3, -4, 0}; au_meter_publish(&a, mixed, 4); expect_meter(&a, 4, 6, 2, 4);
    const int16_t full[] = {32767, -32768}; au_meter_publish(&a, full, 2); expect_meter(&a, 32768, 1073709056u, 0, 2);
    int16_t large[8192]; for (int i = 0; i < 8192; i++) large[i] = -5;
    au_meter_publish(&a, large, 8192); expect_meter(&a, 5, 25, 0, 4096);
    au_meter_publish(&a, NULL, 0); expect_meter(&a, 0, 0, 0, 0);
    puts("PCM meter: silence, mixed/full-scale, zero-length and bounded long input: PASS");
}
static void callback_owner_and_gain(void) {
    aout a; begin(&a, 1);
    int16_t input[] = {1000, -1000, 0, 2000}, output[4] = {777, 777, 777, 777};
    gbuf first = {input, sizeof input, sizeof input}, second = first;
    a.q[0] = &first; atomic_store(&a.q_tail, 1);
    AudioBufferList buffers = {.mNumberBuffers = 1, .mBuffers = {{.mNumberChannels = 2, .mDataByteSize = sizeof output, .mData = output}}};
    AudioTimeStamp time = {0}; AudioUnitRenderActionFlags flags = 0;
    atomic_store(&a.consumer, BK_NULL); // stale AU callback after management handed the queue to its replacement
    assert(!au_render(&a, &flags, &time, 0, 2, &buffers));
    for (int i = 0; i < 4; i++) assert(output[i] == 0);
    assert(atomic_load(&a.q_head) == 0 && atomic_load(&a.q_tail) == 1);
    expect_meter(&a, 0, 0, 4, 4);

    atomic_store(&a.consumer, BK_AU); atomic_store(&a.vol, 0x3f000000u); // 0.5 gain
    assert(!au_render(&a, &flags, &time, 0, 2, &buffers));
    const int16_t scaled[] = {500, -500, 0, 1000}; assert(!memcmp(output, scaled, sizeof output));
    assert(atomic_load(&a.q_head) == 1); expect_meter(&a, 1000, 375000, 1, 4);

    a.q[1] = &second; atomic_store(&a.q_tail, 2); a.mute = 1;
    assert(!au_render(&a, &flags, &time, 0, 2, &buffers));
    for (int i = 0; i < 4; i++) assert(output[i] == 0);
    assert(atomic_load(&a.q_head) == 2); expect_meter(&a, 0, 0, 4, 4);
    finish(&a); puts("real callback: ownership fence, post-gain meter and post-mute zero detection: PASS");
}

static unsigned g_env_calls;
static char g_env_value[64];
static void env_observe(const char *key, const char *value) {
    assert(!strcmp(key, "DM_AUDIO_SESSION_CATEGORY"));
    g_env_calls++; snprintf(g_env_value, sizeof g_env_value, "%s", value ? value : "");
}
static _Atomic unsigned g_env_stress_calls;
static void env_stress_observe(const char *key, const char *value) {
    assert(!strcmp(key, "DM_AUDIO_SESSION_CATEGORY") && !strcmp(value, "playback_mix"));
    atomic_fetch_add_explicit(&g_env_stress_calls, 1, memory_order_relaxed);
}
static void *env_registration_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 100000; i++) dm_audio_env_callback_set(i & 1 ? env_stress_observe : NULL);
    return NULL;
}
static void env_notification(void) {
    dm_audio_env_callback_set(NULL);
    dm_audio_env_changed("DM_AUDIO_SESSION_CATEGORY", "playback_mix");
    assert(!g_env_calls); // non-iOS host does not require a frontend symbol
    dm_audio_env_callback_set(env_observe);
    dm_audio_env_changed(NULL, "playback_mix");
    dm_audio_env_changed("DM_OPMODE", "1");
    dm_audio_env_changed("DM_AUDIO_SESSION_CATEGORY_EXTRA", "playback_mix");
    assert(!g_env_calls);
    dm_audio_env_changed("DM_AUDIO_SESSION_CATEGORY", "playback_mix");
    assert(g_env_calls == 1 && !strcmp(g_env_value, "playback_mix"));
    dm_audio_env_changed("DM_AUDIO_SESSION_CATEGORY", "solo");
    assert(g_env_calls == 2 && !strcmp(g_env_value, "solo"));
    dm_audio_env_callback_set(NULL);
    dm_audio_env_changed("DM_AUDIO_SESSION_CATEGORY", "playback_mix");
    assert(g_env_calls == 2);

    pthread_t registration; assert(!pthread_create(&registration, NULL, env_registration_thread, NULL));
    for (unsigned i = 0; i < 100000; i++) {
        dm_audio_env_changed("DM_AUDIO_SESSION_CATEGORY", "playback_mix");
        dm_audio_env_changed("DM_OPMODE", "1");
    }
    assert(!pthread_join(registration, NULL));
    dm_audio_env_callback_set(env_stress_observe);
    unsigned before = atomic_load(&g_env_stress_calls);
    dm_audio_env_changed("DM_AUDIO_SESSION_CATEGORY", "playback_mix");
    assert(atomic_load(&g_env_stress_calls) == before + 1);
    dm_audio_env_callback_set(NULL);
    puts("CTL category notification: no-frontend no-op, exact-key live round trip and concurrent registration: PASS");
}

int main(void) {
    env_notification(); meter_boundaries(); callback_owner_and_gain();
    aout a;
    begin(&a, 1); g_fail_new = 1; AudioUnit old = a.au;
    dm_audio_reset();
    assert(unit(old)->disposed && !a.au && a.backend == BK_NULL && a.stalled && atomic_load(&a.null_run));
    retry_due(&a);
    assert(a.au && a.au != old && a.backend == BK_AU && !a.stalled && !atomic_load(&a.null_run) && g_running == 1);
    finish(&a); puts("media reset: transient creation failure recovers a NULL AudioUnit with one consumer: PASS");

    begin(&a, 1); g_fail_start = 1; dm_audio_reset();
    assert(a.au && a.backend == BK_NULL && a.stalled && !g_running);
    int created = g_create_calls; retry_due(&a);
    assert(g_create_calls == created && a.backend == BK_AU && !a.stalled && g_running == 1);
    finish(&a); puts("media reset: transient Start failure retries the existing recreated unit: PASS");

    begin(&a, 0); g_fail_new = 1; dm_audio_reset();
    assert(!a.au && atomic_load(&a.state) == STATE_STOPPED);
    int before = g_create_calls; retry_due(&a); dm_audio_suspend(1); dm_audio_suspend(0);
    assert(g_create_calls == before && !g_running && a.backend == BK_NONE);
    h_au_Start(&a);
    if (!a.au || a.backend != BK_AU) retry_due(&a);
    assert(a.au && a.backend == BK_AU && !atomic_load(&a.null_run) && g_running == 1);
    finish(&a); puts("stopped output: stays stopped across reset/resume, explicit Start can recover: PASS");

    begin(&a, 1); g_fail_new = 1; dm_audio_reset(); dm_audio_suspend(1);
    before = g_create_calls; retry_due(&a);
    assert(g_create_calls == before && !g_running && a.backend == BK_NULL);
    dm_audio_suspend(0);
    if (!a.au || a.backend != BK_AU) retry_due(&a);
    assert(a.au && a.backend == BK_AU && !atomic_load(&a.null_run) && g_running == 1);
    finish(&a); puts("suspended output: no recreation/start until foreground resume: PASS");

    begin(&a, 1); g_fail_new = 1; dm_audio_reset(); h_au_Stop(&a);
    before = g_create_calls; retry_due(&a);
    assert(g_create_calls == before && !g_running && a.backend == BK_NONE && !atomic_load(&a.null_run));
    finish(&a); puts("Stop during recovery: pending retries do not revive stopped output: PASS");

    begin(&a, 1); g_fail_new = 10; dm_audio_reset();
    int previous = a.retry_ms;
    for (int i = 0; i < 6; i++) {
        retry_due(&a);
        assert(!a.au && a.backend == BK_NULL && atomic_load(&a.null_run) && !g_running);
        assert(a.retry_ms >= previous && a.retry_ms <= 8000); previous = a.retry_ms;
    }
    assert(a.retry_ms == 8000); g_fail_new = 0; retry_due(&a);
    assert(a.au && a.backend == BK_AU && !atomic_load(&a.null_run));
    finish(&a); puts("repeated creation failures: bounded backoff and eventual recovery: PASS");
    return 0;
}
