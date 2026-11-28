// VideoToolbox reference state is private to its session. Keep the compressed
// input from a random-access picture so a replacement can reconstruct that
// state without skipping pictures or returning substitutes to the player.
#pragma once
#include <VideoToolbox/VideoToolbox.h>

// Owned by a decoder's worker owner, so Stop / Flush / Finalize can cancel a
// wait without touching codec state that is private to the worker thread.
typedef struct movie_vt_control { int cancelled; } movie_vt_control;
void movie_vt_cancel(movie_vt_control *control);
void movie_vt_continue(movie_vt_control *control);
int movie_vt_wait(movie_vt_control *control);
int movie_vt_wait_unavailable(movie_vt_control *control);
enum { MOVIE_VT_CANCELLED = 1 };

typedef struct {
    CMSampleBufferRef *samples;
    size_t count, capacity;
    int key, replaying;
    OSStatus status;
    movie_vt_control *control;
} movie_vt_history;

static void movie_vt_clear(movie_vt_history *h) {
    for (size_t i = 0; i < h->count; i++) CFRelease(h->samples[i]);
    free(h->samples); h->samples = NULL; h->count = h->capacity = 0; h->key = 0;
}
static void movie_vt_remember(movie_vt_history *h, CMSampleBufferRef sample) {
    if (!h->key) return;
    if (h->count == h->capacity) {
        size_t capacity = h->capacity ? h->capacity * 2 : 16;
        CMSampleBufferRef *samples = realloc(h->samples, capacity * sizeof *samples);
        if (!samples) die("movie: compressed VideoToolbox history allocation failed");
        h->samples = samples; h->capacity = capacity;
    }
    h->samples[h->count++] = (CMSampleBufferRef)CFRetain(sample);
}
static OSStatus movie_vt_submit(movie_vt_history *h, VTDecompressionSessionRef session, CMSampleBufferRef sample, void *source) {
    if (h->replaying && !movie_vt_wait(h->control)) return MOVIE_VT_CANCELLED;
    h->status = noErr;
    OSStatus status = VTDecompressionSessionDecodeFrame(session, sample, h->replaying ? kVTDecodeFrame_DoNotOutputFrame : 0, source, NULL);
    // Neither asynchronous nor temporal processing is enabled: the callback's
    // status is available before DecodeFrame returns (also during replay).
    return status ? status : h->status;
}
static OSStatus movie_vt_create(movie_vt_history *h, CMVideoFormatDescriptionRef format, CFDictionaryRef attributes,
                                const VTDecompressionOutputCallbackRecord *callback, VTDecompressionSessionRef *session) {
    for (;;) {
        if (!movie_vt_wait(h->control)) return MOVIE_VT_CANCELLED;
        OSStatus status = VTDecompressionSessionCreate(NULL, format, NULL, attributes, callback, session);
        if (status != kVTVideoDecoderNotAvailableNowErr) return status;
        int resumed = movie_vt_wait_unavailable(h->control);
        if (resumed < 0) return MOVIE_VT_CANCELLED;
        if (!resumed) return status;   // an active app's creation failure, not a lifecycle wait
    }
}
static OSStatus movie_vt_replay(movie_vt_history *h, VTDecompressionSessionRef session) {
    OSStatus status = noErr; h->replaying = 1;
    for (size_t i = 0; i < h->count && !status; i++) status = movie_vt_submit(h, session, h->samples[i], NULL);
    h->replaying = 0;
    return status;
}
