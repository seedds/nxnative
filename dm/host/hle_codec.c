// nn::codec::HardwareOpusDecoder: the real device decodes in hardware via the hwopus service (the game's audio and cutscene tracks are all Opus; this is not an online feature).
// Host: the constructor instead constructs the SDK's software OpusDecoder in the same memory (the sdk bundles libopus, native ARM).
// Basis: both are implementations of nn::codec::IOpusDecoder, with the 10 vtable slots identical one by one in order and signature (destructor×2, GetWorkBufferSize, IsInitialized,
// Initialize, Finalize, GetSampleRate, GetChannelCount, Reset, DecodeInterleaved); the game allocates 0x40 bytes and the software object is 0x30 bytes.
#include "hle.h"

HLE_FORCE("_ZN2nn5codec19HardwareOpusDecoderC1Ei", "hwopus service; replaced by the SDK software OpusDecoder (same IOpusDecoder virtual interface)",
          void, h_codec_HwOpusCtor, void *self, int option) {
    (void)option;
    static void (*soft_ctor)(void *);
    if (!soft_ctor && !(soft_ctor = guest_sym("_ZN2nn5codec11OpusDecoderC1Ev"))) die("missing the OpusDecoder constructor");
    soft_ctor(self);
}

// The C interface to the same hardware decoder (Tears of the Kingdom, SDK 20.5.22: GetHardwareOpusDecoderWorkBufferSize opens hwopus and aborts in sm::Initialize).
// Each call goes to the sdk's software C interface instead. The software decoder state lives in a host block kept per game decoder object (objmap), so the size of the
// game's HardwareOpusDecoderType does not matter; the game's work buffer is sized by GetOpusDecoderWorkBufferSize and handed to the software decoder as is.
#define SOFT_OPUS_STATE 0x200   // bytes for the software OpusDecoderType (larger than the SDK's)
static void *opus_fn(const char *name) { void *f = guest_sym(name); if (!f) die("sdk lacks %s", name); return f; }
static void *opus_soft(const void *hw) { void *s = objmap_get(hw); if (!s) die("Opus decoder %p was not initialized by the host", hw); return s; }
HLE_FORCE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusWorkSize, int rate, int ch, int option) {
    (void)option; size_t (*f)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii"); return f(rate, ch);
}
// SDK 11's form without the option (Big Brain Academy: Brain vs. Brain sizes its decoders with it early in nnMain, main+0x31f8e8; the SDK's own opens hwopus and aborts resolving the service)
HLE_FORCE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEii", "hwopus service; work buffer computed for the software decoder (SDK 11 two-argument version)", size_t, h_codec_HwOpusWorkSize2,
          int rate, int ch) {
    size_t (*f)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii"); return f(rate, ch);
}
HLE_FORCE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", "hwopus service; replaced by the software OpusDecoder", int, h_codec_HwOpusInit,
          void *hw, int rate, int ch, void *buf, size_t size, int option) {
    (void)option; int (*f)(void *, int, int, void *, size_t) = opus_fn("_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
    void *s = objmap_get(hw); if (!s) { s = calloc(1, SOFT_OPUS_STATE); objmap_put(hw, s); }
    return f(s, rate, ch, buf, size);
}
static int opus_mgr_dec_fini(void *gd); static int opus_mgr_dec_reset(void *gd);   // a decoder of a manager (below)
HLE_FORCE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", void, h_codec_HwOpusFini, void *hw) {
    if (opus_mgr_dec_fini(hw)) return;
    void (*f)(void *) = opus_fn("_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE");
    void *s = objmap_del(hw); if (s) { f(s); free(s); }
}
HLE_FORCE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusRate, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusCh, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", void, h_codec_HwOpusReset, void *hw) {
    if (opus_mgr_dec_reset(hw)) return;
    void (*f)(void *) = opus_fn("_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE"); f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", "hwopus service; software OpusDecoder", int, h_codec_HwOpusDecode,
          void *hw, size_t *consumed, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size) {
    int (*f)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
    return f(opus_soft(hw), consumed, samples, out, out_size, in, in_size);
}

