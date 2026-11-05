// Mac spatial audio (DM_AUDIO_SPATIAL, hle_audio.c): a six-channel output goes to macOS as 5.1 through AVSampleBufferAudioRenderer, the route Apple's own players use, so the system spatializes it
// for AirPods / headphones / built-in speakers and Control Center's Spatial Audio control (off, fixed, head tracked) applies to it; with spatial audio off there the system downmixes the 5.1 itself.
// AUSpatialMixer inside the host rendered the same bed audibly harsher (Fire Emblem Fortune's Weave A/B on AirPods Pro 2), and the system menu showed spatial audio as unavailable.
//
// The renderer is fed, not pulled: a thread of its own, under the time-constraint (real-time) policy like Core Audio's IO threads, wakes every 2 ms and keeps the samples queued up to the synchronizer clock + the output device's reported delay + a margin, pulling the output's queue in 5 ms blocks.
// The clock is what is heard, so a block queued less than the device's delay ahead is dropped without any error (AirPods Pro 2 report 170.7 ms; everything fed 100 ms ahead was silent):
// the delay is read from the default output device every 250 ms, on a utility-QoS timer: the query took 170-290 ms right after the renderer started (and 0.3-1 ms later,
// still over the feeder's real-time budget), which made a feed late. Only the margin (DM_AUDIO_SPATIAL_LEAD_MS, default 50, which Fixed mode needs, hle_audio.c) is added to what the AudioUnit route already has; a 5 ms
// margin played without gaps in a standalone test (no game running). A feed that fell behind (the process was stopped, a device change) starts again at the clock + delay + margin and is counted as late.
// The built-in speakers play later than they report (MacBook Pro M4 Pro, macOS 27.2, a standalone test fed a set margin over the reported 22.1 ms: 50 ms silent, 80 crackled, 90 and 100
// clean; in Final Fantasy Resonance at 50 nothing played, with no error), so a built-in output's delay counts SYS_BUILTIN_MS more: 100 ms over what it reports with the default margin. Why is not known
// When the default output changes (AirPods connected mid-game: 71.7 → 170.7 ms), the blocks already queued are too close for the new output; the system flushed the renderer
// only about 140 ms after the delay re-read saw the change, and the game crackled for a second or two meanwhile. So the delay is also re-read as soon as the default output changes
// (a property listener), and a delay that moved by more than SYS_JUMP_MS restarts the renderer at once, as a system flush does
// When the system flushes the renderer itself (a route change) or reports that the output's configuration no longer matches the queued format (Control Center's Spatial Audio mode
// changed, another app reconfigured the device), the feeder flushes, as the SDK asks, and restarts the clock from where it stood with the first block (a flush with the clock running on
// also left it silent); until then the renderer stayed silent (Fortune's Weave, Head Tracked → Fixed). Each is logged
// On a dispatch timer at user-interactive QoS it fell 20+ ms behind twice while Fortune's Weave was loading (clicks); the AudioUnit route's callback runs on a real-time thread.
// The feeder counts each block it pulls through the callback, so the watchdog in hle_audio.c sees a renderer whose clock stopped as a device that stopped calling back.
// The system holds the clock for a while on its own: about 0.5 s at some starts (standalone test: the clock moved 1.5 s in its first 2 s) and after it flushed the renderer when
// Control Center's mode changed (Fortune's Weave: each time over the watchdog's 200 ms, so it stopped and restarted the renderer, a cut of a second or more, then the clock from 0).
// While the clock holds, the feeder pulls the output's queue at real time anyway and drops those blocks (nothing would play them; the game's audio keeps its pace, the watchdog sees
// callbacks), for at most 2 s: a clock held longer is taken as a dead renderer, the feeder stops pulling and the watchdog restarts it. Each hold that dropped audio is logged.
// The first enqueue after a start blocks for about 120 ms while the renderer starts its output (also when primed with a block beforehand); with the clock already running that
// was a late feed at every start, so the feeder starts the clock itself, once the first block is queued
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreMedia/CoreMedia.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "dm.h"
#include "audio_sys_mac.h"

