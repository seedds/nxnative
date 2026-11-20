// H.264 (AVC) decoding for the movie decoder (hle_movie.c), on Apple's VideoToolbox (the hardware decoder on the Mac and the iPhone).
// Super Mario Odyssey's 44 movies are MP4 / H.264 (the hint videos in its tutorial boxes, the kingdom travel videos): 41 High or Main profile with B-frames, 3 Baseline.
// The SDK's demuxer hands over one access unit per packet with Annex B start codes; SPS / PPS come in Configure's csd-0 / csd-1 (as with Android's MediaCodec) and may also appear in the stream.
// VideoToolbox wants a format description built from SPS / PPS and NAL units with 4-byte length prefixes. Decoding is synchronous and yields pictures in decode order; MediaCodec yields them
// in presentation order. For progressive POC type 0 and sliding-window references, Annex C's DPB occupancy and the SPS reorder bound
// determine which pictures can leave, sorted by coded sequence then POC. Other streams retain timestamp sorting and bounded lookahead;
// complete sequences and end of stream drain the queue. PTS is carried through unchanged, never used to infer a frame interval.
// SPS pic_order_cnt_type 2 has decode order equal to output order: even High profile needs no reorder queue in that case
#include "hle.h"
#include <VideoToolbox/VideoToolbox.h>
#include <arm_neon.h>
#include "hle_movie_avc.h"
#include "hle_movie_vt.h"

#define QMAX 32   // up to 16 DPB pictures, plus an incoming picture and the previous sequence's drain
typedef struct {
    int valid, depth, id, poc, frame_bits, poc_bits, chroma, frame_only, refs, dpb, reorder;
} avc_sps;
typedef struct { int valid, id, sps_id, bottom, refs[2], weighted, bipred, redundant; } avc_pps;
typedef struct { uint64_t sequence, ref; int64_t poc; int ordered; } avc_frame;
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty;
    CMVideoFormatDescriptionRef fd; VTDecompressionSessionRef vs;
    struct { CVPixelBufferRef pb; int64_t pts; avc_frame order; int ready; } q[QMAX]; int nq, depth;
    uint64_t sequence; int saw_picture;
    avc_sps sp; avc_pps pp;
    int ordered, prev_frame, prev_lsb; int64_t prev_msb; uint64_t refs;
    uint8_t *au; size_t aucap;   // length-prefixed copy of the access unit being decoded
    int red, rgba;               // rgba: pictures as 4-byte RGBA (native texture mode, Monster Hunter Rise) instead of NV12
    movie_vt_history history;
};

int mavc_bt709(const mavc *a, int h) {
    CFTypeRef m = a->fd ? CMFormatDescriptionGetExtension(a->fd, kCMFormatDescriptionExtension_YCbCrMatrix) : NULL;
    if (m && CFGetTypeID(m) == CFStringGetTypeID()) return CFEqual(m, kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2);
    return h >= 720;
}
mavc *mavc_new(movie_vt_control *control) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); a->history.control = control; return a; }
static void close_session(mavc *a) {
    if (a->vs) { VTDecompressionSessionWaitForAsynchronousFrames(a->vs); VTDecompressionSessionInvalidate(a->vs); CFRelease(a->vs); a->vs = NULL; }
    if (a->fd) { CFRelease(a->fd); a->fd = NULL; }
}
void mavc_drop(mavc *a) { for (int i = 0; i < a->nq; i++) CVPixelBufferRelease(a->q[i].pb); a->nq = 0; a->sequence = 0; a->saw_picture = 0; a->ordered = 0; a->refs = 0; movie_vt_clear(&a->history); }
void mavc_free(mavc *a) { if (!a) return; close_session(a); mavc_drop(a); free(a->au); free(a); }
int mavc_queued(const mavc *a) { return a->nq; }
int mavc_ready(const mavc *a) { return a->nq && (a->q[0].ready || a->q[0].order.sequence != a->sequence || (!a->ordered && a->nq > a->depth)); }

