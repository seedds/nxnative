// movie::VideoDecoder of the Switch 2 movie library (subsdk0), taken over with libvpx. The library's own goes through nn::codec to a decoder service the host does not have, and aborts in nv::InitCheck. Its
// nn::movie::MediaExtractor runs as it is, so the game demuxes and this decodes, as hle_movie.c does for the older movie::Decoder.
// The protocol, from Mario Kart World's player (main+0x808e80, main+0x8084c0) and the library's accessors: the game registers VideoFrames with its own buffers, then on the input event hands packets to DecodeAsync and
// on the output event takes frames with AcquireVideoFrame and gives them back with ReleaseVideoFrame. A VideoFrame is the game's: its accessors run as they are, and this writes the fields they read.
// Open: 4:2:0 8-bit VP9 and VP8 only; DecodeAsync decodes on the caller's thread rather than asynchronously, so a slow frame holds up the player.
#include "hle.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

enum { NX2_MD_FRAMES = 64, VF_BUF = 0x08, VF_FILLED = 0x28, VF_SIZE = 0x48, VF_EOS = 0x88, BUF_DATA = 0x00, BUF_FILLED = 0x10, BUF_FLAGS = 0x20, BUF_FLAG_EOS = 8 };
typedef struct nx2_pkt { struct nx2_pkt *next; uint64_t n; int eos; uint8_t data[]; } nx2_pkt;
typedef struct {
    pthread_mutex_t mu;
    vpx_codec_ctx_t vpx; int vpx_on, mime, bad;
    uint8_t *frames[NX2_MD_FRAMES]; int nframes;
    uint8_t *free_[NX2_MD_FRAMES]; int nfree;
    uint8_t *ready[NX2_MD_FRAMES]; int nready;
    nx2_pkt *qh, *qt;   // packets not decoded yet
    _Alignas(16) uint8_t in_ev[0x100], out_ev[0x100];
} nx2_md;
#define MD(self) (*(nx2_md **)(self))

static uint8_t *md_take(nx2_md *d) { return d->nfree ? d->free_[--d->nfree] : NULL; }
static void md_push(nx2_md *d, uint8_t *f) { if (d->nready < NX2_MD_FRAMES) d->ready[d->nready++] = f; hle_signal_system_event(d->out_ev); }
// I420 to NV12: rows packed at the picture's width, chroma interleaved
static void md_nv12(uint8_t *o, const vpx_image_t *im) {
    uint32_t w = im->d_w, h = im->d_h, cw = (w + 1) / 2, ch = (h + 1) / 2; uint64_t y = (uint64_t)w * h;
    for (uint32_t r = 0; r < h; r++) memcpy(o + (uint64_t)r * w, im->planes[0] + (size_t)r * im->stride[0], w);
    uint8_t *c = o + y;
    for (uint32_t r = 0; r < ch; r++) { const uint8_t *u = im->planes[1] + (size_t)r * im->stride[1], *v = im->planes[2] + (size_t)r * im->stride[2]; for (uint32_t x = 0; x < cw; x++) { *c++ = u[x]; *c++ = v[x]; } }
}
static void md_out(nx2_md *d, const vpx_image_t *im) {
    uint32_t w = im->d_w, h = im->d_h; uint64_t y = (uint64_t)w * h, uv = (uint64_t)((w + 1) / 2) * 2 * ((h + 1) / 2);
    uint8_t *f = md_take(d), *o; uint64_t cap; memcpy(&o, f + VF_BUF, 8); memcpy(&cap, f + VF_SIZE, 8);
    if (o && y + uv <= cap) md_nv12(o, im);
    else { static int n; if (!n++) LOG("[RED] movie: a picture of %llu bytes does not fit its %llu-byte buffer", (unsigned long long)(y + uv), (unsigned long long)cap); y = uv = 0; }
    memcpy(f + VF_FILLED, &y, 8); memcpy(f + VF_FILLED + 8, &uv, 8); f[VF_EOS] = 0;
    md_push(d, f);
}
static void md_decode(nx2_md *d, const uint8_t *p, uint64_t n) {
    if (vpx_codec_decode(&d->vpx, p, (unsigned)n, NULL, 0)) { if (!d->bad++) LOG("[RED] movie: decode error (%llu bytes): %s", (unsigned long long)n, vpx_codec_error(&d->vpx)); return; }
    vpx_codec_iter_t it = NULL; vpx_image_t *im, *last = NULL;
    while ((im = vpx_codec_get_frame(&d->vpx, &it))) last = im;
    if (!last) return;
    if (last->fmt == VPX_IMG_FMT_I420) md_out(d, last);
    else { static int nr; if (!nr++) LOG("[RED] movie: a picture dropped: format %#x is not 4:2:0 8-bit", last->fmt); }
}
// A packet gives at most one picture, so one decodes only with a frame free for it. The rest wait as packets: the game can feed faster than it takes frames, and a dropped picture would break the movie
static void md_queue(nx2_md *d, const uint8_t *p, uint64_t n, int eos) {
    nx2_pkt *k = malloc(sizeof *k + n); if (!k) { LOG("[RED] movie: out of memory for a packet"); return; }
    k->next = NULL; k->n = n; k->eos = eos; if (n) memcpy(k->data, p, n);
    if (d->qt) d->qt->next = k; else d->qh = k;
    d->qt = k;
}
static void md_drain(nx2_md *d) {
    while (d->qh && d->nfree) {
        nx2_pkt *k = d->qh; d->qh = k->next; if (!d->qh) d->qt = NULL;
        if (k->eos) { uint8_t *f = md_take(d); uint64_t z = 0; memcpy(f + VF_FILLED, &z, 8); memcpy(f + VF_FILLED + 8, &z, 8); f[VF_EOS] = 1; md_push(d, f); }
        else md_decode(d, k->data, k->n);
        free(k);
    }
    if (d->nfree && !d->qh) hle_signal_system_event(d->in_ev);
}
static void md_clear(nx2_md *d) { for (nx2_pkt *k = d->qh, *x; k; k = x) { x = k->next; free(k); } d->qh = d->qt = NULL; }

