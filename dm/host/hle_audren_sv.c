// audren:u, the audio renderer service, for an SDK whose renderer is only its client. SDK 1.3.1 (Rayman Legends: Definitive Edition 1.2) has no renderer in the process: no
// ExecuteAudioRendererRendering and no DFC manager, and nn::audio connects to audren:u (CreateAudioRendererManagerByHipc, sdk+0x1641dc) as on Horizon. hle_sf.c hands out the session and
// this file answers it the way the service does, so the SDK's own client keeps its own state: it builds each update, reads the answer back and checks it (RequestUpdateAudioRenderer,
// sdk+0x1617d0). Every layout below was read off that client.
//   IAudioRendererManager: 0 OpenAudioRenderer (the 0x34-byte parameter, the work buffer's transfer memory and the process as copied handles, the work size and the applet resource user
//     ID at +0x38 / +0x40 of the input; the renderer comes back as a moved session), 1 GetWorkBufferSize (the parameter → u64), 2 GetAudioDeviceService (→ a session, answered with zeroes:
//     the device name and volume calls are taken over above the service, hle_audren.c).
//   IAudioRenderer: 0 GetSampleRate, 1 GetSampleCount, 2 GetMixBufferCount, 3 GetState (0 started, 1 stopped), 4 RequestUpdate (A: the update, B: the answer, B: the performance
//     frames), 5 Start, 6 Stop, 7 QuerySystemEvent (a copied read end), 8 / 9 Set / GetRenderingTimeLimit.
//   The update (revision REV2): a 0x40-byte header {revision, sizes of behaviour +4, memory pools +8, voices +0xc, voice channel resources +0x10, effects +0x14, mixes +0x18, sinks +0x1c,
//     performance +0x20, splitters +0x24; total +0x3c}, then the sections in the order behaviour (0x10), memory pools (0x20 each), channel resources (0x70), voices (0x170), effects
//     (0xc0), splitters, mixes (0x930), sinks (0x140), performance (0x10). The answer has the same header and memory pools (0x10 each), voices (0x10), effects (0x10), sinks (0x20),
//     performance (0x10) and behaviour (0xb0), and its total must equal the client's buffer, which it aborts on otherwise.
// The renderer itself: each voice (PCM8, PCM16 or DSP ADPCM, its four wave buffers in a ring) resampled linearly to the renderer's rate, through its channel resources' 24 volumes into its
// mix; sub mixes into their destination by their 24×24 volumes, deepest first; Aux effects (the game's send / return rings) and buffer mixers on their mix; the final mix through the
// first device sink to stereo. Delay, reverb and biquad effects pass the dry signal (a red line names them); splitters are not followed (a red line). A guest thread renders one frame per
// 5 ms output block, as hle_audren.c's render thread does, and signals the renderer's event after each frame while it is started.
#include "hle.h"
#include <math.h>
#include <stdatomic.h>

void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size);
void dm_guest_thread_join(void *tt);
void *dm_pcmout_open(int ch);
void dm_pcmout_close(void *k);
void dm_pcmout_start(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);
void dm_pcmout_wait(void *k, int ms);
size_t dm_audren_work_size(const uint8_t *p, uint32_t revision);   // hle_audren.c: the service's work buffer budget

enum { HDR = 0x40, BEH_IN = 0x10, BEH_OUT = 0xb0, MP_IN = 0x20, MP_OUT = 0x10, CR_IN = 0x70, V_IN = 0x170, V_OUT = 0x10, FX_IN = 0xc0, FX_OUT = 0x10, MIX_IN = 0x930,
       SINK_IN = 0x140, SINK_OUT = 0x20, PERF_IO = 0x10, PARAM = 0x34, MIXBUF = 24, FRAME_MAX = 1024, OUT_RATE = 48000, NBUF = 4 };