// Read the SPS's POC and DPB bounds (H.264 7.4.2.1.1, 8.2.1 and E.2.1).
// CRI Sofdec2 waits for every picture; holding four merely because profile_idc
// is High made the final picture arrive with EOS, which its player discards.
// A truncated/unsupported SPS keeps the old fallback; a complete SPS supplies the signalled or level-inferred reorder bound.
typedef struct { uint8_t rbsp[512]; size_t n, bit; int bad; } avc_bits;
static unsigned avc_u(avc_bits *b, unsigned n) {
    if (n > 32 || b->bit + n > b->n * 8) { b->bad = 1; return 0; }
    unsigned v = 0;
    while (n--) { v = (v << 1) | ((b->rbsp[b->bit / 8] >> (7 - b->bit % 8)) & 1); b->bit++; }
    return v;
}
static unsigned avc_ue(avc_bits *b) {
    unsigned z = 0;
    while (!b->bad && !avc_u(b, 1)) if (++z > 30) { b->bad = 1; return 0; }
    return b->bad ? 0 : ((1u << z) - 1) + avc_u(b, z);
}
static int avc_se(avc_bits *b) { unsigned v = avc_ue(b); return v & 1 ? (int)((v + 1) / 2) : -(int)(v / 2); }
static avc_bits avc_rbsp(const uint8_t *nal, size_t n) {
    avc_bits b = {0};
    for (size_t i = 1; i < n && b.n < sizeof b.rbsp; i++) {
        if (i >= 3 && nal[i] == 3 && !nal[i - 1] && !nal[i - 2]) continue; // emulation_prevention_three_byte
        b.rbsp[b.n++] = nal[i];
    }
    return b;
}
static void avc_hrd(avc_bits *b) {
    unsigned cnt = avc_ue(b);
    if (cnt > 31) { b->bad = 1; return; }
    avc_u(b, 8);
    for (unsigned i = 0; i <= cnt && !b->bad; i++) { avc_ue(b); avc_ue(b); avc_u(b, 1); }
    avc_u(b, 20);
}
// Annex A, Table A-1: the level bounds DPB storage in macroblocks, not a constant four pictures.
static unsigned avc_dpb_mbs(unsigned level, unsigned profile, unsigned flags) {
    if (level == 11 && (flags & 16) && (profile == 66 || profile == 77 || profile == 88)) return 396; // level 1b
    switch (level) {
    case 9: case 10: return 396; case 11: return 900;
    case 12: case 13: case 20: return 2376; case 21: return 4752;
    case 22: case 30: return 8100; case 31: return 18000; case 32: return 20480;
    case 40: case 41: return 32768; case 42: return 34816;
    case 50: return 110400; case 51: case 52: return 184320;
    case 60: case 61: case 62: return 696320; default: return 0;
    }
}
static avc_sps avc_read_sps(const uint8_t *nal, size_t n) {
    avc_sps s = {.depth = n > 1 && nal[1] == 66 ? 0 : 4, .chroma = 1};
    if (n < 4 || n > 512 || (nal[0] & 31) != 7) return s;
    avc_bits b = avc_rbsp(nal, n);
    unsigned profile = avc_u(&b, 8), flags = avc_u(&b, 8), level = avc_u(&b, 8);
    unsigned id = avc_ue(&b); if (id > 31) return s; s.id = (int)id;
    switch (profile) {
    case 44: case 83: case 86: case 100: case 110: case 118: case 122:
    case 128: case 134: case 135: case 138: case 139: case 144: case 244: {
        unsigned chroma = avc_ue(&b);
        if (chroma > 3) return s;
        s.chroma = (int)chroma;
        if (chroma == 3 && avc_u(&b, 1)) return s; // separate colour planes: no frame DPB fast path
        avc_ue(&b); avc_ue(&b); avc_u(&b, 1); // bit depths, transform bypass
        if (avc_u(&b, 1)) for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u) && !b.bad; i++) {
            if (!avc_u(&b, 1)) continue;
            int last = 8, next = 8;
            for (unsigned j = 0; j < (i < 6 ? 16u : 64u) && !b.bad; j++) {
                if (next) { unsigned code = avc_ue(&b); int delta = code & 1 ? (int)((code + 1) / 2) : -(int)(code / 2); next = (last + delta) & 255; }
                if (next) last = next;
            }
        }
        break;
    }
    case 66: case 77: case 88: break;
    default: return s;
    }
    unsigned fn = avc_ue(&b); if (fn > 12) return s;
    s.frame_bits = (int)fn + 4;
    unsigned poc = avc_ue(&b);
    if (b.bad || poc > 2) return s;
    s.poc = (int)poc;
    if (poc == 2) s.depth = 0; // retain the prefix-only POC type 2 contract, even without the rest of the SPS
    if (poc == 0) { unsigned bits = avc_ue(&b); if (bits > 12) return s; s.poc_bits = (int)bits + 4; }
    if (poc == 1) {
        avc_u(&b, 1); avc_se(&b); avc_se(&b);
        unsigned cnt = avc_ue(&b); if (cnt > 255) return s;
        for (unsigned i = 0; i < cnt && !b.bad; i++) avc_se(&b);
    }
    unsigned refs = avc_ue(&b); if (refs > 16) return s; s.refs = (int)refs;
    avc_u(&b, 1); // gaps_in_frame_num_value_allowed_flag; actual gaps disable tracking below
    unsigned width = avc_ue(&b), height = avc_ue(&b);
    s.frame_only = (int)avc_u(&b, 1); if (!s.frame_only) avc_u(&b, 1);
    avc_u(&b, 1);
    if (avc_u(&b, 1)) for (int i = 0; i < 4; i++) avc_ue(&b); // cropping
    if (b.bad || width > 65535 || height > 65535) return s;
    uint64_t mbs = ((uint64_t)width + 1) * (height + 1) * (2 - s.frame_only);
    unsigned capacity = (unsigned)(avc_dpb_mbs(level, profile, flags) / mbs);
    if (!capacity) return s;
    if (capacity > 16) capacity = 16;
    unsigned dpb = capacity, reorder = capacity;
    if ((flags & 16) && (profile == 44 || profile == 86 || profile == 100 || profile == 110 || profile == 122 || profile == 244)) dpb = reorder = 0;
    if (avc_u(&b, 1)) { // VUI through bitstream_restriction; E.2.1 supplies the bounds when absent
        if (avc_u(&b, 1) && avc_u(&b, 8) == 255) avc_u(&b, 32);
        if (avc_u(&b, 1)) avc_u(&b, 1);
        if (avc_u(&b, 1)) { avc_u(&b, 4); if (avc_u(&b, 1)) avc_u(&b, 24); }
        if (avc_u(&b, 1)) { avc_ue(&b); avc_ue(&b); }
        if (avc_u(&b, 1)) { avc_u(&b, 32); avc_u(&b, 32); avc_u(&b, 1); }
        unsigned nal_hrd = avc_u(&b, 1); if (nal_hrd) avc_hrd(&b);
        unsigned vcl_hrd = avc_u(&b, 1); if (vcl_hrd) avc_hrd(&b);
        if (nal_hrd || vcl_hrd) avc_u(&b, 1);
        avc_u(&b, 1);
        if (avc_u(&b, 1)) {
            avc_u(&b, 1); for (int i = 0; i < 4; i++) avc_ue(&b);
            reorder = avc_ue(&b); dpb = avc_ue(&b);
        }
    }
    if (b.bad || dpb > capacity || dpb < refs || reorder > dpb) return s;
    s.dpb = dpb ? (int)dpb : 1; s.reorder = (int)reorder; s.valid = 1;
    if (profile != 66 && poc != 2) s.depth = (int)reorder;
    return s;
}

