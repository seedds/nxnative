// AV1 decoding for the Switch 2 decoder API (hle_codec_video.c). Fire Emblem Fortune's Weave's 91 movies are AV1 in WebM; the SDK's movie module hands over one temporal unit
// per packet, in the low-overhead OBU format (AV1 5.2) as WebM keeps it, and no codec private data. Two backends behind one interface:
//   - VideoToolbox where the chip decodes AV1 (VTIsHardwareDecodeSupported: Apple M3 and A17 Pro on). Its format description carries the AV1CodecConfigurationRecord (av1C),
//     built here from the stream's sequence header OBU, kept from the packets; it decodes one temporal unit per sample, synchronously.
//   - dav1d (dm/third_party/dav1d) on other chips, and where no VideoToolbox session can be made for the stream; DM_AV1_DAV1D=1 takes it on any chip.
// A temporal unit shows at most one picture (show_frame or show_existing_frame), so pictures leave in decode order with no reorder queue. They are handed over as 8-bit I420
// (10- and 12-bit streams shifted down), with the colour matrix and range of the sequence header.
#include "hle.h"
#include <VideoToolbox/VideoToolbox.h>
#include "dav1d/dav1d.h"
#include "hle_movie_av1.h"

#define AV1_QMAX 8
typedef struct { uint8_t profile, level, tier, hbd, twelve, mono, ssx, ssy, csp, range, mc; uint32_t w, h; } av1_seq;
struct mav1 {
    int vt;                              // the backend: 1 VideoToolbox, 0 dav1d
    Dav1dContext *dav1d;
    uint8_t seq[512]; size_t nseq;       // the sequence header OBU (header, size field and payload) the session was made for
    av1_seq sh; int have_seq;
    CMVideoFormatDescriptionRef fd; VTDecompressionSessionRef vs; int dirty;
    struct { vpx_image_t *im; int64_t ts; } q[AV1_QMAX]; int nq;
    int red;
};

