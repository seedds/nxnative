// The system renderer (audio_sbar.h): AVSampleBufferAudioRenderer on an AVSampleBufferRenderSynchronizer, fed 32-bit float interleaved PCM with an explicit channel layout,
// one CMSampleBuffer per block at a running presentation timestamp. With multichannel content (the game's 5.1 as MPEG_5_1_A) macOS offers its Spatial Audio for it on AirPods
// and other headphones (the Sound menu's Off / Fixed / Head Tracked, the listener's personalized profile), with no entitlement (WWDC21 10265 "Immerse your app in spatial audio"; nxnative issue #2650: TotK on AirPods
// Pro 3 through this same path). The renderer follows the default output itself (audioOutputDeviceUniqueID nil); it flushes itself when that changes, which the pump sees as
// SBAR_FLUSHED and answers with a new pre-roll. Mac host only (the _mac name keeps it out of the iOS build, dm/ios/tools/gen_build.py). Every call comes from one thread at a
// time: the output's pump thread, or the host under its audio registry lock once the pump has stopped
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_sbar.h"

struct dm_sbar {
    void *renderer, *sync, *observer;   // AVSampleBufferAudioRenderer, AVSampleBufferRenderSynchronizer, the flush observer (retained through CFBridgingRetain)
    CMAudioFormatDescriptionRef format;
    int ch; double rate;
    int64_t pts;                        // frames enqueued since the last stop: the next buffer's timestamp
    _Atomic int flushed;                // the renderer flushed itself (the output device changed)
};
#define R(s) ((__bridge AVSampleBufferAudioRenderer *)(s)->renderer)
#define S(s) ((__bridge AVSampleBufferRenderSynchronizer *)(s)->sync)

int dm_sbar_available(void) { return NSClassFromString(@"AVSampleBufferAudioRenderer") != nil && NSClassFromString(@"AVSampleBufferRenderSynchronizer") != nil; }

dm_sbar *dm_sbar_create(int ch, double rate, char *why, size_t whyn) {
    @autoreleasepool {
        if (!dm_sbar_available()) { snprintf(why, whyn, "no AVSampleBufferAudioRenderer"); return NULL; }
        if (ch != 6 && ch != 2) { snprintf(why, whyn, "%d channels (6 or 2)", ch); return NULL; }
        dm_sbar *s = calloc(1, sizeof *s); if (!s) { snprintf(why, whyn, "out of memory"); return NULL; }
        s->ch = ch; s->rate = rate;
        AudioStreamBasicDescription f = {.mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                         .mBytesPerPacket = 4u * (UInt32)ch, .mFramesPerPacket = 1, .mBytesPerFrame = 4u * (UInt32)ch, .mChannelsPerFrame = (UInt32)ch, .mBitsPerChannel = 32};
        // The guest's order (front-left front-right center LFE back-left back-right) is MPEG_5_1_A: the layout is what makes it multichannel content to spatialize
        AudioChannelLayout l; memset(&l, 0, sizeof l);   // a tag alone: no bitmap, no channel descriptions
        l.mChannelLayoutTag = ch == 6 ? kAudioChannelLayoutTag_MPEG_5_1_A : kAudioChannelLayoutTag_Stereo;
        OSStatus st = CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &f, sizeof l, &l, 0, NULL, NULL, &s->format);
        if (st) { snprintf(why, whyn, "CMAudioFormatDescriptionCreate status %d", (int)st); free(s); return NULL; }
        AVSampleBufferAudioRenderer *r = [[AVSampleBufferAudioRenderer alloc] init];
        AVSampleBufferRenderSynchronizer *y = [[AVSampleBufferRenderSynchronizer alloc] init];
        if (!r || !y) { snprintf(why, whyn, "the renderer or synchronizer could not be made"); CFRelease(s->format); free(s); return NULL; }
        // Multichannel is the default; stereo too, should a game ever send two (the system's "Spatialize Stereo")
        r.allowedAudioSpatializationFormats = AVAudioSpatializationFormatMonoStereoAndMultichannel;
        [y addRenderer:r];
        dm_sbar *sp = s;
        id obs = [[NSNotificationCenter defaultCenter] addObserverForName:AVSampleBufferAudioRendererWasFlushedAutomaticallyNotification object:r queue:nil
                                                               usingBlock:^(NSNotification *n) { (void)n; atomic_store(&sp->flushed, 1); }];
        s->renderer = (void *)CFBridgingRetain(r); s->sync = (void *)CFBridgingRetain(y); s->observer = (void *)CFBridgingRetain(obs);
        return s;
    }
}