static avc_pps avc_read_pps(const uint8_t *nal, size_t n) {
    avc_pps p = {0}; avc_bits b = avc_rbsp(nal, n);
    unsigned id = avc_ue(&b), sid = avc_ue(&b);
    if (id > 255 || sid > 31) return p;
    p.id = (int)id; p.sps_id = (int)sid;
    avc_u(&b, 1); p.bottom = (int)avc_u(&b, 1);
    if (avc_ue(&b)) return p; // slice groups need a different slice-header layout
    for (int i = 0; i < 2; i++) { unsigned r = avc_ue(&b); if (r > 31) return p; p.refs[i] = (int)r + 1; }
    p.weighted = (int)avc_u(&b, 1); p.bipred = (int)avc_u(&b, 2);
    avc_se(&b); avc_se(&b); avc_se(&b); avc_u(&b, 2); p.redundant = (int)avc_u(&b, 1);
    p.valid = !b.bad; return p;
}

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    if (!n) return;
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
    a->ordered = 0; // a changed parameter set cannot inherit the previous DPB interpretation
    if (t == 7) { a->sp = avc_read_sps(nal, n); a->depth = a->sp.depth; }
    else a->pp = avc_read_pps(nal, n);
}
// walk the NAL units of p, an Annex B byte stream; fn gets each unit without its start code. Zero bytes before a start code (leading_zero_8bits, the first byte of a
// 4-byte start code, trailing_zero_8bits) are not part of the unit. 0 = p does not begin with a start code, and fn saw nothing.
// The SDK's demuxer converts the MP4's length-prefixed samples to start codes, also when it hands over the avcC box (hle_movie.c), so packets and csd are Annex B. Length
// prefixes are not taken as a fallback: a 4-byte length from 256 to 511 begins 00 00 01, so a packet's framing cannot be told from its bytes (issue #629)
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static int each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s == n) return 0;
    for (size_t i = 0; i < s; i++) if (p[i]) return 0;
    while (s < n) {
        size_t b = s + 3, e = next_sc(p, n, b), end = e;
        while (end > b && !p[end - 1]) end--;
        if (end > b) fn(ctx, p + b, end - b);
        s = e;
    }
    return 1;
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) {
    if (!each_nal(p, n, ps_only, a) && !a->red++) LOG("[RED] movie: AVC parameter sets without an Annex B start code (%zu bytes), not taken", n);
}
// AVCDecoderConfigurationRecord: version, profile, compatibility, level, NAL length size - 1, SPS count (low 5 bits) and 16-bit-length SPS, PPS count and 16-bit-length PPS
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}

