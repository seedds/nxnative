// nn::audio AudioOut: the game (FMOD nx output layer) appends PCM buffers block by block; once played the system puts them back on the "released" queue and signals the event.
// Host: real audio output = CoreAudio AudioUnit (Mac DefaultOutput / iOS RemoteIO); silent sink = consumes buffers in real time (no sound, pacing matches the real device).
//       iOS defaults to real audio output (the product must have sound), tests that want silence set DM_AUDIO=0 explicitly; Mac defaults to the silent sink (replay / gate checks are silent), DM_AUDIO=1 for real audio output.
// The contents of AudioOut / AudioOutBuffer / AudioOutParameter are read and written only through the interfaces below; the host defines its own internal layout and only writes the front part of the structs (AudioOut: the SDK's own fields, au_info).
#include "hle.h"
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <unistd.h>

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };
enum { BK_NONE, BK_AU, BK_NULL };   // what is consuming an output's queue right now (aout.backend)
enum { AU_IDLE, AU_STARTING, AU_CANCELED, AU_WAIT_CALLBACK };

typedef struct { void *ptr; uint64_t cap, size; } gbuf;     // The first 24 bytes written into the guest AudioOutBuffer
#define QMAX 64
#define RELMAX QMAX                    // Released max = number of in-flight buffers (the game must take a buffer back before it can append again), same limit as pending
typedef struct { uint64_t calls, count, qsum, underruns, zero_bytes; uint32_t fmin, fmax; } audio_stats;
typedef struct {
    pthread_mutex_t m;
    void *event;                       // Guest SystemEvent: signaled whenever a buffer is released
    int ch;
    _Atomic int state;
    _Atomic uint32_t vol;              // SetAudioOutVolume gain as float bits (1.0 at open), applied to the samples as they are pulled
    gbuf *q[QMAX]; _Atomic uint32_t q_head, q_tail;       // Pending: written by the game side, read by the output side
    gbuf *rel[RELMAX]; _Atomic uint32_t rel_head, rel_tail; // Released: written by the output side, read by the game side
    uint64_t pos;                      // Bytes already played of the current block
    AudioUnit au;
    int start_phase, reset_pending, mute;   // a reset during start replaces the unit after the call returns
    int closed;                             // Close returned while a start was in flight: that start's worker frees the output
    _Atomic int hold_device;               // callbacks output silence until the silent sink has joined
    int backend;                       // BK_*: the running consumer. Start, Stop and the background suspend change it only under g_aos_lock and act on it rather than re-deriving it from state and g_suspended
    pthread_t null_th; _Atomic int null_run;
    dispatch_semaphore_t notify_sem;
    pthread_t notify_th; _Atomic int notify_run, notify_pending, notify_armed;
    dispatch_semaphore_t rel_sem;      // Host outputs only (dm_pcmout_open): signaled for each release, so the in-process audio renderer blocks instead of polling (dm_pcmout_wait)
    int id;                            // open sequence number (#N in the log; tells simultaneous outputs apart, e.g. Engage's Wwise and Unity's built-in FMOD)
    int nlog;                          // Start / Stop / Close lines logged so far (at most 32 per output)
    _Atomic uint64_t under_n, under_b; // underruns: times the pending queue was empty when the callback needed data / zero-filled bytes
    uint64_t st_calls, st_count, st_qsum, st_u0, st_ub0; uint32_t st_fmin, st_fmax;   // callback-owned accumulators
    audio_stats st_report; _Atomic int st_pending; // one snapshot, consumed by the watchdog or Close under g_aos_lock
    _Atomic uint64_t cb_n;             // device callbacks of this output (the watchdog's evidence that the device still plays it)
    uint64_t wd_n, wd_t;               // watchdog: callback count last seen and when it last moved (under g_aos_lock)
    int stalled, retry_ms; uint64_t retry_at;   // the device stopped calling back (or would not start): the silent sink stands in and the watchdog retries the AudioUnit at retry_at, backing off
} aout;

// Registry of open outputs: suspended / resumed all at once by the front-end lifecycle (going to background).
// g_aos_lock also serializes every backend change (Start / Stop / suspend / resume). Start used to mark the output started and pick its backend outside it: a suspend in between
// started a silent sink, then Start started a second one over the same single-consumer queues (or the AudioUnit beside it); a Stop racing a suspend joined a sink that never existed
#define AOMAX 8
static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_suspended;
static aout *ao(const void *g) { aout *a = objmap_get(g); if (!a) die("AudioOut %p was not created by the host", g); return a; }
// the call site as "module+offset"; "host" when the host itself calls (in-process audio renderer)
static const char *site(void *lr, char *buf, size_t n) {
    module_t *m = lr ? module_of(lr) : NULL;
    if (m) snprintf(buf, n, "%s+%#llx", m->name, (unsigned long long)((uint8_t *)lr - m->base)); else snprintf(buf, n, "host");
    return buf;
}
// Start / Stop / Close: at most 32 lines per output; middleware calls them only at init, suspend and resume, more means it keeps toggling
static void au_note(aout *a, const char *what, void *lr) {
    char b[64];
    if (a->nlog < 32) { a->nlog++; LOG("audio #%d %s (call site %s)%s", a->id, what, site(lr, b, sizeof b), a->nlog == 32 ? "; not logged from here on" : ""); }
}

static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    if (atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
        dispatch_semaphore_signal(a->notify_sem);
}

void dm_host_helper_qos(qos_class_t q);   // hle_thread.c: under DM_QOS_MAP a helper thread sets its own class instead of inheriting its creator's
static void *event_notify(void *p) {
    aout *a = p; dm_host_helper_qos(QOS_CLASS_USER_INTERACTIVE);   // the game's audio thread waits on the release event it signals
    for (;;) {
        dispatch_semaphore_wait(a->notify_sem, DISPATCH_TIME_FOREVER);
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event)
            hle_signal_system_event(a->event);
        // Acquire a producer that saw the old armed=1, so the pending recheck
        // cannot miss its release. Otherwise the producer or our re-arm posts.
        // A release-only store permits pending=1/armed=0 with both sides asleep.
        atomic_exchange_explicit(&a->notify_armed, 0, memory_order_acq_rel);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) &&
            atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
            dispatch_semaphore_signal(a->notify_sem);
    }
    return NULL;
}

// Take n bytes from the pending queue and hand them to the output side; once a whole block has played, move it to released and notify the ordinary thread.
static void pull(aout *a, uint8_t *dst, uint64_t n) {
    int released = 0;
    int16_t *s16 = (int16_t *)dst; uint64_t ns = n / 2;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) {
            if (qh == qt && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) { atomic_fetch_add(&a->under_n, 1); atomic_fetch_add(&a->under_b, n); }
            memset(dst, 0, n); break; }
        gbuf *b = a->q[qh & (QMAX - 1)];
        uint64_t k = b->size - a->pos; if (k > n) k = n;
        memcpy(dst, (uint8_t *)b->ptr + a->pos, k);
        dst += k; n -= k; a->pos += k;
        if (a->pos >= b->size) {
            uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
            uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire);
            if (rw - rr == RELMAX) die("AudioOut released queue overflow");
            a->rel[rw & (RELMAX - 1)] = b;
            atomic_store_explicit(&a->rel_tail, rw + 1, memory_order_release);
            qh++;
            atomic_store_explicit(&a->q_head, qh, memory_order_release);
            a->pos = 0; released = 1;
        }
    }
    uint32_t vb = atomic_load_explicit(&a->vol, memory_order_relaxed);
    if (vb != 0x3f800000u) {   // the output's volume (SetAudioOutVolume), as the audout session applies it to the PCM16 it plays; saturating
        float v; memcpy(&v, &vb, 4);
        for (uint64_t i = 0; i < ns; i++) { float x = s16[i] * v; s16[i] = (int16_t)(x > 32767.f ? 32767.f : x < -32768.f ? -32768.f : x); }
    }
    if (released) {
        if (a->event) notify_release(a);   // the notify thread only signals the guest's event: outputs without one (the host's renderer output, games that poll) need no wake-up
        if (a->rel_sem) dispatch_semaphore_signal(a->rel_sem);
    }
}