int dm_sbar_enqueue(dm_sbar *s, const float *frames, int n) {
    size_t bytes = (size_t)n * (size_t)s->ch * sizeof(float);
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, bytes, kCFAllocatorDefault, NULL, 0, bytes, kCMBlockBufferAssureMemoryNowFlag, &bb);
    if (!st) st = CMBlockBufferReplaceDataBytes(frames, bb, 0, bytes);
    if (!st) st = CMAudioSampleBufferCreateReadyWithPacketDescriptions(kCFAllocatorDefault, bb, s->format, n, CMTimeMake(s->pts, (int32_t)s->rate), NULL, &sb);
    if (!st) { [R(s) enqueueSampleBuffer:sb]; s->pts += n; }
    if (sb) CFRelease(sb);
    if (bb) CFRelease(bb);
    return (int)st;
}

double dm_sbar_lead(dm_sbar *s) {
    CMTime now = CMTimebaseGetTime(S(s).timebase);
    double t = CMTIME_IS_NUMERIC(now) ? CMTimeGetSeconds(now) : 0;
    return (double)s->pts / s->rate - (t > 0 ? t : 0);
}

int dm_sbar_play(dm_sbar *s) {
    @autoreleasepool { [S(s) setRate:1.0 time:CMTimeMake(0, (int32_t)s->rate)]; }   // the timeline starts at the first buffer: everything enqueued is the lead
    return R(s).status == AVQueuedSampleBufferRenderingStatusFailed;
}

void dm_sbar_stop(dm_sbar *s) {
    // The clock back to 0 as well as stopped: setRate:0 alone leaves it at the elapsed time, so the next pre-roll would read a lead of minus that and enqueue that much silence
    @autoreleasepool { [S(s) setRate:0.0 time:kCMTimeZero]; [R(s) flush]; }
    s->pts = 0; atomic_store(&s->flushed, 0);
}

int dm_sbar_status(dm_sbar *s, char *why, size_t whyn) {
    @autoreleasepool {
        if (R(s).status == AVQueuedSampleBufferRenderingStatusFailed) {
            NSError *e = R(s).error;
            snprintf(why, whyn, "%s (%ld)", e ? e.localizedDescription.UTF8String : "no error given", e ? (long)e.code : 0L);
            return SBAR_FAILED;
        }
        return atomic_exchange(&s->flushed, 0) ? SBAR_FLUSHED : SBAR_OK;
    }
}

void dm_sbar_destroy(dm_sbar *s) {
    if (!s) return;
    @autoreleasepool {
        id obs = CFBridgingRelease(s->observer);
        if (obs) [[NSNotificationCenter defaultCenter] removeObserver:obs];
        AVSampleBufferAudioRenderer *r = CFBridgingRelease(s->renderer);
        AVSampleBufferRenderSynchronizer *y = CFBridgingRelease(s->sync);
        [y setRate:0.0]; [r flush];
        [y removeRenderer:r atTime:kCMTimeZero completionHandler:nil];
    }
    CFRelease(s->format);
    free(s);
}

int dm_sbar_describe(dm_sbar *s, int *ch, uint32_t *layout_tag, double *rate, int *is_float) {
    const AudioStreamBasicDescription *f = CMAudioFormatDescriptionGetStreamBasicDescription(s->format);
    size_t n = 0; const AudioChannelLayout *l = CMAudioFormatDescriptionGetChannelLayout(s->format, &n);
    if (!f) return -1;
    *ch = (int)f->mChannelsPerFrame; *rate = f->mSampleRate; *is_float = (f->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    *layout_tag = l ? l->mChannelLayoutTag : 0;
    return 0;
}

void dm_sbar_test_set_time(dm_sbar *s, double t) { @autoreleasepool { [S(s) setRate:0.0 time:CMTimeMakeWithSeconds(t, (int32_t)s->rate)]; } }