typedef struct { int valid, frame, lsb, bottom, reference, idr, discard; } avc_slice;
static avc_slice avc_read_slice(const mavc *a, const uint8_t *nal, size_t n) {
    avc_slice s = {0}; const avc_sps *sp = &a->sp; const avc_pps *pp = &a->pp;
    int type = nal[0] & 31;
    if (!sp->valid || !pp->valid || pp->sps_id != sp->id || !sp->frame_only || sp->poc != 0 || (type != 1 && type != 5)) return s;
    avc_bits b = avc_rbsp(nal, n);
    avc_ue(&b); unsigned slice = avc_ue(&b), pid = avc_ue(&b);
    if (slice > 9 || pid != (unsigned)pp->id) return s;
    slice %= 5;
    s.reference = (nal[0] & 96) != 0; s.idr = type == 5;
    s.frame = (int)avc_u(&b, (unsigned)sp->frame_bits);
    if (s.idr) avc_ue(&b);
    s.lsb = (int)avc_u(&b, (unsigned)sp->poc_bits);
    if (pp->bottom) s.bottom = avc_se(&b);
    if (pp->redundant && avc_ue(&b)) return s;
    if (slice == 1) avc_u(&b, 1); // direct_spatial_mv_pred_flag
    int refs[2] = {pp->refs[0], pp->refs[1]};
    if ((slice == 0 || slice == 1 || slice == 3) && avc_u(&b, 1)) {
        unsigned r = avc_ue(&b); if (r > 31) return s; refs[0] = (int)r + 1;
        if (slice == 1) { r = avc_ue(&b); if (r > 31) return s; refs[1] = (int)r + 1; }
    }
    if (slice != 2 && slice != 4) for (int list = 0; list < (slice == 1 ? 2 : 1); list++) {
        if (avc_u(&b, 1)) for (int i = 0; !b.bad; i++) {
            unsigned op = avc_ue(&b); if (op == 3) break;
            if (op > 2 || i >= 32) { b.bad = 1; break; }
            avc_ue(&b);
        }
    }
    if ((pp->weighted && (slice == 0 || slice == 3)) || (pp->bipred == 1 && slice == 1)) {
        if (avc_ue(&b) > 7) return s;
        if (sp->chroma && avc_ue(&b) > 7) return s;
        for (int list = 0; list < (slice == 1 ? 2 : 1); list++) for (int i = 0; i < refs[list] && !b.bad; i++) {
            if (avc_u(&b, 1)) { avc_se(&b); avc_se(&b); }
            if (sp->chroma && avc_u(&b, 1)) for (int j = 0; j < 4; j++) avc_se(&b);
        }
    }
    if (s.reference) {
        if (s.idr) { s.discard = (int)avc_u(&b, 1); if (avc_u(&b, 1)) return s; } // long-term IDR reference
        else if (avc_u(&b, 1)) return s; // adaptive marking, including MMCO 5: no sliding-window guarantee
    }
    s.valid = !b.bad && (!s.idr || (s.reference && !s.frame)); return s;
}

