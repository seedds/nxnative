#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <stdatomic.h>
#include "dm.h"
#include "capture_apple.h"

struct dm_capture {
    struct dm_capture *next;
    AudioQueueRef queue;
    dispatch_queue_t callbacks;
    dm_capture_samples samples;
    void *context;
    uint32_t rate;
    int wanted;
    _Atomic int running;
};
static dispatch_queue_t capture_control;
static dm_capture *captures;
static int capture_suspended, capture_wanted;
static void capture_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ capture_control = dispatch_queue_create("org.nxnative.capture.control", DISPATCH_QUEUE_SERIAL); });
}
static int capture_session(void) {
#if TARGET_OS_IPHONE
    AVAudioSession *s = AVAudioSession.sharedInstance;
    NSError *error = nil;
    NSString *category = capture_wanted ? AVAudioSessionCategoryPlayAndRecord : AVAudioSessionCategoryPlayback;
    AVAudioSessionCategoryOptions options = capture_wanted ? AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothHFP : 0;
    if (![s setCategory:category mode:AVAudioSessionModeDefault options:options error:&error]) {
        LOG("[RED] audio session category failed: %s", error.localizedDescription.UTF8String); return 0;
    }
    [s setPreferredSampleRate:48000 error:nil];
    [s setPreferredIOBufferDuration:0.005 error:nil];
    if (![s setActive:YES error:&error]) {
        LOG("[RED] audio session activation failed: %s", error.localizedDescription.UTF8String); return 0;
    }
    LOG("audio session: %s, %.0f Hz, %.2f ms", capture_wanted ? "play and record" : "playback", s.sampleRate, s.IOBufferDuration * 1000);
#endif
    return 1;
}
int dm_capture_audio_session(void) {
    capture_init(); __block int ok;
    dispatch_sync(capture_control, ^{ ok = capture_session(); });
    return ok;
}
static int capture_permission(void) {
    AVAudioApplicationRecordPermission permission = AVAudioApplication.sharedInstance.recordPermission;
    if (permission == AVAudioApplicationRecordPermissionGranted) return 1;
    if (permission == AVAudioApplicationRecordPermissionDenied) return 0;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block BOOL granted = NO;
    [AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL allowed) {
        granted = allowed; dispatch_semaphore_signal(done);
    }];
    // Guest calls run off the UI thread. Also permit standalone host diagnostics
    // to ask from main while keeping the system permission dialog responsive.
    if (NSThread.isMainThread) {
        while (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW))
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    } else dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return granted;
}
static void capture_dispose(dm_capture *c) {
    atomic_store(&c->running, 0);
    if (c->queue) {
        // Synchronous disposal guarantees there can be no later buffer callback.
        AudioQueueDispose(c->queue, true); c->queue = NULL;
    }
}
static int capture_begin(dm_capture *c) {
    AudioStreamBasicDescription fmt = {.mSampleRate = c->rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 1, .mBitsPerChannel = 32};
    // AudioQueue converts the hardware's rate to the client format (e.g. a
    // 44.1 kHz Mac microphone or a Bluetooth route to a 48 kHz guest stream).
    OSStatus err = AudioQueueNewInputWithDispatchQueue(&c->queue, &fmt, 0, c->callbacks,
        ^(AudioQueueRef queue, AudioQueueBufferRef buffer, const AudioTimeStamp *time, UInt32 packets, const AudioStreamPacketDescription *descriptions) {
            (void)time; (void)packets; (void)descriptions;
            if (!atomic_load(&c->running)) return;
            c->samples(c->context, buffer->mAudioData, buffer->mAudioDataByteSize / sizeof(float));
            if (atomic_load(&c->running)) AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
        });
    for (int i = 0; !err && i < 3; i++) {
        AudioQueueBufferRef buffer;
        err = AudioQueueAllocateBuffer(c->queue, (c->rate / 100) * sizeof(float), &buffer);
        if (!err) err = AudioQueueEnqueueBuffer(c->queue, buffer, 0, NULL);
    }
    if (!err) { atomic_store(&c->running, 1); err = AudioQueueStart(c->queue, NULL); }
    if (err) { capture_dispose(c); LOG("[RED] microphone capture could not start (OSStatus %d)", (int)err); return 0; }
    LOG("microphone: default input, mono float at %u Hz", c->rate);
    return 1;
}
dm_capture *dm_capture_open(uint32_t rate, dm_capture_samples samples, void *context) {
    if (!samples || (rate != 16000 && rate != 32000 && rate != 44100 && rate != 48000)) return NULL;
    dm_capture *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->rate = rate; c->samples = samples; c->context = context;
    c->callbacks = dispatch_queue_create("org.nxnative.capture.samples", DISPATCH_QUEUE_SERIAL);
    capture_init();
    dispatch_sync(capture_control, ^{ c->next = captures; captures = c; });
    return c;
}
int dm_capture_start(dm_capture *c) {
    if (!c || !capture_permission()) { LOG("[RED] microphone permission denied; allow microphone access in system settings to record"); return 0; }
    __block int ok = 1;
    dispatch_sync(capture_control, ^{
        if (c->wanted) return;
        c->wanted = 1; capture_wanted++;
        if (!capture_suspended) ok = capture_session() && capture_begin(c);
        if (!ok) { c->wanted = 0; capture_wanted--; capture_session(); }
    });
    return ok;
}
void dm_capture_stop(dm_capture *c) {
    if (!c) return;
    dispatch_sync(capture_control, ^{
        capture_dispose(c);
        if (c->wanted) {
            c->wanted = 0; capture_wanted--;
            if (!capture_suspended) capture_session();
        }
    });
}
void dm_capture_close(dm_capture *c) {
    if (!c) return;
    dm_capture_stop(c);
    dispatch_sync(capture_control, ^{
        dm_capture **p = &captures;
        while (*p && *p != c) p = &(*p)->next;
        if (*p) *p = c->next;
    });
    c->callbacks = nil;
    free(c);
}
void dm_capture_suspend(int suspended) {
    capture_init();
    dispatch_sync(capture_control, ^{
        if (capture_suspended && suspended) return;
        if (!capture_suspended && !suspended) {
            // Activation may have failed while another app owned the session.
            // A later foreground/active notification can finish that resume.
            for (dm_capture *c = captures; c; c = c->next)
                if (c->wanted && !c->queue && capture_session()) capture_begin(c);
            return;
        }
        capture_suspended = !!suspended;
        if (!suspended && !capture_session()) return;
        for (dm_capture *c = captures; c; c = c->next) {
            if (suspended) capture_dispose(c);
            else if (c->wanted) capture_begin(c);
        }
    });
}
void dm_capture_reset(void) {
    capture_init();
    dispatch_sync(capture_control, ^{
        for (dm_capture *c = captures; c; c = c->next) capture_dispose(c);
        if (!capture_suspended && capture_session())
            for (dm_capture *c = captures; c; c = c->next) if (c->wanted) capture_begin(c);
    });
}