// ---- OBUs and the sequence header (AV1 5.3, 5.5) ----
static int leb128(const uint8_t *p, size_t n, size_t *i, uint64_t *v) {
    *v = 0;
    for (int k = 0; k < 8; k++) { if (*i >= n) return -1; uint8_t b = p[(*i)++]; *v |= (uint64_t)(b & 0x7f) << (7 * k); if (!(b & 0x80)) return 0; }
    return -1;
}
typedef struct { const uint8_t *p; size_t n, bit; int bad; } av1_bits;
static uint32_t av1_f(av1_bits *b, int n) {
    uint32_t v = 0;
    while (n-- > 0) { if (b->bit >= b->n * 8) { b->bad = 1; return 0; } v = v << 1 | (b->p[b->bit / 8] >> (7 - b->bit % 8) & 1); b->bit++; }
    return v;
}
static void av1_uvlc(av1_bits *b) { int z = 0; while (!b->bad && !av1_f(b, 1)) if (++z >= 32) { b->bad = 1; return; } av1_f(b, z); }
// What av1C needs (seq_profile, the first operating point's level and tier, the colour config) and the largest frame size
static int parse_seq(const uint8_t *pl, size_t n, av1_seq *s) {
    av1_bits b = {pl, n, 0, 0}; memset(s, 0, sizeof *s);
    s->profile = (uint8_t)av1_f(&b, 3); av1_f(&b, 1); int reduced = (int)av1_f(&b, 1);   // still_picture, reduced_still_picture_header
    if (reduced) s->level = (uint8_t)av1_f(&b, 5);
    else {
        int dm = 0, dlen = 0;
        if (av1_f(&b, 1)) {   // timing_info_present_flag
            av1_f(&b, 32); av1_f(&b, 32); if (av1_f(&b, 1)) av1_uvlc(&b);
            if ((dm = (int)av1_f(&b, 1))) { dlen = (int)av1_f(&b, 5) + 1; av1_f(&b, 32); av1_f(&b, 5); av1_f(&b, 5); }   // decoder_model_info
        }
        int idd = (int)av1_f(&b, 1), ops = (int)av1_f(&b, 5) + 1;
        for (int i = 0; i < ops && !b.bad; i++) {
            av1_f(&b, 12); uint8_t lv = (uint8_t)av1_f(&b, 5), tier = lv > 7 ? (uint8_t)av1_f(&b, 1) : 0;
            if (!i) { s->level = lv; s->tier = tier; }
            if (dm && av1_f(&b, 1)) { av1_f(&b, dlen); av1_f(&b, dlen); av1_f(&b, 1); }   // operating_parameters_info
            if (idd && av1_f(&b, 1)) av1_f(&b, 4);
        }
    }
    int wb = (int)av1_f(&b, 4) + 1, hb = (int)av1_f(&b, 4) + 1;
    s->w = av1_f(&b, wb) + 1; s->h = av1_f(&b, hb) + 1;
    if (!reduced && av1_f(&b, 1)) { av1_f(&b, 4); av1_f(&b, 3); }   // frame_id_numbers_present_flag
    av1_f(&b, 3);   // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter
    if (!reduced) {
        av1_f(&b, 4);   // interintra, masked compound, warped motion, dual filter
        int oh = (int)av1_f(&b, 1);
        if (oh) av1_f(&b, 2);   // jnt_comp, ref_frame_mvs
        int sct = av1_f(&b, 1) ? 2 : (int)av1_f(&b, 1);   // seq_choose_screen_content_tools, else seq_force_screen_content_tools
        if (sct && !av1_f(&b, 1)) av1_f(&b, 1);           // seq_choose_integer_mv, else seq_force_integer_mv
        if (oh) av1_f(&b, 3);
    }
    av1_f(&b, 3);   // superres, cdef, restoration
    s->hbd = (uint8_t)av1_f(&b, 1);
    if (s->profile == 2 && s->hbd) s->twelve = (uint8_t)av1_f(&b, 1);
    s->mono = s->profile == 1 ? 0 : (uint8_t)av1_f(&b, 1);
    int cp = 2, tc = 2; s->mc = 2;
    if (av1_f(&b, 1)) { cp = (int)av1_f(&b, 8); tc = (int)av1_f(&b, 8); s->mc = (uint8_t)av1_f(&b, 8); }
    if (s->mono) { s->range = (uint8_t)av1_f(&b, 1); s->ssx = s->ssy = 1; }
    else if (cp == 1 && tc == 13 && s->mc == 0) { s->range = 1; }   // sRGB: 4:4:4
    else {
        s->range = (uint8_t)av1_f(&b, 1);
        if (s->profile == 0) s->ssx = s->ssy = 1;
        else if (s->profile == 2) {
            if (s->twelve) { s->ssx = (uint8_t)av1_f(&b, 1); s->ssy = s->ssx ? (uint8_t)av1_f(&b, 1) : 0; } else { s->ssx = 1; s->ssy = 0; }
        }
        if (s->ssx && s->ssy) s->csp = (uint8_t)av1_f(&b, 2);
    }
    return b.bad ? -1 : 0;
}
// The sequence header OBU of a temporal unit, kept with a size field (av1C's configOBUs need one); a new one (another stream) makes a new VideoToolbox session
static void find_seq(mav1 *a, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        size_t s = i; uint8_t h = p[i++]; int type = h >> 3 & 15, ext = h >> 2 & 1, has_size = h >> 1 & 1;
        if (h & 0x80) return;   // obu_forbidden_bit
        if (ext) { if (i >= n) return; i++; }
        uint64_t sz = n - i;
        if (has_size && leb128(p, n, &i, &sz)) return;
        if (sz > n - i) return;
        if (type == 1) {   // OBU_SEQUENCE_HEADER
            av1_seq sh;
            if (parse_seq(p + i, (size_t)sz, &sh)) { if (!a->red++) LOG("[RED] nn::codec AV1: a sequence header that does not parse (%llu bytes)", (unsigned long long)sz); return; }
            uint8_t o[sizeof a->seq]; size_t k = 0;
            if (sz + 16 > sizeof o) { if (!a->red++) LOG("[RED] nn::codec AV1: a sequence header of %llu bytes", (unsigned long long)sz); return; }
            o[k++] = h | 2; if (ext) o[k++] = p[s + 1];   // obu_has_size_field set
            uint64_t v = sz; do { o[k++] = (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0)); v >>= 7; } while (v);
            memcpy(o + k, p + i, (size_t)sz); k += (size_t)sz;
            if (!a->have_seq || k != a->nseq || memcmp(o, a->seq, k)) { memcpy(a->seq, o, k); a->nseq = k; a->sh = sh; a->have_seq = 1; a->dirty = 1; }
            return;
        }
        i += (size_t)sz;
    }
}