enum { UNUSED_MIX = 0x7fffffff };
// Voice parameter (the client's VoiceInfo from +0x40): +0x08 new, +0x09 in use, +0x0a play state (0 started, 1 stopped, 2 paused), +0x0b sample format (1 PCM8, 2 PCM16, 6 ADPCM),
// +0x0c sample rate, +0x18 channel count, +0x1c pitch, +0x20 volume, +0x24 two biquad filters (+0 enabled), +0x48 ADPCM coefficients (16 s16), +0x58 mix ID, +0x5c splitter ID,
// +0x60 four wave buffers of 0x38 {+0 address, +8 size, +0x10 start sample, +0x14 end sample, +0x18 loop, +0x19 end of stream, +0x1a sent to the service, +0x20 ADPCM context address
// {predictor and scale, history 0, history 1}, +0x28 its size}, +0x140 six channel resource IDs, +0x159 wave buffers to flush. Answer: +0 played samples (u64), +8 wave buffers consumed (u32)
typedef struct {
    uint8_t p[V_IN];
    int inuse, state, valid[4], idx, have, dpos;
    uint32_t consumed; uint64_t played;
    int32_t pos; double frac;
    int16_t h1, h2;
    float cur[6];
} sv_voice;
typedef struct {
    pthread_mutex_t mu;
    uint8_t param[PARAM];
    int rate, samples, nmixbuf;
    sv_voice *voice; int nvoice;
    uint8_t *cr, *fx, *mix, *sink; int ncr, nfx, nmix, nsink;
    float *mb;                               // nmix × 24 × samples
    int started; uint32_t time_limit;
    uint32_t ev_r, ev_w;
    _Atomic int open; void *thread;
    _Atomic uint64_t frames, loud;
    unsigned red;                            // one-time red lines (bits)
    double up_pos; float up_last[2];         // the output's resampling to 48 kHz
    int16_t frame[FRAME_MAX * 2];
    int16_t pcm[NBUF][FRAME_MAX * 3 * 2]; aout_buf bufs[NBUF];
} sv_rnd;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void sv_red(sv_rnd *r, unsigned bit, const char *what, int n) { if (!(r->red & 1u << bit)) { r->red |= 1u << bit; LOG("[RED] audren:u: %s (%d)", what, n); } }