enum { SYS_RATE = 48000, SYS_CH = 6, SYS_BLOCK = 240, SYS_TICK_US = 2000, SYS_DELAY_MS = 250 };   // 5 ms blocks, a 2 ms tick, the device delay re-read every 250 ms
enum { SYS_HOLD_MS = 20, SYS_HOLD_MAX_MS = 2000 };   // a clock that has not moved for 20 ms is held: drop the game's blocks at real time, for at most 2 s
enum { SYS_BUILTIN_MS = 50 };   // more delay than a built-in output reports (see the top)
enum { SYS_JUMP_MS = 20 };      // a delay change larger than this restarts the renderer (see the top); AirPods move by 5 ms on their own

@interface DmSysOut : NSObject {
@public
    AVSampleBufferAudioRenderer *ren;
    AVSampleBufferRenderSynchronizer *sync;
    CMAudioFormatDescriptionRef fmt;
    pthread_t th; pthread_mutex_t m; pthread_cond_t cv;   // the feeder thread; m serializes its ticks with start / stop; cv wakes it from idle
    int running, started, quit, thread_ok;   // started: the feeder has queued the first block and started the clock
    id flushed_obs, config_obs;
    dm_sys_pull_fn pull; void *ctx; int id;
    int64_t next;          // timeline position (frames) of the next block to queue
    int64_t base;          // where the clock starts when the feeder starts it: 0 at a Start, where it stood at a restart (the timeline stays monotonic)
    int delay, margin;     // frames: the device's reported delay, the margin on top
    _Atomic int dev_delay;          // the device's delay (frames), kept current by delay_timer
    dispatch_source_t delay_timer;
    AudioObjectPropertyListenerBlock dev_listener;   // the default output changed: re-read the delay at once
    _Atomic int resync;    // the renderer flushed itself or the output was reconfigured: stop the clock, flush, start again as a Start does
    _Atomic uint64_t late;
    int64_t last_clock;    // the clock on the previous tick (-1 before the first)
    uint64_t held_ns, dropped;   // when the clock was first seen not moving (0 = moving), blocks pulled and dropped since
    int16_t scrap[SYS_BLOCK * SYS_CH];
}
@end
@implementation DmSysOut
- (void)dealloc { if (fmt) CFRelease(fmt); pthread_mutex_destroy(&m); pthread_cond_destroy(&cv); }
@end

static UInt32 prop_u32(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = {sel, sc, kAudioObjectPropertyElementMain}; UInt32 v = 0, n = sizeof v;
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &n, &v) ? 0 : v;
}
// The default output device's delay in 48 kHz frames: device and stream latency, safety offset, IO buffer, and SYS_BUILTIN_MS for a built-in output (*builtin set)
static int device_delay(int *builtin) {
    *builtin = 0;
    AudioObjectID dev = 0; UInt32 n = sizeof dev;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &n, &dev) || !dev) return 0;
    Float64 rate = SYS_RATE; n = sizeof rate; a.mSelector = kAudioDevicePropertyNominalSampleRate;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &n, &rate) || !(rate > 0)) rate = SYS_RATE;
    UInt32 frames = prop_u32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) + prop_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput)
                  + prop_u32(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    AudioStreamID st = 0; n = sizeof st; a.mSelector = kAudioDevicePropertyStreams; a.mScope = kAudioObjectPropertyScopeOutput;
    if (!AudioObjectGetPropertyData(dev, &a, 0, NULL, &n, &st) && n >= sizeof st) frames += prop_u32(st, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
    *builtin = prop_u32(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) == kAudioDeviceTransportTypeBuiltIn;
    return (int)(frames * (SYS_RATE / rate) + 0.5) + (*builtin ? SYS_BUILTIN_MS * SYS_RATE / 1000 : 0);
}

static int64_t clock_frames(DmSysOut *o) { return CMTimeConvertScale(o->sync.currentTime, SYS_RATE, kCMTimeRoundingMethod_RoundTowardNegativeInfinity).value; }

static int queue_block(DmSysOut *o) {
    size_t bytes = (size_t)SYS_BLOCK * SYS_CH * sizeof(int16_t);
    CMBlockBufferRef bb = NULL;
    if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, bytes, NULL, NULL, 0, bytes, kCMBlockBufferAssureMemoryNowFlag, &bb)) return 0;
    void *p = NULL;
    if (CMBlockBufferGetDataPointer(bb, 0, NULL, NULL, (char **)&p) || !p) { CFRelease(bb); return 0; }
    o->pull(o->ctx, p, SYS_BLOCK);
    CMSampleBufferRef sb = NULL;
    OSStatus st = CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, bb, o->fmt, SYS_BLOCK, CMTimeMake(o->next, SYS_RATE), NULL, &sb);
    CFRelease(bb);
    if (st) return 0;
    [o->ren enqueueSampleBuffer:sb];
    CFRelease(sb);
    o->next += SYS_BLOCK;
    return 1;
}

