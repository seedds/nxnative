// AV1 for the nn::codec decoders (hle_codec_video.c), on Apple's VideoToolbox: Fire Emblem Fortune's Weave's 82 movies are AV1 in WebM (2560x1440). VideoToolbox's AV1 decoder is hardware only (Apple M3
// and A17 Pro on): on an older Mac there is none, mav1_supported says so and the caller gives black pictures with a red line, a platform limit.
// VideoToolbox wants a format description holding the stream's av1C record (ISO-BMFF AV1 binding, 2.3.3): four bytes read from the sequence header, then that header's OBU. The decoder API takes no
// configuration for AV1, so the sequence header comes from the stream, where every key frame carries one (AV1 5.5); a changed one makes a new session. A temporal unit goes in whole, its OBUs in
// the low-overhead format WebM and the SDK's demuxer keep them in (each with its size). Decoding is synchronous and a temporal unit shows at most one frame, in presentation order, so no queue
#include "hle.h"
#include <VideoToolbox/VideoToolbox.h>
#include "hle_movie_av1.h"

struct mav1 {
    uint8_t seq[512]; size_t nseq;
    CMVideoFormatDescriptionRef fd; VTDecompressionSessionRef vs;
    CVPixelBufferRef got, locked;
    int red;
};
mav1 *mav1_new(void) { mav1 *a = calloc(1, sizeof *a); if (!a) die("movie: AV1 decoder allocation failed"); return a; }
static void unlock_picture(mav1 *a) { if (a->locked) { CVPixelBufferUnlockBaseAddress(a->locked, kCVPixelBufferLock_ReadOnly); CVPixelBufferRelease(a->locked); a->locked = NULL; } }
static void av1_close(mav1 *a) {
    if (a->vs) { VTDecompressionSessionWaitForAsynchronousFrames(a->vs); VTDecompressionSessionInvalidate(a->vs); CFRelease(a->vs); a->vs = NULL; }
    if (a->fd) { CFRelease(a->fd); a->fd = NULL; }
    if (a->got) { CVPixelBufferRelease(a->got); a->got = NULL; }
}
void mav1_reset(mav1 *a) { unlock_picture(a); av1_close(a); a->nseq = 0; }
void mav1_free(mav1 *a) { if (a) { mav1_reset(a); free(a); } }
int mav1_supported(void) { return VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1); }

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
// The sequence header OBU of a temporal unit, or none
static const uint8_t *find_seq(const uint8_t *p, size_t n, size_t *len) {
    for (size_t i = 0; i < n;) {
        if (!(p[i] & 2)) return NULL;   // no size field: not the low-overhead format
        size_t hdr = p[i] & 4 ? 2 : 1; uint64_t size;
        size_t l = i + hdr < n ? leb128(p + i + hdr, n - i - hdr, &size) : 0;
        if (!l || size > n - i - hdr - l) return NULL;
        if ((p[i] >> 3 & 15) == 1) { *len = hdr + l + (size_t)size; return p + i; }
        i += hdr + l + (size_t)size;
    }
    return NULL;
}
static void av1_on_frame(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)src; (void)fl; (void)pts; (void)dur; mav1 *a = ref;
    if (st || !img) { if (st && !a->red++) LOG("[RED] movie: AV1 decode error (VideoToolbox %d, first case)", (int)st); return; }
    if (a->got) CVPixelBufferRelease(a->got);
    a->got = CVPixelBufferRetain(img);
}
static int av1_open(mav1 *a, const uint8_t *seq, size_t n) {
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
    if (st) { if (!a->red++) LOG("[RED] movie: VideoToolbox cannot create an AV1 session (%d)", (int)st); a->vs = NULL; return 0; }
    memcpy(a->seq, seq, n); a->nseq = n;
    static int once; if (!once++) LOG("[movie] AV1: VideoToolbox decoding %dx%d, profile %d level %d", w, h, rec[1] >> 5, rec[1] & 31);
    return 1;
}
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out) {
    unlock_picture(a);
    size_t seqn = 0; const uint8_t *seq = find_seq(data, n, &seqn);
    if (seq && (seqn != a->nseq || memcmp(seq, a->seq, seqn) || !a->vs) && !av1_open(a, seq, seqn)) return -1;
    if (!a->vs) return 0;   // the stream's first sequence header has not come
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL; size_t ssz = n;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, n, NULL, NULL, 0, n, 0, &bb);
    if (!st) st = CMBlockBufferReplaceDataBytes(data, bb, 0, n);
    if (!st) st = CMSampleBufferCreateReady(NULL, bb, a->fd, 1, 0, NULL, 1, &ssz, &sb);
    if (!st) st = VTDecompressionSessionDecodeFrame(a->vs, sb, 0, NULL, NULL);   // flags 0: synchronous, the callback has run when this returns
    if (sb) CFRelease(sb);
    if (bb) CFRelease(bb);
    if (st) { if (!a->red++) LOG("[RED] movie: AV1 decode submission failed (%d, first case)", (int)st); return -1; }
    if (!a->got) return 0;
    CVPixelBufferRef pb = a->got; a->got = NULL; a->locked = pb;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CFStringRef m = CVBufferCopyAttachment(pb, kCVImageBufferYCbCrMatrixKey, NULL);
    *out = (mav1_picture){CVPixelBufferGetBaseAddressOfPlane(pb, 0), CVPixelBufferGetBaseAddressOfPlane(pb, 1), CVPixelBufferGetBytesPerRowOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
                          (int)CVPixelBufferGetWidthOfPlane(pb, 0), (int)CVPixelBufferGetHeightOfPlane(pb, 0),
                          !m || CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_709_2) ? 2 : CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_2020) ? 5 : 1, 0};
    if (m) CFRelease(m);
    return 1;
}
