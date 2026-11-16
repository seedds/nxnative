// AV1 for the nn::codec decoders (hle_codec_video.c): Fire Emblem Fortune's Weave's 82 movies are AV1 in WebM (2560x1440). Two backends behind one interface: Apple's VideoToolbox, whose AV1
// decoder is hardware only (Apple M3 and A17 Pro on), and dav1d (dm/third_party/dav1d) on older chips, for a stream VideoToolbox refuses, or with DM_AV1_DAV1D=1 on any chip. The movie module
// aborts on a decoder that does not initialize (movie::detail::InitializeDecoderByTransferMemory, subsdk2+0xf9b4), so without one a game with AV1 movies could not run there.
// VideoToolbox wants a format description holding the stream's av1C record (ISO-BMFF AV1 binding, 2.3.3): four bytes read from the sequence header, then that header's OBU. The decoder API takes no
// configuration for AV1, so the sequence header comes from the stream, where every key frame carries one (AV1 5.5); a changed one makes a new session. A temporal unit goes in whole, its OBUs in
// the low-overhead format WebM and the SDK's demuxer keep them in (each with its size). Decoding is synchronous and a temporal unit shows at most one frame, in presentation order, so no queue.
// dav1d decodes several frames at once (frame threads), so its pictures come out later than their units go in, each with its unit's time (mav1_next)
#include "hle.h"
#include <VideoToolbox/VideoToolbox.h>
#include "dav1d/dav1d.h"
#include "hle_movie_av1.h"

struct mav1 {
    uint8_t seq[512]; size_t nseq;
    CMVideoFormatDescriptionRef fd; VTDecompressionSessionRef vs;
    CVPixelBufferRef got, locked;
    int soft;                     // dav1d decodes: no hardware decoder, DM_AV1_DAV1D=1, or a stream VideoToolbox refused (until mav1_reset)
    Dav1dContext *dav1d; Dav1dPicture pic; int has_pic;
    Dav1dData in;                 // the temporal unit dav1d has not taken whole yet
    int draining;                 // a drain gave a picture and has not reached its end (dav1d_step)
    uint8_t *y8, *uv; size_t ny8, nuv;   // dav1d's picture as NV12: the chroma interleaved, and the luma of a 10- or 12-bit stream shifted to 8 bits
    int red;
    int lost, restart, restarts;   // VideoToolbox's session invalidated by iOS (the app in the background; movie_vt_restart): lost in the callback, restart while waiting for a key frame
    CVPixelBufferRef last;         // movie_vt_restart: the last picture VideoToolbox gave (retained), shown again for each unit while waiting for a key frame
};
static int hw_av1(void) { const char *e = getenv("DM_AV1_DAV1D"); return !(e && *e && *e != '0') && VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1); }
mav1 *mav1_new(void) { mav1 *a = calloc(1, sizeof *a); if (!a) die("movie: AV1 decoder allocation failed"); a->soft = !hw_av1(); return a; }
const char *mav1_backend(const mav1 *a) { return a->soft ? "dav1d" : "VideoToolbox"; }
static void unlock_picture(mav1 *a) {
    if (a->locked) { CVPixelBufferUnlockBaseAddress(a->locked, kCVPixelBufferLock_ReadOnly); CVPixelBufferRelease(a->locked); a->locked = NULL; }
    if (a->has_pic) { dav1d_picture_unref(&a->pic); a->has_pic = 0; }
}
static void av1_close(mav1 *a) {
    if (a->vs) { VTDecompressionSessionWaitForAsynchronousFrames(a->vs); VTDecompressionSessionInvalidate(a->vs); CFRelease(a->vs); a->vs = NULL; }
    if (a->fd) { CFRelease(a->fd); a->fd = NULL; }
    if (a->got) { CVPixelBufferRelease(a->got); a->got = NULL; }
}
void mav1_reset(mav1 *a) { unlock_picture(a); av1_close(a); if (a->last) { CVPixelBufferRelease(a->last); a->last = NULL; } a->nseq = 0; a->lost = a->restart = 0; if (a->in.sz) dav1d_data_unref(&a->in); if (a->dav1d) dav1d_flush(a->dav1d); a->draining = 0; a->soft = !hw_av1(); }
void mav1_free(mav1 *a) { if (a) { mav1_reset(a); if (a->dav1d) dav1d_close(&a->dav1d); free(a->y8); free(a->uv); free(a); } }