// ---- Pictures ----
static vpx_color_space_t cs_of(uint8_t mc) { return mc == 1 ? VPX_CS_BT_709 : mc == 5 || mc == 6 ? VPX_CS_BT_601 : VPX_CS_UNKNOWN; }
static void push(mav1 *a, vpx_image_t *im, int64_t ts) {
    if (!im) return;
    if (a->nq == AV1_QMAX) { if (!a->red++) LOG("[RED] nn::codec AV1: picture queue full, the oldest dropped"); vpx_img_free(a->q[0].im); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    a->q[a->nq].im = im; a->q[a->nq].ts = ts; a->nq++;
}
static vpx_image_t *from_dav1d(const Dav1dPicture *p) {
    int w = p->p.w, h = p->p.h, sh = p->p.bpc - 8, l = p->p.layout, sx = l != DAV1D_PIXEL_LAYOUT_I444, sy = l == DAV1D_PIXEL_LAYOUT_I420;
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, (unsigned)w, (unsigned)h, 1); if (!c) return NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *o = c->planes[0] + (size_t)y * c->stride[0];
        if (!sh) memcpy(o, (const uint8_t *)p->data[0] + y * p->stride[0], (size_t)w);
        else { const uint16_t *s = (const uint16_t *)((const uint8_t *)p->data[0] + y * p->stride[0]); for (int x = 0; x < w; x++) o[x] = (uint8_t)(s[x] >> sh); }
    }
    for (int k = 1; k < 3; k++) for (int y = 0; y < (h + 1) / 2; y++) {
        uint8_t *o = c->planes[k] + (size_t)y * c->stride[k]; int cw = (w + 1) / 2;
        if (l == DAV1D_PIXEL_LAYOUT_I400) { memset(o, 128, (size_t)cw); continue; }
        const uint8_t *row = (const uint8_t *)p->data[k] + (ptrdiff_t)(sy ? y : 2 * y) * p->stride[1];
        for (int x = 0; x < cw; x++) { int sxp = sx ? x : 2 * x; o[x] = sh ? (uint8_t)(((const uint16_t *)row)[sxp] >> sh) : row[sxp]; }
    }
    c->cs = cs_of((uint8_t)p->seq_hdr->mtrx); c->range = p->seq_hdr->color_range ? VPX_CR_FULL_RANGE : VPX_CR_STUDIO_RANGE;
    return c;
}
static vpx_image_t *from_nv12(CVPixelBufferRef pb, const av1_seq *s) {   // VideoToolbox's 8-bit NV12 (video range, as requested) as I420
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    unsigned w = (unsigned)CVPixelBufferGetWidthOfPlane(pb, 0), h = (unsigned)CVPixelBufferGetHeightOfPlane(pb, 0);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, w, h, 1);
    if (c) {
        for (unsigned r = 0; r < h; r++) memcpy(c->planes[0] + (size_t)r * c->stride[0], y + r * ys, w);
        for (unsigned r = 0; r < (h + 1) / 2; r++) for (unsigned x = 0; x < (w + 1) / 2; x++) {
            c->planes[1][(size_t)r * c->stride[1] + x] = uv[r * uvs + 2 * x]; c->planes[2][(size_t)r * c->stride[2] + x] = uv[r * uvs + 2 * x + 1]; }
        c->cs = cs_of(s->mc); c->range = VPX_CR_STUDIO_RANGE;
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return c;
}