// nn::codec::HardwareOpusMultiStreamDecoder (Hyrule Warriors: Age of Calamity, SDK 9.6.0: its sound streams are multi-stream Opus): the same swap as HardwareOpusDecoder above.
// Both classes implement one interface: their 12 vtable slots match one by one (destructor×2, GetWorkBufferSize, IsInitialized, Initialize, Finalize, GetSampleRate, GetChannelCount,
// GetTotalStreamCount, GetStereoStreamCount, Reset, DecodeInterleaved); the hardware object is 0x48 bytes, the software one 0x38. Both destructors are a bare ret.
// Without it the game's KIDS resource loader never finished the first sound stream (asset/data/0x272c6efb.file) and the main thread waited for it forever (main+0x8d9bc8)
HLE_FORCE("_ZN2nn5codec30HardwareOpusMultiStreamDecoderC1Ei", "hwopus service; replaced by the SDK software OpusMultiStreamDecoder (same virtual interface)",
          void, h_codec_HwOpusMsCtor, void *self, int option) {
    (void)option; void (*f)(void *) = opus_fn("_ZN2nn5codec22OpusMultiStreamDecoderC1Ev"); f(self);
}
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusMsWorkSize,
          int rate, int ch, int streams, int stereo) {
    size_t (*f)(int, int, int, int) = opus_fn("_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii"); return f(rate, ch, streams, stereo);
}

// The C interface to the hardware multi-stream decoder (Unicorn Overlord, SDK 16.2.4: CRI's Opus streams; GetHardwareOpusMultiStreamDecoderWorkBufferSize opened hwopus and the
// game aborted in sm::Initialize at nnMain). The same swap as the single-stream C interface above: software state per game decoder object, the work buffer sized for the software decoder.
// The software OpusMultiStreamDecoderType is 0x28 bytes (InitializeOpusMultiStreamDecoder writes up to +0x24), well inside SOFT_OPUS_STATE.
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusMsWorkSize5,
          int rate, int ch, int streams, int stereo, int option) {
    (void)option; size_t (*f)(int, int, int, int) = opus_fn("_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii"); return f(rate, ch, streams, stereo);
}
HLE_FORCE("_ZN2nn5codec40InitializeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeEiiiiPKhPvmi", "hwopus service; replaced by the software OpusMultiStreamDecoder",
          int, h_codec_HwOpusMsInit, void *hw, int rate, int ch, int streams, int stereo, const uint8_t *mapping, void *buf, size_t size, int option) {
    (void)option; int (*f)(void *, int, int, int, int, const uint8_t *, void *, size_t) = opus_fn("_ZN2nn5codec32InitializeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeEiiiiPKhPvm");
    void *s = objmap_get(hw); if (!s) { s = calloc(1, SOFT_OPUS_STATE); objmap_put(hw, s); }
    return f(s, rate, ch, streams, stereo, mapping, buf, size);
}
HLE_FORCE("_ZN2nn5codec38FinalizeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", void, h_codec_HwOpusMsFini, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec30FinalizeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE");
    void *s = objmap_del(hw); if (s) { f(s); free(s); }
}
HLE_FORCE("_ZN2nn5codec43GetHardwareOpusMultiStreamDecoderSampleRateEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsRate, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec35GetOpusMultiStreamDecoderSampleRateEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec45GetHardwareOpusMultiStreamDecoderChannelCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsCh, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec37GetOpusMultiStreamDecoderChannelCountEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusMultiStreamDecoderTotalStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsTotal, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec41GetOpusMultiStreamDecoderTotalStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec50GetHardwareOpusMultiStreamDecoderStereoStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsStereo, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec42GetOpusMultiStreamDecoderStereoStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec35ResetHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", void, h_codec_HwOpusMsReset, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec27ResetOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE"); f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec44DecodeOpusMultiStreamInterleavedWithHardwareEPNS0_34HardwareOpusMultiStreamDecoderTypeEPmPiPsmPKvm", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsDecode,
          void *hw, size_t *consumed, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size) {
    int (*f)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec32DecodeOpusMultiStreamInterleavedEPNS0_26OpusMultiStreamDecoderTypeEPmPiPsmPKvm");
    return f(opus_soft(hw), consumed, samples, out, out_size, in, in_size);
}
// The hardware's decode time of the last frame (nn::TimeSpan); the software decoder runs inline on the caller's thread, so none is reported
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderProcessingTimeEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; no hardware decode time", int64_t, h_codec_HwOpusMsTime, const void *hw) {
    (void)hw; return 0;
}

