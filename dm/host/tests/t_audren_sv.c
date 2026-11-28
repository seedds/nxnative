// audren:u (hle_audren_sv.c) through the host sessions (hle_sf.c), as SDK 1.3.1's client drives it (Rayman Legends: Definitive Edition): GetWorkBufferSize and OpenAudioRenderer on the
// manager (the parameter, two copied handles and the process ID in; the renderer as a moved session out), then the renderer's own commands, RequestUpdate with an A buffer in and two B
// buffers out. An update of REV2 with one memory pool, one PCM16 voice into the final mix, one device sink: the answer's header and sections at the client's sizes, the pool attached,
// the mix to stereo by the voice's channel resource, the wave buffer consumed at its end and reported, a stop consuming what is queued, an Aux effect's send and return rings, and
// DSP ADPCM decoding. No render thread runs here: the frames are rendered by calling the renderer directly
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_sf.c"
#include "../hle_audren_sv.c"

static int checks, fails, signals;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
uint32_t hle_new_event(uint32_t *write_end) { if (write_end) *write_end = 0x2001; return 0x1001; }
uint32_t hle_dup_readable_handle(uint32_t h) { return h == 0x1001 ? 0x1002 : 0; }
int hle_signal_event_handle(uint32_t w) { signals += w == 0x2001; return 1; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) { (void)fn; (void)arg; (void)prio; (void)stack_size; return NULL; }
void dm_guest_thread_join(void *tt) { (void)tt; }
void *dm_pcmout_open(int ch) { (void)ch; return NULL; }
void dm_pcmout_close(void *k) { (void)k; }
void dm_pcmout_start(void *k) { (void)k; }
int dm_pcmout_append(void *k, void *b) { (void)k; (void)b; return 1; }
void *dm_pcmout_released(void *k) { (void)k; return NULL; }
void dm_pcmout_wait(void *k, int ms) { (void)k; (void)ms; }
size_t dm_audren_work_size(const uint8_t *p, uint32_t revision) { return revision == 0x32564552 && rd32(p) == 48000 ? 0x12000 : 0; }

// A non-domain CMIF request: optional process ID and copied handles in a special header, A then B descriptors, the raw input after "SFCI"
static uint32_t msg[0x100];
static Result call(uint32_t h, unsigned cmd, const void *raw, size_t raw_len, int pid, const uint32_t *copies, int ncopy,
                   const void *a, uint64_t alen, void *b0, uint64_t b0len, void *b1, uint64_t b1len) {
    memset(msg, 0xcc, sizeof msg);
    unsigned na = a ? 1 : 0, nb = (b0 ? 1 : 0) + (b1 ? 1 : 0);
    msg[0] = 4 | na << 20 | nb << 24; msg[1] = (uint32_t)(16 + (raw_len + 3) / 4) | (pid || ncopy ? 1u << 31 : 0);
    uint32_t *p = msg + 2;
    if (pid || ncopy) { *p++ = (pid ? 1u : 0) | (uint32_t)ncopy << 1; if (pid) { *p++ = 0; *p++ = 0; } for (int i = 0; i < ncopy; i++) *p++ = copies[i]; }
    const void *bufs[3] = {a, b0, b1}; uint64_t lens[3] = {alen, b0len, b1len};
    for (int i = 0; i < 3; i++) {
        if (!bufs[i]) continue;
        uint64_t ad = (uint64_t)(uintptr_t)bufs[i];
        *p++ = (uint32_t)lens[i]; *p++ = (uint32_t)ad; *p++ = (uint32_t)(ad >> 32 & 0xf) << 28 | (uint32_t)(ad >> 36 & 0x3fffff) << 2 | (uint32_t)(lens[i] >> 32 & 0xf) << 24;
    }
    size_t cm = ((size_t)((uint8_t *)p - (uint8_t *)msg) + 15) & ~(size_t)15;
    uint32_t *d = (uint32_t *)((uint8_t *)msg + cm);
    d[0] = 0x49434653; d[1] = 0; d[2] = cmd; d[3] = 0;
    if (raw_len) memcpy(d + 4, raw, raw_len);
    if (h_sf_Send(h, msg, sizeof msg)) return 0xdead;
    size_t dw = 8 + (msg[1] >> 31 ? 4 + 4 * ((msg[2] >> 1 & 15) + (msg[2] >> 5 & 15)) : 0), out = (dw + 15) & ~(size_t)15;
    uint32_t *o = (uint32_t *)((uint8_t *)msg + out);
    return o[0] == 0x4f434653 ? o[2] : 0xbad;
}
static const uint8_t *out_raw(void) { size_t dw = 8 + (msg[1] >> 31 ? 4 + 4 * ((msg[2] >> 1 & 15) + (msg[2] >> 5 & 15)) : 0); return (const uint8_t *)msg + ((dw + 15) & ~(size_t)15) + 16; }

