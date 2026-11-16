// The Switch 2's video decoder API as the host serves it (hle_codec_video.c), without a guest: the buffer protocol, results, sync handles, sync point events and the
// readback, on synthetic red key frames: 16x16 VP9 and VP8 (the ones t_movie_vpx.c decodes), a 64x64 H.264 baseline frame (ffmpeg's libx264, SEI removed) and a 64x64 AV1 frame
// (SVT-AV1 through PyAV, preset 12, CRF 30); no game data. The SystemEvent functions are stubs that record each event's state and the thread that signalled it last.
// A submission returns with its packet queued for the decoder's thread: settle() waits for that thread before a check reads what the packet leads to; the checks made
// without it hold however far the thread has got.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_movie.c"
#include "../hle_codec_video.c"
// VideoToolbox's decode is wrapped so a test can play iOS invalidating a session in the background (kVTInvalidSessionErr): returned by the decode, or given to the callback.
// Its session creation is wrapped too, so a test can play iOS refusing a new hardware session while the app is still inactive (kVTVideoDecoderNotAvailableNowErr)
#include <VideoToolbox/VideoToolbox.h>
static OSStatus vt_decode(VTDecompressionSessionRef, CMSampleBufferRef, VTDecodeFrameFlags, void *, VTDecodeInfoFlags *);
static OSStatus vt_create(CFAllocatorRef, CMVideoFormatDescriptionRef, CFDictionaryRef, CFDictionaryRef, const VTDecompressionOutputCallbackRecord *, VTDecompressionSessionRef *);
#define VTDecompressionSessionDecodeFrame vt_decode
#define VTDecompressionSessionCreate vt_create
#include "../hle_movie_avc.c"
#include "../hle_movie_av1.c"
#undef VTDecompressionSessionDecodeFrame
#undef VTDecompressionSessionCreate
#include <assert.h>
#include <unistd.h>
static int vt_fail, vt_calls;   // vt_fail: 1 = the next decode returns kVTInvalidSessionErr, 2 = it gives that error to the callback and returns noErr (then 0 again)
static mavc *vt_avc; static mav1 *vt_av1;   // whose callback mode 2 calls
static OSStatus vt_decode(VTDecompressionSessionRef vs, CMSampleBufferRef sb, VTDecodeFrameFlags fl, void *src, VTDecodeInfoFlags *info) {
    vt_calls++;
    int f = vt_fail; vt_fail = 0;
    if (f == 1) return kVTInvalidSessionErr;
    if (f == 2) { if (vt_avc) on_frame(vt_avc, src, kVTInvalidSessionErr, 0, NULL, kCMTimeInvalid, kCMTimeInvalid); else av1_on_frame(vt_av1, src, kVTInvalidSessionErr, 0, NULL, kCMTimeInvalid, kCMTimeInvalid); return noErr; }
    return VTDecompressionSessionDecodeFrame(vs, sb, fl, src, info);
}
static OSStatus vt_create_fail; static int vt_creates;   // vt_create_fail: the next session creation returns it (then 0 again)
static OSStatus vt_create(CFAllocatorRef al, CMVideoFormatDescriptionRef fd, CFDictionaryRef spec, CFDictionaryRef attrs, const VTDecompressionOutputCallbackRecord *cb, VTDecompressionSessionRef *vs) {
    vt_creates++;
    OSStatus f = vt_create_fail; vt_create_fail = 0;
    if (f) { *vs = NULL; return f; }
    return VTDecompressionSessionCreate(al, fd, spec, attrs, cb, vs);
}
static char g_err[8192];   // what one call logged
#define LOGGED(stmt) do { fflush(stderr); int s_ = dup(2); FILE *t_ = tmpfile(); assert(s_ >= 0 && t_); dup2(fileno(t_), 2); stmt; fflush(stderr); dup2(s_, 2); close(s_); \
                          rewind(t_); size_t k_ = fread(g_err, 1, sizeof g_err - 1, t_); g_err[k_] = 0; fclose(t_); } while (0)