// ---------- SDK 21's hardware Opus decoder manager (Xenoblade Chronicles 2's Switch 2 Edition, SDK 21.4.4) ----------
// The asynchronous interface over hwopus: a manager (the game's 0x18-byte HardwareOpusDecoderManagerType, with a system event) owns decoders (0x30-byte HardwareOpusDecoderType each)
// and a pool of I/O buffers. The game enqueues one packet per call (Nintendo's 8-byte header, big-endian size and final range, then the packet: what DecodeOpusInterleaved takes),
// calls ExecuteHardwareOpusDecoderDecoding on each manager once per audio frame, then dequeues each finished frame (960 samples a channel at 48 kHz) and reads the request counts
// (main+0xad2810, main+0xad2e80, main+0xad3140). Host: Execute decodes every waiting packet with the sdk's software OpusDecoder on the caller's thread and signals the event, so a request is
// waiting until Execute, never executing, and completed until dequeued; each holds one I/O buffer from Enqueue to Dequeue. The game tells an initialized manager by its first word
// (main+0xad2828), so that holds the host state. Without these the SDK opened hwopus itself and aborted with 0xe15 (sdk+0x323180, under GetHardwareOpusDecoderManagerWorkBufferSize)
#define OPUS_MAX_PACKET 0x1800                 // header + packet: a 20 ms frame of at most 6 streams' bytes is far below it
#define OPUS_MAX_PCM (960 * 2 * 2 * 3)         // a frame of up to 60 ms, stereo, 16-bit
typedef struct opus_req { struct opus_req *next; size_t n; int samples; uint8_t data[]; } opus_req;   // a waiting packet, then (decoded) its PCM
typedef struct opus_mgr opus_mgr;
typedef struct { opus_mgr *m; void *soft; void *work; int ch; opus_req *wait, *wait_tail, *done, *done_tail; int nwait, ndone; } opus_dec;
struct opus_mgr { pthread_mutex_t lock; void *event; int io_total, io_used, ndec, cap; opus_dec **dec; };
static opus_mgr *opus_mgr_of(const void *gm) { opus_mgr *m = *(opus_mgr *const *)gm; if (!m) die("Opus decoder manager %p was not initialized by the host", gm); return m; }
static opus_dec *opus_dec_of(const void *gd) { opus_dec *d = objmap_get(gd); if (!d) die("Opus decoder %p was not initialized by the host", gd); return d; }
static void opus_free_list(opus_req *r) { while (r) { opus_req *n = r->next; free(r); r = n; } }