// ---------- voices ----------
static const uint8_t *wb_of(const sv_voice *v, int k) { return v->p + 0x60 + k * 0x38; }
static void adpcm_ctx(sv_voice *v, const uint8_t *wb) {   // the wave buffer's context, when it has one: the history at its start (and at its loop)
    const uint8_t *ctx = (const uint8_t *)(uintptr_t)rd64(wb + 0x20);
    if (ctx && rd64(wb + 0x28) >= 6) { int16_t h[2]; memcpy(h, ctx + 2, 4); v->h1 = h[0]; v->h2 = h[1]; }
}
static float adpcm_next(sv_voice *v, const uint8_t *buf, uint64_t size) {   // the sample at dpos, then dpos + 1
    const int16_t *co = (const int16_t *)(uintptr_t)rd64(v->p + 0x48);
    size_t frame = (size_t)v->dpos / 14, sub = (size_t)v->dpos % 14;
    v->dpos++;
    if (!co || frame * 8 + 1 + sub / 2 >= size) return 0;
    uint8_t hdr = buf[frame * 8], b = buf[frame * 8 + 1 + sub / 2];
    int pred = hdr >> 4 & 7, nib = sub & 1 ? b & 15 : b >> 4;
    if (nib >= 8) nib -= 16;
    int32_t s = (nib * (1 << (hdr & 15)) * 2048 + 1024 + co[pred * 2] * v->h1 + co[pred * 2 + 1] * v->h2) >> 11;
    s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
    v->h2 = v->h1; v->h1 = (int16_t)s;
    return (float)s;
}
// The current sample frame (v->cur) at v->pos of the wave buffer at v->idx
static void sv_decode(sv_rnd *r, sv_voice *v) {
    const uint8_t *wb = wb_of(v, v->idx), *buf = (const uint8_t *)(uintptr_t)rd64(wb);
    uint64_t size = rd64(wb + 8);
    int fmt = v->p[0xb], nch = (int)rd32(v->p + 0x18);
    if (fmt == 6) { while (v->dpos <= v->pos) v->cur[0] = adpcm_next(v, buf, size); return; }
    size_t bytes = fmt == 1 ? 1 : 2, off = (size_t)v->pos * (size_t)nch * bytes;
    for (int c = 0; c < nch; c++) {
        if (!buf || off + (size_t)(c + 1) * bytes > size) { v->cur[c] = 0; continue; }
        v->cur[c] = fmt == 1 ? (float)((const int8_t *)buf)[off + (size_t)c] * 256.f : (float)((const int16_t *)(buf + off))[c];
    }
    (void)r;
}
static void sv_consume(sv_voice *v) { v->valid[v->idx] = 0; v->consumed++; v->idx = (v->idx + 1) & 3; v->have = 0; }
// Start the wave buffer at v->idx: its first sample. 0 when there is none to play
static int sv_begin(sv_rnd *r, sv_voice *v) {
    for (int tries = 0; tries < 4; tries++) {
        if (!v->valid[v->idx]) return 0;
        const uint8_t *wb = wb_of(v, v->idx);
        int32_t start = (int32_t)rd32(wb + 0x10), end = (int32_t)rd32(wb + 0x14);
        if (!rd64(wb) || start < 0 || end <= start) { sv_consume(v); continue; }   // an empty wave buffer is consumed at once
        v->pos = start; v->frac = 0;
        if (v->p[0xb] == 6) { adpcm_ctx(v, wb); v->dpos = start / 14 * 14; }
        v->have = 1; sv_decode(r, v);
        return 1;
    }
    return 0;
}
// One sample on: the wave buffer's end loops it or consumes it (an end of stream restarts the played count) and starts the next. 0 when the voice runs out
static int sv_step(sv_rnd *r, sv_voice *v) {
    const uint8_t *wb = wb_of(v, v->idx);
    v->pos++; v->played++;
    if (v->pos >= (int32_t)rd32(wb + 0x14)) {
        if (wb[0x18]) { v->pos = (int32_t)rd32(wb + 0x10); if (v->p[0xb] == 6) { adpcm_ctx(v, wb); v->dpos = v->pos / 14 * 14; } }
        else { int eos = wb[0x19]; sv_consume(v); if (eos) v->played = 0; return sv_begin(r, v); }
    }
    sv_decode(r, v);
    return 1;
}
static float *mb_of(sv_rnd *r, int mix, int buf) { return r->mb + ((size_t)mix * MIXBUF + (size_t)buf) * (size_t)r->samples; }
static int mix_index(sv_rnd *r, uint32_t id) {
    if (id == UNUSED_MIX) return -1;
    if ((int)id < r->nmix && rd32(r->mix + (size_t)id * MIX_IN + 0x10) == id) return (int)id;
    for (int i = 0; i < r->nmix; i++) if (rd32(r->mix + (size_t)i * MIX_IN + 0x10) == id) return i;
    return -1;
}
static void sv_voice_render(sv_rnd *r, sv_voice *v) {
    if (!v->inuse || v->state != 0) return;
    int fmt = v->p[0xb], nch = (int)rd32(v->p + 0x18);
    if (fmt != 1 && fmt != 2 && fmt != 6) { sv_red(r, 0, "a voice sample format other than PCM8, PCM16 and ADPCM is silent", fmt); return; }
    if (nch < 1 || nch > 6 || (fmt == 6 && nch != 1)) { sv_red(r, 1, "a voice channel count the renderer does not take is silent", nch); return; }
    if (rd32(v->p + 0x5c) != 0xffffffffu) sv_red(r, 2, "a voice sends to a splitter, which the host does not follow", (int)rd32(v->p + 0x5c));
    if (v->p[0x24] || v->p[0x30]) sv_red(r, 3, "a voice biquad filter is not applied", v->p[0x24] + v->p[0x30]);
    int m = mix_index(r, rd32(v->p + 0x58));
    if (m < 0) return;
    int dn = (int)rd32(r->mix + (size_t)m * MIX_IN + 8); if (dn > MIXBUF) dn = MIXBUF;
    float vol = rdf(v->p + 0x20), pitch = rdf(v->p + 0x1c), g[6][MIXBUF];
    for (int c = 0; c < nch; c++) {
        uint32_t id = rd32(v->p + 0x140 + 4 * c);
        // NOLINTNEXTLINE(clang-analyzer-core.NullPointerArithm): sv_grow sets ncr above 0 only with cr allocated. Unsigned: an id of 2^31 or more is no resource, not one before the array
        const uint8_t *cr = id < (uint32_t)r->ncr ? r->cr + (size_t)id * CR_IN : NULL;
        for (int j = 0; j < dn; j++) g[c][j] = cr && cr[0x64] ? rdf(cr + 4 + 4 * j) * vol : 0;
    }
    double step = (double)rd32(v->p + 0xc) * pitch / r->rate;
    if (!(step > 0)) return;
    if (step > 16) step = 16;
    if (!v->have && !sv_begin(r, v)) return;
    for (int s = 0; s < r->samples; s++) {
        for (int c = 0; c < nch; c++) for (int j = 0; j < dn; j++) if (g[c][j] != 0) mb_of(r, m, j)[s] += v->cur[c] * g[c][j];
        for (v->frac += step; v->frac >= 1; v->frac -= 1) if (!sv_step(r, v)) return;
    }
}