// A six-channel output (front-left front-right center LFE back-left back-right) goes to the device as stereo, downmixed here with the audio renderer's default Q15 coefficients (hle_audren.c):
// Core Audio's own 6 → 2 mapping keeps only the front pair, so voices on the center (Luigi's Mansion 3) and the back channels were silent. The queue still takes all six guest channels,
// in blocks through a stack buffer (no allocation in the real-time callback)
static int device_channels(const aout *a) { return a->ch == 6 ? 2 : a->ch; }
static void pull_device(aout *a, uint8_t *dst, uint64_t bytes) {
    if (a->ch != 6) { pull(a, dst, bytes); return; }
    enum { BLOCK = 128, CF = 32768, CC = 23167, CL = 8224, CB = 23167 };
    int16_t scratch[BLOCK * 6], *out = (int16_t *)dst;
    uint64_t frames = bytes / (2 * sizeof *out);
    while (frames) {
        uint64_t n = frames < BLOCK ? frames : BLOCK;
        pull(a, (uint8_t *)scratch, n * 6 * sizeof *scratch);
        for (uint64_t i = 0; i < n; i++) {
            const int16_t *p = scratch + 6 * i;
            int64_t c = (int64_t)p[2] * CC + (int64_t)p[3] * CL;
            int64_t l = ((int64_t)p[0] * CF + c + (int64_t)p[4] * CB) >> 15;
            int64_t r = ((int64_t)p[1] * CF + c + (int64_t)p[5] * CB) >> 15;
            *out++ = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            *out++ = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        frames -= n;
    }
}

static _Atomic uint64_t g_render_calls;   // Cumulative sound card callbacks (lifecycle verification: must keep growing after resume)
uint64_t dm_audio_render_calls(void) { return atomic_load_explicit(&g_render_calls, memory_order_relaxed); }
static OSStatus au_render(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl) {
    (void)f; (void)ts; (void)bus; (void)frames;
    aout *a = ctx;
    atomic_fetch_add_explicit(&g_render_calls, 1, memory_order_relaxed); atomic_fetch_add_explicit(&a->cb_n, 1, memory_order_relaxed);
    {
        // At each 2000-callback checkpoint publish a snapshot for the watchdog: LOG takes a stdio lock and must never run here or in the release-event notifier.
        // A busy logger leaves its mailbox full; keep accumulating then, rather than waiting, overwriting its snapshot or losing counts.
        uint32_t qd = atomic_load(&a->q_tail) - atomic_load(&a->q_head);
        if (!a->st_calls || frames < a->st_fmin) a->st_fmin = frames; if (frames > a->st_fmax) a->st_fmax = frames; a->st_qsum += qd;
        a->st_count++;
        if (++a->st_calls % 2000 == 0 && !atomic_load_explicit(&a->st_pending, memory_order_acquire)) {
            uint64_t un = atomic_load(&a->under_n), ubb = atomic_load(&a->under_b);
            a->st_report = (audio_stats){a->st_calls, a->st_count, a->st_qsum, un - a->st_u0, ubb - a->st_ub0, a->st_fmin, a->st_fmax};
            atomic_store_explicit(&a->st_pending, 1, memory_order_release);
            a->st_fmin = ~0u; a->st_fmax = 0; a->st_qsum = a->st_count = 0; a->st_u0 = un; a->st_ub0 = ubb;
        } }
    if (atomic_load_explicit(&a->hold_device, memory_order_acquire)) {
        memset(bl->mBuffers[0].mData, 0, bl->mBuffers[0].mDataByteSize);
    } else {
        pull_device(a, bl->mBuffers[0].mData, bl->mBuffers[0].mDataByteSize);
        if (a->mute) memset(bl->mBuffers[0].mData, 0, bl->mBuffers[0].mDataByteSize);
    }
    return noErr;
}
#if !TARGET_OS_IPHONE
// Mac DefaultOutput's default 512 device frames can exceed the PCM a game has queued, producing zero-filled gaps.
// Request about 5 ms of device frames, as front_ios.m asks of the iOS audio session; reapply on device or output-rate changes within the supported range.
// DM_DBG_AU_FRAMES=N asks N frames instead (1024 = the iOS default, which reproduces the crackling)
static void au_buffer(AudioUnit au, const char *why) {
    AudioDeviceID device = kAudioObjectUnknown; UInt32 sz = sizeof device;
    if (AudioUnitGetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, &sz) || device == kAudioObjectUnknown) return;
    AudioStreamBasicDescription df; sz = sizeof df;
    OSStatus st = AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &df, &sz);
    if (st || !(df.mSampleRate > 0)) { LOG("[RED] audio device buffer: cannot read the output sample rate (status %d)", (int)st); return; }
    const char *e = getenv("DM_DBG_AU_FRAMES");
    UInt32 fr = e ? (UInt32)atoi(e) : (UInt32)(df.mSampleRate / 200 + 0.5);
    AudioValueRange range; sz = sizeof range;
    if (!e && !AudioUnitGetProperty(au, kAudioDevicePropertyBufferFrameSizeRange, kAudioUnitScope_Global, 0, &range, &sz)) {
        if (fr < range.mMinimum) fr = (UInt32)range.mMinimum;
        if (fr > range.mMaximum) fr = (UInt32)range.mMaximum;
    }
    // Changing the buffer can itself notify the stream-format listener. Do not write an unchanged value again.
    UInt32 actual = 0; sz = sizeof actual;
    st = AudioUnitGetProperty(au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &actual, &sz);
    if (st || actual != fr) st = AudioUnitSetProperty(au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &fr, sizeof fr);
    if (!st) { sz = sizeof actual; st = AudioUnitGetProperty(au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &actual, &sz); }
    LOG("%saudio device buffer: %u frames at %.0f Hz%s (requested %u, status %d)", st ? "[RED] " : "", actual, df.mSampleRate, why ? why : "", fr, (int)st);
}
static void au_device_changed(void *ctx, AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el) {
    (void)ctx;
    if (el == 0 && ((id == kAudioOutputUnitProperty_CurrentDevice && scope == kAudioUnitScope_Global) ||
                   (id == kAudioUnitProperty_StreamFormat && scope == kAudioUnitScope_Output)))
        au_buffer(au, ", after output changed");
}
#endif
static void au_dispose(AudioUnit au) {
#if !TARGET_OS_IPHONE
    // Disposal clears CurrentDevice and notifies listeners. Remove them before the unit tears down its device.
    AudioUnitRemovePropertyListenerWithUserData(au, kAudioOutputUnitProperty_CurrentDevice, au_device_changed, NULL);
    AudioUnitRemovePropertyListenerWithUserData(au, kAudioUnitProperty_StreamFormat, au_device_changed, NULL);
#endif
    AudioUnitUninitialize(au); AudioComponentInstanceDispose(au);
}
static OSStatus au_create(aout *a) {
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
                                   .componentSubType = kAudioUnitSubType_RemoteIO,
#else
                                   .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
                                   .componentManufacturer = kAudioUnitManufacturer_Apple};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    a->au = NULL; if (!c || AudioComponentInstanceNew(c, &a->au)) { a->au = NULL; return -1; }
    unsigned ch = (unsigned)device_channels(a);
    AudioStreamBasicDescription f = {.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
                                     .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 2u * ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * ch,
                                     .mChannelsPerFrame = ch, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {au_render, a};
#if !TARGET_OS_IPHONE
    const AudioUnitPropertyID props[] = {kAudioOutputUnitProperty_CurrentDevice, kAudioUnitProperty_StreamFormat};
    for (unsigned i = 0; i < sizeof props / sizeof *props; i++) {
        OSStatus st = AudioUnitAddPropertyListener(a->au, props[i], au_device_changed, NULL);
        if (st) LOG("[RED] audio: cannot listen for output property %u (status %d)", (unsigned)props[i], (int)st);
    }
    au_buffer(a->au, NULL);
#endif
    OSStatus st = AudioUnitSetProperty(a->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (!st) st = AudioUnitSetProperty(a->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (!st) st = AudioUnitInitialize(a->au);
    if (st) { au_dispose(a->au); a->au = NULL; }
    return st;
}
// OpenAudioOut runs on the game's thread, and creating the AudioUnit takes about 24 KB of stack (measured on the Mac: component lookup, instance, Initialize), more than some game threads have
// left (Super Mario 3D World's save thread has 8 KB in all; Pokémon: Let's Go overflowed here at startup, PR #36). So it runs on a host thread, once per AudioOut (it predates the big-stack entry, which Start / Stop / Close use)
static void *au_create_th(void *a) { return (void *)(intptr_t)au_create(a); }
static void au_open(aout *a) {
    pthread_t t; void *r = NULL;
    if (pthread_create(&t, NULL, au_create_th, a) || dm_host_thread_join(t, &r)) die("AudioUnit setup: cannot start its thread");
    OSStatus st = (OSStatus)(intptr_t)r; if (st) die("AudioUnit setup failed (status %d)", (int)st);
}
// Silent sink: consumes in real time at 48 kHz every 5 ms. When more than 100 ms behind (iOS suspended the whole process in the background, SIGSTOP on the Mac), restart the count from now and do not make up the missed time --
// making it up would pull hundreds of times in one go (seconds' worth), releasing all of the game's buffers and logging hundreds of underruns; a suspended console does not play that stretch either
static void *null_sink(void *p) {
    aout *a = p; uint8_t scratch[RATE / 200 * 2 * 8]; dm_host_helper_qos(QOS_CLASS_USER_INTERACTIVE);   // paces the game's audio in real time
    uint64_t bytes = (uint64_t)RATE / 200 * 2 * a->ch, t = now_ns();
    while (atomic_load_explicit(&a->null_run, memory_order_acquire)) {
        t += 5000000; uint64_t now = now_ns();
        if (t > now) usleep((useconds_t)((t - now) / 1000)); else if (now - t > 100000000) t = now;
        pull(a, scratch, bytes);
    }
    return NULL;
}
// Backend changes; callers hold g_aos_lock. pull() takes no lock, so joining a sink under it cannot deadlock
static void null_start(aout *a) { atomic_store_explicit(&a->hold_device, 1, memory_order_release); atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); a->backend = BK_NULL; }
static void null_stop(aout *a) { atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); a->backend = BK_NONE; }
static void au_halt(aout *a) {
    if (a->start_phase == AU_STARTING) a->start_phase = AU_CANCELED;
    if (a->backend == BK_AU || a->start_phase == AU_WAIT_CALLBACK) {
        AudioOutputUnitStop(a->au);
        a->start_phase = AU_IDLE;
        if (a->backend == BK_AU) a->backend = BK_NONE;
    }
    a->stalled = 0; a->retry_ms = 0; a->retry_at = 0;
}
static int real_audio(void) { const char *e = getenv("DM_AUDIO"); return e ? atoi(e) != 0 : TARGET_OS_IPHONE; }
static void au_retry(aout *a, uint64_t now) {
    a->stalled = 1;
    a->retry_ms = a->retry_ms ? (a->retry_ms < 8000 ? a->retry_ms * 2 : 8000) : 1000;
    a->retry_at = now + (uint64_t)a->retry_ms * 1000000;
}
static void au_replace(aout *a) {
    au_dispose(a->au);
    OSStatus st = au_create(a);
    if (st) LOG("[RED] audio #%d: no AudioUnit after the media services reset (status %d): this output stays silent", a->id, (int)st);
    a->reset_pending = 0;
}
static void aout_free(aout *a) {
    if (a->au) au_dispose(a->au);
    dispatch_release(a->notify_sem);
    if (a->rel_sem) dispatch_release(a->rel_sem);
    pthread_mutex_destroy(&a->m); free(a);
}
// A HAL start can block for seconds. Each output keeps consuming on the silent sink while its
// own start is in flight. Stop/suspend cancel it without waiting; Close leaves the unit and the output to this worker.
static void au_start_device(void *p) {
    aout *a = p;
    OSStatus st = AudioOutputUnitStart(a->au);
    pthread_mutex_lock(&g_aos_lock);
    if (a->closed) {   // unregistered by Close, which no other path reaches: stop and free outside the lock
        pthread_mutex_unlock(&g_aos_lock);
        if (!st) AudioOutputUnitStop(a->au);
        aout_free(a);
        return;
    }
    if (a->start_phase == AU_CANCELED) {
        if (!st) AudioOutputUnitStop(a->au);
        a->start_phase = AU_IDLE;
        if (a->reset_pending) au_replace(a);
    } else if (st) {
        if (!a->stalled) LOG("[RED] audio #%d: the AudioUnit did not start (status %d): the silent sink stands in, retrying", a->id, (int)st);
        a->start_phase = AU_IDLE; au_retry(a, now_ns());
    } else {
        a->wd_n = atomic_load_explicit(&a->cb_n, memory_order_relaxed);
        a->start_phase = AU_WAIT_CALLBACK; a->wd_t = now_ns();
    }
    pthread_mutex_unlock(&g_aos_lock);
}
static void au_request_start(aout *a) {
    if (!a->au || g_suspended || a->start_phase != AU_IDLE) return;
    a->start_phase = AU_STARTING;
    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), a, au_start_device);
}
// Watchdog (real output only): an AudioUnit that is started while the app is in focus must keep calling back. iOS stops calling back without telling the AudioUnit when another audio session takes over
// (Siri, an alarm, a call) or the media services reset; the output's buffers were then never released, and a game whose audio thread waits for them (Wwise's poll-only output, FMOD) stayed silent for good.
// After 200 ms without a callback the silent sink takes over (the game's audio keeps its real-time pace) and the AudioUnit is retried after 1, 2, 4, 8 s. A late wake-up of the watchdog itself (the whole process was stopped) only restarts the count.
// The time is read under g_aos_lock: the startup helper stamps wd_t under it too, and a watchdog time read before the lock was older than the stamp of an AudioUnit the return from the background had just
// started (DRAGON QUEST VII Reimagined on an iPhone: "has not called back for 18446744073662 ms", a restart of a running AudioUnit)
enum { WD_TICK_MS = 50, WD_STALL_MS = 200 };
typedef struct { audio_stats stats; int id, ch; } audio_report;
// Both consumers hold g_aos_lock. A local copy owns every log argument, so a slow log cannot hold up registry changes or retain a closed output.
static int stats_take(aout *a, audio_report *r) {
    if (!atomic_load_explicit(&a->st_pending, memory_order_acquire)) return 0;
    *r = (audio_report){a->st_report, a->id, a->ch};
    atomic_store_explicit(&a->st_pending, 0, memory_order_release);
    return 1;
}
static void stats_log(const audio_report *r) {
    const audio_stats *s = &r->stats;
    LOG("audio #%d device callbacks %llu: %u–%u frames each, %.1f blocks queued on average at callback; in these %llu, %llu underruns, %.1f ms zero-padded", r->id,
        (unsigned long long)s->calls, s->fmin, s->fmax, (double)s->qsum / s->count, (unsigned long long)s->count,
        (unsigned long long)s->underruns, s->zero_bytes / (2.0 * r->ch) / (RATE / 1000.0));
}
static void stats_drain(void) {
    audio_report reports[AOMAX]; int n = 0;
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] && stats_take(g_aos[i], &reports[n])) n++;
    pthread_mutex_unlock(&g_aos_lock);
    for (int i = 0; i < n; i++) stats_log(&reports[i]);
}
static void *au_watchdog(void *p) {
    (void)p; uint64_t last = now_ns();
    for (;;) {
        usleep(WD_TICK_MS * 1000);
        pthread_mutex_lock(&g_aos_lock);
        uint64_t t = now_ns(), gap = t - last; last = t;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i]; if (!a || !a->au) continue;
            uint64_t n = atomic_load_explicit(&a->cb_n, memory_order_relaxed);
            if (a->start_phase == AU_WAIT_CALLBACK) {
                if (n != a->wd_n) {
                    null_stop(a);
                    atomic_store_explicit(&a->hold_device, 0, memory_order_release);
                    if (a->stalled) LOG("audio #%d: AudioUnit restarted", a->id);
                    a->backend = BK_AU; a->start_phase = AU_IDLE; a->stalled = a->retry_ms = 0;
                    a->wd_n = n; a->wd_t = t;
                } else if (gap > 4ull * WD_TICK_MS * 1000000) a->wd_t = t;
                else if (t - a->wd_t >= 1000000000ull) {
                    if (!a->stalled) LOG("[RED] audio #%d: no device callback after start; continuing on the silent sink", a->id);
                    AudioOutputUnitStop(a->au); a->start_phase = AU_IDLE; au_retry(a, t);
                }
                continue;
            }
            if (a->backend == BK_NULL) {
                if (t >= a->retry_at) au_request_start(a);
                continue;
            }
            if (a->backend != BK_AU) continue;
            if (gap > 4ull * WD_TICK_MS * 1000000 || n != a->wd_n) {
                a->wd_n = n; a->wd_t = t;
            } else if (t - a->wd_t > WD_STALL_MS * 1000000ull) {
                LOG("[RED] audio #%d: the device has not called back for %.0f ms while playing: the silent sink stands in, retrying the AudioUnit", a->id, (t - a->wd_t) / 1e6);
                AudioOutputUnitStop(a->au); a->backend = BK_NONE; null_start(a);
                au_retry(a, t);
            }
        }
        pthread_mutex_unlock(&g_aos_lock);
        stats_drain();
    }
    return NULL;
}
static void au_watchdog_start(void) { pthread_t t; if (!pthread_create(&t, NULL, au_watchdog, NULL)) pthread_detach(t); }

// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }, 0 = system default (48 kHz / 2 channels)
HLE_FORCE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", "audio output parameter defaults", void, h_au_InitParam, uint8_t *p) { memset(p, 0, 8); }
// AudioOutInfo = { char name[256] }
HLE_FORCE("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", "audio output device list; 1 on the host", int, h_au_List, char *info, int n) {
    if (n < 1) return 0;
    memset(info, 0, 256); strcpy(info, "DeviceOut"); return 1;
}
// The front of the guest AudioOut as the SDK's OpenAudioOut fills it from the service's answer: sample rate, channel count, sample format, state (+0 / +4 / +8 / +0xc, the same in SDK 8, 12, 18 and 20;
// its GetAudioOutSampleRate / ChannelCount / SampleFormat are single loads from there); CloseAudioOut sets the state to stopped and leaves the rest. Code outside the SDK reads these fields too:
// middleware that checks the first word before it stops and closes its output skipped the close while the host left it zero, freed the PCM buffers and the host's consumer read freed memory (Luigi's Mansion 3, PR #109)
static void au_info(void *g, int ch, int state) { uint32_t *f = g; f[0] = RATE; f[1] = (uint32_t)ch; f[2] = FMT_PCM16; f[3] = (uint32_t)state; }
static Result au_open_at(void *g, void *ev, const char *name, const uint8_t *param, void *lr) {
    static _Atomic int nopen;
    int ch = *(const uint16_t *)(param + 4); int rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if (rate && rate != RATE) die("AudioOut sample rate %d not supported (48 kHz only)", rate);
    aout *a = calloc(1, sizeof *a);
    const char *mute = getenv("DM_AUDIO_MUTE"); a->mute = mute && atoi(mute);
    pthread_mutex_init(&a->m, NULL); a->event = ev; a->ch = ch;
    atomic_init(&a->state, STATE_STOPPED); atomic_init(&a->vol, 0x3f800000u);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0);
    atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->null_run, 0); atomic_init(&a->notify_run, 1);
    atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0);
    atomic_init(&a->st_pending, 0);
    a->notify_sem = dispatch_semaphore_create(0); a->id = atomic_fetch_add(&nopen, 1);
    pthread_create(&a->notify_th, NULL, event_notify, a);
    if (ev) hle_bind_system_event(ev, 1);   // Buffer release event: bind a kernel event on the guest thread (auto-clear), afterwards signaled by the audio thread
    char sb[64]; int use_au = real_audio();
    LOG("audio #%d OpenAudioOut %s channels %d %s%s (call site %s)", a->id, name ? name : "(default)", ch, use_au ? "real output" : "silent sink", ev ? "" : ", no release event (the game polls)", site(lr, sb, sizeof sb));
    // The helper finishes (and is joined) before lifecycle/watchdog readers can find this output.
    // Keep construction outside g_aos_lock: joining the helper can yield the guest core.
    if (use_au) au_open(a);
    objmap_put(g, a);
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1; for (int i = 0; i < AOMAX; i++) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) die("more than %d AudioOuts open at once", AOMAX);
    g_aos[slot] = a;
    pthread_mutex_unlock(&g_aos_lock);
    if (use_au) { static pthread_once_t wd = PTHREAD_ONCE_INIT; pthread_once(&wd, au_watchdog_start); }
    au_info(g, ch, STATE_STOPPED);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", "connect to the audout service; host AudioUnit / silent sink",
          Result, h_au_Open, void *g, void *ev, const char *name, const uint8_t *param) { return au_open_at(g, ev, name, param, dm_bigstack_caller(__builtin_return_address(0))); }
