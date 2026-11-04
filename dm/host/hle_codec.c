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
// A game decoder object's host state: the software decoder first, so the pointer serves as the OpusDecoderType; then what the SDK 21 manager API (below) keeps per decoder
typedef struct opus_pkt { struct opus_pkt *next; size_t n; int samples; uint8_t data[]; } opus_pkt_t;   // an input packet as enqueued, or an output PCM block (samples: frames decoded)
typedef struct hwdec_t { uint8_t soft[SOFT_OPUS_STATE]; struct hwmgr_t *mgr; void *work; int rate, ch; opus_pkt_t *in, *in_tail, *out, *out_tail; int n_in, n_out; } hwdec_t;
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
    void *s = objmap_get(hw); if (!s) { s = calloc(1, sizeof(hwdec_t)); objmap_put(hw, s); }
    return f(s, rate, ch, buf, size);
}
static void hwdec_drop(hwdec_t *d);   // the manager API's queues and work buffer (below)
HLE_FORCE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", void, h_codec_HwOpusFini, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE");
    hwdec_t *s = objmap_del(hw); if (s) { f(s->soft); hwdec_drop(s); free(s); }
}
HLE_FORCE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusRate, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusCh, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", void, h_codec_HwOpusReset, void *hw) {
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

// ---------- SDK 21's asynchronous hardware Opus decoder: a manager (Star Fox, Wwise 2024.1) ----------
// nn::codec::HardwareOpusDecoderManager: the game enqueues Opus packets on each decoder (EnqueueOpusDataToHardwareOpusDecoder), asks the manager to decode what is queued
// (ExecuteHardwareOpusDecoderDecoding), waits on the manager's SystemEvent, and dequeues PCM (DequeuePcmInterleavedDataFromHardwareOpusDecoder); the counts say how many requests
// wait, run and are done, and how many of the manager's I/O buffers are free. On the console the decoding runs in the hwopus service; GetHardwareOpusDecoderManagerWorkBufferSize
// is the first call and opened hwopus, which the host does not serve: the SDK aborted at sdk+0x323180 (sm ResultNotRegistered 0xe15) as the game's audio started.
// Here the manager is a host object: Execute decodes every queued packet with the SDK's software OpusDecoder (as the synchronous C interface above) on the calling thread and signals
// the event; so a request is never "executing", and what was waiting is complete when Execute returns. The decoders' work buffers are the host's (the manager's one buffer from the
// game is not used), sized by GetOpusDecoderWorkBufferSize. The packets keep the SDK's framing (a big-endian u32 length, a u32 final range, the Opus packet), as the synchronous
// DecodeOpusInterleaved takes them. Parameter meanings not confirmed on hardware are logged on the first call (DM_OPUS_LOG=1 logs every call).
#include <pthread.h>
// The manager's parameters, as Star Fox gives them: sample rate, channels, frame length, decoder count, I/O buffer count (48000, 1, 20 ms, 128, 255 for its mono voices; 48000, 2, 20 ms, 72, 160 for stereo)
typedef struct hwmgr_t { void *event; int rate, ch, ndec_max, nio; int64_t span; hwdec_t **decs; int ndecs; } hwmgr_t;
static pthread_mutex_t g_hwmgr_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_opus_log = -1;
static int opus_log(void) { if (g_opus_log < 0) { const char *e = getenv("DM_OPUS_LOG"); g_opus_log = e && *e && *e != '0'; } return g_opus_log; }
static hwmgr_t *hwmgr_of(const void *game) { hwmgr_t *m = objmap_get(game); if (!m) die("Opus decoder manager %p was not initialized by the host", game); return m; }
static void pkt_push(opus_pkt_t **head, opus_pkt_t **tail, opus_pkt_t *p) { p->next = NULL; if (*tail) (*tail)->next = p; else *head = p; *tail = p; }
static opus_pkt_t *pkt_pop(opus_pkt_t **head, opus_pkt_t **tail) { opus_pkt_t *p = *head; if (p) { *head = p->next; if (!*head) *tail = NULL; } return p; }
static void hwdec_drop(hwdec_t *d) {
    opus_pkt_t *p;
    while ((p = pkt_pop(&d->in, &d->in_tail))) free(p);
    while ((p = pkt_pop(&d->out, &d->out_tail))) free(p);
    d->n_in = d->n_out = 0; free(d->work); d->work = NULL;
    if (d->mgr) { hwmgr_t *m = d->mgr; for (int i = 0; i < m->ndecs; i++) if (m->decs[i] == d) { m->decs[i] = m->decs[--m->ndecs]; break; } d->mgr = NULL; }
}
HLE_FORCE("_ZN2nn5codec43GetHardwareOpusDecoderManagerWorkBufferSizeEiiNS_8TimeSpanEii", "hwopus service (SDK 21 manager); the host keeps its own state, a small buffer is asked for",
          size_t, h_codec_HwMgrWorkSize, int a, int b, int64_t span, int c, int d) {
    static int once; if (!once++ || opus_log()) LOG("opus manager: work buffer size asked for (%d, %d, %lld ns, %d, %d)", a, b, (long long)span, c, d);
    return 0x1000;
}
HLE_FORCE("_ZN2nn5codec36InitializeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeEPNS_2os15SystemEventTypeEiiNS_8TimeSpanEiiPvm", "hwopus service (SDK 21 manager) → a host manager; its event is a local system event the host signals after each Execute",
          int, h_codec_HwMgrInit, void *game, void *ev, int a, int b, int64_t span, int c, int d, void *buf, size_t size) {
    (void)buf; (void)size;
    static void (*create)(void *, int, int); if (!create) create = opus_fn("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    hwmgr_t *m = objmap_get(game);
    if (!m) { m = calloc(1, sizeof *m); objmap_put(game, m); }
    m->event = ev; m->rate = a; m->ch = b; m->span = span; m->ndec_max = c > 0 ? c : 1; m->nio = d > 0 ? d : 1;
    free(m->decs); m->decs = calloc((size_t)m->ndec_max, sizeof *m->decs); m->ndecs = 0;
    if (ev) create(ev, 1, 0);   // EventClearMode_AutoClear, not inter-process
    static int once; if (!once++ || opus_log()) LOG("opus manager %p: initialized (event %p, %d Hz, %d ch, %lld ns frames, %d decoders, %d I/O buffers, work buffer %zu bytes)", game, ev, a, b, (long long)span, c, d, size);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5codec34FinalizeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service (SDK 21 manager); the host manager goes, its event with it", void, h_codec_HwMgrFini, void *game) {
    static void (*destroy)(void *); if (!destroy) destroy = opus_fn("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    pthread_mutex_lock(&g_hwmgr_mu);
    hwmgr_t *m = objmap_del(game);
    if (m) { for (int i = 0; i < m->ndecs; i++) m->decs[i]->mgr = NULL; if (m->event) destroy(m->event); free(m->decs); free(m); }
    pthread_mutex_unlock(&g_hwmgr_mu);
}
HLE_FORCE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiNS_8TimeSpanEPNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service (SDK 21 manager); a software OpusDecoder with a host work buffer, registered with the manager",
          int, h_codec_HwOpusInitMgr, void *hw, int rate, int ch, int64_t span, void *game) {
    size_t (*wsz)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii");
    int (*init)(void *, int, int, void *, size_t) = opus_fn("_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
    pthread_mutex_lock(&g_hwmgr_mu);
    hwmgr_t *m = hwmgr_of(game);
    hwdec_t *d = objmap_get(hw); if (!d) { d = calloc(1, sizeof *d); objmap_put(hw, d); }
    hwdec_drop(d);
    size_t n = wsz(rate, ch); d->work = calloc(1, n); d->rate = rate; d->ch = ch;
    int r = init(d->soft, rate, ch, d->work, n);
    if (!r) { if (m->ndecs < m->ndec_max) { d->mgr = m; m->decs[m->ndecs++] = d; } else { static int once; if (!once++) LOG("[RED] opus manager %p: more than its %d decoders", game, m->ndec_max); } }
    pthread_mutex_unlock(&g_hwmgr_mu);
    static int once; if (!once++ || opus_log()) LOG("opus manager %p: decoder %p initialized (%d Hz, %d ch, %lld ns, work %zu) → %#x", game, hw, rate, ch, (long long)span, n, r);
    return r;
}
HLE_FORCE("_ZN2nn5codec36EnqueueOpusDataToHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPmPKvm", "hwopus service (SDK 21 manager); the packet is queued for the next Execute",
          int, h_codec_HwOpusEnqueue, void *hw, size_t *consumed, const void *data, size_t size) {
    hwdec_t *d = opus_soft(hw);
    opus_pkt_t *p = malloc(sizeof *p + size); p->n = size; p->samples = 0; memcpy(p->data, data, size);
    pthread_mutex_lock(&g_hwmgr_mu); pkt_push(&d->in, &d->in_tail, p); d->n_in++; pthread_mutex_unlock(&g_hwmgr_mu);
    if (consumed) *consumed = size;
    if (opus_log()) LOG("opus manager: decoder %p enqueue %zu bytes (queued %d)", hw, size, d->n_in);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5codec34ExecuteHardwareOpusDecoderDecodingEPNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service (SDK 21 manager); every queued packet is decoded now with the software OpusDecoder, then the event is signaled",
          int, h_codec_HwMgrExecute, void *game) {
    int (*dec)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
    static void (*signal)(void *); if (!signal) signal = opus_fn("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    pthread_mutex_lock(&g_hwmgr_mu);
    hwmgr_t *m = hwmgr_of(game); int done = 0, failed = 0;
    for (int i = 0; i < m->ndecs; i++) {
        hwdec_t *d = m->decs[i]; opus_pkt_t *p;
        while ((p = pkt_pop(&d->in, &d->in_tail))) {
            d->n_in--;
            size_t max_samples = (size_t)d->rate * 120 / 1000, pcm_bytes = max_samples * d->ch * sizeof(int16_t);   // an Opus packet holds at most 120 ms
            opus_pkt_t *o = malloc(sizeof *o + pcm_bytes); o->n = 0; o->samples = 0;
            size_t off = 0;
            while (off < p->n && o->n < pcm_bytes) {
                size_t consumed = 0; int samples = 0;
                int r = dec(d->soft, &consumed, &samples, (int16_t *)(o->data + o->n), pcm_bytes - o->n, p->data + off, p->n - off);
                if (r || !consumed) { failed++; static int once; if (!once++) LOG("[RED] opus manager: DecodeOpusInterleaved failed %#x on a %zu-byte packet at +%zu (consumed %zu)", r, p->n, off, consumed); break; }
                off += consumed; o->samples += samples; o->n += (size_t)samples * d->ch * sizeof(int16_t);
            }
            free(p); pkt_push(&d->out, &d->out_tail, o); d->n_out++; done++;
        }
    }
    void *ev = m->event;
    pthread_mutex_unlock(&g_hwmgr_mu);
    if (ev) signal(ev);
    if (opus_log()) LOG("opus manager %p: execute decoded %d packets (%d failed)", game, done, failed);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5codec48DequeuePcmInterleavedDataFromHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPiPsm", "hwopus service (SDK 21 manager); the oldest decoded block",
          int, h_codec_HwOpusDequeue, void *hw, int *samples, int16_t *pcm, size_t pcm_bytes) {
    hwdec_t *d = opus_soft(hw);
    pthread_mutex_lock(&g_hwmgr_mu); opus_pkt_t *o = pkt_pop(&d->out, &d->out_tail); if (o) d->n_out--; pthread_mutex_unlock(&g_hwmgr_mu);
    if (!o) { if (samples) *samples = 0; static int once; if (!once++) LOG("[RED] opus manager: dequeue from decoder %p with nothing decoded", hw); return RES_FAIL; }
    size_t n = o->n < pcm_bytes ? o->n : pcm_bytes;
    if (o->n > pcm_bytes) { static int once; if (!once++) LOG("[RED] opus manager: a decoded block of %zu bytes does not fit the game's %zu-byte buffer (cut)", o->n, pcm_bytes); }
    memcpy(pcm, o->data, n); if (samples) *samples = (int)(n / (d->ch * sizeof(int16_t)));
    if (opus_log()) LOG("opus manager: decoder %p dequeue %d samples (%zu bytes, %d blocks left)", hw, o->samples, n, d->n_out);
    free(o); return RES_OK;
}
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusDecoderWaitingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service (SDK 21 manager): packets queued and not yet decoded", int, h_codec_HwOpusWaiting, const void *hw) {
    hwdec_t *d = opus_soft(hw); return d->n_in;
}
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusDecoderExecutingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service (SDK 21 manager): the host decodes inside Execute, so none is ever executing", int, h_codec_HwOpusExecuting, const void *hw) {
    (void)hw; return 0;
}
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusDecoderCompletedDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service (SDK 21 manager): decoded blocks not yet dequeued", int, h_codec_HwOpusCompleted, const void *hw) {
    hwdec_t *d = opus_soft(hw); return d->n_out;
}
HLE_FORCE("_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service (SDK 21 manager): I/O buffers less what is queued or undelivered", int, h_codec_HwMgrFree, const void *game) {
    pthread_mutex_lock(&g_hwmgr_mu);
    hwmgr_t *m = hwmgr_of(game); int used = 0;
    for (int i = 0; i < m->ndecs; i++) used += m->decs[i]->n_in + m->decs[i]->n_out;
    pthread_mutex_unlock(&g_hwmgr_mu);
    int nio = m->nio;
    return used < nio ? nio - used : 0;
}