// ---------- effects, mixes, sink ----------
// The DSP side of an Aux buffer: its own read and write offsets at +0x40 / +0x44 of the info (the game's at +0 / +4: nn::audio::ReadAuxBuffer, sdk+0x1595bc), samples as int32
static void aux_write(uint8_t *info, int32_t *ring, uint32_t cap, const float *src, int n, uint32_t at, uint32_t advance) {
    uint32_t w = (rd32(info + 0x44) + at) % cap;
    for (int i = 0; i < n; i++, w = (w + 1) % cap) { float x = src[i]; ring[w] = x > 2147483520.f ? INT32_MAX : x < -2147483520.f ? INT32_MIN : (int32_t)lrintf(x); }
    if (advance) wr32(info + 0x44, (rd32(info + 0x44) + advance) % cap);
}
static void aux_read(uint8_t *info, const int32_t *ring, uint32_t cap, float *dst, int n, uint32_t at, uint32_t advance) {
    uint32_t rpos = (rd32(info + 0x40) + at) % cap;
    for (int i = 0; i < n; i++, rpos = (rpos + 1) % cap) dst[i] = (float)ring[rpos];
    if (advance) wr32(info + 0x40, (rd32(info + 0x40) + advance) % cap);
}
static void sv_effects(sv_rnd *r, int m, uint32_t id) {
    int order[64], n = 0;
    for (int i = 0; i < r->nfx && n < 64; i++) { const uint8_t *e = r->fx + (size_t)i * FX_IN; if (e[0] && rd32(e + 4) == id) order[n++] = i; }
    for (int a = 1; a < n; a++) for (int b = a; b > 0 && rd32(r->fx + (size_t)order[b] * FX_IN + 0x18) < rd32(r->fx + (size_t)order[b - 1] * FX_IN + 0x18); b--) { int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t; }
    int nb = (int)rd32(r->mix + (size_t)m * MIX_IN + 8), S = r->samples;
    for (int k = 0; k < n; k++) {
        const uint8_t *e = r->fx + (size_t)order[k] * FX_IN, *d = e + 0x20;
        int type = e[0], on = e[2];
        if (type == 2) {   // Aux: the mix's input buffers to the send ring, the return ring to the output buffers, one channel's frame after another
            uint32_t nch = rd32(d + 0x30), cap = rd32(d + 0x38);
            uint8_t *si = (uint8_t *)(uintptr_t)rd64(d + 0x40), *ri = (uint8_t *)(uintptr_t)rd64(d + 0x50);
            int32_t *sb = (int32_t *)(uintptr_t)rd64(d + 0x48), *rb = (int32_t *)(uintptr_t)rd64(d + 0x58);
            if (nch > MIXBUF || !cap || nch * (uint32_t)S > cap) { sv_red(r, 4, "an Aux effect whose ring does not hold a frame is skipped", (int)cap); continue; }
            for (uint32_t c = 0; c < nch; c++) {
                int in = d[c], out = d[0x18 + c];
                if (in >= nb || out >= nb) continue;
                uint32_t advance = c == nch - 1 ? nch * (uint32_t)S : 0;
                if (on && si && sb) aux_write(si, sb, cap, mb_of(r, m, in), S, c * (uint32_t)S, advance);
                if (on && ri && rb) aux_read(ri, rb, cap, mb_of(r, m, out), S, c * (uint32_t)S, advance);
                else if (in != out) memcpy(mb_of(r, m, out), mb_of(r, m, in), sizeof(float) * (size_t)S);
            }
        } else if (type == 1) {   // buffer mixer: out[k] += in[k] × volume[k]
            if (!on) continue;
            uint32_t cnt = rd32(d + 0x90); if (cnt > MIXBUF) cnt = MIXBUF;
            for (uint32_t c = 0; c < cnt; c++) {
                int in = d[c], out = d[0x18 + c]; float g = rdf(d + 0x30 + 4 * c);
                if (in >= nb || out >= nb) continue;
                float *o = mb_of(r, m, out); const float *i0 = mb_of(r, m, in);
                for (int s = 0; s < S; s++) o[s] += i0[s] * g;
            }
        } else if (type >= 3 && type <= 6) {   // delay, reverb, I3DL2 reverb, biquad: the dry signal
            if (!on) continue;
            sv_red(r, 5 + type, "a delay / reverb / I3DL2 reverb / biquad effect passes the dry signal only (type)", type);
            int nc = type == 6 ? 0 : d[0xe] | d[0xf] << 8;
            for (int c = 0; c < nc && c < 6; c++) { int in = d[c], out = d[6 + c]; if (in < nb && out < nb && in != out) memcpy(mb_of(r, m, out), mb_of(r, m, in), sizeof(float) * (size_t)S); }
        }
    }
}
// One frame: voices into their mixes, sub mixes deepest first, the final mix through the device sink. dst: stereo, r->samples frames. A sink's parameter: +0 type (1 device,
// 2 circular buffer), +1 in use, +4 node ID, the device's from +0x20 (SinkManager::UpdateSinkInParameter, sdk+0x15d6e0): its name, +0x120 input count, +0x124 six input buffers,
// +0x12b down-mix parameters set, +0x12c the four (front, centre, LFE, back)
static void sv_render(sv_rnd *r, int16_t *dst) {
    int S = r->samples;
    memset(r->mb, 0, sizeof(float) * (size_t)r->nmix * MIXBUF * (size_t)S);
    memset(dst, 0, sizeof(int16_t) * 2 * (size_t)S);
    for (int i = 0; i < r->nvoice; i++) sv_voice_render(r, &r->voice[i]);
    int depth[64] = {0}, maxd = 0, fin = -1;
    for (int i = 0; i < r->nmix && i < 64; i++) {
        const uint8_t *m = r->mix + (size_t)i * MIX_IN;
        if (!m[0xc]) { depth[i] = -1; continue; }
        if (rd32(m + 0x10) == 0) { fin = i; continue; }
        int d = 0, k = i;
        while (d < 64 && k >= 0 && rd32(r->mix + (size_t)k * MIX_IN + 0x10) != 0) { k = mix_index(r, rd32(r->mix + (size_t)k * MIX_IN + 0x924)); d++; }
        depth[i] = k >= 0 && d < 64 ? d : -1;
        if (depth[i] > maxd) maxd = depth[i];
        if (rd32(m + 0x928) != 0xffffffffu) sv_red(r, 12, "a sub mix sends to a splitter, which the host does not follow", (int)rd32(m + 0x928));
    }
    for (int d = maxd; d >= 1; d--)
        for (int i = 0; i < r->nmix && i < 64; i++) {
            if (depth[i] != d) continue;
            const uint8_t *m = r->mix + (size_t)i * MIX_IN;
            sv_effects(r, i, rd32(m + 0x10));
            int t = mix_index(r, rd32(m + 0x924)); if (t < 0) continue;
            int nb = (int)rd32(m + 8), tb = (int)rd32(r->mix + (size_t)t * MIX_IN + 8); float vol = rdf(m);
            if (nb > MIXBUF) nb = MIXBUF;
            if (tb > MIXBUF) tb = MIXBUF;
            for (int a = 0; a < nb; a++) for (int b = 0; b < tb; b++) {
                float g = vol * rdf(m + 0x24 + 4 * (a * MIXBUF + b)); if (g == 0) continue;
                const float *src = mb_of(r, i, a); float *o = mb_of(r, t, b);
                for (int s = 0; s < S; s++) o[s] += src[s] * g;
            }
        }
    if (fin < 0) return;
    sv_effects(r, fin, 0);
    float fv = rdf(r->mix + (size_t)fin * MIX_IN); int fb = (int)rd32(r->mix + (size_t)fin * MIX_IN + 8);
    for (int k = 0; k < r->nsink; k++) {
        const uint8_t *sk = r->sink + (size_t)k * SINK_IN;
        if (!sk[1]) continue;
        if (sk[0] != 1) { sv_red(r, 13, "a circular buffer sink is not filled", sk[0]); continue; }
        int n = (int)rd32(sk + 0x120), in[6];
        if (n < 1 || n > 6) continue;
        for (int c = 0; c < n; c++) in[c] = sk[0x124 + c] < fb ? sk[0x124 + c] : -1;
        float co[4] = {1.f, 0.707f, 0.251f, 0.707f};   // front, centre, LFE, back
        if (sk[0x12b]) for (int c = 0; c < 4; c++) co[c] = rdf(sk + 0x12c + 4 * c);
        for (int s = 0; s < S; s++) {
            float ch[6] = {0};
            for (int c = 0; c < n; c++) if (in[c] >= 0) ch[c] = mb_of(r, fin, in[c])[s] * fv;
            float L, R;
            if (n == 1) L = R = ch[0];
            else if (n < 6) { L = ch[0]; R = ch[1]; }
            else { L = ch[0] * co[0] + ch[2] * co[1] + ch[3] * co[2] + ch[4] * co[3]; R = ch[1] * co[0] + ch[2] * co[1] + ch[3] * co[2] + ch[5] * co[3]; }
            int32_t l = (int32_t)lrintf(L), rr = (int32_t)lrintf(R);
            dst[2 * s] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            dst[2 * s + 1] = (int16_t)(rr > 32767 ? 32767 : rr < -32768 ? -32768 : rr);
        }
        break;   // the first device sink is the console's output
    }
}