uint64_t now_ns(void) { return 1; }
__thread _Atomic int dm_host_work;   // hle_thread.c
void dm_host_helper_qos(qos_class_t q) { (void)q; }   // hle_thread.c: the decoder's thread keeps the default class in the unit test
static struct { void *ev; int bound, set; pthread_t by; } g_ev[16];   // by: the thread that signalled it last
static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;   // the decoder's thread signals and clears events too
static int ev_slot_locked(void *e) { for (int i = 0; i < 16; i++) if (g_ev[i].ev == e) return i; for (int i = 0; i < 16; i++) if (!g_ev[i].ev) { g_ev[i].ev = e; return i; } abort(); }
static int ev_slot(void *e) { pthread_mutex_lock(&g_ev_mu); int i = ev_slot_locked(e); pthread_mutex_unlock(&g_ev_mu); return i; }
void hle_bind_system_event(void *e, int autoclear) { (void)autoclear; pthread_mutex_lock(&g_ev_mu); g_ev[ev_slot_locked(e)].bound = 1; pthread_mutex_unlock(&g_ev_mu); }
void hle_signal_system_event(void *e) { pthread_mutex_lock(&g_ev_mu); int i = ev_slot_locked(e); g_ev[i].set = 1; g_ev[i].by = pthread_self(); pthread_mutex_unlock(&g_ev_mu); }
void hle_clear_system_event(void *e) { pthread_mutex_lock(&g_ev_mu); g_ev[ev_slot_locked(e)].set = 0; pthread_mutex_unlock(&g_ev_mu); }
static int ev_set(void *e) { pthread_mutex_lock(&g_ev_mu); int r = g_ev[ev_slot_locked(e)].set; pthread_mutex_unlock(&g_ev_mu); return r; }
static int ev_by_me(void *e) { pthread_mutex_lock(&g_ev_mu); int r = pthread_equal(g_ev[ev_slot_locked(e)].by, pthread_self()); pthread_mutex_unlock(&g_ev_mu); return r; }
static void destroy_ev(void *e) { pthread_mutex_lock(&g_ev_mu); int i = ev_slot_locked(e); g_ev[i].bound = g_ev[i].set = 0; pthread_mutex_unlock(&g_ev_mu); }   // nn::os::DestroySystemEvent: closing the handle unbinds the event
static void settle(void *t) {   // the decoder's thread has decoded every packet it can (all of them, or it waits for room for another picture)
    cv_dec *d = cv_of(t);
    for (;;) { pthread_mutex_lock(&d->mu); int idle = !d->busy && (!d->qcount || d->nheld >= CV_HOLD); pthread_mutex_unlock(&d->mu); if (idle) return; usleep(200); }
}
void *guest_sym(const char *n) { return !strcmp(n, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE") ? (void *)destroy_ev : NULL; }
static vpx_image_t *black_i420(unsigned w, unsigned h) {
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, w, h, 1); if (!c) return NULL;
    memset(c->planes[0], 16, (size_t)c->stride[0] * h);
    memset(c->planes[1], 128, (size_t)c->stride[1] * ((h + 1) / 2)); memset(c->planes[2], 128, (size_t)c->stride[2] * ((h + 1) / 2));
    return c;
}

static const uint8_t vp8[] = {
    0xf0,0x02,0x00,0x9d,0x01,0x2a,0x10,0x00,0x10,0x00,0x00,0x47,0x08,0x85,0x85,0x88,
    0x85,0x84,0x88,0x02,0x02,0x02,0x75,0xaa,0x03,0xf8,0x03,0xfa,0x02,0x0d,0x4d,0x18,
    0x00,0xfe,0xfd,0x6e,0xf3,0xff,0xe3,0x99,0x37,0x30,0xc4,0xff,0x8e,0x6d,0xff,0xf1,
    0x61,0x3c,0x0e,0x28,0xc8,0xff,0xf1,0x51,0x00,
};
static const uint8_t av1[] = {   // one temporal unit: temporal delimiter, sequence header (profile 0, 64x64), frame
    0x12,0x00,0x0a,0x0b,0x02,0x00,0x00,0x04,0x95,0x7f,0xfc,0x0a,0xf9,0x00,0x40,0x32,0x15,0x10,0x00,0xbc,0x00,0x49,0x22,0x08,0x08,0x21,0x10,0x00,0xad,0x3b,0x67,0x00,
    0x1e,0xb5,0xdb,0x79,0x52,0x90,
};
static const uint8_t h264[] = {   // Annex B: SPS, PPS, one IDR slice
    0x00,0x00,0x00,0x01,0x67,0x42,0xc0,0x0a,0xdc,0x42,0x6c,0x04,0x40,0x00,0x00,0x03,0x00,0x40,0x00,0x00,0x0c,0xa3,0xc4,0x89,
    0xe0,0x00,0x00,0x00,0x01,0x68,0xce,0x0f,0xc8,0x00,0x00,0x01,0x65,0x88,0x84,0x3a,0x11,0x8a,0x00,0x02,0x18,0xf1,0xc0,0x00,
    0x40,0xf6,0x38,0x00,0x08,0x79,0x49,0xc9,0xc9,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xe0,
};
static const uint8_t vp9[] = {
    0x82,0x49,0x83,0x42,0x00,0x00,0xf0,0x00,0xf6,0x00,0x38,0x24,0x1c,0x18,0x4a,0x00,
    0x00,0x30,0x60,0x00,0x00,0x10,0xbf,0xff,0xf7,0x1d,0xaf,0xff,0xff,0xff,0x5f,0xdf,
    0xff,0xff,0xff,0xf2,0x2a,0xc0,0x00,
};

// A VideoToolbox session iOS invalidated in the background (hle_movie_avc.c, hle_movie_av1.c; Ocarina of Time's opening movie after the notification panel): with movie_vt_restart the
// session is dropped and a new one starts at the next key frame. The units before it reach no decoder and show the last picture again at their own times; one plain line says
// so, no red one. Off, as before
static const uint8_t h264_p[] = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x00, 0x10};   // a non-IDR slice header alone: it must never reach a decoder (vt_fail fails it before)
static const uint8_t av1_inter[] = {0x12, 0x00, 0x32, 0x02, 0x30, 0x00};   // temporal delimiter, then a frame OBU whose header starts show_existing_frame 0, frame_type 1 (inter)
static int red_y(const uint8_t *y, size_t stride, int w, int h) { for (int r = 0; r < h; r++) for (int x = 0; x < w; x++) if (abs(y[(size_t)r * stride + x] - 81) > 4) return 0; return 1; }
static void vt_restart_avc(void) {
    static uint8_t nv12[64 * 64 * 3 / 2]; int64_t pts; int r, calls;
    setenv("DM_MOVIE_RESTART", "1", 1);
    mavc *a = mavc_new(); vt_avc = a;
    assert(!mavc_decode(a, h264, sizeof h264, 0) && a->vs && mavc_take(a, 0, nv12, 64, 64, &pts) == 1 && red_y(nv12, 64, 64, 64));
    // Pictures before the IDR show the last picture taken again, at their own times (STILL(t): the red frame at t, and nothing else queued)
    #define STILL(t) (mavc_queued(a) == 1 && mavc_take(a, 0, nv12, 64, 64, &pts) == 1 && pts == (t) && red_y(nv12, 64, 64, 64))
    vt_fail = 1; LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 1000));   // the session died while the app was away: a non-IDR packet finds it
    assert(!r && !a->vs && a->restart && a->restarts == 1 && STILL(1000));
    assert(strstr(g_err, "[movie] AVC: VideoToolbox's session was invalidated") && strstr(g_err, "at the next IDR picture") && !strstr(g_err, "[RED]"));
    calls = vt_calls; LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 2000));   // skipped until the IDR: no decoder, the still picture, nothing logged
    assert(!r && vt_calls == calls && !a->vs && STILL(2000) && !g_err[0]);
    assert(!mavc_decode(a, h264_p, sizeof h264_p, 1500) && !mavc_queued(a));   // a picture whose time is before one shown (a B picture): none, the times stay in order
    assert(!mavc_decode(a, h264, sizeof h264, 3000) && a->vs && !a->restart && vt_calls == calls + 1);   // the IDR: a new session from the parameter sets
    assert(mavc_take(a, 0, nv12, 64, 64, &pts) == 1 && pts == 3000 && red_y(nv12, 64, 64, 64));
    vt_fail = 1; LOGGED(r = mavc_decode(a, h264, sizeof h264, 4000));   // found at an IDR: a new session decodes that same picture
    assert(!r && a->vs && !a->restart && a->restarts == 2 && strstr(g_err, "at this IDR picture") && !strstr(g_err, "[RED]"));
    assert(mavc_take(a, 0, nv12, 64, 64, &pts) == 1 && pts == 4000 && red_y(nv12, 64, 64, 64));
    vt_fail = 2; LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 5000));   // the error in the callback, the decode returning noErr
    assert(!r && !a->vs && a->restart && !a->lost && a->restarts == 3 && !strstr(g_err, "[RED]") && STILL(5000));
    assert(!mavc_decode(a, h264, sizeof h264, 6000) && a->vs && mavc_take(a, 0, nv12, 64, 64, &pts) == 1 && pts == 6000);
    // iOS refuses the new session (the app still inactive, kVTVideoDecoderNotAvailableNowErr): still waiting for an IDR, so the P pictures after it reach no session that lacks their reference
    vt_fail = 1; LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 7000)); assert(!r && !a->vs && a->restart && a->restarts == 4 && STILL(7000));
    vt_create_fail = kVTVideoDecoderNotAvailableNowErr; calls = vt_creates; LOGGED(r = mavc_decode(a, h264, sizeof h264, 8000));
    assert(!r && vt_creates == calls + 1 && !a->vs && a->restart && STILL(8000) && strstr(g_err, "not available yet") && !strstr(g_err, "[RED]"));
    calls = vt_calls; LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 9000)); assert(!r && vt_calls == calls && !a->vs && a->restart && STILL(9000) && !g_err[0]);
    assert(!mavc_decode(a, h264, sizeof h264, 10000) && a->vs && !a->restart && mavc_take(a, 0, nv12, 64, 64, &pts) == 1 && pts == 10000 && red_y(nv12, 64, 64, 64));
    vt_fail = 1; vt_create_fail = kVTVideoDecoderNotAvailableNowErr; LOGGED(r = mavc_decode(a, h264, sizeof h264, 11000));   // found at an IDR, and the new session refused: the next IDR
    assert(!r && !a->vs && a->restart && a->restarts == 5 && !strstr(g_err, "[RED]") && STILL(11000));
    LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 12000)); assert(!r && !a->vs && a->restart && STILL(12000));
    assert(!mavc_decode(a, h264, sizeof h264, 13000) && a->vs && !a->restart && mavc_take(a, 0, nv12, 64, 64, &pts) == 1 && pts == 13000);
    vt_fail = 1; LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 14000)); assert(a->restart && STILL(14000));   // any other refusal: the old red line, and still the next IDR
    vt_create_fail = kVTVideoDecoderMalfunctionErr; LOGGED(r = mavc_decode(a, h264, sizeof h264, 15000));
    assert(r == -1 && !a->vs && a->restart && strstr(g_err, "[RED] movie: VideoToolbox cannot create an AVC decode session") && STILL(15000));
    LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 16000)); assert(!r && !a->vs && a->restart && STILL(16000));
    assert(!mavc_decode(a, h264, sizeof h264, 17000) && a->vs && !a->restart && mavc_take(a, 0, nv12, 64, 64, &pts) == 1 && pts == 17000);
    mavc_drop(a); assert(!a->last);   // a Flush or Stop: nothing of the old stream is shown again
    #undef STILL
    mavc_free(a);
    // Off (DM_MOVIE_RESTART=0, or a profile without movie_restart): as before, the packet fails with a red line and the session is kept
    setenv("DM_MOVIE_RESTART", "0", 1);
    a = mavc_new(); vt_avc = a;
    assert(!mavc_decode(a, h264, sizeof h264, 0) && mavc_take(a, 0, nv12, 64, 64, &pts) == 1);
    vt_fail = 1; LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 1000));
    assert(r == -1 && a->vs && !a->restart && !a->restarts && !a->last && !mavc_queued(a) && strstr(g_err, "[RED] movie: AVC decode submission failed (-12903"));
    vt_fail = 2; LOGGED(r = mavc_decode(a, h264_p, sizeof h264_p, 2000));
    assert(!r && a->vs && !a->lost && !a->restart);   // the callback's error is the old red one (already logged once for this decoder)
    mavc_free(a); vt_avc = NULL;
    unsetenv("DM_MOVIE_RESTART"); assert(movie_vt_restart() == DM_GAME_MOVIE_RESTART);   // without the variable: the profile's movie_restart
    setenv("DM_MOVIE_RESTART", "1", 1); assert(movie_vt_restart()); setenv("DM_MOVIE_RESTART", "0", 1); assert(!movie_vt_restart()); unsetenv("DM_MOVIE_RESTART");
}
static void vt_restart_av1(void) {
    // Key frames: a sequence header and a key frame; a sequence header beside an inter frame is none
    uint8_t seq_inter[32]; memcpy(seq_inter, av1, 15); memcpy(seq_inter + 15, av1_inter + 2, 4);
    assert(av1_key(av1, sizeof av1) && !av1_key(av1_inter, sizeof av1_inter) && !av1_key(seq_inter, 19) && !av1_key(av1, 2));
    if (!VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1)) { puts("AV1 session restart: skipped, this Mac has no hardware AV1 decoder"); return; }
    setenv("DM_MOVIE_RESTART", "1", 1); setenv("DM_AV1_DAV1D", "0", 1);
    mav1 *a = mav1_new(); vt_av1 = a; mav1_picture pic; int r, calls; int64_t t = 0;
    assert(mav1_decode(a, av1, sizeof av1, ++t, &pic) == 1 && pic.ts == t && red_y(pic.y, pic.ystride, 64, 64));
    // Units before the key frame show the last picture again (STILL: the red frame), for their own times
    #define STILL (r == 1 && pic.ts == t && red_y(pic.y, pic.ystride, 64, 64))   // the last picture again, with this unit's time
    vt_fail = 1; LOGGED(r = mav1_decode(a, av1_inter, sizeof av1_inter, ++t, &pic));
    assert(STILL && !a->vs && a->restart && a->restarts == 1 && !a->soft);
    assert(strstr(g_err, "[movie] AV1: VideoToolbox's session was invalidated") && strstr(g_err, "at the next key frame") && !strstr(g_err, "[RED]"));
    calls = vt_calls; LOGGED(r = mav1_decode(a, av1_inter, sizeof av1_inter, ++t, &pic)); assert(STILL && vt_calls == calls && !g_err[0]);
    LOGGED(r = mav1_decode(a, seq_inter, 19, ++t, &pic)); assert(STILL && vt_calls == calls && !a->vs && a->restart && !g_err[0]);   // a sequence header alone does not start it
    assert(mav1_decode(a, av1, sizeof av1, ++t, &pic) == 1 && a->vs && !a->restart && red_y(pic.y, pic.ystride, 64, 64));
    vt_fail = 1; LOGGED(r = mav1_decode(a, av1, sizeof av1, ++t, &pic));   // found at a key frame: a new session decodes it
    assert(r == 1 && a->vs && !a->restart && a->restarts == 2 && strstr(g_err, "at this key frame") && !strstr(g_err, "[RED]") && red_y(pic.y, pic.ystride, 64, 64));
    vt_fail = 2; LOGGED(r = mav1_decode(a, av1_inter, sizeof av1_inter, ++t, &pic));   // the error in the callback
    assert(STILL && !a->vs && a->restart && !a->lost && a->restarts == 3 && !strstr(g_err, "[RED]"));
    // iOS refuses the new session (the app still inactive, kVTVideoDecoderNotAvailableNowErr): still waiting for a key frame on VideoToolbox, not dav1d for the rest of the movie
    vt_create_fail = kVTVideoDecoderNotAvailableNowErr; calls = vt_creates; LOGGED(r = mav1_decode(a, av1, sizeof av1, ++t, &pic));
    assert(STILL && vt_creates == calls + 1 && !a->vs && a->restart && !a->soft && strstr(g_err, "not available yet") && !strstr(g_err, "[RED]"));
    calls = vt_calls; LOGGED(r = mav1_decode(a, av1_inter, sizeof av1_inter, ++t, &pic)); assert(STILL && vt_calls == calls && !a->vs && a->restart && !g_err[0]);
    assert(mav1_decode(a, av1, sizeof av1, ++t, &pic) == 1 && a->vs && !a->restart && !a->soft && red_y(pic.y, pic.ystride, 64, 64));
    vt_fail = 1; vt_create_fail = kVTVideoDecoderNotAvailableNowErr; LOGGED(r = mav1_decode(a, av1, sizeof av1, ++t, &pic));   // found at a key frame, and the new session refused
    assert(STILL && !a->vs && a->restart && !a->soft && a->restarts == 4 && !strstr(g_err, "[RED]"));
    assert(mav1_decode(a, av1, sizeof av1, ++t, &pic) == 1 && a->vs && !a->restart && !a->soft);
    #undef STILL
    mav1_reset(a); assert(!a->restart && !a->last);   // a new stream (Stop / Flush) starts as before, with no picture to show again
    mav1_free(a);
    // Off: as before
    setenv("DM_MOVIE_RESTART", "0", 1);
    a = mav1_new(); vt_av1 = a;
    assert(mav1_decode(a, av1, sizeof av1, ++t, &pic) == 1);
    vt_fail = 1; LOGGED(r = mav1_decode(a, av1_inter, sizeof av1_inter, ++t, &pic));
    assert(r == -1 && a->vs && !a->restart && !a->restarts && !a->last && strstr(g_err, "[RED] movie: AV1 decode submission failed (-12903"));
    mav1_free(a); vt_av1 = NULL;
    unsetenv("DM_MOVIE_RESTART"); unsetenv("DM_AV1_DAV1D");
}
int main(void) {
    _Alignas(16) static uint8_t dec[0x400], pool[3][0x40], spt[0x80], mem[16 * 16 * 4];
    uint64_t size, align, id; uint32_t idx; cv_sync sync; uint16_t w, h, cw, ch; int64_t ts; bool eos;

    // Pools: one picture each, NV12 (format 3, as Bananza asks)
    cv_pref pref = h_cv_pref(16, 16, 3, 0);
    assert(!h_cv_poolsize(&size, &align, &pref, 1) && size >= 16 * 16 * 4 && align == CV_NATIVE_ALIGN);
    for (int i = 0; i < 3; i++) assert(!h_cv_pool(pool[i], mem, size, &pref, 1, 0) && pool_of(pool[i]));

    // Decoder: the submission-ready event is bound in its type and signalled
    assert(!h_cv_vp9init2(dec, 0x10000, 16, 16, 1, 1, 3, NULL, 0));
    cv_dec *d = cv_of(dec); assert(d && d->vpx_ok);
    void *ev = NULL; assert(!h_cv_vp9event(&ev, dec) && ev == dec + CV_EVENT_OFF && g_ev[ev_slot(ev)].bound && ev_set(ev));

    // Registering makes a buffer acquirable at once, its sync handle signalled; an empty queue answers 3604
    for (uint32_t i = 0; i < 2; i++) assert(!h_cv_vp9reg(dec, i, pool[i], 0));
    for (uint32_t i = 0; i < 2; i++) { assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == i && sync.magic == CV_SYNC && sync.gen == 0 && h_gs_trywait(&sync)); }
    assert(h_cv_vp9acq(&idx, &sync, dec) == CV_NONE);
    assert(h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) == CV_NONE);

    // Released: acquirable again with a new handle that signals once a picture is in the buffer; the picture's properties and its NV12 / RGBA readback
    h_cv_vp9rel(dec, 0, sync);
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 0 && sync.gen == 1 && !h_gs_trywait(&sync));
    assert(h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) == CV_NONE);
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 1000, false) && id == 1);
    settle(dec); assert(h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) && w == 16 && h == 16 && cw == 16 && ch == 16 && ts == 1000 && !eos);
    uint32_t cs = 7; w = h = 0;   // SDK 19's form: the same properties, and colour space 0
    assert(!h_cv_vp9props19(&w, &h, &cw, &ch, &cs, &ts, &eos, dec, 0) && w == 16 && h == 16 && ts == 1000 && !eos && cs == 0);
    uint8_t nv12[16 * 16 * 3 / 2], rgba[16 * 16 * 4];
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 16, 16, nv12, 16, 256, nv12 + 256, 16, 128));
    for (int i = 0; i < 256; i++) assert(abs(nv12[i] - 81) <= 3);
    for (int i = 256; i < 384; i += 2) { assert(abs(nv12[i] - 90) <= 3); assert(abs(nv12[i + 1] - 240) <= 3); }
    uint8_t y[256], uv[128];   // separate planes
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 16, 16, y, 16, 256, uv, 16, 128) && !memcmp(y, nv12, 256) && !memcmp(uv, nv12 + 256, 128));
    assert(!h_cv_rgba(pool[0], 0, 0, 0, 16, 16, rgba, 64, sizeof rgba));
    for (int i = 0; i < 1024; i += 4) assert(rgba[i] >= 250 && rgba[i + 1] <= 5 && rgba[i + 2] <= 5 && rgba[i + 3] == 255);

    // No released buffer: pictures wait; the event goes down at CV_READY, with room left for a client's signal in flight, and packets are refused (3603) at CV_HOLD.
    // A packet not yet decoded counts as the picture it will give: the event and the refusal are the same whether the decoder's thread has got to it or not
    for (int i = 0; i < CV_READY; i++) assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 2000 + i, false));
    assert(!ev_set(ev));
    settle(dec); assert(d->nheld == CV_READY && !d->qcount && !ev_set(ev));
    for (int i = CV_READY; i < CV_HOLD; i++) assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 2000 + i, false));
    assert(h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 9999, false) == CV_BUSY && !ev_set(ev));
    settle(dec); assert(d->nheld == CV_HOLD && !d->qcount && !ev_set(ev));
    assert(h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 9999, false) == CV_BUSY);
    // A release takes the oldest; the event comes back below CV_READY
    for (int i = CV_HOLD; i > CV_READY; i--) { h_cv_vp9rel(dec, 1, sync); assert(d->nheld == i - 1 && !ev_set(ev)); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1); }
    h_cv_vp9rel(dec, 1, sync);
    assert(d->nheld == CV_READY - 1 && ev_set(ev));
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1 && h_gs_trywait(&sync));
    // Start signals it again: a client may drop the signal Initialize gave
    hle_clear_system_event(ev); assert(!ev_set(ev));
    assert(!h_cv_vp9start(dec) && ev_set(ev));
    // A release below CV_READY with no packet taken signals nothing: a client counts each signal as one packet it may send. A packet taken does
    hle_clear_system_event(ev);
    h_cv_vp9rel(dec, 1, sync); assert(d->nheld == CV_READY - 2 && !ev_set(ev));
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1);
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 2000 + CV_HOLD, false) && ev_set(ev) && ev_by_me(ev));   // signalled by the submission itself, before the packet is decoded
    settle(dec); assert(d->nheld == CV_READY - 1 && ev_set(ev));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 2000 + CV_HOLD - CV_READY + 1);

    // Sync point event: armed on an unfilled buffer, signalled when the picture lands; one armed on a filled buffer is signalled at once
    assert(!h_spt_create(spt) && g_ev[ev_slot(spt)].bound);
    h_cv_vp9rel(dec, 0, sync);   // buffer 0 takes the next waiting picture at once
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 0);
    assert(!h_spt_request(spt, &sync) && ev_set(spt) && !SPT(spt)->armed);
    assert(!h_cv_vp9reg(dec, 2, pool[2], 0) && !h_cv_vp9acq(&idx, &sync, dec) && idx == 2);
    for (int i = 0; i < 2; i++) { h_cv_vp9rel(dec, (uint32_t)i, sync); assert(!h_cv_vp9acq(&idx, &sync, dec)); }   // the last two waiting pictures go out
    h_cv_vp9rel(dec, 2, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 2 && !h_gs_trywait(&sync));
    assert(!h_spt_request(spt, &sync) && !ev_set(spt) && SPT(spt)->armed);
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 3000, false));
    settle(dec); assert(ev_set(spt) && !SPT(spt)->armed && h_gs_trywait(&sync));
    assert(!ev_by_me(spt));   // the picture came from the decoder's thread, not from inside the submission

    // End of stream with no data: the next released buffer carries it, with no picture
    assert(!h_cv_vp9submit(&id, dec, NULL, 0, 0, true));
    settle(dec); assert(d->eos_pending);
    h_cv_vp9rel(dec, 2, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 2 && h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 2) && eos && w == 0 && h == 0);

    // A pool finalized while its buffer is still registered: the buffer keeps working, its picture has nowhere to go (ASan checks no freed pool is written)
    h_cv_poolfin(pool[1]); assert(!pool_of(pool[1]) && d->buf[1].pool == NULL);
    h_cv_vp9rel(dec, 1, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1 && !h_gs_trywait(&sync));
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 4000, false));
    settle(dec); assert(h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 4000 && w == 16);
    // A flush drops the waiting pictures and the packets not yet decoded, wherever the decoder's thread is with them, and restarts the decoder: none of them reaches a buffer
    // released afterwards, the next key frame does; finalize frees the decoder and destroys its event
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < 6; i++) assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 5000 + i, false));
        if (round & 1) usleep(100u * (unsigned)round);   // some rounds catch the thread inside a packet, some before or after
        assert(!h_cv_vp9flush(dec) && d->nheld == 0 && !d->qcount && ev_set(ev));
        settle(dec); assert(d->nheld == 0 && !d->eos_pending);
        h_cv_vp9rel(dec, 1, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1 && !h_gs_trywait(&sync));
        assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 6000 + round, false));
        settle(dec); assert(d->vpx_ok && !d->reopen && h_gs_trywait(&sync));
        assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 6000 + round && w == 16 && d->nheld == 0);
    }
    // The frame step's movie signal (hle_movie.c dm_movie_hz): times microseconds apart give no rate, and nn::codec does not count a stream without one; two pictures
    // 1/60 s apart after a gap of a second (the rate is taken anew) make a 60-picture movie, a flush (a seek, a loop) keeps it and a stop ends it at once
    assert(dm_movie_hz() == 0);
    for (int k = 0; k < 2; k++) {
        h_cv_vp9rel(dec, 1, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1);
        assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 1000000000 + k * 16666667, false));
        settle(dec); assert(h_gs_trywait(&sync));
    }
    assert(dm_movie_hz() == 60);
    assert(!h_cv_vp9flush(dec) && dm_movie_hz() == 60);
    assert(!h_cv_vp9stop(dec) && dm_movie_hz() == 0);
    assert(!h_cv_vp9fin(dec) && !cv_of(dec) && !g_ev[ev_slot(ev)].bound);
    // A decoder initialised again in the same memory, cleared in between (Donkey Kong Bananza's second movie): its event is bound anew.
    // Finalized with packets it has not decoded: its thread ends first, and they go with it
    memset(dec, 0, sizeof dec);
    assert(!h_cv_vp9init2(dec, 0x10000, 16, 16, 1, 1, 3, NULL, 0) && cv_of(dec) && g_ev[ev_slot(ev)].bound && ev_set(ev));
    for (int i = 0; i < 6; i++) assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 7000 + i, false));
    assert(!h_cv_vp9fin(dec) && !cv_of(dec) && !g_ev[ev_slot(ev)].bound);
    for (int i = 0; i < 3; i += 2) { h_cv_poolfin(pool[i]); assert(!pool_of(pool[i])); }
    // VP8 (libvpx) and H.264 (VideoToolbox) take the same protocol: one released buffer, one key frame, its picture read back red
    {   _Alignas(16) static uint8_t d8[0x400], p8[0x40], m8[16 * 16 * 4];
        assert(!h_cv_poolsize(&size, &align, &pref, 1) && !h_cv_pool(p8, m8, size, &pref, 1, 0));
        assert(!h_cv_vp8init2(d8, 0x10000, 16, 16, 1, 1, 3, NULL, 0) && cv_of(d8) && cv_of(d8)->codec == CV_VP8 && cv_of(d8)->vpx_ok);
        assert(!h_cv_vp8reg(d8, 0, p8, 0) && !h_cv_vp8acq(&idx, &sync, d8) && idx == 0);
        h_cv_vp8rel(d8, 0, sync); assert(!h_cv_vp8acq(&idx, &sync, d8) && !h_gs_trywait(&sync));
        assert(!h_cv_vp8submit(&id, d8, vp8, sizeof vp8, 7000, false));
        settle(d8); assert(h_gs_trywait(&sync));
        assert(!h_cv_vp8props(&w, &h, &cw, &ch, &ts, &eos, d8, 0) && w == 16 && h == 16 && ts == 7000);
        assert(!h_cv_nv12(p8, 0, 0, 0, 16, 16, nv12, 16, 256, nv12 + 256, 16, 128));
        for (int i = 0; i < 256; i++) assert(abs(nv12[i] - 81) <= 3);
        for (int i = 256; i < 384; i += 2) { assert(abs(nv12[i] - 90) <= 4); assert(abs(nv12[i + 1] - 240) <= 4); }
        // Unregistered: no longer acquirable or filled
        h_cv_vp8rel(d8, 0, sync); assert(!h_cv_vp8unreg(d8, 0) && h_cv_vp8acq(&idx, &sync, d8) == CV_NONE && !cv_of(d8)->buf[0].reg);
        assert(!h_cv_vp8fin(d8) && !cv_of(d8)); h_cv_poolfin(p8);
    }
    {   _Alignas(16) static uint8_t dh[0x400], ph[0x40], mh[64 * 64 * 4]; static uint8_t yuv[64 * 64 * 3 / 2];
        cv_pref p64 = h_cv_pref(64, 64, 3, 0);
        assert(!h_cv_poolsize(&size, &align, &p64, 1) && !h_cv_pool(ph, mh, size, &p64, 1, 0));
        assert(!h_cv_h264init2(dh, 0x10000, 64, 64, 1, 1, 3, NULL, 0) && cv_of(dh) && cv_of(dh)->codec == CV_H264 && cv_of(dh)->avc);
        assert(!h_cv_h264reg(dh, 0, ph, 0) && !h_cv_h264acq(&idx, &sync, dh));
        h_cv_h264rel(dh, 0, sync); assert(!h_cv_h264acq(&idx, &sync, dh) && !h_gs_trywait(&sync));
        assert(!h_cv_h264submit(&id, dh, h264, sizeof h264, 5000000, true));   // end of stream: VideoToolbox gives its picture out
        settle(dh); assert(h_gs_trywait(&sync));
        assert(!h_cv_h264props(&w, &h, &cw, &ch, &ts, &eos, dh, 0) && w == 64 && h == 64 && ts == 5000000 && eos);
        assert(!h_cv_nv12(ph, 0, 0, 0, 64, 64, yuv, 64, 4096, yuv + 4096, 64, 2048));
        for (int i = 0; i < 4096; i++) assert(abs(yuv[i] - 81) <= 4);
        for (int i = 4096; i < 6144; i += 2) { assert(abs(yuv[i] - 90) <= 6); assert(abs(yuv[i + 1] - 240) <= 6); }
        assert(!h_cv_h264fin(dh) && !cv_of(dh)); h_cv_poolfin(ph);
    }
    {   // AV1: a decoder of its own codec through SDK 22's entry point, whose last two arguments lie on the stack (the guest's 8-byte slots), and a packet with no sequence header yet gives no picture
        uint8_t da[0x400]; memset(da, 0, sizeof da);
        assert(!h_cv_av1tm(da, NULL, 0, 0x10000, 0xabcd0080, 0xabcd0040, 1, 3, 2, 2) && cv_of(da) && cv_of(da)->codec == CV_AV1 && cv_of(da)->w == 0x80 && cv_of(da)->h == 0x40);
        assert(h_cv_av1ev22(da) == da + CV_EVENT_OFF && ev_set(da + CV_EVENT_OFF));
        static const uint8_t delimiter[] = {0x12, 0x00};   // a temporal delimiter OBU alone
        uint64_t n; assert(!h_cv_av1submit(&n, da, delimiter, sizeof delimiter, 0, false));
        settle(da); assert(!cv_of(da)->nheld);
        // Key frames sent without waiting for their pictures, the last with the end of stream: every picture waits with its own packet's time, in order, and the last one
        // carries the end of stream, whichever backend decodes and however many frames it holds at once
        for (int k = 0; k < 7; k++) assert(!h_cv_av1submit(&n, da, av1, sizeof av1, 9000 + k, k == 6));
        settle(da); assert(cv_of(da)->nheld == 7 && !cv_of(da)->eos_pending);
        for (int k = 0; k < 7; k++) assert(cv_of(da)->held_ts[k] == 9000 + k && cv_of(da)->held_eos[k] == (k == 6) && cv_of(da)->held[k]->d_w == 64);
        assert(!h_cv_av1fin(da) && !cv_of(da));
        // The av1C record from a sequence header: profile 0, one operating point at level 12 (tier 0), 2560x1440, 8-bit 4:2:0 with chroma position 0, written field by field
        uint8_t obu[32] = {0x0a, 0}; size_t bit = 16;
        #define PUT(v, nb) do { for (int k_ = (nb) - 1; k_ >= 0; k_--, bit++) if (((uint32_t)(v) >> k_) & 1) obu[bit / 8] |= (uint8_t)(0x80 >> (bit % 8)); } while (0)
        PUT(0, 3); PUT(0, 1); PUT(0, 1);            // profile, still picture, reduced header
        PUT(0, 1); PUT(0, 1); PUT(0, 5);            // no timing info, no display delay, one operating point
        PUT(0, 12); PUT(12, 5); PUT(0, 1);          // idc, level 12, tier
        PUT(11, 4); PUT(10, 4); PUT(2559, 12); PUT(1439, 11);   // frame size bits and frame size
        PUT(0, 1); PUT(0, 3); PUT(0, 4);            // no frame ids, block tools, inter tools
        PUT(1, 1); PUT(0, 2);                       // order hint, jnt_comp and ref_frame_mvs
        PUT(1, 1); PUT(1, 1);                       // screen content tools: choose; integer mv: choose
        PUT(6, 3); PUT(0, 3);                       // order hint bits, superres / cdef / restoration
        PUT(0, 1); PUT(0, 1); PUT(0, 1);            // not high bit depth, not monochrome, no colour description
        PUT(0, 1); PUT(0, 2); PUT(0, 1);            // colour range, chroma position, separate delta q
        #undef PUT
        obu[1] = (uint8_t)((bit + 7) / 8 - 2);
        uint8_t c[4]; int aw = 0, ah = 0;
        assert(mav1_av1c(obu, (bit + 7) / 8, c, &aw, &ah) && aw == 2560 && ah == 1440);
        assert(c[0] == 0x81 && c[1] == 12 && c[2] == 0x0c && c[3] == 0);
        size_t seqn = 0; assert(find_seq(obu, (bit + 7) / 8, &seqn) == obu && seqn == (bit + 7) / 8);
        assert(!mav1_av1c(delimiter, sizeof delimiter, c, &aw, &ah) && !find_seq(delimiter, sizeof delimiter, &seqn));
        // The red frame on each backend this Mac has (VideoToolbox where the chip decodes AV1, and dav1d, which DM_AV1_DAV1D=1 picks), as NV12: Y 81, Cb 90, Cr 240 (BT.601, video range)
        for (int soft = VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1) ? 0 : 1; soft < 2; soft++) {
            setenv("DM_AV1_DAV1D", soft ? "1" : "0", 1);
            mav1 *a = mav1_new(); assert(!strcmp(mav1_backend(a), soft ? "dav1d" : "VideoToolbox"));
            // dav1d decodes several frames at once: a unit's picture comes with that unit's time, at once or when the frames in flight are asked for (mav1_next's drain).
            // VideoToolbox gives each unit's picture as it takes the unit
            mav1_picture pic; int got = mav1_decode(a, av1, sizeof av1, 4242, &pic);
            if (!got) { assert(!strcmp(mav1_backend(a), "dav1d")); got = mav1_next(a, 1, &pic); }   // dav1d also on the VideoToolbox pass when the session cannot be made (a VM: -12906)
            assert(got == 1 && pic.ts == 4242 && pic.w == 64 && pic.h == 64 && pic.matrix == 1 && !pic.full_range);   // no matrix in the stream: BT.601 at 64 lines
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) assert(abs(pic.y[(size_t)y * pic.ystride + x] - 81) <= 4);
            for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x += 2) { assert(abs(pic.uv[(size_t)y * pic.uvstride + x] - 90) <= 6); assert(abs(pic.uv[(size_t)y * pic.uvstride + x + 1] - 240) <= 6); }
            assert(mav1_next(a, 1, &pic) == 0);   // nothing more inside
            // Key frames one after the other, with no picture asked for in between beyond what each call gives: every unit's picture comes out once, in order, with its time
            int64_t want = 100;
            for (int k = 0; k < 9; k++) {
                for (int r = mav1_decode(a, av1, sizeof av1, 100 + k, &pic); r == 1; r = mav1_next(a, 0, &pic)) { assert(pic.ts == want && pic.w == 64); want++; }
                if (k == 4) while (mav1_next(a, 1, &pic) == 1) { assert(pic.ts == want); want++; }   // drained in the middle of the stream: it goes on after
            }
            while (mav1_next(a, 1, &pic) == 1) { assert(pic.ts == want && abs(pic.y[0] - 81) <= 4); want++; }
            assert(want == 109);
            // Drains at any point of the stream, each to its end, with one, two or three frames in flight: dav1d goes on after each (left halfway, a drain breaks it: dav1d_step)
            uint32_t rng = 12345; int64_t sent = 1000; want = 1000;
            for (int k = 0; k < 600; k++) {
                for (int r = mav1_decode(a, av1, sizeof av1, sent++, &pic); r == 1; r = mav1_next(a, 0, &pic)) { assert(pic.ts == want); want++; }
                rng = rng * 1664525u + 1013904223u;
                if ((rng >> 28) < 5) { while (mav1_next(a, 1, &pic) == 1) { assert(pic.ts == want); want++; } assert(want == sent); }
            }
            while (mav1_next(a, 1, &pic) == 1) { assert(pic.ts == want); want++; }
            assert(want == sent);
            mav1_reset(a); assert(mav1_decode(a, delimiter, sizeof delimiter, 0, &pic) == 0 && mav1_next(a, 1, &pic) == 0);   // a new stream: nothing until its sequence header
            mav1_free(a);
        }
        unsetenv("DM_AV1_DAV1D");
    }
    {   // A native image: SDK 22's preference and pool, the handle names the pool's memory, and a picture lands there as RGBA in NVN's linear layout
        cv_pref p = h_cv_pref22(0xffff0020, 0xffff0010, 2, 2); assert(p.w == 32 && p.h == 16);
        uint64_t sz, al; assert(!h_cv_poolsize(&sz, &al, &p, 2) && al == CV_NATIVE_ALIGN && sz == 2 * CV_NATIVE_ALIGN);
        uint8_t pt[0x40] = {0}; void *m; assert(!posix_memalign(&m, al, sz)); memset(m, 0, sz);
        assert(!h_cv_pool22(pt, m, sz, &p, 2));
        uint32_t ha = h_cv_native(pt, 1), nw, nh, ns; uint8_t *nm = NULL; uint64_t nz;
        assert(ha && dm_native_image(ha, &nm, &nz, &nw, &nh, &ns) && nm == (uint8_t *)m + CV_NATIVE_ALIGN && nz == CV_NATIVE_ALIGN && nw == 32 && nh == 16 && ns == 128);
        vpx_image_t *im = black_i420(32, 16); im->range = VPX_CR_STUDIO_RANGE;
        pool_native_write(pool_of(pt), 1, im); vpx_img_free(im);
        assert(nm[0] == 0 && nm[3] == 255 && nm[ns * 15 + 31 * 4 + 3] == 255);
        h_cv_poolfin(pt); assert(!dm_native_image(ha, &nm, &nz, &nw, &nh, &ns)); free(m);
    }
    {   // SDK 19's Initialize by remote memory (two sizes before the picture size) and its preference: a VP9 decoder as the other forms make
        _Alignas(16) static uint8_t d19[0x400];
        cv_pref p19 = h_cv_pref19(0xffff0000u | 64, 32, 3, 1);   // the 16-bit sizes come unextended
        assert(p19.w == 64 && p19.h == 32 && p19.fmt == 3 && p19.magic == CV_MAGIC);
        assert(!h_cv_vp9rm19(d19, 0x10000, 0x200000, 0xabcd0000u | 16, 16, 1, 1, 3) && cv_of(d19) && cv_of(d19)->w == 16);
        assert(!h_cv_vp9fin(d19));
    }
    {   // Blank native images (GameChat's camera pictures with no camera): zeroed storage with room for 16 bytes a pixel, apart from the decoder pools' handles, gone once freed
        uint32_t a = dm_native_image_blank(384, 216), b = dm_native_image_blank(384, 216), nw, nh, ns; uint8_t *nm = NULL; uint64_t nz;
        assert(a && b && a != b && a >> 31 && b >> 31);
        assert(dm_native_image(a, &nm, &nz, &nw, &nh, &ns) && nw == 384 && nh == 216 && ns >= 384 * 16 && ns % 64 == 0 && nz >= (uint64_t)ns * 216);
        for (uint64_t k = 0; k < nz; k++) assert(!nm[k]);
        dm_native_image_blank_free(a); assert(!dm_native_image(a, &nm, &nz, &nw, &nh, &ns)); assert(dm_native_image(b, &nm, &nz, &nw, &nh, &ns));
        dm_native_image_blank_free(b); dm_native_image_blank_free(b); dm_native_image_blank_free(0x100);   // twice, and a decoder pool's handle: nothing happens
        uint32_t k[BLANK_MAX + 1]; for (int i = 0; i < BLANK_MAX; i++) assert((k[i] = dm_native_image_blank(32, 32)));
        assert(!(k[BLANK_MAX] = dm_native_image_blank(32, 32)));   // full
        for (int i = 0; i < BLANK_MAX; i++) dm_native_image_blank_free(k[i]);
    }
    vt_restart_avc(); vt_restart_av1();
    puts("VideoToolbox session restart: an invalidated session is dropped, the units before the next key frame skipped, one plain line; off, the red line as before");
    printf("t_codec_video: all passed\n");
    return 0;
}