// The update: header, behaviour, memory pools, channel resources, voices, effects, (no splitters), mixes, sinks, performance
enum { NMP = 1, NCR = 2, NV = 2, NFX = 1, NMIX = 1, NSINK = 1 };
static uint8_t upd[HDR + BEH_IN + NMP * MP_IN + NCR * CR_IN + NV * V_IN + NFX * FX_IN + NMIX * MIX_IN + NSINK * SINK_IN + PERF_IO];
static uint8_t *u_mp, *u_cr, *u_v, *u_fx, *u_mix, *u_sink;
static void wrf(uint8_t *p, float v) { memcpy(p, &v, 4); }
static void build(void) {
    memset(upd, 0, sizeof upd);
    wr32(upd, 0x32564552); wr32(upd + 4, BEH_IN); wr32(upd + 8, NMP * MP_IN); wr32(upd + 0xc, NV * V_IN); wr32(upd + 0x10, NCR * CR_IN); wr32(upd + 0x14, NFX * FX_IN);
    wr32(upd + 0x18, NMIX * MIX_IN); wr32(upd + 0x1c, NSINK * SINK_IN); wr32(upd + 0x20, PERF_IO); wr32(upd + 0x3c, sizeof upd);
    u_mp = upd + HDR + BEH_IN; u_cr = u_mp + NMP * MP_IN; u_v = u_cr + NCR * CR_IN; u_fx = u_v + NV * V_IN; u_mix = u_fx + NFX * FX_IN; u_sink = u_mix + NMIX * MIX_IN;
    wr32(u_mp + 0x10, 4);                                                   // a pool the client asks to attach
    wr32(u_cr, 0); wrf(u_cr + 4, 1.0f); wrf(u_cr + 8, 0.5f); u_cr[0x64] = 1;   // channel resource 0: buffer 0 at 1, buffer 1 at 0.5
    wrf(u_mix, 1.0f); wr32(u_mix + 4, 48000); wr32(u_mix + 8, 2); u_mix[0xc] = 1; wr32(u_mix + 0x10, 0); wr32(u_mix + 0x924, UNUSED_MIX); wr32(u_mix + 0x928, 0xffffffffu);
    u_sink[0] = 1; u_sink[1] = 1; wr32(u_sink + 0x120, 2); u_sink[0x124] = 0; u_sink[0x125] = 1;
}
static int16_t pcm[300];
static void voice(uint8_t *v, int isnew, int state, int fmt, const void *buf, uint64_t size, int start, int end, int sent, const void *coeffs) {
    v[8] = (uint8_t)isnew; v[9] = 1; v[0xa] = (uint8_t)state; v[0xb] = (uint8_t)fmt; wr32(v + 0xc, 48000); wr32(v + 0x18, 1); wrf(v + 0x1c, 1.0f); wrf(v + 0x20, 1.0f);
    wr64(v + 0x48, (uint64_t)(uintptr_t)coeffs); wr32(v + 0x58, 0); wr32(v + 0x5c, 0xffffffffu);
    uint8_t *wb = v + 0x60; wr64(wb, (uint64_t)(uintptr_t)buf); wr64(wb + 8, size); wr32(wb + 0x10, (uint32_t)start); wr32(wb + 0x14, (uint32_t)end); wb[0x1a] = (uint8_t)sent;
    for (int k = 1; k < 4; k++) v[0x60 + k * 0x38 + 0x1a] = 1;
    wr32(v + 0x140, 0);
}
static uint8_t ans[0x400], perf[0x100];
static Result update(uint32_t r) { return call(r, 4, NULL, 0, 0, NULL, 0, upd, sizeof upd, ans, HDR + NMP * MP_OUT + NV * V_OUT + NFX * FX_OUT + NSINK * SINK_OUT + PERF_IO + BEH_OUT, perf, sizeof perf); }