// The clock held (see the top): 1 when this tick's blocks were pulled and dropped instead of queued
static int held(DmSysOut *o, int64_t now) {
    uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now != o->last_clock) {
        if (o->dropped) LOG("audio #%d system spatial: the system held the clock for %.0f ms: %llu ms of the game's audio dropped meanwhile", o->id, (t - o->held_ns) / 1e6,
                            (unsigned long long)(o->dropped * SYS_BLOCK * 1000 / SYS_RATE));
        o->last_clock = now; o->held_ns = 0; o->dropped = 0;
        return 0;
    }
    if (!o->held_ns) { o->held_ns = t; return 0; }
    uint64_t for_ns = t - o->held_ns;
    if (for_ns < SYS_HOLD_MS * 1000000ull) return 0;
    if (for_ns > SYS_HOLD_MAX_MS * 1000000ull) return 1;   // a dead renderer: nothing pulled, the watchdog takes over
    uint64_t due = for_ns * SYS_RATE / 1000000000ull / SYS_BLOCK;   // real time since the clock last moved
    for (; o->dropped < due; o->dropped++) o->pull(o->ctx, o->scrap, SYS_BLOCK);
    return 1;
}

static void tick(DmSysOut *o) {
    o->delay = atomic_load_explicit(&o->dev_delay, memory_order_relaxed);
    if (!o->started) {   // the first block, then the clock (see the top)
        queue_block(o);
        [o->sync setRate:1 time:CMTimeMake(o->base, SYS_RATE)];
        o->started = 1;
        return;
    }
    if (atomic_exchange_explicit(&o->resync, 0, memory_order_relaxed)) {
        // A full restart, as a Start does: clock stopped, flushed (serialized with the enqueues: this thread), the first block, then the clock again from where it stood (setting it to 0 right after the stop left the old time readable and the clock at half speed). Flushing and queueing
        // on from the running clock left the renderer silent after a Control Center mode change (Fortune's Weave, Fixed: a little crackle, then nothing, with every block
        // fed on time); the watchdog's stop and start after the same flushes had recovered every time
        int64_t at = clock_frames(o);
        o->sync.rate = 0;
        [o->ren flush];
        o->base = at; o->next = at + o->delay + o->margin; o->started = 0; o->last_clock = -1; o->held_ns = 0; o->dropped = 0;
        return;
    }
    int64_t now = clock_frames(o), want = now + o->delay + o->margin;
    if (held(o, now)) return;
    if (o->next < now + o->delay) {
        if (atomic_fetch_add_explicit(&o->late, 1, memory_order_relaxed) < 10)   // the first ten in full, the rest only counted (stats line)
            LOG("audio #%d system spatial: late feed: clock %.3f s, queued to %.3f s, %.1f ms short of the device delay %.1f ms", o->id, (double)now / SYS_RATE,
                (double)o->next / SYS_RATE, (now + o->delay - o->next) * 1000.0 / SYS_RATE, o->delay * 1000.0 / SYS_RATE);
        o->next = want;   // what would have been late is not queued: start again a margin ahead
    }
    while (o->next < want && o->ren.readyForMoreMediaData) if (!queue_block(o)) break;
}

// The feeder: idle on cv while stopped, a tick every 2 ms while running (late wake-ups are not made up in a burst)
static void *feeder(void *p) {
    DmSysOut *o = (__bridge DmSysOut *)p;   // dispose joins this thread before it releases the object
    pthread_setname_np("dm.audio.system");
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t period = (uint64_t)SYS_TICK_US * 1000 * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {.period = (uint32_t)period, .computation = (uint32_t)(period / 4), .constraint = (uint32_t)period, .preemptible = 1};
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) LOG("[RED] audio #%d system spatial: the feeder thread did not get the real-time policy (%d)", o->id, kr);
    uint64_t t = mach_absolute_time();
    pthread_mutex_lock(&o->m);
    while (!o->quit) {
        if (!o->running) { pthread_cond_wait(&o->cv, &o->m); t = mach_absolute_time(); continue; }
        @autoreleasepool { tick(o); }
        pthread_mutex_unlock(&o->m);
        t += period; uint64_t now = mach_absolute_time();
        if (t < now) t = now;
        mach_wait_until(t);
        pthread_mutex_lock(&o->m);
    }
    pthread_mutex_unlock(&o->m);
    return NULL;
}