// ---------- the update ----------
static int sv_grow(uint8_t **p, int *n, int want, size_t each) {
    if (want > *n) { uint8_t *q = realloc(*p, (size_t)want * each); if (!q) return 0; memset(q + (size_t)*n * each, 0, (size_t)(want - *n) * each); *p = q; }
    *n = want; return 1;
}
static Result sv_update(sv_rnd *r, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    if (!in || in_len < HDR) { LOG("[RED] audren:u: RequestUpdate without an update (%zu bytes)", in_len); return RES_FAIL; }
    uint32_t beh = rd32(in + 4), mp = rd32(in + 8), vo = rd32(in + 0xc), cr = rd32(in + 0x10), fx = rd32(in + 0x14), mx = rd32(in + 0x18), sk = rd32(in + 0x1c),
             perf = rd32(in + 0x20), spl = rd32(in + 0x24), total = rd32(in + 0x3c);
    uint64_t sum = (uint64_t)HDR + beh + mp + vo + cr + fx + mx + sk + perf + spl;
    if (sum != total || total > in_len || mp % MP_IN || vo % V_IN || cr % CR_IN || fx % FX_IN || mx % MIX_IN || sk % SINK_IN) {
        LOG("[RED] audren:u: update header does not add up (total %#x of %#zx; memory pools %#x voices %#x resources %#x effects %#x mixes %#x sinks %#x)", total, in_len, mp, vo, cr, fx, mx, sk);
        return RES_FAIL;
    }
    int nmp = (int)(mp / MP_IN), nvo = (int)(vo / V_IN), nfx = (int)(fx / FX_IN), nsk = (int)(sk / SINK_IN);
    size_t need = HDR + (size_t)nmp * MP_OUT + (size_t)nvo * V_OUT + (size_t)nfx * FX_OUT + (size_t)nsk * SINK_OUT + PERF_IO + BEH_OUT;
    if (!out || out_len < need) { LOG("[RED] audren:u: the answer needs %#zx bytes, the client's buffer has %#zx", need, out_len); return RES_FAIL; }
    if (out_len != need) { static int once; if (!once++) LOG("[RED] audren:u: the answer is %#zx bytes, the client's buffer %#zx (SDK 1.3.1 aborts unless they are equal)", need, out_len); }
    const uint8_t *q = in + HDR, *mps = q + beh, *crs = mps + mp, *vos = crs + cr, *fxs = vos + vo, *mxs = fxs + fx + spl, *sks = mxs + mx;
    pthread_mutex_lock(&r->mu);
    int ok = sv_grow(&r->cr, &r->ncr, (int)(cr / CR_IN), CR_IN) && sv_grow(&r->fx, &r->nfx, nfx, FX_IN) && sv_grow(&r->sink, &r->nsink, nsk, SINK_IN);
    int nmx = (int)(mx / MIX_IN);
    if (ok && nmx > r->nmix) {
        float *mb = realloc(r->mb, sizeof(float) * (size_t)nmx * MIXBUF * (size_t)r->samples);
        ok = mb != NULL; if (mb) r->mb = mb;
    }
    if (ok) ok = sv_grow(&r->mix, &r->nmix, nmx, MIX_IN);
    if (ok && nvo > r->nvoice) {
        sv_voice *v = realloc(r->voice, sizeof *v * (size_t)nvo);
        ok = v != NULL; if (v) { memset(v + r->nvoice, 0, sizeof *v * (size_t)(nvo - r->nvoice)); r->voice = v; r->nvoice = nvo; }
    }
    if (!ok) { pthread_mutex_unlock(&r->mu); LOG("[RED] audren:u: out of memory for the renderer's state"); return RES_FAIL; }
    memcpy(r->cr, crs, cr); memcpy(r->fx, fxs, fx); memcpy(r->mix, mxs, mx); memcpy(r->sink, sks, sk);
    for (int i = 0; i < nvo; i++) {
        const uint8_t *p = vos + (size_t)i * V_IN; sv_voice *v = &r->voice[i];
        if (!p[9]) { v->inuse = 0; continue; }
        if (p[8] || !v->inuse) { memset(v, 0, sizeof *v); v->state = 1; }   // a new voice starts from its first wave buffer slot, nothing played
        v->inuse = 1;
        memcpy(v->p, p, V_IN);
        for (int k = 0; k < 4; k++) if (!wb_of(v, k)[0x1a]) { v->valid[k] = 1; if (k == v->idx) v->have = 0; }   // appended since the last update
        for (int n = p[0x159]; n > 0 && v->valid[v->idx]; n--) sv_consume(v);
        int st = p[0xa];
        if (st == 1 && v->state != 1) {   // stopped: every queued wave buffer is consumed, the played count starts over
            for (int k = 0; k < 4 && v->valid[v->idx]; k++) sv_consume(v);   // in ring order from the one playing: the next slot is where the client appends next
            v->played = 0; v->have = 0; v->h1 = v->h2 = 0;
        }
        v->state = st;
    }
    for (int i = nvo; i < r->nvoice; i++) r->voice[i].inuse = 0;
    // The answer, in the client's order: memory pools (a requested attach is attached, a requested detach detached), voices, effects (3 enabled, 4 disabled), sinks, performance, behaviour
    memset(out, 0, need);
    wr32(out, rd32(in)); wr32(out + 4, BEH_OUT); wr32(out + 8, (uint32_t)nmp * MP_OUT); wr32(out + 0xc, (uint32_t)nvo * V_OUT); wr32(out + 0x14, (uint32_t)nfx * FX_OUT);
    wr32(out + 0x1c, (uint32_t)nsk * SINK_OUT); wr32(out + 0x20, PERF_IO); wr32(out + 0x3c, (uint32_t)need);
    uint8_t *o = out + HDR;
    for (int i = 0; i < nmp; i++, o += MP_OUT) { uint32_t st = rd32(mps + (size_t)i * MP_IN + 0x10); wr32(o, st == 4 ? 5 : st == 2 ? 3 : 0); }
    for (int i = 0; i < nvo; i++, o += V_OUT) { const sv_voice *v = &r->voice[i]; if (v->inuse) { wr64(o, v->played); wr32(o + 8, v->consumed); } }
    for (int i = 0; i < nfx; i++, o += FX_OUT) { const uint8_t *e = r->fx + (size_t)i * FX_IN; o[0] = e[0] ? (e[2] ? 3 : 4) : 0; }
    static int logged; if (!logged++) LOG("audren:u: first update: %d memory pools, %d voices, %d channel resources, %d effects, %d mixes, %d sinks, splitters %#x bytes, answer %#zx", nmp, nvo, r->ncr, nfx, r->nmix, nsk, spl, need);
    pthread_mutex_unlock(&r->mu);
    return RES_OK;
}