// The two default-device overloads (Engage, SDK 12): as OpenAudioOut with the default device name; the overload without an event binds no release event (the game polls GetReleasedAudioOutBuffer)
// SDK 22 dropped the release event from the parameter-carrying overload (Well Dweller calls this one; unbound it reached the SDK's audout session, which fails with 0xe15 and the game aborts)
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterE", "connect to the audout service, no release event (SDK 22 overload)",
          Result, h_au_Open4, void *g, const char *name, const uint8_t *param) { return au_open_at(g, NULL, name, param, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", "connect to the audout service (default device); same as OpenAudioOut",
          Result, h_au_OpenDef, void *g, void *ev, const uint8_t *param) { return au_open_at(g, ev, NULL, param, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", "connect to the audout service (default device, no event); same as OpenAudioOut",
          Result, h_au_OpenDef0, void *g, const uint8_t *param) { return au_open_at(g, NULL, NULL, param, dm_bigstack_caller(__builtin_return_address(0))); }
static Result au_start_at(void *g, void *lr) {
    aout *a = ao(g);
    pthread_mutex_lock(&g_aos_lock);
    int was = atomic_exchange_explicit(&a->state, STATE_STARTED, memory_order_acq_rel);
    au_note(a, was == STATE_STARTED ? "Start (already playing)" : g_suspended ? "Start (app in the background: the silent sink takes over for now)" : "Start", lr);
    if (was != STATE_STARTED) {
        null_start(a); au_request_start(a);
    }
    pthread_mutex_unlock(&g_aos_lock);
    return RES_OK;
}
// Start, Stop and Close run on the game's thread and call into Core Audio, which needs far more stack than a game thread may have left; they run on the big host stack (hle_bigstack.c).
// The first AudioOutputUnitStart of the process sets up the Core Audio session (HALC_ProxyIOContext::StartIOProc -> +[AVAudioSession sharedInstance], which connects to the session service over XPC).
// On the guest stack that ran off the stack's bottom with SIGBUS, sp in the guest stack block, in a frame that varies between runs (CFString formatting under NSXPCInterface, the Swift runtime's type
// demangler under CAReporter); Arcade Archives RAINBOW ISLANDS 1.0.2, SDK 19.3.11, M1, macOS 27.2, PR #721. Stop (AudioOutputUnitStop) and Close (disposing the AudioUnit) talk to the same service.
// AudioOutputUnitStart itself now runs on a host worker (au_start_device, issue #1030).
// dm_bigstack_caller gives the game's call site back for the log line
HLE_FORCE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "start playback", Result, h_au_Start, void *g) { return au_start_at(g, dm_bigstack_caller(__builtin_return_address(0))); }
// After the backend stops, return even its partly played buffer. FMOD can wait for these releases before joining its output thread (Pokemon Brilliant Diamond 1.0.0, PR #190).
// The stopped backend excludes the consumer; m excludes append/reclaim. Buffers appended while already stopped belong to the next Start, so only a started -> stopped transition calls this.
static void au_release_pending(aout *a) {
    pthread_mutex_lock(&a->m);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed), qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire), rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
    int released = qh != qt;
    while (qh != qt) {
        if (rw - rr >= RELMAX) die("AudioOut released queue overflow while stopping");
        a->rel[rw++ & (RELMAX - 1)] = a->q[qh++ & (QMAX - 1)];
    }
    atomic_store_explicit(&a->rel_tail, rw, memory_order_release);
    atomic_store_explicit(&a->q_head, qh, memory_order_release); a->pos = 0;
    pthread_mutex_unlock(&a->m);
    if (released) { if (a->event) notify_release(a); if (a->rel_sem) dispatch_semaphore_signal(a->rel_sem); }
}
static void au_stop_at(void *g, void *lr) {   // lr NULL: the stop inside Close, not logged separately
    aout *a = ao(g);
    pthread_mutex_lock(&g_aos_lock);
    int was = atomic_exchange_explicit(&a->state, STATE_STOPPED, memory_order_acq_rel);
    if (lr) au_note(a, was == STATE_STARTED ? "Stop" : "Stop (already stopped)", lr);
    au_halt(a);
    if (a->backend == BK_NULL) null_stop(a);
    if (was == STATE_STARTED) au_release_pending(a);
    pthread_mutex_unlock(&g_aos_lock);
}
HLE_FORCE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "stop playback", void, h_au_Stop, void *g) { au_stop_at(g, dm_bigstack_caller(__builtin_return_address(0))); }
static void au_close(void *g, void *lr) {
    au_note(ao(g), "Close", lr);
    au_stop_at(g, NULL);
    aout *a = objmap_del(g);
    atomic_store_explicit(&a->notify_run, 0, memory_order_release);
    dispatch_semaphore_signal(a->notify_sem);
    pthread_join(a->notify_th, NULL);
    audio_report report;
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] == a) g_aos[i] = NULL;
    int have_report = stats_take(a, &report), ch = a->ch;
    // A start the driver still holds keeps using the unit. Waiting for it here held the game's thread as long as the driver blocked: hand the output to that start's worker instead
    int in_flight = a->start_phase == AU_CANCELED;
    a->closed = in_flight; a->reset_pending = 0;
    pthread_mutex_unlock(&g_aos_lock);
    if (have_report) stats_log(&report);
    if (!in_flight) aout_free(a);
    au_info(g, ch, STATE_STOPPED);
}
HLE_FORCE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "close the audio output", void, h_au_Close, void *g) { au_close(g, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "playback state", int, h_au_State, const void *g) { return atomic_load_explicit(&ao(g)->state, memory_order_acquire); }
// Pokémon Legends: Z-A (SDK 19): Wwise's sink wakeup thread (main+0x30faae8) asks after every release event and marks the sink starved at 0; the SDK's own version read the audout session the host never made
HLE_FORCE("_ZN2nn5audio22GetAudioOutBufferCountEPKNS0_8AudioOutE", "buffers appended and not yet played to the end (the one playing included)", unsigned, h_au_BufCount, const void *g) {
    aout *a = ao(g);
    return atomic_load_explicit(&a->q_tail, memory_order_acquire) - atomic_load_explicit(&a->q_head, memory_order_acquire);
}
HLE_FORCE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", "sample rate", int, h_au_Rate, const void *g) { (void)ao(g); return RATE; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", "channel count", int, h_au_Ch, const void *g) { return ao(g)->ch; }
HLE_FORCE("_ZN2nn5audio15GetAudioOutNameEPKNS0_8AudioOutE", "device name; the host has only the default device", const char *, h_au_Name, const void *g) { (void)g; return "DeviceOut"; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "sample format; PCM16 only", int, h_au_Fmt, const void *g) { (void)ao(g); return FMT_PCM16; }
// STEINS;GATE RE:BOOT (SDK 20) sets its movie sound's output volume right after opening it; the SDK's versions call the audout session the host never made (a null read at sdk+0x1f2314)
HLE_FORCE("_ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", "output volume; a gain on the samples the host plays", void, h_au_SetVol, void *g, float v) {
    if (!(v >= 0.f)) v = 0.f; uint32_t b; memcpy(&b, &v, 4); atomic_store_explicit(&ao(g)->vol, b, memory_order_relaxed);
}
HLE_FORCE("_ZN2nn5audio17GetAudioOutVolumeEPKNS0_8AudioOutE", "output volume", float, h_au_GetVol, const void *g) {
    uint32_t b = atomic_load_explicit(&ao(g)->vol, memory_order_relaxed); float v; memcpy(&v, &b, 4); return v;
}