// ---- The sequence header (AV1 5.5), read as far as the colour config ----
typedef struct { const uint8_t *p; size_t n, bit; int bad; } av1_bits;
static uint32_t rb(av1_bits *b, unsigned n) {
    uint32_t v = 0;
    if (b->bit + n > b->n * 8) { b->bad = 1; return 0; }
    while (n--) { v = v << 1 | ((b->p[b->bit / 8] >> (7 - b->bit % 8)) & 1); b->bit++; }
    return v;
}
static void uvlc(av1_bits *b) { unsigned z = 0; while (!b->bad && !rb(b, 1)) if (++z >= 32) return; if (z < 32) rb(b, z); }
static size_t leb128(const uint8_t *p, size_t n, uint64_t *v) {
    *v = 0;
    for (size_t i = 0; i < n && i < 8; i++) { *v |= (uint64_t)(p[i] & 0x7f) << (i * 7); if (!(p[i] & 0x80)) return i + 1; }
    return 0;
}
int mav1_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h) {
    if (n < 2 || (obu[0] >> 3 & 15) != 1 || !(obu[0] & 2)) return 0;   // a sequence header with a size field
    size_t at = obu[0] & 4 ? 2 : 1; uint64_t size;
    size_t l = at < n ? leb128(obu + at, n - at, &size) : 0;
    if (!l || size > n - at - l) return 0;
    av1_bits b = {obu + at + l, (size_t)size, 0, 0};
    unsigned profile = rb(&b, 3); rb(&b, 1); unsigned reduced = rb(&b, 1), level = 0, tier = 0;
    unsigned decoder_model = 0, delay_len = 0;
    if (reduced) level = rb(&b, 5);
    else {
        if (rb(&b, 1)) {   // timing_info
            rb(&b, 32); rb(&b, 32); if (rb(&b, 1)) uvlc(&b);
            if ((decoder_model = rb(&b, 1))) { delay_len = rb(&b, 5) + 1; rb(&b, 32); rb(&b, 5); rb(&b, 5); }
        }
        unsigned display_delay = rb(&b, 1), points = rb(&b, 5) + 1;
        for (unsigned i = 0; i < points && !b.bad; i++) {
            rb(&b, 12); unsigned lv = rb(&b, 5), t = lv > 7 ? rb(&b, 1) : 0;
            if (!i) { level = lv; tier = t; }
            if (decoder_model && rb(&b, 1)) { rb(&b, delay_len); rb(&b, delay_len); rb(&b, 1); }
            if (display_delay && rb(&b, 1)) rb(&b, 4);
        }
    }
    unsigned wb = rb(&b, 4) + 1, hb = rb(&b, 4) + 1;
    *w = (int)rb(&b, wb) + 1; *h = (int)rb(&b, hb) + 1;
    if (!reduced && rb(&b, 1)) { rb(&b, 4); rb(&b, 3); }   // frame ids
    rb(&b, 3);   // 128x128 superblocks, filter intra, intra edge filter
    if (!reduced) {
        rb(&b, 4);   // inter-intra, masked compound, warped motion, dual filter
        unsigned order_hint = rb(&b, 1);
        if (order_hint) rb(&b, 2);   // jnt_comp, ref_frame_mvs
        unsigned tools = rb(&b, 1) ? 2 : rb(&b, 1);
        if (tools && !rb(&b, 1)) rb(&b, 1);   // force_integer_mv
        if (order_hint) rb(&b, 3);
    }
    rb(&b, 3);   // superres, cdef, restoration
    unsigned hbd = rb(&b, 1), twelve = profile == 2 && hbd ? rb(&b, 1) : 0, mono = profile == 1 ? 0 : rb(&b, 1), cp = 2, tc = 2, mc = 2, ssx = 1, ssy = 1, pos = 0;
    if (rb(&b, 1)) { cp = rb(&b, 8); tc = rb(&b, 8); mc = rb(&b, 8); }
    if (!mono) {
        if (cp == 1 && tc == 13 && mc == 0) ssx = ssy = 0;   // sRGB
        else {
            rb(&b, 1);   // colour range
            if (profile == 1) ssx = ssy = 0;
            else if (profile == 2) { if (twelve) { ssx = rb(&b, 1); ssy = ssx ? rb(&b, 1) : 0; } else { ssx = 1; ssy = 0; } }
            if (ssx && ssy) pos = rb(&b, 2);
        }
    }
    if (b.bad) return 0;
    c[0] = 0x81;   // marker, version 1
    c[1] = (uint8_t)(profile << 5 | (level & 31));
    c[2] = (uint8_t)(tier << 7 | hbd << 6 | twelve << 5 | mono << 4 | ssx << 3 | ssy << 2 | pos);
    c[3] = 0;
    return 1;
}
// The first OBU of a type (or of either of two) in a temporal unit, or none; *pay is its payload
static const uint8_t *find_obu(const uint8_t *p, size_t n, unsigned t1, unsigned t2, size_t *len, const uint8_t **pay) {
    for (size_t i = 0; i < n;) {
        if (!(p[i] & 2)) return NULL;   // no size field: not the low-overhead format
        size_t hdr = p[i] & 4 ? 2 : 1; uint64_t size;
        size_t l = i + hdr < n ? leb128(p + i + hdr, n - i - hdr, &size) : 0;
        if (!l || size > n - i - hdr - l) return NULL;
        unsigned t = p[i] >> 3 & 15;
        if (t == t1 || t == t2) { *len = hdr + l + (size_t)size; if (pay) *pay = size ? p + i + hdr + l : NULL; return p + i; }
        i += hdr + l + (size_t)size;
    }
    return NULL;
}
// The sequence header OBU of a temporal unit, or none
static const uint8_t *find_seq(const uint8_t *p, size_t n, size_t *len) { return find_obu(p, n, 1, 1, len, NULL); }
// A temporal unit a new decoder can start at: its sequence header, and a key frame (AV1 5.9.2: reduced_still_picture_header's frames are key frames; otherwise show_existing_frame 0
// and frame_type 0 in the first bits of the frame header, of an OBU_FRAME_HEADER or an OBU_FRAME)
static int av1_key(const uint8_t *p, size_t n) {
    size_t len; const uint8_t *seq = NULL, *fh = NULL;
    if (!find_obu(p, n, 1, 1, &len, &seq) || !seq) return 0;
    if (seq[0] >> 3 & 1) return 1;   // reduced_still_picture_header (after seq_profile and still_picture)
    if (!find_obu(p, n, 3, 6, &len, &fh) || !fh) return 0;
    return !(fh[0] >> 7) && !(fh[0] >> 5 & 3);
}
static void av1_on_frame(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)src; (void)fl; (void)pts; (void)dur; mav1 *a = ref;
    if (st == kVTInvalidSessionErr && movie_vt_restart()) { a->lost = 1; return; }   // mav1_decode starts a new session at the next key frame
    if (st || !img) { if (st && !a->red++) LOG("[RED] movie: AV1 decode error (VideoToolbox %d, first case)", (int)st); return; }
    if (a->got) CVPixelBufferRelease(a->got);
    a->got = CVPixelBufferRetain(img);
}
// 1 = a session; 0 = none (a red line); -1 = none yet: restarting after iOS invalidated the session, and iOS refused a new hardware session for now (the app still inactive)
static int av1_open(mav1 *a, const uint8_t *seq, size_t n, int restarting) {
    av1_close(a);
    uint8_t rec[4 + sizeof a->seq]; int w = 0, h = 0;
    if (n > sizeof a->seq || !mav1_av1c(seq, n, rec, &w, &h)) { if (!a->red++) LOG("[RED] movie: an AV1 sequence header the host cannot read (%zu bytes)", n); return 0; }
    memcpy(rec + 4, seq, n);
    CFDataRef data = CFDataCreate(NULL, rec, (CFIndex)(4 + n));
    const void *ak[] = {CFSTR("av1C")}, *av[] = {data};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(data);
    const void *ek[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *ev[] = {atoms};
    CFDictionaryRef ext = CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(atoms);
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_AV1, w, h, ext, &a->fd); CFRelease(ext);
    if (st) { if (!a->red++) LOG("[RED] movie: no format description for AV1 (%d)", (int)st); return 0; }
    int32_t pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *k[] = {kCVPixelBufferPixelFormatTypeKey}, *v[] = {num};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(num);
    VTDecompressionOutputCallbackRecord cb = {av1_on_frame, a};
    st = VTDecompressionSessionCreate(NULL, a->fd, NULL, attrs, &cb, &a->vs); CFRelease(attrs);
    if (st && restarting && st == kVTVideoDecoderNotAvailableNowErr) { a->vs = NULL; return -1; }
    if (st) { if (!a->red++) LOG("[RED] movie: VideoToolbox cannot create an AV1 session (%d)", (int)st); a->vs = NULL; return 0; }
    memcpy(a->seq, seq, n); a->nseq = n;
    static int once; if (!once++) LOG("[movie] AV1: VideoToolbox decoding %dx%d, profile %d level %d", w, h, rec[1] >> 5, rec[1] & 31);
    return 1;
}
// ---- dav1d ----
static void d_log(void *cookie, const char *fmt, va_list ap) {
    (void)cookie; char b[256]; vsnprintf(b, sizeof b, fmt, ap);
    size_t l = strlen(b); if (l && b[l - 1] == '\n') b[l - 1] = 0;
    LOG("[movie] dav1d: %s", b);
}
static int grow(uint8_t **p, size_t *have, size_t want) {
    if (*have >= want) return 1;
    uint8_t *q = realloc(*p, want); if (!q) return 0;
    *p = q; *have = want; return 1;
}
// dav1d's planar picture as the NV12 the interface gives: the luma in place for 8 bits, else shifted down into y8; the chroma interleaved into uv (subsampled from 4:2:2 and 4:4:4, grey for 4:0:0)
static int dav1d_nv12(mav1 *a, const Dav1dPicture *p, mav1_picture *out) {
    int w = p->p.w, h = p->p.h, sh = p->p.bpc - 8, l = p->p.layout, cw = (w + 1) / 2, ch = (h + 1) / 2;
    int sx = l != DAV1D_PIXEL_LAYOUT_I444, sy = l == DAV1D_PIXEL_LAYOUT_I420;
    if (!grow(&a->uv, &a->nuv, (size_t)cw * 2 * ch) || (sh && !grow(&a->y8, &a->ny8, (size_t)w * h))) return 0;
    const uint8_t *y = p->data[0]; size_t ys = (size_t)p->stride[0];
    if (sh) {
        for (int r = 0; r < h; r++) { const uint16_t *s = (const uint16_t *)((const uint8_t *)p->data[0] + (ptrdiff_t)r * p->stride[0]); uint8_t *o = a->y8 + (size_t)r * w; for (int x = 0; x < w; x++) o[x] = (uint8_t)(s[x] >> sh); }
        y = a->y8; ys = (size_t)w;
    }
    for (int r = 0; r < ch; r++) {
        uint8_t *o = a->uv + (size_t)r * cw * 2;
        if (l == DAV1D_PIXEL_LAYOUT_I400) { memset(o, 128, (size_t)cw * 2); continue; }
        const uint8_t *u = (const uint8_t *)p->data[1] + (ptrdiff_t)(sy ? r : 2 * r) * p->stride[1], *v = (const uint8_t *)p->data[2] + (ptrdiff_t)(sy ? r : 2 * r) * p->stride[1];
        for (int x = 0; x < cw; x++) {
            int c = sx ? x : 2 * x;
            o[2 * x] = sh ? (uint8_t)(((const uint16_t *)u)[c] >> sh) : u[c]; o[2 * x + 1] = sh ? (uint8_t)(((const uint16_t *)v)[c] >> sh) : v[c];
        }
    }
    unsigned mc = p->seq_hdr->mtrx;   // matrix coefficients (AV1 6.4.2); unspecified, BT.601 below 720 lines and BT.709 from there, as VideoToolbox attaches to such a stream's pictures
    if (mc == DAV1D_MC_UNKNOWN) mc = h < 720 ? DAV1D_MC_BT601 : DAV1D_MC_BT709;
    *out = (mav1_picture){y, a->uv, ys, (size_t)cw * 2, w, h, mc == DAV1D_MC_BT709 ? 2 : mc == DAV1D_MC_BT2020_NCL || mc == DAV1D_MC_BT2020_CL ? 5 : 1, p->seq_hdr->color_range, p->m.timestamp};
    return 1;
}
// dav1d's threads. A temporal unit of a hierarchical stream holds several frames (the hidden ones the next units only show), and decoded one after the other they made that
// unit many times longer than the others: Ocarina of Time's 1080p 60 fps title movie took 80 to 122 ms for one unit in 32 on an M2 Pro (two task threads, one frame at a time)
// and 15.4 ms a unit over its heaviest five seconds, at a budget of 16.7. Frame threads decode those frames, and the units waiting behind them, side by side
#define DAV1D_THREADS 4
#define DAV1D_FRAMES 3
static int dav1d_open_once(mav1 *a) {
    if (a->dav1d) return 1;
    Dav1dSettings st; dav1d_default_settings(&st);
    st.n_threads = DAV1D_THREADS; st.max_frame_delay = DAV1D_FRAMES;
    st.logger.cookie = a; st.logger.callback = d_log;
    if (dav1d_open(&a->dav1d, &st)) { a->dav1d = NULL; if (!a->red++) LOG("[RED] movie: dav1d could not be opened"); return 0; }
    static int once; if (!once++) LOG("[movie] AV1: dav1d %s decoding, %d threads on %d frames at once (%s)", dav1d_version(), st.n_threads, st.max_frame_delay,
                                      hw_av1() ? "a stream VideoToolbox refused" : getenv("DM_AV1_DAV1D") ? "DM_AV1_DAV1D" : "no hardware AV1 decoder");
    return 1;
}
// Gives dav1d what it has not taken of the unit, then asks it for one picture: 1 with it, 0 none now, -1 the unit was refused. One dav1d_get_picture for each dav1d_send_data:
// a second one in a row is dav1d's drain, which waits for the frames in flight and so decodes them one after the other. With drain, and the unit taken, that is what is asked.
// A drain that has given a picture must go on to its end (EAGAIN) before the next unit: dav1d (1.5.4, lib.c drain_picture) steps over its empty frame contexts on the way to
// the oldest frame, which leaves frame_thread.next on a frame still in flight if data comes then. The next frame waits for that one and retires it with task_thread.first one
// context past the oldest frame for good, and retiring a frame cancels the task rescan asked for task_thread.first: a live frame's from then on. Its task is never run, the
// workers sleep and dav1d_submit_frame waits for ever (found when the drain stopped as soon as a packet arrived: Ocarina of Time's title movie froze in 3 runs of 6)
static int dav1d_step(mav1 *a, int drain, mav1_picture *out) {
    for (int tries = 0; tries < 64; tries++) {
        int draining = !a->in.sz;   // nothing to give: this get is the second in a row
        if (a->in.sz) {
            int r = dav1d_send_data(a->dav1d, &a->in);   // EAGAIN: a picture has to be taken first
            if (r && r != DAV1D_ERR(EAGAIN)) { dav1d_data_unref(&a->in); if (!a->red++) LOG("[RED] movie: dav1d refused a temporal unit (%d, first case)", r); return -1; }
        } else if (!drain) return 0;
        Dav1dPicture p = {0};
        int g = dav1d_get_picture(a->dav1d, &p);
        if (!g) {
            if (draining) a->draining = 1;
            if (a->has_pic) dav1d_picture_unref(&a->pic);
            a->pic = p; a->has_pic = 1;
            if (!dav1d_nv12(a, &a->pic, out)) { if (!a->red++) LOG("[RED] movie: no memory for a %dx%d AV1 picture", a->pic.p.w, a->pic.p.h); return -1; }
            return 1;
        }
        if (g == DAV1D_ERR(EAGAIN)) { if (draining) a->draining = 0; }
        else if (!a->red++) LOG("[RED] movie: dav1d could not decode a frame (%d, first case)", g);
        if (!a->in.sz && (g == DAV1D_ERR(EAGAIN) || !drain)) return 0;   // the unit is in and no picture is ready; a frame in error is passed over while draining
    }
    if (a->in.sz) { dav1d_data_unref(&a->in); if (!a->red++) LOG("[RED] movie: dav1d took no more data, a temporal unit dropped (first case)"); return -1; }
    return 0;
}
static int decode_dav1d(mav1 *a, const uint8_t *data, size_t n, int64_t ts, mav1_picture *out) {
    if (!dav1d_open_once(a)) return -1;
    if (a->in.sz) dav1d_data_unref(&a->in);   // a unit left unfinished (its caller stopped): dropped
    if (a->draining) {   // a drain its caller left: ended here, its pictures lost
        int lost = 0;
        for (int tries = 0; tries < 64; tries++) { Dav1dPicture p = {0}; int g = dav1d_get_picture(a->dav1d, &p); if (!g) { dav1d_picture_unref(&p); lost++; } else if (g == DAV1D_ERR(EAGAIN)) break; }
        a->draining = 0;
        LOG("[RED] movie: an AV1 drain was left before its end: %d picture(s) dropped", lost);
    }
    uint8_t *buf = dav1d_data_create(&a->in, n);
    if (!buf) return -1;
    memcpy(buf, data, n); a->in.m.timestamp = ts;
    return dav1d_step(a, 0, out);
}
int mav1_next(mav1 *a, int drain, mav1_picture *out) { return a->soft && a->dav1d ? dav1d_step(a, drain, out) : 0; }