// ---------- the render thread ----------
static int sv_out_frames(sv_rnd *r) { return (int)((int64_t)r->samples * OUT_RATE / r->rate); }
static void sv_to_48k(sv_rnd *r, int16_t *dst) {   // a renderer below 48 kHz (32 kHz: 160 samples per 5 ms) to the output's 240, linearly
    int n = sv_out_frames(r);
    if (r->rate == OUT_RATE) { memcpy(dst, r->frame, sizeof(int16_t) * 2 * (size_t)n); return; }
    double step = (double)r->rate / OUT_RATE, pos = r->up_pos;
    for (int k = 0; k < n; k++, pos += step) {
        int i = (int)pos; float f = (float)(pos - i);
        for (int c = 0; c < 2; c++) {
            // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): OpenAudioRenderer takes 1 to FRAME_MAX samples, and pos stays below samples here, so 0 <= i - 1 < samples
            float a = i == 0 ? r->up_last[c] : r->frame[2 * (i - 1) + c], b = r->frame[2 * (i < r->samples ? i : r->samples - 1) + c];
            dst[2 * k + c] = (int16_t)lrintf(a + (b - a) * f);
        }
    }
    r->up_pos = pos - r->samples;
    r->up_last[0] = r->frame[2 * (r->samples - 1)]; r->up_last[1] = r->frame[2 * (r->samples - 1) + 1];
}
static void sv_render_main(void *arg) {
    sv_rnd *r = arg;
    void *out = dm_pcmout_open(2);
    aout_buf *free_list[NBUF]; int nfree = NBUF, n = sv_out_frames(r);
    for (int i = 0; i < NBUF; i++) { r->bufs[i] = (aout_buf){.ptr = r->pcm[i], .cap = sizeof r->pcm[i], .size = 0}; free_list[i] = &r->bufs[i]; }
    dm_pcmout_start(out);
    LOG("audren:u: render thread started (%d Hz, %d samples per frame, paced by the output)", r->rate, r->samples);
    while (atomic_load(&r->open)) {
        while (!nfree) {
            aout_buf *b = dm_pcmout_released(out);
            if (b) { free_list[nfree++] = b; break; }
            dm_pcmout_wait(out, 10);
            if (!atomic_load(&r->open)) break;
        }
        if (!nfree) break;
        aout_buf *b = free_list[--nfree];
        pthread_mutex_lock(&r->mu);
        int started = r->started;
        if (started) sv_render(r, r->frame); else memset(r->frame, 0, sizeof(int16_t) * 2 * (size_t)r->samples);
        pthread_mutex_unlock(&r->mu);
        sv_to_48k(r, (int16_t *)b->ptr);
        uint64_t loud = 0; for (int k = 0; k < 2 * r->samples; k++) loud |= (uint16_t)r->frame[k];
        uint64_t f = atomic_fetch_add(&r->frames, 1) + 1; if (loud) atomic_fetch_add(&r->loud, 1);
        if (f == 200 || f == 2000 || f % 12000 == 0) LOG("audren:u: %llu frames rendered, %llu of them not silent", (unsigned long long)f, (unsigned long long)atomic_load(&r->loud));
        b->size = (uint64_t)n * 4;
        if (!dm_pcmout_append(out, b)) free_list[nfree++] = b;
        if (started) hle_signal_event_handle(r->ev_w);
    }
    dm_pcmout_close(out);
}