// The optimized DPB is deliberately limited to progressive POC type 0 with sliding-window reference marking.
// A changed/unsupported header, a frame_num gap, or a decode error returns to the existing lookahead/EOS path until an IDR.
static void avc_order_stop(mavc *a) {
    a->ordered = 0;
    for (int i = 1; i < a->nq; i++) {
        __typeof__(a->q[0]) p = a->q[i]; int k = i;
        while (k && a->q[k - 1].order.sequence == p.order.sequence && a->q[k - 1].pts > p.pts) { a->q[k] = a->q[k - 1]; k--; }
        a->q[k] = p;
    }
}
static avc_frame avc_order_picture(mavc *a, const avc_slice *s) {
    avc_frame f = {.sequence = a->sequence};
    if (!a->depth || !s->valid || !a->sp.refs || (!s->idr && (!a->ordered || s->frame != (a->prev_frame + 1) % (1 << a->sp.frame_bits)))) {
        avc_order_stop(a); return f;
    }
    if (s->idr) {
        a->ordered = 1; a->refs = 0; a->prev_frame = a->prev_lsb = 0; a->prev_msb = 0;
        if (s->discard) {
            for (int i = 0; i < a->nq; i++) CVPixelBufferRelease(a->q[i].pb);
            a->nq = 0;
        }
    }
    int max_lsb = 1 << a->sp.poc_bits; int64_t msb = a->prev_msb;
    if (s->lsb < a->prev_lsb && a->prev_lsb - s->lsb >= max_lsb / 2) msb += max_lsb;
    else if (s->lsb > a->prev_lsb && s->lsb - a->prev_lsb > max_lsb / 2) msb -= max_lsb;
    f.poc = msb + s->lsb + (s->bottom < 0 ? s->bottom : 0); f.ordered = 1;
    if (s->reference) { f.ref = ++a->refs; a->prev_frame = s->frame; a->prev_lsb = s->lsb; a->prev_msb = msb; }
    return f;
}
static int avc_active_ref(const mavc *a, uint64_t ref) { return ref && ref <= a->refs && a->refs - ref < (uint64_t)a->sp.refs; }
// H.264 C.4.4 / C.4.5: output does NOT free a reference buffer. Sliding-window marking retires the oldest reference when a new
// one arrives. DPB fullness is active references plus pending non-references/retired references; the incoming picture needs space
// too. Bump the lowest POC until it fits (a non-reference can be output directly), or E.2.1's reorder bound is satisfied. Ready pictures
// remain in q until the player has an output slot, but no longer count as needed for output in the codec DPB. No PTS cadence is inferred.
static void avc_bump(mavc *a) {
    if (!a->ordered) return;
    int full = a->refs < (uint64_t)a->sp.refs ? (int)a->refs : a->sp.refs, pending = 0;
    for (int i = 0; i < a->nq; i++) if (a->q[i].order.sequence == a->sequence && !a->q[i].ready) {
        pending++; if (!avc_active_ref(a, a->q[i].order.ref)) full++;
    }
    for (int i = 0; i < a->nq && (full > a->sp.dpb || pending > a->sp.reorder); i++) {
        if (a->q[i].order.sequence != a->sequence || a->q[i].ready) continue;
        a->q[i].ready = 1; pending--;
        if (!avc_active_ref(a, a->q[i].order.ref)) full--;
    }
}