HLE_FORCE("_ZN5movie12VideoDecoderC1Ev", "the Switch 2 movie decoder: the host's, on libvpx", void, h_nx2_vd_ctor, void *self) { memset(self, 0, 0x50); }
HLE_FORCE("_ZN5movie12VideoDecoderD1Ev", "the Switch 2 movie decoder", void, h_nx2_vd_dtor, void *self) { (void)self; }
HLE_FORCE("_ZN5movie12VideoDecoder10InitializeEPvmN2nn5movie13MediaMimeTypeENS_24VideoDecoderOutputFormatENS_14VideoCodecModeE", "the Switch 2 movie decoder", Result, h_nx2_vd_init, void *self, void *mem, uint64_t size, int mime, int fmt, int mode) {
    (void)mem; (void)size;
    nx2_md *d = calloc(1, sizeof *d); pthread_mutex_init(&d->mu, NULL); d->mime = mime; MD(self) = d;
    hle_bind_system_event(d->in_ev, 1); hle_bind_system_event(d->out_ev, 1);
    LOG("[movie] VideoDecoder: mime %d, output format %d, mode %d", mime, fmt, mode);
    return RES_OK;
}
HLE_FORCE("_ZN5movie12VideoDecoder9ConfigureEPKN2nn5movie13MediaPropertyEPKvm", "the Switch 2 movie decoder: libvpx reads the stream's own headers", Result, h_nx2_vd_conf, void *self, const void *prop, const void *cfg, uint64_t n) { (void)self; (void)prop; (void)cfg; (void)n; return RES_OK; }
HLE_FORCE("_ZN5movie12VideoDecoder18RegisterVideoFrameEPPNS_10VideoFrameEi", "the Switch 2 movie decoder", Result, h_nx2_vd_reg, void *self, uint8_t **frames, int n) {
    nx2_md *d = MD(self); if (n > NX2_MD_FRAMES) { LOG("[RED] movie: %d frames registered", n); n = NX2_MD_FRAMES; }
    pthread_mutex_lock(&d->mu); d->nframes = n; d->nfree = 0;
    for (int i = n - 1; i >= 0; i--) { d->frames[i] = frames[i]; d->free_[d->nfree++] = frames[i]; }
    pthread_mutex_unlock(&d->mu); return RES_OK;
}
HLE_FORCE("_ZN5movie12VideoDecoder18GetInputReadyEventEPPN2nn2os15SystemEventTypeE", "the Switch 2 movie decoder", Result, h_nx2_vd_inev, void *self, void **out) { *out = MD(self)->in_ev; return RES_OK; }
HLE_FORCE("_ZN5movie12VideoDecoder19GetOutputReadyEventEPPN2nn2os15SystemEventTypeE", "the Switch 2 movie decoder", Result, h_nx2_vd_outev, void *self, void **out) { *out = MD(self)->out_ev; return RES_OK; }
HLE_FORCE("_ZN5movie12VideoDecoder5StartEv", "the Switch 2 movie decoder", Result, h_nx2_vd_start, void *self) { hle_signal_system_event(MD(self)->in_ev); return RES_OK; }
HLE_FORCE("_ZN5movie12VideoDecoder11DecodeAsyncEPKN2nn5movie6BufferE", "the Switch 2 movie decoder: decoded here with libvpx", Result, h_nx2_vd_dec, void *self, const uint8_t *buf) {
    nx2_md *d = MD(self); const uint8_t *p; uint64_t n; uint32_t flags;
    memcpy(&p, buf + BUF_DATA, 8); memcpy(&n, buf + BUF_FILLED, 8); memcpy(&flags, buf + BUF_FLAGS, 4);
    pthread_mutex_lock(&d->mu);
    if (!d->vpx_on) {
        // libvpx has no MIME type here: a VP8 stream's first frame says so, and the movies seen are VP9
        vpx_codec_dec_cfg_t cfg = { .threads = 4 };
        vpx_codec_iface_t *iface = vpx_codec_vp9_dx();
        if (vpx_codec_dec_init(&d->vpx, iface, &cfg, 0)) { LOG("[RED] movie: libvpx failed to start"); pthread_mutex_unlock(&d->mu); return RES_FAIL; }
        vpx_codec_control(&d->vpx, VP9D_SET_ROW_MT, 1); d->vpx_on = 1;
    }
    if (n && p) md_queue(d, p, n, 0);
    if (flags & BUF_FLAG_EOS) md_queue(d, NULL, 0, 1);
    md_drain(d);
    pthread_mutex_unlock(&d->mu);
    return RES_OK;
}
HLE_FORCE("_ZN5movie12VideoDecoder17AcquireVideoFrameEPPNS_10VideoFrameE", "the Switch 2 movie decoder", Result, h_nx2_vd_acq, void *self, uint8_t **out) {
    nx2_md *d = MD(self); pthread_mutex_lock(&d->mu); Result r = RES_OK;
    if (d->nready) { *out = d->ready[0]; memmove(d->ready, d->ready + 1, (size_t)--d->nready * sizeof *d->ready); if (d->nready) hle_signal_system_event(d->out_ev); }
    else { *out = NULL; r = RES_FAIL; }
    pthread_mutex_unlock(&d->mu); return r;
}
HLE_FORCE("_ZN5movie12VideoDecoder17ReleaseVideoFrameEPNS_10VideoFrameE", "the Switch 2 movie decoder", Result, h_nx2_vd_rel, void *self, uint8_t *f) {
    nx2_md *d = MD(self); pthread_mutex_lock(&d->mu);
    if (f && d->nfree < NX2_MD_FRAMES) d->free_[d->nfree++] = f;
    md_drain(d);
    pthread_mutex_unlock(&d->mu); return RES_OK;
}
HLE_FORCE("_ZN5movie12VideoDecoder5FlushEv", "the Switch 2 movie decoder", Result, h_nx2_vd_flush, void *self) {
    nx2_md *d = MD(self); pthread_mutex_lock(&d->mu);
    md_clear(d); d->nfree = d->nready = 0; for (int i = d->nframes - 1; i >= 0; i--) d->free_[d->nfree++] = d->frames[i];
    if (d->vpx_on) { vpx_codec_destroy(&d->vpx); d->vpx_on = 0; }
    hle_signal_system_event(d->in_ev); pthread_mutex_unlock(&d->mu); return RES_OK;
}
HLE_FORCE("_ZN5movie12VideoDecoder4StopEv", "the Switch 2 movie decoder", Result, h_nx2_vd_stop, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN5movie12VideoDecoder8FinalizeEv", "the Switch 2 movie decoder", Result, h_nx2_vd_fini, void *self) {
    nx2_md *d = MD(self); if (!d) return RES_OK;
    md_clear(d); if (d->vpx_on) vpx_codec_destroy(&d->vpx);
    MD(self) = NULL;   // the two events stay: a player may still hold them, and a movie is few enough
    return RES_OK;
}