// ---------- the sessions ----------
static void sv_close(void *ctx) {
    sv_rnd *r = ctx;
    atomic_store(&r->open, 0);
    if (r->thread) dm_guest_thread_join(r->thread);
    LOG("audren:u: renderer closed after %llu frames", (unsigned long long)atomic_load(&r->frames));
    free(r->voice); free(r->cr); free(r->fx); free(r->mix); free(r->sink); free(r->mb);
    pthread_mutex_destroy(&r->mu); free(r);
}
static Result sv_renderer(sf_req *q) {
    sv_rnd *r = q->ctx;
    switch (q->cmd) {
    case 0: wr32(q->out, (uint32_t)r->rate); q->out_len = 4; return RES_OK;
    case 1: wr32(q->out, (uint32_t)r->samples); q->out_len = 4; return RES_OK;
    case 2: wr32(q->out, (uint32_t)r->nmixbuf); q->out_len = 4; return RES_OK;
    case 3: wr32(q->out, r->started ? 0 : 1); q->out_len = 4; return RES_OK;
    case 4: return sv_update(r, q->na ? q->a[0].p : NULL, q->na ? q->a[0].size : 0, q->nb ? q->b[0].p : NULL, q->nb ? q->b[0].size : 0);
    case 5: pthread_mutex_lock(&r->mu); r->started = 1; pthread_mutex_unlock(&r->mu); return RES_OK;
    case 6: pthread_mutex_lock(&r->mu); r->started = 0; pthread_mutex_unlock(&r->mu); return RES_OK;
    case 7: q->out_copy = hle_dup_readable_handle(r->ev_r); return q->out_copy ? RES_OK : RES_FAIL;
    case 8: if (q->in_len >= 4) r->time_limit = rd32(q->in); return RES_OK;
    case 9: wr32(q->out, r->time_limit); q->out_len = 4; return RES_OK;
    default: LOG("[RED] audren:u: renderer command %u answered with success and no output", q->cmd); return RES_OK;
    }
}
// The parameter: +0 sample rate, +4 samples per frame, +8 mix buffers, +0xc sub mixes, +0x10 voices, +0x14 sinks, +0x18 effects, +0x1c performance frames, +0x20 voice drop,
// +0x24 splitters, +0x28 splitter destinations, +0x30 revision
static Result sf_audren(sf_req *q) {
    if (q->cmd == 1) {
        if (q->in_len < PARAM) return RES_FAIL;
        size_t n = dm_audren_work_size(q->in, rd32(q->in + 0x30));
        if (!n) { LOG("[RED] audren:u: GetWorkBufferSize for revision %#x, which the service budget does not know: 1 MB", rd32(q->in + 0x30)); n = 0x100000; }
        wr64(q->out, n); q->out_len = 8;
        LOG("audren:u: work buffer %#zx for %d Hz, %d voices, revision %.4s", n, (int)rd32(q->in), (int)rd32(q->in + 0x10), (const char *)q->in + 0x30);
        return RES_OK;
    }
    if (q->cmd == 2) { q->out_move = sf_session("audren:d", NULL, NULL, NULL); LOG("audren:u: audio device service (zeroed answers)"); return RES_OK; }
    if (q->cmd != 0) { LOG("[RED] audren:u: manager command %u answered with success and no output", q->cmd); return RES_OK; }
    if (q->in_len < 0x48) return RES_FAIL;
    int rate = (int)rd32(q->in), samples = (int)rd32(q->in + 4);
    if ((rate != 48000 && rate != 32000) || samples < 1 || samples > FRAME_MAX || samples * OUT_RATE % rate) {
        LOG("[RED] audren:u: OpenAudioRenderer at %d Hz with %d samples per frame, which the host does not render", rate, samples); return 0x2c99u;   // audio ResultInvalidArgument (as the client checks)
    }
    sv_rnd *r = calloc(1, sizeof *r);
    if (!r) return RES_FAIL;
    pthread_mutex_init(&r->mu, NULL);
    memcpy(r->param, q->in, PARAM);
    r->rate = rate; r->samples = samples; r->nmixbuf = (int)rd32(q->in + 8); r->time_limit = 100;
    r->ev_r = hle_new_event(&r->ev_w);
    atomic_store(&r->open, 1);
    LOG("audren:u: OpenAudioRenderer %d Hz, %d samples per frame, %d mix buffers, %d sub mixes, %d voices, %d sinks, %d effects, %d splitters, revision %.4s, work buffer %#llx",
        rate, samples, r->nmixbuf, (int)rd32(q->in + 0xc), (int)rd32(q->in + 0x10), (int)rd32(q->in + 0x14), (int)rd32(q->in + 0x18), (int)rd32(q->in + 0x24),
        (const char *)q->in + 0x30, (unsigned long long)rd64(q->in + 0x38));
    r->thread = dm_guest_thread_spawn(sv_render_main, r, 9, 0x10000);
    q->out_move = sf_session("audren:r", sv_renderer, r, sv_close);
    return RES_OK;
}
SF_SERVICE("audren:u", sf_audren);