static void remove_listener(DmSysOut *o) {
    if (!o->dev_listener) return;
    AudioObjectPropertyAddress da = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &da, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), o->dev_listener);
    o->dev_listener = nil;
}

// Re-reads the default output's delay (the delay timer, the default output listener); a jump restarts the renderer (see the top)
static void delay_update(DmSysOut *s, const char *why) {
    int builtin, d = device_delay(&builtin), was = atomic_exchange_explicit(&s->dev_delay, d, memory_order_relaxed);
    if (d == was) return;
    int jump = abs(d - was) > SYS_JUMP_MS * SYS_RATE / 1000;
    if (jump) atomic_store_explicit(&s->resync, 1, memory_order_relaxed);
    LOG("audio #%d system spatial: output delay %.1f → %.1f ms%s%s%s", s->id, was * 1000.0 / SYS_RATE, d * 1000.0 / SYS_RATE, why,
        builtin ? " (a built-in output: part of it not reported)" : "", jump ? ": restarting it" : "");
}

void *dm_sys_audio_create(int id, dm_sys_pull_fn pull, void *ctx, int margin_ms, char *err, size_t errn) {
    DmSysOut *o = [DmSysOut new];
    o->pull = pull; o->ctx = ctx; o->id = id; o->margin = margin_ms * SYS_RATE / 1000;
    AudioStreamBasicDescription f = {.mSampleRate = SYS_RATE, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
                                     .mBytesPerPacket = SYS_CH * 2, .mFramesPerPacket = 1, .mBytesPerFrame = SYS_CH * 2, .mChannelsPerFrame = SYS_CH, .mBitsPerChannel = 16};
    AudioChannelLayout layout;   // zeroed, not brace-initialized: mChannelBitmap (unused with a layout tag) is an enum with no zero enumerator
    memset(&layout, 0, sizeof layout);
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A;   // L R C LFE Ls Rs, the Switch's order
    OSStatus st = CMAudioFormatDescriptionCreate(NULL, &f, sizeof layout, &layout, 0, NULL, NULL, &o->fmt);
    if (st) { snprintf(err, errn, "cannot describe the 5.1 format (status %d)", (int)st); return NULL; }
    o->ren = [AVSampleBufferAudioRenderer new];
    o->sync = [AVSampleBufferRenderSynchronizer new];
    if (!o->ren || !o->sync) { snprintf(err, errn, "cannot make the renderer"); return NULL; }
    o->ren.allowedAudioSpatializationFormats = AVAudioSpatializationFormatMultichannel;
    o->sync.delaysRateChangeUntilHasSufficientMediaData = NO;   // the feeder paces itself on the clock: the clock has to run before anything is queued
    [o->sync addRenderer:o->ren];
    __weak DmSysOut *w = o;
    // Delivered on an arbitrary thread: only a flag for the feeder, and the log line
    o->flushed_obs = [NSNotificationCenter.defaultCenter addObserverForName:AVSampleBufferAudioRendererWasFlushedAutomaticallyNotification object:o->ren queue:nil usingBlock:^(NSNotification *n) {
        DmSysOut *s = w; if (!s) return;
        atomic_store_explicit(&s->resync, 1, memory_order_relaxed);
        NSValue *t = n.userInfo[AVSampleBufferAudioRendererFlushTimeKey];
        LOG("audio #%d system spatial: the system flushed the renderer (from %.3f s, clock %.3f s): restarting it", s->id,
            t ? CMTimeGetSeconds(t.CMTimeValue) : -1.0, CMTimeGetSeconds(s->sync.currentTime));
    }];
    o->config_obs = [NSNotificationCenter.defaultCenter addObserverForName:AVSampleBufferAudioRendererOutputConfigurationDidChangeNotification object:o->ren queue:nil usingBlock:^(NSNotification *n) {
        (void)n; DmSysOut *s = w; if (!s) return;
        atomic_store_explicit(&s->resync, 1, memory_order_relaxed);
        LOG("audio #%d system spatial: the output's configuration changed (clock %.3f s): flushing and restarting it", s->id, CMTimeGetSeconds(s->sync.currentTime));
    }];
    int builtin;
    atomic_init(&o->dev_delay, device_delay(&builtin));
    if (builtin) LOG("audio #%d system spatial: a built-in output: its delay counts %d ms more than it reports", o->id, (int)SYS_BUILTIN_MS);
    o->delay_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(o->delay_timer, dispatch_time(DISPATCH_TIME_NOW, SYS_DELAY_MS * NSEC_PER_MSEC), SYS_DELAY_MS * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(o->delay_timer, ^{ DmSysOut *s = w; if (s) delay_update(s, ""); });
    dispatch_resume(o->delay_timer);
    o->dev_listener = ^(UInt32 n, const AudioObjectPropertyAddress *addrs) { (void)n; (void)addrs; DmSysOut *s = w; if (s) delay_update(s, ", the default output changed"); };
    AudioObjectPropertyAddress da = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &da, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), o->dev_listener)) o->dev_listener = nil;   // the timer still catches it
    pthread_mutex_init(&o->m, NULL); pthread_cond_init(&o->cv, NULL);
    void *h = (void *)CFBridgingRetain(o);
    if (pthread_create(&o->th, NULL, feeder, h)) {
        snprintf(err, errn, "cannot start the feeder thread");
        [NSNotificationCenter.defaultCenter removeObserver:o->flushed_obs]; [NSNotificationCenter.defaultCenter removeObserver:o->config_obs];
        dispatch_source_cancel(o->delay_timer);
        remove_listener(o);
        CFBridgingRelease(h); return NULL;
    }
    o->thread_ok = 1;
    return h;
}