// A VideoToolbox picture, locked until the next decode, as the interface gives it; ts is its temporal unit's time
static int vt_picture(mav1 *a, CVPixelBufferRef pb, int64_t ts, mav1_picture *out) {
    a->locked = pb;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CFStringRef m = CVBufferCopyAttachment(pb, kCVImageBufferYCbCrMatrixKey, NULL);
    *out = (mav1_picture){CVPixelBufferGetBaseAddressOfPlane(pb, 0), CVPixelBufferGetBaseAddressOfPlane(pb, 1), CVPixelBufferGetBytesPerRowOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
                          (int)CVPixelBufferGetWidthOfPlane(pb, 0), (int)CVPixelBufferGetHeightOfPlane(pb, 0),
                          !m || CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_709_2) ? 2 : CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_2020) ? 5 : 1, 0, ts};
    if (m) CFRelease(m);
    return 1;
}
// While waiting for a key frame after a restart: the last picture again, for this unit's time, so the movie's pictures keep their times and the player's clock runs through the
// gap at real speed (a still picture) instead of jumping to the key frame and running ahead of the game's sound for the rest of the movie. None before the first picture
static int av1_still(mav1 *a, int64_t ts, mav1_picture *out) { return a->last ? vt_picture(a, CVPixelBufferRetain(a->last), ts, out) : 0; }
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, int64_t ts, mav1_picture *out) {
    if (a->soft) return decode_dav1d(a, data, n, ts, out);
    unlock_picture(a);
    size_t seqn = 0; const uint8_t *seq = find_seq(data, n, &seqn);
    int restarting = a->restart;
    if (a->restart) {   // iOS invalidated the session: the temporal units before the next key frame refer to pictures that went with it, so none is decoded (no garbage: the last picture again)
        if (!av1_key(data, n)) return av1_still(a, ts, out);
        a->restart = 0;
    }
    for (int again = 0;; again++) {
        int o = seq && (seqn != a->nseq || memcmp(seq, a->seq, seqn) || !a->vs) ? av1_open(a, seq, seqn, restarting) : 1;
        if (o < 0) {   // no new session for this key frame: the units after it still wait for the next one, on VideoToolbox (not dav1d for the rest of the movie)
            static int nlog; if (nlog++ < 8) LOG("[movie] AV1: VideoToolbox's decoder is not available yet (the app still inactive), restarting at the next key frame");
            av1_close(a); a->restart = 1; return av1_still(a, ts, out);
        }
        if (!o) {   // a stream VideoToolbox does not take: dav1d from this temporal unit on (it holds the sequence header)
            LOG("[movie] AV1: dav1d decodes the stream instead"); av1_close(a); a->soft = 1;
            return decode_dav1d(a, data, n, ts, out);
        }
        if (!a->vs) return 0;   // the stream's first sequence header has not come
        CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL; size_t ssz = n;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, n, NULL, NULL, 0, n, 0, &bb);
        if (!st) st = CMBlockBufferReplaceDataBytes(data, bb, 0, n);
        if (!st) st = CMSampleBufferCreateReady(NULL, bb, a->fd, 1, 0, NULL, 1, &ssz, &sb);
        if (!st) st = VTDecompressionSessionDecodeFrame(a->vs, sb, 0, NULL, NULL);   // flags 0: synchronous, the callback has run when this returns
        if (sb) CFRelease(sb);
        if (bb) CFRelease(bb);
        // iOS ended the hardware session while the app was in the background (Ocarina of Time's opening, the notification panel pulled down): every later unit failed with -12903 and
        // the movie ran through its file in about 2 s. Only that error, and only with movie_vt_restart: a new session, at once when this unit is a key frame, else at the next one
        if ((st == kVTInvalidSessionErr || a->lost) && movie_vt_restart()) {
            int now = !again && av1_key(data, n);
            av1_close(a); a->lost = 0; a->restart = !now; restarting = 1;
            if (++a->restarts <= 8) LOG("[movie] AV1: VideoToolbox's session was invalidated (the app was in the background), restarting %s (restart %d)", now ? "at this key frame" : "at the next key frame", a->restarts);
            if (now) continue;
            return av1_still(a, ts, out);
        }
        if (st) { if (!a->red++) LOG("[RED] movie: AV1 decode submission failed (%d, first case)", (int)st); return -1; }
        break;
    }
    if (!a->got) return 0;
    CVPixelBufferRef pb = a->got; a->got = NULL;
    if (movie_vt_restart()) { if (a->last) CVPixelBufferRelease(a->last); a->last = CVPixelBufferRetain(pb); }
    return vt_picture(a, pb, ts, out);
}