HLE_FORCE("_ZN2nn5codec43GetHardwareOpusDecoderManagerWorkBufferSizeEiiNS_8TimeSpanEii", "hwopus service; the host keeps the manager's state, the work buffer is not used",
          size_t, h_codec_HwOpusMgrWorkSize, int rate, int ch, int64_t frame, int decoders, int io) {
    (void)rate; (void)ch; (void)frame; (void)decoders; (void)io; return 0x1000;
}
HLE_FORCE("_ZN2nn5codec36InitializeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeEPNS_2os15SystemEventTypeEiiNS_8TimeSpanEiiPvm",
          "hwopus service; decoded by the software OpusDecoder in ExecuteHardwareOpusDecoderDecoding", int, h_codec_HwOpusMgrInit,
          void *gm, void *event, int rate, int ch, int64_t frame, int decoders, int io, void *buf, size_t size) {
    (void)rate; (void)ch; (void)frame; (void)buf; (void)size;
    opus_mgr *m = calloc(1, sizeof *m); if (!m) die("Opus manager calloc");
    pthread_mutex_init(&m->lock, NULL); m->event = event; m->io_total = io; m->cap = decoders > 0 ? decoders : 1;
    if (!(m->dec = calloc((size_t)m->cap, sizeof *m->dec))) die("Opus manager calloc");
    hle_bind_system_event(event, 1);   // the SDK creates it; the game destroys it after FinalizeHardwareOpusDecoderManager (main+0xad27c4)
    *(opus_mgr **)gm = m;
    return 0;
}
HLE_FORCE("_ZN2nn5codec34FinalizeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service; host manager state", void, h_codec_HwOpusMgrFini, void *gm) {
    opus_mgr *m = *(opus_mgr **)gm; if (!m) return;
    for (int i = 0; i < m->ndec; i++) if (m->dec[i]) m->dec[i]->m = NULL;
    *(opus_mgr **)gm = NULL; pthread_mutex_destroy(&m->lock); free(m->dec); free(m);
}
HLE_FORCE("_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service; host manager state", int, h_codec_HwOpusMgrFreeIo, const void *gm) {
    opus_mgr *m = opus_mgr_of(gm); pthread_mutex_lock(&m->lock); int n = m->io_total - m->io_used; pthread_mutex_unlock(&m->lock); return n;
}
HLE_FORCE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiNS_8TimeSpanEPNS0_30HardwareOpusDecoderManagerTypeE",
          "hwopus service; replaced by the software OpusDecoder (manager form)", int, h_codec_HwOpusMgrDecInit, void *gd, int rate, int ch, int64_t frame, void *gm) {
    (void)frame;
    opus_mgr *m = opus_mgr_of(gm);
    size_t (*wsize)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii");
    int (*init)(void *, int, int, void *, size_t) = opus_fn("_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
    opus_dec *d = calloc(1, sizeof *d); if (!d) die("Opus decoder calloc");
    size_t ws = wsize(rate, ch);
    if (!(d->soft = calloc(1, SOFT_OPUS_STATE)) || !(d->work = calloc(1, ws))) die("Opus decoder calloc");
    int r = init(d->soft, rate, ch, d->work, ws);
    if (r) { free(d->work); free(d->soft); free(d); return r; }
    d->m = m; d->ch = ch;
    pthread_mutex_lock(&m->lock);
    if (m->ndec == m->cap) { m->cap *= 2; if (!(m->dec = realloc(m->dec, (size_t)m->cap * sizeof *m->dec))) die("Opus manager realloc"); }
    m->dec[m->ndec++] = d;
    pthread_mutex_unlock(&m->lock);
    objmap_put(gd, d);
    return 0;
}
static void opus_dec_clear(opus_dec *d) {   // under the manager's lock: every request of the decoder dropped, its I/O buffers back to the pool
    if (d->m) d->m->io_used -= d->nwait + d->ndone;
    opus_free_list(d->wait); opus_free_list(d->done);
    d->wait = d->wait_tail = d->done = d->done_tail = NULL; d->nwait = d->ndone = 0;
}
// FinalizeHardwareOpusDecoder and ResetHardwareOpusDecoder are the hooks above (the older C interface): both find this decoder through objmap first
static int opus_mgr_dec_fini(void *gd) {
    opus_dec *d = objmap_get(gd); if (!d || !d->work) return 0;   // the older interface's state has no work buffer of its own
    objmap_del(gd);
    void (*fini)(void *) = opus_fn("_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE");
    opus_mgr *m = d->m;
    if (m) {
        pthread_mutex_lock(&m->lock); opus_dec_clear(d);
        for (int i = 0; i < m->ndec; i++) if (m->dec[i] == d) { m->dec[i] = m->dec[--m->ndec]; break; }
        pthread_mutex_unlock(&m->lock);
    } else opus_dec_clear(d);
    fini(d->soft); free(d->soft); free(d->work); free(d);
    return 1;
}
static int opus_mgr_dec_reset(void *gd) {
    opus_dec *d = objmap_get(gd); if (!d || !d->work) return 0;
    void (*reset)(void *) = opus_fn("_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE");
    if (d->m) pthread_mutex_lock(&d->m->lock);
    opus_dec_clear(d); reset(d->soft);
    if (d->m) pthread_mutex_unlock(&d->m->lock);
    return 1;
}
HLE_FORCE("_ZN2nn5codec36EnqueueOpusDataToHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPmPKvm", "hwopus service; queued for ExecuteHardwareOpusDecoderDecoding", int, h_codec_HwOpusEnqueue,
          void *gd, size_t *consumed, const void *in, size_t in_size) {
    opus_dec *d = opus_dec_of(gd); opus_mgr *m = d->m;
    if (consumed) *consumed = 0;
    if (!m) return 0x3a6f;   // the manager was finalized
    if (in_size < 8 || in_size > OPUS_MAX_PACKET) { LOG("[RED] codec: Opus packet of %zu bytes enqueued", in_size); return 0x3a6f; }
    opus_req *r = malloc(sizeof *r + (in_size > OPUS_MAX_PCM ? in_size : OPUS_MAX_PCM)); if (!r) die("Opus request malloc");
    r->next = NULL; r->n = in_size; r->samples = 0; memcpy(r->data, in, in_size);
    pthread_mutex_lock(&m->lock);
    if (m->io_used >= m->io_total) { pthread_mutex_unlock(&m->lock); free(r); return 0x3a6f; }   // no free I/O buffer: the game checks the count first (main+0xad2ec4)
    m->io_used++; d->nwait++;
    if (d->wait_tail) d->wait_tail->next = r; else d->wait = r;
    d->wait_tail = r;
    pthread_mutex_unlock(&m->lock);
    if (consumed) *consumed = in_size;
    return 0;
}
HLE_FORCE("_ZN2nn5codec34ExecuteHardwareOpusDecoderDecodingEPNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service; the software OpusDecoder decodes the waiting packets here",
          int, h_codec_HwOpusExecute, void *gm) {
    opus_mgr *m = opus_mgr_of(gm);
    int (*dec)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
    int any = 0;
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < m->ndec; i++) {
        opus_dec *d = m->dec[i];
        while (d->wait) {
            opus_req *r = d->wait; d->wait = r->next; if (!d->wait) d->wait_tail = NULL; d->nwait--;
            uint8_t pkt[OPUS_MAX_PACKET]; size_t n = r->n, used = 0; int samples = 0; memcpy(pkt, r->data, n);
            int res = dec(d->soft, &used, &samples, (int16_t *)r->data, OPUS_MAX_PCM, pkt, n);
            if (res || samples <= 0) {   // a frame the decoder refuses still completes, as silence of a 20 ms frame, so the game's stream stays in step
                static int nred; if (nred++ < 4) LOG("[RED] codec: software Opus decode of a %zu-byte packet failed (%#x), silence instead", n, res);
                samples = 960; memset(r->data, 0, (size_t)samples * d->ch * 2);
            }
            r->samples = samples; r->n = (size_t)samples * d->ch * 2; r->next = NULL;
            if (d->done_tail) d->done_tail->next = r; else d->done = r;
            d->done_tail = r; d->ndone++; any = 1;
        }
    }
    pthread_mutex_unlock(&m->lock);
    if (any) hle_signal_system_event_binding(hle_system_event_binding(m->event));
    return 0;
}
HLE_FORCE("_ZN2nn5codec48DequeuePcmInterleavedDataFromHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPiPsm", "hwopus service; frames the software OpusDecoder decoded",
          int, h_codec_HwOpusDequeue, void *gd, int *samples, int16_t *out, size_t out_size) {
    opus_dec *d = opus_dec_of(gd); opus_mgr *m = d->m;
    if (samples) *samples = 0;
    if (!m) return 0x3a6f;
    pthread_mutex_lock(&m->lock);
    opus_req *r = d->done;
    if (!r) { pthread_mutex_unlock(&m->lock); return 0x3a6f; }   // nothing completed: the game dequeues only as many as the completed count (main+0xad31ac)
    if (r->n > out_size) { pthread_mutex_unlock(&m->lock); LOG("[RED] codec: a %zu-byte Opus frame for a %zu-byte buffer", r->n, out_size); return 0x3a6f; }
    d->done = r->next; if (!d->done) d->done_tail = NULL; d->ndone--; m->io_used--;
    pthread_mutex_unlock(&m->lock);
    memcpy(out, r->data, r->n); if (samples) *samples = r->samples;
    free(r);
    return 0;
}
static int opus_count(const void *gd, int which) {
    opus_dec *d = opus_dec_of(gd); if (!d->m) return 0;
    pthread_mutex_lock(&d->m->lock); int n = which == 0 ? d->nwait : which == 2 ? d->ndone : 0; pthread_mutex_unlock(&d->m->lock);
    return n;
}
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusDecoderWaitingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; packets not yet decoded", int, h_codec_HwOpusWaiting, const void *gd) {
    return opus_count(gd, 0);
}
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusDecoderExecutingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; Execute decodes synchronously, none run in the background",
          int, h_codec_HwOpusExecuting, const void *gd) {
    return opus_count(gd, 1);
}
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusDecoderCompletedDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; decoded frames not yet dequeued", int, h_codec_HwOpusCompleted, const void *gd) {
    return opus_count(gd, 2);
}