int dm_sys_audio_start(void *h, double *delay_ms) {
    DmSysOut *o = (__bridge DmSysOut *)h;
    int failed = 0;
    pthread_mutex_lock(&o->m);
    if (!o->running) {
        if (o->ren.status == AVQueuedSampleBufferRenderingStatusFailed) failed = 1;
        else {
            o->delay = atomic_load_explicit(&o->dev_delay, memory_order_relaxed); o->started = 0; o->last_clock = -1; o->held_ns = 0; o->dropped = 0;
            atomic_store_explicit(&o->resync, 0, memory_order_relaxed);
            [o->ren flush];
            o->base = 0; o->next = o->delay + o->margin;   // the first block goes on the timeline where it is due once the clock starts: no burst pulled from the game's queue
            o->running = 1; pthread_cond_signal(&o->cv);
        }
    }
    pthread_mutex_unlock(&o->m);
    if (delay_ms) *delay_ms = o->delay * 1000.0 / SYS_RATE;
    return failed ? -1 : 0;
}

// Returns once the feeder is idle: the caller's silent sink may consume the queue right after
void dm_sys_audio_stop(void *h) {
    DmSysOut *o = (__bridge DmSysOut *)h;
    pthread_mutex_lock(&o->m);   // taken between ticks: the feeder is idle once it is held
    if (o->running) { o->running = 0; o->sync.rate = 0; [o->ren flush]; }
    pthread_mutex_unlock(&o->m);
}

void dm_sys_audio_post_flush(void *h) {
    DmSysOut *o = (__bridge DmSysOut *)h;
    [NSNotificationCenter.defaultCenter postNotificationName:AVSampleBufferAudioRendererWasFlushedAutomaticallyNotification object:o->ren userInfo:nil];
}

void dm_sys_audio_hold_clock(void *h) { DmSysOut *o = (__bridge DmSysOut *)h; pthread_mutex_lock(&o->m); o->sync.rate = 0; pthread_mutex_unlock(&o->m); }

uint64_t dm_sys_audio_late(void *h) { DmSysOut *o = (__bridge DmSysOut *)h; return atomic_load_explicit(&o->late, memory_order_relaxed); }

void dm_sys_audio_dispose(void *h) {
    if (!h) return;
    dm_sys_audio_stop(h);
    DmSysOut *b = (__bridge DmSysOut *)h;
    pthread_mutex_lock(&b->m); b->quit = 1; pthread_cond_signal(&b->cv); pthread_mutex_unlock(&b->m);
    if (b->thread_ok) pthread_join(b->th, NULL);
    DmSysOut *o = (DmSysOut *)CFBridgingRelease(h);
    dispatch_source_cancel(o->delay_timer);
    remove_listener(o);
    [NSNotificationCenter.defaultCenter removeObserver:o->flushed_obs];
    [NSNotificationCenter.defaultCenter removeObserver:o->config_obs];
    [o->sync removeRenderer:o->ren atTime:kCMTimeInvalid completionHandler:nil];
}
#endif