HLE_FORCE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", "buffer description; written into the first 24 bytes of the guest struct", void, h_au_SetBuf, gbuf *b, void *p, uint64_t cap, uint64_t size) {
    b->ptr = p; b->cap = cap; b->size = size;
}
HLE_FORCE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "buffer data pointer", void *, h_au_BufPtr, const gbuf *b) { return b->ptr; }
HLE_FORCE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "buffer data size", uint64_t, h_au_BufSize, const gbuf *b) { return b->size; }
HLE_FORCE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "append a buffer to play", unsigned, h_au_Append, void *g, gbuf *b) {
    aout *a = ao(g);
    // Diagnostic switch DM_AUDIO_DUMP=<file>: every PCM block the game appends is written out as is (16-bit, 48 kHz, channel count per the OpenAudioOut log, usually 6). Used to judge "did a given sound play or not":
    // cross-correlate the original game's audio against it (this is how the tier-one drop sound was verified). The file is opened only when the variable is set; when unset this costs only one extra cached check
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("DM_AUDIO_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("[RED] DM_AUDIO_DUMP cannot open %s", e); }
      if (df && b && b->ptr && b->size) fwrite(b->ptr, 1, (size_t)b->size, df); }
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    int ok = qt - qh < QMAX;
    if (ok) { a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return ok;      // bool: false when the queue is full (the real device has a limit too)
}
HLE_FORCE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", "get a played buffer; NULL if none", gbuf *, h_au_Released, void *g) {
    aout *a = ao(g); gbuf *b = NULL;
    pthread_mutex_lock(&a->m);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_relaxed);
    uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_acquire);
    if (rr != rw) { b = a->rel[rr & (RELMAX - 1)]; atomic_store_explicit(&a->rel_head, rr + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return b;
}

// Audio input (a microphone): the host has none. SDL3's Switch audio driver lists the input devices when its audio starts (Absolum); the SDK's versions reach the audin service.
// No device is listed and an open fails, so the calls that need an open AudioIn are never reached; they still answer without touching the guest object, in case a game ignores the open's result.
// The buffer descriptor functions (SetAudioInBufferInfo, GetAudioInBufferDataPointer / Size) only fill and read the game's own struct and run in the sdk
HLE_FORCE("_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", "audio input device list; the host has no microphone: 0 entries", int, h_ai_List, void *info, int n) { (void)info; (void)n; return 0; }
static Result ai_open(const char *name, void *lr) {
    static _Atomic int once; char sb[64];
    if (!atomic_exchange(&once, 1)) LOG("audio input %s: the host has no microphone, the open fails (call site %s)", name ? name : "(default)", site(lr, sb, sizeof sb));
    return RES_FAIL;
}
HLE_FORCE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPNS_2os11SystemEventEPKcRKNS0_16AudioInParameterE", "connect to the audin service; no microphone on the host: fails",
          Result, h_ai_Open, void *g, void *ev, const char *name, const void *param) { (void)g; (void)ev; (void)param; return ai_open(name, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", "connect to the audin service (no buffer event: Patrick's Parabox imports this one); no microphone on the host: fails",
          Result, h_ai_OpenNoEv, void *g, const char *name, const void *param) { (void)g; (void)param; return ai_open(name, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn5audio18OpenDefaultAudioInEPNS0_7AudioInEPNS_2os11SystemEventERKNS0_16AudioInParameterE", "connect to the audin service (default device); no microphone on the host: fails",
          Result, h_ai_OpenDef, void *g, void *ev, const void *param) { (void)g; (void)ev; (void)param; return ai_open(NULL, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn5audio12StartAudioInEPNS0_7AudioInE", "start recording; never opened on the host: fails", Result, h_ai_Start, void *g) { (void)g; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio11StopAudioInEPNS0_7AudioInE", "stop recording; never opened on the host", void, h_ai_Stop, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio12CloseAudioInEPNS0_7AudioInE", "close the audio input; never opened on the host", void, h_ai_Close, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio19AppendAudioInBufferEPNS0_7AudioInEPNS0_13AudioInBufferE", "append a recording buffer; never opened on the host: refused", unsigned, h_ai_Append, void *g, void *b) { (void)g; (void)b; return 0; }
HLE_FORCE("_ZN2nn5audio24GetReleasedAudioInBufferEPNS0_7AudioInE", "get a recorded buffer; never opened on the host: none", void *, h_ai_Released, void *g) { (void)g; return NULL; }
HLE_FORCE("_ZN2nn5audio20GetAudioInSampleRateEPKNS0_7AudioInE", "recording sample rate", int, h_ai_Rate, const void *g) { (void)g; return RATE; }
HLE_FORCE("_ZN2nn5audio22GetAudioInChannelCountEPKNS0_7AudioInE", "recording channel count", int, h_ai_Ch, const void *g) { (void)g; return 2; }
HLE_FORCE("_ZN2nn5audio22GetAudioInSampleFormatEPKNS0_7AudioInE", "recording sample format", int, h_ai_Fmt, const void *g) { (void)g; return FMT_PCM16; }

// ---------- SDK 21's OutputStream (Xenoblade Chronicles 2's Switch 2 Edition, SDK 21.4.4) ----------
// nn::audio::OpenOutputStream on a virtual output device (GetDefaultVirtualOutputDevice) takes a StreamFormat {sample format, channels, sample rate} and a capacity in frames; the game
// writes one period at a time with WriteOutputStream once GetOutputStreamFreeFrameCount covers it, and its audio thread waits on the stream's event with a short timeout between writes
// (main+0xad0860, main+0xad14c0, main+0xad15c0). Its frames are 32-bit float, interleaved (L, R, L, R), though main+0xad1684 zeroes an unfilled tail as if each channel had its own run of a
// period: read as planar, every 10 ms block's first half played as left and its second half as right, each at half speed, so all sound came out an octave low with a jump at each block.
// Host: each write becomes one 16-bit interleaved block on an AudioOut of the stream's own (the AudioOut machinery above: real output or silent sink, its release event the stream's),
// so the free count is the capacity less the frames still queued and the event fires as blocks play. States as the game reads them: 0 not open, 1 open (stopped), 2 started
// (main+0xad1310: a started stream is stopped, then purged and closed). Without these the SDK connected to auddmg:u itself and aborted with 0xa99 (sdk+0x221520)
enum { OS_NONE = 0, OS_STOPPED = 1, OS_STARTED = 2, OS_FLOAT = 5, OS_PCM16 = 2 };
typedef struct { uint8_t key[64]; pthread_mutex_t m; int ch, fmt, cap, queued, nblk, state; } ostream;   // key: the AudioOut's object (au_info writes its first 16 bytes)
typedef struct { gbuf b; int frames; int16_t pcm[]; } oblock;
static ostream *os_of(const void *g) { ostream *o = objmap_get(g); if (!o) die("OutputStream %p was not opened by the host", g); return o; }
static void os_reclaim(ostream *o) {   // under o->m: the blocks the output played
    gbuf *b; while ((b = h_au_Released(o->key))) { oblock *k = (oblock *)b; o->queued -= k->frames; o->nblk--; free(k); }
}
enum { VDEV_NAME = 0x100 };   // nn::audio::AudioDeviceName
static const char VDEV_OUT[] = "AudioVirtualOutput";   // the host's one virtual output device, a name of the host's
HLE_FORCE("_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", "connect to auddmg:u; the host has one output: its name", void, h_os_DefaultDev, char *name) {
    memset(name, 0, VDEV_NAME); strlcpy(name, VDEV_OUT, VDEV_NAME);
}
// nn::audio::VirtualDeviceProperties, 0x110 bytes, as audio::detail::GetVirtualOutputDeviceProperties fills it from the device manager's 0x228-byte profile (Nintendo Switch Sports
// Resort's sdk+0x224cd4; the input and injection twins at sdk+0x1f6314 and sdk+0x1f4db4 do the same): the name asked about, the profile's +0x218 at +0x100 and its +0x108 at +0x108.
// The profile's +0x218 is the shortest stream as a duration in ns: OutputStreamImpl::Open refuses a capacity under ceil(it x rate / 1e9) frames with 0x899 (sdk+0x225a94..0x225ad0,
// InputStreamImpl::Open the same at sdk+0x1f6928). Nothing in the sdk reads the +0x108 word; its meaning is open, 0 here. Both of the game's callers (main+0x5eda90, main+0x7a0414)
// and Donkey Kong Bananza's (main+0x57cadc, main+0x187cee4) only test the Result before opening a stream. Unhooked, the SDK connected to auddmg:u and aborted with 0xa99 (sdk+0x224d1c)
#define OS_MIN_NS 10000000LL   // the host's AudioOut takes any block size and os_open sets no minimum: one 10 ms period, what Xenoblade Chronicles 2 and Star Fox write per block
typedef struct { char name[VDEV_NAME]; int64_t min_ns; uint32_t word_108, pad; } vdev_props;
HLE_FORCE("_ZN2nn5audio32GetVirtualOutputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", "connect to auddmg:u; the host's one output device",
          Result, h_os_Props, vdev_props *p, const char *name) {
    if (strncmp(name, VDEV_OUT, VDEV_NAME)) { LOG("[RED] virtual output device %.64s: the host has only %s, no properties", name, VDEV_OUT); return RES_FAIL; }
    memset(p, 0, sizeof *p); memcpy(p->name, name, sizeof p->name);
    p->min_ns = OS_MIN_NS;
    return RES_OK;
}
static Result os_open(void *g, void *ev, const int32_t *fmt, int frames, void *lr) {
    int f = fmt[0], ch = fmt[1], rate = fmt[2];
    if (f != OS_FLOAT && f != OS_PCM16) { LOG("[RED] OutputStream sample format %d: not supported, the open fails", f); return RES_FAIL; }
    if (rate && rate != RATE) { LOG("[RED] OutputStream sample rate %d: not supported (48 kHz only), the open fails", rate); return RES_FAIL; }
    if (ch <= 0 || frames <= 0) return RES_FAIL;
    ostream *o = calloc(1, sizeof *o); if (!o) die("OutputStream calloc");
    pthread_mutex_init(&o->m, NULL); o->ch = ch; o->fmt = f; o->cap = frames; o->state = OS_STOPPED;
    uint8_t param[8] = {0}; *(int32_t *)param = RATE; *(uint16_t *)(param + 4) = (uint16_t)ch;
    Result r = au_open_at(o->key, ev, "OutputStream", param, lr);
    if (r) { free(o); return r; }
    objmap_put(g, o);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE",
          "connect to auddmg:u; an AudioOut of the host's", Result, h_os_Open, void *g, void *ev, void *buf, size_t size, const char *dev, const int32_t *fmt, int frames, int threshold, uint32_t flags) {
    (void)buf; (void)size; (void)dev; (void)threshold; (void)flags;
    return os_open(g, ev, fmt, frames, dm_bigstack_caller(__builtin_return_address(0)));
}
// The overload without flags (Nintendo Switch Sports Resort, main+0x5edb34): the SDK's (sdk+0x224740) is the one above with no flags (sdk+0x225d10)
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEii",
          "connect to auddmg:u (no flags); an AudioOut of the host's", Result, h_os_OpenNoFlags, void *g, void *ev, void *buf, size_t size, const char *dev, const int32_t *fmt, int frames, int threshold) {
    (void)buf; (void)size; (void)dev; (void)threshold;
    return os_open(g, ev, fmt, frames, dm_bigstack_caller(__builtin_return_address(0)));
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE",
          "connect to auddmg:u (no event: the game polls); an AudioOut of the host's", Result, h_os_OpenNoEv, void *g, void *buf, size_t size, const char *dev, const int32_t *fmt, int frames, uint32_t flags) {
    (void)buf; (void)size; (void)dev; (void)flags;
    return os_open(g, NULL, fmt, frames, dm_bigstack_caller(__builtin_return_address(0)));
}
HLE_FORCE("_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", "start the stream's output", Result, h_os_Start, void *g) {
    ostream *o = os_of(g); o->state = OS_STARTED; return au_start_at(o->key, dm_bigstack_caller(__builtin_return_address(0)));
}
// Star Fox (Wwise 2024.1, main+0xbce9294) starts its main and passthrough streams together
HLE_FORCE("_ZN2nn5audio18StartOutputStreamsEPKPNS0_16OutputStreamTypeEi", "start several streams' outputs", Result, h_os_StartN, void *const *gs, int n) {
    void *lr = dm_bigstack_caller(__builtin_return_address(0));
    for (int i = 0; i < n; i++) {
        ostream *o = os_of(gs[i]); o->state = OS_STARTED;
        Result r = au_start_at(o->key, lr); if (r) return r;
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", "stop the stream's output", void, h_os_Stop, void *g) {
    ostream *o = os_of(g); au_stop_at(o->key, dm_bigstack_caller(__builtin_return_address(0))); o->state = OS_STOPPED;
}
HLE_FORCE("_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", "drop the frames not yet played", void, h_os_Purge, void *g) {
    ostream *o = os_of(g);
    pthread_mutex_lock(&o->m); os_reclaim(o); pthread_mutex_unlock(&o->m);   // the released queue holds every block from here on, room for the pending ones too
    pthread_mutex_lock(&g_aos_lock); au_release_pending(ao(o->key)); pthread_mutex_unlock(&g_aos_lock);
    pthread_mutex_lock(&o->m); os_reclaim(o); pthread_mutex_unlock(&o->m);
}
HLE_FORCE("_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", "close the stream's output", void, h_os_Close, void *g) {
    ostream *o = objmap_get(g); if (!o) return;
    au_stop_at(o->key, NULL);   // every block the output still held comes back as released
    pthread_mutex_lock(&o->m); os_reclaim(o); pthread_mutex_unlock(&o->m);
    au_close(o->key, dm_bigstack_caller(__builtin_return_address(0)));
    objmap_del(g); pthread_mutex_destroy(&o->m); free(o);
}
HLE_FORCE("_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", "stream state: 0 not open, 1 stopped, 2 started", int, h_os_State, const void *g) {
    ostream *o = objmap_get(g); return o ? o->state : OS_NONE;
}
HLE_FORCE("_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", "frames the stream can take: the capacity less the frames queued", int, h_os_Free, const void *g) {
    ostream *o = os_of(g);
    pthread_mutex_lock(&o->m); os_reclaim(o);
    int n = o->nblk >= QMAX - 1 ? 0 : o->cap - o->queued;
    pthread_mutex_unlock(&o->m);
    return n < 0 ? 0 : n;
}
HLE_FORCE("_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", "frames to play: one 16-bit interleaved block on the stream's AudioOut", int, h_os_Write, void *g, const void *data, int frames) {
    ostream *o = os_of(g);
    if (frames <= 0 || !data) return 0;
    pthread_mutex_lock(&o->m); os_reclaim(o);
    if (o->nblk >= QMAX - 1 || frames > o->cap - o->queued) { pthread_mutex_unlock(&o->m); return 0; }   // the game writes only what the free count allows (main+0xad14f0)
    oblock *k = malloc(sizeof *k + (size_t)frames * o->ch * 2); if (!k) die("OutputStream block malloc");
    size_t n = (size_t)frames * o->ch;
    if (o->fmt == OS_FLOAT) {
        const float *in = data;
        for (size_t i = 0; i < n; i++) {
            float v = in[i] * 32767.0f;
            k->pcm[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
    } else memcpy(k->pcm, data, n * 2);
    k->b.ptr = k->pcm; k->b.cap = k->b.size = (uint64_t)frames * o->ch * 2; k->frames = frames;
    if (!h_au_Append(o->key, &k->b)) { pthread_mutex_unlock(&o->m); free(k); return 0; }
    o->queued += frames; o->nblk++;
    pthread_mutex_unlock(&o->m);
    return frames;
}
HLE_FORCE("_ZN2nn5audio19SetOutputStreamGainEPNS0_16OutputStreamTypeEfNS_8TimeSpanE", "stream gain: the AudioOut's volume (no fade)", void, h_os_Gain, void *g, float gain, int64_t fade) {
    (void)fade; h_au_SetVol(os_of(g)->key, gain);
}
HLE_FORCE("_ZN2nn5audio31GetOutputStreamFilledFrameCountEPKNS0_16OutputStreamTypeE", "frames written and not yet played", int, h_os_Filled, const void *g) {
    ostream *o = os_of(g);
    pthread_mutex_lock(&o->m); os_reclaim(o); int n = o->queued; pthread_mutex_unlock(&o->m);
    return n;
}
// The statistics, returned by value through x8, 24 bytes: Star Fox keeps them at sp with its stack canary right behind them at sp+0x18 (main+0xbce9b90), so a larger result ends in
// __stack_chk_fail on its audio thread. The game reads only +0x10 and treats a count above 0 as a glitch (Star Fox main+0xbce9ba0): the host reports none
typedef struct { int64_t a, b, c; } os_stats;
HLE_FORCE("_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", "stream statistics: none kept", os_stats, h_os_Stats, void *g, bool reset) {
    (void)g; (void)reset; os_stats s = {0}; return s;
}
// SDK 21's input streams (a microphone, GameChat; Star Fox imports them), as AudioIn above: no device is listed and an open fails, so the calls that need an open stream are never reached;
// they still answer without touching the guest object, in case a game ignores the open's result
HLE_FORCE("_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", "connect to auddmg:u; the host has no microphone: no device", int, h_is_List, void *names, int n) { (void)names; (void)n; return 0; }
HLE_FORCE("_ZN2nn5audio31GetVirtualInputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", "connect to auddmg:u; no such device", Result, h_is_Props, void *props, const char *name) { (void)props; (void)name; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", "connect to auddmg:u; no microphone on the host: fails", Result, h_is_Open,
          void *g, void *buf, size_t size, const char *dev, const void *fmt, int frames) { (void)g; (void)buf; (void)size; (void)dev; (void)fmt; (void)frames; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", "input stream, never open", void, h_is_Close, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", "input stream, never open", Result, h_is_Start, void *g) { (void)g; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", "input stream, never open", void, h_is_Stop, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", "input stream, never open: nothing read", int, h_is_Read, void *g, void *buf, int frames) { (void)g; (void)buf; (void)frames; return 0; }
HLE_FORCE("_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", "input stream, never open: nothing captured", int, h_is_Filled, const void *g) { (void)g; return 0; }
typedef struct { int32_t format, channels, rate; } os_format;   // nn::audio::StreamFormat, 12 bytes: returned in registers
HLE_FORCE("_ZN2nn5audio20GetInputStreamFormatEPKNS0_15InputStreamTypeE", "input stream, never open: 48 kHz mono PCM16", os_format, h_is_Format, const void *g) { (void)g; return (os_format){OS_PCM16, 1, RATE}; }
// The rest of the input stream calls. The SDK passes them to the stream'"'"'s Impl pointer, which is null for a type that never opened (sdk+0x1f7040 reads [Impl+0x340] first: SIGSEGV at address 0x340),
// and only GetInputStreamState answers a null Impl itself (0, closed; sdk+0x1f6020). The Legend of Zelda: Ocarina of Time asks the state of its two microphone streams, purges them when they are not
// started (main+0x336ee7c, the crash when a song is played), and reads their statistics each update (main+0x336d5d4); it opens them only when a virtual input device is listed, so here never.
// The state is closed, a purge does nothing, the statistics are zero (InputStreamImpl::GetStatistics zeroes its result first, sdk+0x1f6ff4), the free and capacity counts 0, the gain 1
HLE_FORCE("_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE", "input stream, never open: closed (0)", int, h_is_State, const void *g) { (void)g; return 0; }
HLE_FORCE("_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE", "input stream, never open: nothing buffered", void, h_is_Purge, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb", "input stream, never open: zero", os_stats, h_is_Stats, void *g, bool reset) { (void)g; (void)reset; os_stats s = {0}; return s; }
HLE_FORCE("_ZN2nn5audio28GetInputStreamFreeFrameCountEPKNS0_15InputStreamTypeE", "input stream, never open: none", int, h_is_Free, const void *g) { (void)g; return 0; }
HLE_FORCE("_ZN2nn5audio32GetInputStreamCapacityFrameCountEPKNS0_15InputStreamTypeE", "input stream, never open: none", int, h_is_Capacity, const void *g) { (void)g; return 0; }
HLE_FORCE("_ZN2nn5audio18GetInputStreamGainEPKNS0_15InputStreamTypeE", "input stream, never open: 1", float, h_is_GetGain, const void *g) { (void)g; return 1.0f; }
HLE_FORCE("_ZN2nn5audio18SetInputStreamGainEPNS0_15InputStreamTypeEfNS_8TimeSpanE", "input stream, never open: nothing to ramp", void, h_is_SetGain, void *g, float gain, int64_t ramp_ns) { (void)g; (void)gain; (void)ramp_ns; }

// Host-internal PCM output (used by the in-process audio renderer, see hle_audren.c): the same AudioOut machinery (real output / silent sink / background take-over), keyed by a host-allocated dummy object
void *dm_pcmout_open(int ch) {
    void *key = calloc(1, 64); uint8_t param[8] = {0}; *(uint16_t *)(param + 4) = (uint16_t)ch;
    if (au_open_at(key, NULL, "AudioRenderer", param, NULL)) die("host PCM output cannot be opened");
    ao(key)->rel_sem = dispatch_semaphore_create(0);
    return key;
}
// Wait until the output releases a block, at most ms milliseconds (spurious returns are fine: the caller checks dm_pcmout_released again)
void dm_pcmout_wait(void *k, int ms) { dispatch_semaphore_wait(ao(k)->rel_sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * 1000000)); }
void dm_pcmout_start(void *k) { au_start_at(k, NULL); }
void dm_pcmout_close(void *k) { au_close(k, NULL); free(k); }   // stop, take it out of the registry (frees its AOMAX slot), free it and its key
int dm_pcmout_append(void *k, void *b) { return (int)h_au_Append(k, b); }
void *dm_pcmout_released(void *k) { return h_au_Released(k); }

// Front-end lifecycle / audio session interruption: suspend real audio output, with the silent sink taking over real-time consumption meanwhile (pacing unbroken); switch back to the AudioUnit on resume.
// The front end calls with 1 on going to background / interruption began, and with 0 on returning to foreground / interruption ended (shouldResume).
void dm_audio_suspend(int on) {
    pthread_mutex_lock(&g_aos_lock);
    if (g_suspended != on) {
        g_suspended = on;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i];
            if (!a || !a->au) continue;
            // While suspended the silent sink consumes in real time instead: otherwise buffers are no longer released, the FMOD output thread waits forever and the game's audio pacing stalls (same for an incoming call interruption)
            if (on) {
                au_halt(a);
                if (a->backend == BK_NONE && atomic_load(&a->state) == STATE_STARTED) null_start(a);
            } else if (a->backend == BK_NULL) au_request_start(a);
        }
        LOG("audio %s", on ? "suspended (background / interruption)" : "resumed (foreground / interruption ended)");
    }
    pthread_mutex_unlock(&g_aos_lock);
}
// The media services were reset (iOS): every AudioUnit is invalid. Create each output's unit again and restart the ones that were playing; the front end has set the audio session up again first
void dm_audio_reset(void) {
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) {
        aout *a = g_aos[i]; if (!a || !a->au) continue;
        au_halt(a);
        if (a->backend == BK_NONE && atomic_load(&a->state) == STATE_STARTED) null_start(a);
        if (a->start_phase == AU_CANCELED) a->reset_pending = 1;
        else { au_replace(a); if (a->backend == BK_NULL) au_request_start(a); }
    }
    LOG("audio outputs set up again after the media services reset");
    pthread_mutex_unlock(&g_aos_lock);
}