int main(void) {
    uint32_t mgr = 0;
    CHECK(h_sf_Connect(&mgr, "audren:u") == RES_OK && mgr, "audren:u is served");
    uint8_t param[0x48] = {0};
    wr32(param, 48000); wr32(param + 4, 240); wr32(param + 8, 2); wr32(param + 0x10, NV); wr32(param + 0x14, 1); wr32(param + 0x18, NFX); wr32(param + 0x30, 0x32564552);
    CHECK(call(mgr, 1, param, PARAM, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == RES_OK && rd64(out_raw()) == 0x12000, "GetWorkBufferSize: the service budget for the REV2 parameter");
    wr64(param + 0x38, 0x12000); wr64(param + 0x40, 0x1234);
    uint32_t copies[2] = {0xd1000000u, 0xffff8001u};
    CHECK(call(mgr, 0, param, sizeof param, 1, copies, 2, NULL, 0, NULL, 0, NULL, 0) == RES_OK, "OpenAudioRenderer succeeds");
    CHECK(msg[1] >> 31 && (msg[2] >> 5 & 15) == 1 && (msg[2] >> 1 & 15) == 0, "the renderer comes back as one moved handle");
    uint32_t r = msg[3];
    int slot = sf_slot(r); sv_rnd *rnd = slot >= 0 && g_sf[slot].obj ? g_sf[slot].obj->ctx : NULL;
    CHECK(rnd && g_sf[slot].obj->answer == sv_renderer, "the moved handle is a renderer session");
    if (!rnd) { printf("t_audren_sv: %d checks, %d failed\n", checks, fails); return 1; }
    CHECK(call(r, 0, NULL, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == RES_OK && rd32(out_raw()) == 48000, "GetSampleRate");
    CHECK(call(r, 1, NULL, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == RES_OK && rd32(out_raw()) == 240, "GetSampleCount");
    CHECK(call(r, 3, NULL, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == RES_OK && rd32(out_raw()) == 1, "a new renderer is stopped");
    CHECK(call(r, 7, NULL, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == RES_OK && msg[1] >> 31 && (msg[2] >> 1 & 15) == 1 && msg[3] == 0x1002, "QuerySystemEvent copies a read end of the renderer's event");
    CHECK(call(r, 5, NULL, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == RES_OK && rnd->started, "Start");

    for (int i = 0; i < 300; i++) pcm[i] = (int16_t)(i * 10);
    build(); voice(u_v, 1, 0, 2, pcm, sizeof pcm, 0, 300, 0, NULL);
    CHECK(update(r) == RES_OK, "RequestUpdate succeeds");
    CHECK(rd32(ans + 4) == BEH_OUT && rd32(ans + 8) == NMP * MP_OUT && rd32(ans + 0xc) == NV * V_OUT && rd32(ans + 0x14) == NFX * FX_OUT && rd32(ans + 0x1c) == NSINK * SINK_OUT
          && rd32(ans + 0x20) == PERF_IO, "the answer's section sizes are the client's");
    CHECK(rd32(ans + 0x3c) == HDR + NMP * MP_OUT + NV * V_OUT + NFX * FX_OUT + NSINK * SINK_OUT + PERF_IO + BEH_OUT, "the answer's total equals the client's buffer");
    CHECK(rd32(ans + HDR) == 5, "the requested attach answers attached");
    int16_t f[FRAME_MAX * 2];
    sv_render(rnd, f);
    CHECK(f[0] == 0 && f[2 * 10] == 100 && f[2 * 10 + 1] == 50 && f[2 * 239] == 2390 && f[2 * 239 + 1] == 1195, "the voice mixes by its channel resource: left at 1, right at 0.5");
    sv_render(rnd, f);
    CHECK(f[2 * 59] == 2990 && f[2 * 60] == 0, "the wave buffer ends at sample 300, then silence");
    build(); voice(u_v, 0, 0, 2, pcm, sizeof pcm, 0, 300, 1, NULL);
    CHECK(update(r) == RES_OK, "a second update");
    const uint8_t *vo = ans + HDR + NMP * MP_OUT;
    CHECK(rd32(vo + 8) == 1 && rd64(vo) == 300, "voice answer: one wave buffer consumed, 300 samples played");
    // A wave buffer appended to a stopped voice waits; the stop of a playing one consumes what is queued
    build(); voice(u_v, 0, 0, 2, pcm, sizeof pcm, 0, 300, 1, NULL); u_v[0x60 + 0x38 + 0x1a] = 0;
    memcpy(u_v + 0x60 + 0x38, u_v + 0x60, 0x1a);   // slot 1: the same samples
    CHECK(update(r) == RES_OK, "append to slot 1");
    sv_render(rnd, f);
    CHECK(f[2 * 5] == 50, "slot 1 plays");
    build(); voice(u_v, 0, 1, 2, pcm, sizeof pcm, 0, 300, 1, NULL);
    CHECK(update(r) == RES_OK && rd32(vo + 8) == 2 && rd64(vo) == 0, "stopping consumes the playing wave buffer and restarts the played count");
    CHECK(rnd->voice[0].idx == 2, "the ring moves on to the slot the client appends to next");
    // Aux on the final mix: buffer 0 to the send ring, the return ring back into buffer 0
    static int32_t send[4096], ret[4096]; static uint8_t sinfo[0x80], rinfo[0x80];
    for (int i = 0; i < 240; i++) ret[i] = 7;
    build(); voice(u_v, 1, 0, 2, pcm, sizeof pcm, 0, 300, 0, NULL);
    u_fx[0] = 2; u_fx[2] = 1; wr32(u_fx + 4, 0); wr32(u_fx + 0x18, 0);
    uint8_t *d = u_fx + 0x20; d[0] = 0; d[0x18] = 0; wr32(d + 0x30, 1); wr32(d + 0x38, 4096);
    wr64(d + 0x40, (uint64_t)(uintptr_t)sinfo); wr64(d + 0x48, (uint64_t)(uintptr_t)send); wr64(d + 0x50, (uint64_t)(uintptr_t)rinfo); wr64(d + 0x58, (uint64_t)(uintptr_t)ret);
    CHECK(update(r) == RES_OK && ans[HDR + NMP * MP_OUT + NV * V_OUT] == 3, "the enabled Aux answers enabled");
    sv_render(rnd, f);
    CHECK(send[10] == 100 && rd32(sinfo + 0x44) == 240, "the send ring gets the mix buffer and its write offset moves a frame");
    CHECK(f[2 * 10] == 7 && f[2 * 10 + 1] == 50 && rd32(rinfo + 0x40) == 240, "the return ring replaces the buffer and its read offset moves a frame");
    // DSP ADPCM: with zero coefficients a sample is its nibble shifted by the scale; the history and predictor do not enter
    static int16_t coeffs[16]; static uint8_t adpcm[8 * 18];
    for (int fr = 0; fr < 18; fr++) { adpcm[fr * 8] = 2; for (int k = 1; k < 8; k++) adpcm[fr * 8 + k] = 0x1f; }   // scale 2: nibbles 1 and -1
    build(); voice(u_v, 1, 0, 6, adpcm, sizeof adpcm, 0, 240, 0, coeffs);
    CHECK(update(r) == RES_OK, "an ADPCM voice");
    sv_render(rnd, f);
    CHECK(f[0] == 4 && f[2] == -4 && f[2 * 238] == 4 && f[2 * 239] == -4, "ADPCM: nibble × 2^scale, frame after frame");
    CHECK(call(r, 6, NULL, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == RES_OK && !rnd->started, "Stop");
    // Duplicate the renderer handle and close the original first: the duplicate still serves commands and destroys the renderer once on final close
    uint32_t r_dup = 0;
    CHECK(h_sf_Dup(&r_dup, r) == RES_OK && r_dup && r_dup != r, "duplicate the renderer session handle");
    h_sf_Close(r);
    CHECK(sf_slot(r) < 0 && sf_slot(r_dup) >= 0, "closing original leaves duplicate slot open");
    CHECK(call(r_dup, 0, NULL, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == RES_OK && rd32(out_raw()) == 48000, "duplicate renderer answers GetSampleRate after original closes");
    h_sf_Close(r_dup); h_sf_Close(mgr);
    CHECK(sf_slot(r_dup) < 0, "closing the last renderer handle ends it");
    (void)signals;
    printf("t_audren_sv: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