// ---- dav1d ----
static void d_log(void *cookie, const char *fmt, va_list ap) {
    (void)cookie; char b[256]; vsnprintf(b, sizeof b, fmt, ap);
    size_t l = strlen(b); if (l && b[l - 1] == '\n') b[l - 1] = 0;
    LOG("[movie] dav1d: %s", b);
}
static int open_dav1d(mav1 *a) {
    Dav1dSettings s; dav1d_default_settings(&s);
    s.n_threads = 2; s.max_frame_delay = 1;   // a picture per temporal unit, decoded on the submitting thread with one helper
    s.logger.cookie = a; s.logger.callback = d_log;
    if (dav1d_open(&a->dav1d, &s)) { a->dav1d = NULL; LOG("[RED] nn::codec AV1: dav1d could not be opened"); return 0; }
    return 1;
}
static void pull_dav1d(mav1 *a) {
    Dav1dPicture p = {0};
    while (a->nq < AV1_QMAX && !dav1d_get_picture(a->dav1d, &p)) { push(a, from_dav1d(&p), p.m.timestamp); dav1d_picture_unref(&p); }
}
static int decode_dav1d(mav1 *a, const uint8_t *p, size_t n, int64_t ts) {
    if (!a->dav1d && !open_dav1d(a)) return -1;
    Dav1dData d = {0}; uint8_t *buf = dav1d_data_create(&d, n);
    if (!buf) return -1;
    memcpy(buf, p, n); d.m.timestamp = ts;
    for (int tries = 0; d.sz && tries < 64; tries++) {
        int r = dav1d_send_data(a->dav1d, &d);
        if (r && r != DAV1D_ERR(EAGAIN)) { dav1d_data_unref(&d); if (!a->red++) LOG("[RED] nn::codec AV1: dav1d refused a temporal unit (%d)", r); return -1; }
        pull_dav1d(a);
    }
    if (d.sz) { dav1d_data_unref(&d); if (!a->red++) LOG("[RED] nn::codec AV1: dav1d took no more data, a temporal unit dropped"); return -1; }
    return 0;
}