static void on_frame(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)fl; (void)dur; mavc *a = ref;
    if (st) { a->history.status = st; return; }
    if (a->history.replaying || !img) return;
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC reorder queue full, dropping the oldest frame"); CVPixelBufferRelease(a->q[0].pb); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    int64_t t = pts.timescale ? pts.value * 1000000 / pts.timescale : 0;
    avc_frame order = src ? *(const avc_frame *)src : (avc_frame){.sequence = a->sequence};
    if (!order.ordered) avc_order_stop(a);
    int k = a->nq;
    while (k > 0 && (a->q[k - 1].order.sequence > order.sequence ||
                    (a->q[k - 1].order.sequence == order.sequence && (a->ordered ? a->q[k - 1].order.poc > order.poc : a->q[k - 1].pts > t)))) { a->q[k] = a->q[k - 1]; k--; }
    a->q[k].pb = CVPixelBufferRetain(img); a->q[k].pts = t; a->q[k].order = order; a->q[k].ready = 0; a->nq++;
    avc_bump(a);
}
static int open_session(mavc *a) {
    close_session(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    const uint8_t *ps[2] = {a->sps, a->pps}; size_t sz[2] = {(size_t)a->nsps, (size_t)a->npps};
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, ps, sz, 4, &a->fd);
    if (st) { if (!a->red++) LOG("[RED] movie: AVC parameter sets give no format description (%d), outputting black frames", (int)st); return 0; }
    int32_t pf = a->rgba ? kCVPixelFormatType_32BGRA : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;   // NV12, video range: what the SDK's decoder hands out; BGRA (converted by VideoToolbox with the stream's colour matrix) for RGBA output
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *k[] = {kCVPixelBufferPixelFormatTypeKey}, *v[] = {n};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(n);
    VTDecompressionOutputCallbackRecord cb = {on_frame, a};
    st = movie_vt_create(&a->history, a->fd, attrs, &cb, &a->vs); CFRelease(attrs);
    if (st == MOVIE_VT_CANCELLED) return -1;
    if (st) { if (!a->red++) LOG("[RED] movie: VideoToolbox cannot create an AVC decode session (%d), outputting black frames", (int)st); a->vs = NULL; return 0; }
    CMVideoDimensions dm = CMVideoFormatDescriptionGetDimensions(a->fd);
    LOG("[movie] AVC: VideoToolbox decoding %d×%d, profile %d, reorder depth %d", dm.width, dm.height, a->nsps > 1 ? a->sps[1] : 0, a->depth);
    a->dirty = 0; return 1;
}
typedef struct { mavc *a; size_t len; int idr, picture; avc_slice slice; } au_ctx;
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; mavc *a = c->a; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    if (t >= 1 && t <= 5) {
        avc_slice s = avc_read_slice(a, nal, n);
        if (!c->picture) c->slice = s;
        else if (!s.valid || s.frame != c->slice.frame || s.lsb != c->slice.lsb || s.bottom != c->slice.bottom ||
                 s.reference != c->slice.reference || s.idr != c->slice.idr || s.discard != c->slice.discard) c->slice.valid = 0;
        c->picture = 1;
    }
    if (t == 5) c->idr = 1;
    if (c->len + 4 + n > a->aucap) { a->aucap = (c->len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + c->len; o[0] = (uint8_t)(n >> 24); o[1] = (uint8_t)(n >> 16); o[2] = (uint8_t)(n >> 8); o[3] = (uint8_t)n;
    memcpy(o + 4, nal, n); c->len += 4 + n;
}
// PTS may restart when an MP4 loops without a Flush or EOS. Sorting all pictures
// by that time strands the previous loop's last `depth` pictures forever, so
// the next loop effectively has no reorder lookahead. An IDR starts a new coded
// sequence: previous pictures precede it, and only the current sequence needs
// lookahead. Keep them all; neither a timestamp clamp nor a playback delay is needed.
static void avc_picture_sequence(mavc *a, int idr) {
    if (idr && a->saw_picture) a->sequence++;
    a->saw_picture = 1;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0};
    if (n > 0 && !each_nal(p, (size_t)n, au_add, &c)) { if (!a->red++) LOG("[RED] movie: AVC packet without an Annex B start code (%d bytes), not decoded", n); return -1; }
    if (!c.len) return 0;
    if (a->dirty || c.idr) { movie_vt_clear(&a->history); a->history.key = c.idr; }
    if (a->dirty || !a->vs) { int opened = open_session(a); if (opened <= 0) return opened < 0 ? 0 : -1; }
    if (c.picture) avc_picture_sequence(a, c.idr);
    avc_frame order = c.picture ? avc_order_picture(a, &c.slice) : (avc_frame){.sequence = a->sequence};
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, c.len, NULL, NULL, 0, c.len, 0, &bb);
    if (!st) st = CMBlockBufferReplaceDataBytes(a->au, bb, 0, c.len);
    CMSampleTimingInfo ti = {kCMTimeInvalid, CMTimeMake(pts_us, 1000000), kCMTimeInvalid}; size_t ssz = c.len;
    if (!st) st = CMSampleBufferCreateReady(NULL, bb, a->fd, 1, 1, &ti, 1, &ssz, &sb);
    if (!st) {
        st = movie_vt_submit(&a->history, a->vs, sb, &order);
        while (st == kVTInvalidSessionErr && a->history.key) {
            int opened = open_session(a);
            if (opened < 0) st = MOVIE_VT_CANCELLED;
            else if (opened) {
                st = movie_vt_replay(&a->history, a->vs);
                if (!st) st = movie_vt_submit(&a->history, a->vs, sb, &order);
            } else break;
        }
        if (!st) movie_vt_remember(&a->history, sb);
    }
    if (sb) CFRelease(sb); if (bb) CFRelease(bb);
    if (st == MOVIE_VT_CANCELLED) return 0;
    if (st) { avc_order_stop(a); if (!a->red++) LOG("[RED] movie: AVC decode submission failed (%d, first case)", (int)st); return -1; }
    return 0;
}
void mavc_set_rgba(mavc *a) { if (!a->rgba) { a->rgba = 1; a->dirty = 1; } }
// BGRA picture → RGBA rows of w pixels (stride w * 4); a smaller picture is padded with opaque black, a larger one cropped
static void take_rgba(CVPixelBufferRef pb, uint8_t *dst, int w, int h) {
    int pw = (int)CVPixelBufferGetWidth(pb), ph = (int)CVPixelBufferGetHeight(pb), cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    const uint8_t *src = CVPixelBufferGetBaseAddress(pb); size_t ss = CVPixelBufferGetBytesPerRow(pb);
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    for (int r = 0; r < chh; r++) {
        const uint8_t *s = src + r * ss; uint8_t *o = dst + (size_t)r * w * 4; int x = 0;
        for (; x + 16 <= cw; x += 16) { uint8x16x4_t v = vld4q_u8(s + 4 * x); uint8x16_t t = v.val[0]; v.val[0] = v.val[2]; v.val[2] = t; vst4q_u8(o + 4 * x, v); }
        for (; x < cw; x++) { o[4 * x] = s[4 * x + 2]; o[4 * x + 1] = s[4 * x + 1]; o[4 * x + 2] = s[4 * x]; o[4 * x + 3] = 255; }
    }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (!a->nq || (!flush && !mavc_ready(a))) return 0;
    CVPixelBufferRef pb = a->q[0].pb; if (pts_us) *pts_us = a->q[0].pts;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (a->rgba) { take_rgba(pb, dst, w, h); CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); CVPixelBufferRelease(pb); return 1; }
    int pw = (int)CVPixelBufferGetWidthOfPlane(pb, 0), ph = (int)CVPixelBufferGetHeightOfPlane(pb, 0);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
    for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, y + r * ys, (size_t)cw);
    size_t uvb = (size_t)((cw + 1) & ~1); if (uvb > (size_t)w) uvb = (size_t)w;   // interleaved U V: one byte per luma column
    for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, uv + r * uvs, uvb);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(pb);
    return 1;
}