// ---- VideoToolbox ----
static void vt_close(mav1 *a) {
    if (a->vs) { VTDecompressionSessionWaitForAsynchronousFrames(a->vs); VTDecompressionSessionInvalidate(a->vs); CFRelease(a->vs); a->vs = NULL; }
    if (a->fd) { CFRelease(a->fd); a->fd = NULL; }
}
static void vt_on_frame(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)src; (void)dur; mav1 *a = ref;
    if (fl & kVTDecodeInfo_FrameDropped) return;
    if (st || !img) { if (!a->red++) LOG("[RED] nn::codec AV1: VideoToolbox decode error %d (first case)", (int)st); return; }
    push(a, from_nv12(img, &a->sh), pts.timescale ? pts.value : 0);
}
static int vt_open(mav1 *a) {
    vt_close(a);
    uint8_t rec[4 + sizeof a->seq]; const av1_seq *s = &a->sh;   // AV1CodecConfigurationRecord: marker and version, then profile / level, tier and colour config, no presentation delay
    rec[0] = 0x81; rec[1] = (uint8_t)(s->profile << 5 | (s->level & 31));
    rec[2] = (uint8_t)(s->tier << 7 | s->hbd << 6 | s->twelve << 5 | s->mono << 4 | s->ssx << 3 | s->ssy << 2 | s->csp); rec[3] = 0;
    memcpy(rec + 4, a->seq, a->nseq);
    CFDataRef cfg = CFDataCreate(NULL, rec, (CFIndex)(4 + a->nseq));
    const void *ak[] = {CFSTR("av1C")}, *av[] = {cfg};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *ek[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *ev[] = {atoms};
    CFDictionaryRef ext = CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_AV1, (int32_t)s->w, (int32_t)s->h, ext, &a->fd);
    CFRelease(ext); CFRelease(atoms); CFRelease(cfg);
    if (st) { a->fd = NULL; LOG("[movie] AV1: no VideoToolbox format description for the stream (%d)", (int)st); return 0; }
    int32_t pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *k[] = {kCVPixelBufferPixelFormatTypeKey}, *v[] = {n};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(n);
    VTDecompressionOutputCallbackRecord cb = {vt_on_frame, a};
    st = VTDecompressionSessionCreate(NULL, a->fd, NULL, attrs, &cb, &a->vs); CFRelease(attrs);
    if (st) { a->vs = NULL; LOG("[movie] AV1: VideoToolbox cannot decode the stream (%d)", (int)st); return 0; }
    LOG("[movie] AV1: VideoToolbox decoding %u×%u, profile %u, level %u, %d-bit", s->w, s->h, s->profile, s->level, s->twelve ? 12 : s->hbd ? 10 : 8);
    a->dirty = 0; return 1;
}
static int decode_vt(mav1 *a, const uint8_t *p, size_t n, int64_t ts) {
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, n, NULL, NULL, 0, n, 0, &bb);
    if (!st) st = CMBlockBufferReplaceDataBytes(p, bb, 0, n);
    CMSampleTimingInfo ti = {kCMTimeInvalid, CMTimeMake(ts, 1000000000), kCMTimeInvalid};
    if (!st) st = CMSampleBufferCreateReady(NULL, bb, a->fd, 1, 1, &ti, 1, &n, &sb);
    if (!st) st = VTDecompressionSessionDecodeFrame(a->vs, sb, 0, NULL, NULL);   // flags 0: synchronous, the picture is queued before this returns
    if (sb) CFRelease(sb); if (bb) CFRelease(bb);
    if (st) { if (!a->red++) LOG("[RED] nn::codec AV1: VideoToolbox could not take a temporal unit (%d, first case)", (int)st); return -1; }
    return 0;
}

// ---- Interface ----
mav1 *mav1_new(void) {
    mav1 *a = calloc(1, sizeof *a); if (!a) die("nn::codec AV1: decoder allocation failed");
    const char *e = getenv("DM_AV1_DAV1D");
    a->vt = !(e && *e && *e != '0') && VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1);
    if (!a->vt) open_dav1d(a);
    return a;
}
const char *mav1_backend(const mav1 *a) { return a->vt ? "VideoToolbox" : "dav1d"; }
void mav1_drop(mav1 *a) {
    for (int i = 0; i < a->nq; i++) vpx_img_free(a->q[i].im);
    a->nq = 0;
    if (a->dav1d) dav1d_flush(a->dav1d);
}
void mav1_free(mav1 *a) { if (!a) return; mav1_drop(a); vt_close(a); if (a->dav1d) dav1d_close(&a->dav1d); free(a); }
int mav1_decode(mav1 *a, const uint8_t *p, size_t n, int64_t ts) {
    if (!n) return 0;
    if (!a->vt) return decode_dav1d(a, p, n, ts);
    find_seq(a, p, n);
    if (!a->have_seq) { if (!a->red++) LOG("[RED] nn::codec AV1: a temporal unit before any sequence header, dropped"); return -1; }
    if ((a->dirty || !a->vs) && !vt_open(a)) {   // the stream is one VideoToolbox does not take: dav1d from this temporal unit on (it holds the sequence header)
        LOG("[movie] AV1: dav1d decodes the stream instead"); a->vt = 0; vt_close(a);
        return decode_dav1d(a, p, n, ts);
    }
    return decode_vt(a, p, n, ts);
}
vpx_image_t *mav1_take(mav1 *a, int64_t *ts) {
    if (!a->nq) return NULL;
    vpx_image_t *im = a->q[0].im; if (ts) *ts = a->q[0].ts;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    return im;
}
