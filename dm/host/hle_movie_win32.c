// H.264 and AAC decoding for the movie decoder (hle_movie.c) on Windows: Media Foundation's decoder transforms, which every Windows 10 and 11 has but the N editions (those
// get them with the Media Feature Pack), so the port adds no decoder of its own there. Behind the interfaces Apple's backends use: mavc_* (hle_movie_avc.h; VideoToolbox
// in hle_movie_avc.c, and Linux's black-picture stopgap there) and maac_* (hle_movie_aac.h; Apple's AudioToolbox converter is in hle_movie.c). docs/reference/movies.md,
// "H.264 and AAC on Windows through Media Foundation".
//
// Loading. mfplat.dll is loaded at the first movie (LoadLibraryEx from System32) and its four functions taken by name, so the program starts where it is absent; ole32
// (COM) and mfuuid (the attribute and format GUIDs, a static library) are linked. The decoders are the transforms by their class IDs: CLSID_MSH264DecoderMFT
// (msmpeg2vdec.dll) and CLSID_CMSAACDecMFT (msauddecmft.dll), software and synchronous; no Direct3D device is given to them, so their pictures are in system memory.
// Where Media Foundation or a transform is missing, or refuses the stream, one [RED] line says why and that an N edition needs the Media Feature Pack, and the movie goes
// on as on Linux: black pictures at the access units' times, silence for the sound.
//
// Threads. hle_movie.c runs every decode, take and close on the movie decoder's own host thread (md_worker); COM is initialised there when a transform is made and released
// in mavc_free / maac_free, which md_close calls on the same thread. The parameter sets (Configure) and a flush (md_reset) come from guest threads: they only change this
// file's own state, and a flush reaches the transform at the next call on the decoder's thread.
//
// Video. The access units come as the SDK's demuxer gives them (Annex B, or 4-byte lengths) and the parameter sets from Configure (csd-0 / csd-1, or the avcC record) or the
// stream; the transform takes Annex B, with the SPS and PPS put in front of every IDR access unit. It reorders B-frames itself and hands pictures out in presentation order with
// the input samples' times, so no queue depth is kept (VideoToolbox, which decodes in decode order, keeps 4); with flush (the end of the stream) it is drained. Its NV12
// output is read through IMF2DBuffer's pitch, the chroma plane after the frame size's padded height (1088 for 1080 lines), the picture cut to the minimum display
// aperture; a format change (MF_E_TRANSFORM_STREAM_CHANGE, a new size) picks NV12 again. NV12 is handed out in video range, as VideoToolbox's 420YpCbCr8BiPlanarVideoRange
// output is (a full-range stream is scaled); RGBA (the native texture mode) is converted here with the matrix and range the SPS's VUI declares, BT.709 or BT.601, and
// when it declares none BT.709 from 720 lines up, else BT.601, as hle_movie.c converts VP9.
#if defined(_WIN32)
#define COBJMACROS
#include "dm.h"
#include "hle_movie_avc.h"
#include "hle_movie_aac.h"
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>

static const GUID k_clsid_h264 = {0x62ce7e72, 0x4c71, 0x4d20, {0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d}};   // CLSID_MSH264DecoderMFT (wmcodecdsp.h)
static const GUID k_clsid_aac = {0x32d186a7, 0x218f, 0x4c75, {0x88, 0x76, 0xdd, 0x77, 0x27, 0x3a, 0x89, 0x99}};    // CLSID_CMSAACDecMFT (wmcodecdsp.h)

// ---------- Media Foundation, loaded at the first use ----------
static struct {
    pthread_once_t once; int ok; char why[160];
    HRESULT (WINAPI *startup)(ULONG, DWORD);
    HRESULT (WINAPI *media_type)(IMFMediaType **);
    HRESULT (WINAPI *sample)(IMFSample **);
    HRESULT (WINAPI *buffer)(DWORD, IMFMediaBuffer **);
} g_mf = {PTHREAD_ONCE_INIT};
static int g_mf_test_absent;   // t_movie_mf, t_movie_avc: Media Foundation taken as absent (the N editions' case)
// DM_MOVIE_TRACE (hle_movie.c's switch): every call into a transform and its result
static int mf_trace(void) { static int on = -1; if (on < 0) on = getenv("DM_MOVIE_TRACE") != NULL; return on; }
#define MF_TRACE(fmt, ...) do { if (mf_trace()) LOG("[movie] [mf] " fmt, ##__VA_ARGS__); } while (0)
static void mf_load(void) {
    HMODULE m = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) { snprintf(g_mf.why, sizeof g_mf.why, "Media Foundation is not installed (mfplat.dll: error %lu)", (unsigned long)GetLastError()); return; }
    g_mf.startup = (void *)GetProcAddress(m, "MFStartup"); g_mf.media_type = (void *)GetProcAddress(m, "MFCreateMediaType");
    g_mf.sample = (void *)GetProcAddress(m, "MFCreateSample"); g_mf.buffer = (void *)GetProcAddress(m, "MFCreateMemoryBuffer");
    if (!g_mf.startup || !g_mf.media_type || !g_mf.sample || !g_mf.buffer) { snprintf(g_mf.why, sizeof g_mf.why, "mfplat.dll lacks MFStartup, MFCreateMediaType, MFCreateSample or MFCreateMemoryBuffer"); return; }
    HRESULT hr = g_mf.startup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) { snprintf(g_mf.why, sizeof g_mf.why, "MFStartup failed (0x%08lx)", (unsigned long)hr); return; }
    g_mf.ok = 1;
}
// COM on the calling thread, once per decoder; released with it on the same thread
typedef struct { int on; DWORD tid; } com_t;
static int com_enter(com_t *c, char *why, size_t wn) {
    if (c->on) return 1;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) return 1;   // the thread is in a single-threaded apartment already: the transforms (both-threaded) work there, and it is not ours to leave
    if (FAILED(hr)) { snprintf(why, wn, "CoInitializeEx failed (0x%08lx)", (unsigned long)hr); return 0; }
    c->on = 1; c->tid = GetCurrentThreadId(); return 1;
}
static void com_leave(com_t *c) {
    if (!c->on) return;
    if (c->tid == GetCurrentThreadId()) CoUninitialize();
    else LOG("[RED] movie: a Media Foundation decoder was freed on another thread than the one that made it: that thread's COM is left initialised");
    c->on = 0;
}
// A transform by class ID, with the reason when there is none
static IMFTransform *mf_transform(const GUID *clsid, const char *what, com_t *com, char *why, size_t wn) {
    if (g_mf_test_absent) { snprintf(why, wn, "Media Foundation taken as absent (test)"); return NULL; }
    pthread_once(&g_mf.once, mf_load);
    if (!g_mf.ok) { snprintf(why, wn, "%s", g_mf.why); return NULL; }
    if (!com_enter(com, why, wn)) return NULL;
    IMFTransform *t = NULL;
    HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&t);
    if (FAILED(hr)) {
        snprintf(why, wn, hr == REGDB_E_CLASSNOTREG ? "Media Foundation's %s decoder is not installed (CoCreateInstance 0x%08lx)" : "Media Foundation's %s decoder cannot be made (CoCreateInstance 0x%08lx)",
                 what, (unsigned long)hr);
        return NULL;
    }
    return t;
}
// A sample of one buffer of cap bytes, holding n bytes of data when data is given; t: its time in 100 ns (-1: none)
static IMFSample *mf_sample(const void *data, DWORD n, DWORD cap, LONGLONG t) {
    IMFMediaBuffer *b = NULL; IMFSample *s = NULL; BYTE *p = NULL;
    if (FAILED(g_mf.buffer(cap ? cap : 1, &b))) return NULL;
    if (data && SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) { memcpy(p, data, n); IMFMediaBuffer_Unlock(b); IMFMediaBuffer_SetCurrentLength(b, n); }
    if (FAILED(g_mf.sample(&s))) { IMFMediaBuffer_Release(b); return NULL; }
    IMFSample_AddBuffer(s, b); IMFMediaBuffer_Release(b);
    if (t >= 0) IMFSample_SetSampleTime(s, t);
    return s;
}

// ---------- H.264 ----------
#define QMAX 16
typedef struct { uint8_t *nv12; int w, h; int64_t pts; } pic_t;   // nv12 NULL: a black picture (no decoder)
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps;
    int profile, coded_w, coded_h, disp_w, disp_h, full, matrix;   // from the SPS: its profile, sizes, VUI range and matrix (0: not declared)
    int rgba, red, failed, flush_pending, drained; char why[200];
    com_t com; IMFTransform *t;
    int fw, fh, ax, ay, aw, ah, stride;   // the output type: frame size (the planes' padded size), display aperture, default stride (0: none)
    DWORD out_cb, out_flags;
    pic_t q[QMAX]; int nq;
    uint8_t *au; size_t aucap, aulen;
};
mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); return a; }
static void q_free(mavc *a) { for (int i = 0; i < a->nq; i++) free(a->q[i].nv12); a->nq = 0; }
void mavc_drop(mavc *a) { q_free(a); if (a->t) a->flush_pending = 1; }   // the transform is flushed on the decoder's thread, at its next call
void mavc_free(mavc *a) {
    if (!a) return;
    if (a->t) { IMFTransform_ProcessMessage(a->t, MFT_MESSAGE_NOTIFY_END_STREAMING, 0); IMFTransform_Release(a->t); a->t = NULL; }
    com_leave(&a->com); q_free(a); free(a->au); free(a);
}
int mavc_queued(const mavc *a) { return a->nq; }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }
int mavc_constant(const mavc *a) { return a->failed; }   // without a decoder every picture is black: hle_movie.c md_out_tex writes a texture once

// The SPS's profile, sizes and VUI colour description (H.264 7.3.2.1.1, E.1.1), over its RBSP (emulation prevention bytes taken out)
typedef struct { const uint8_t *p; size_t n, i; } br_t;
static unsigned rb(br_t *b, int k) { unsigned v = 0; while (k-- > 0) { v = v << 1 | (b->i >> 3 < b->n ? b->p[b->i >> 3] >> (7 - (b->i & 7)) & 1 : 0); b->i++; } return v; }
static unsigned ue(br_t *b) { int z = 0; while (!rb(b, 1) && z < 31 && b->i >> 3 < b->n) z++; return (1u << z) - 1 + rb(b, z); }
static int se(br_t *b) { unsigned k = ue(b); return k & 1 ? (int)((k + 1) / 2) : -(int)(k / 2); }
static void sps_info(mavc *a, const uint8_t *nal, size_t n) {
    uint8_t r[512]; size_t m = 0; int z = 0;
    for (size_t i = 1; i < n && m < sizeof r; i++) { if (z >= 2 && nal[i] == 3) { z = 0; continue; } r[m++] = nal[i]; z = nal[i] ? 0 : z + 1; }
    br_t b = {r, m, 0};
    int profile = (int)rb(&b, 8); rb(&b, 16); ue(&b);
    int cf = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128 ||
        profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        cf = (int)ue(&b); if (cf == 3) rb(&b, 1);
        ue(&b); ue(&b); rb(&b, 1);
        if (rb(&b, 1)) for (int i = 0; i < (cf != 3 ? 8 : 12); i++) if (rb(&b, 1)) {   // scaling lists: skipped
            int last = 8, next = 8;
            for (int j = 0; j < (i < 6 ? 16 : 64); j++) { if (next) next = (last + se(&b) + 256) % 256; if (next) last = next; }
        }
    }
    ue(&b);
    unsigned poc = ue(&b);
    if (poc == 0) ue(&b);
    else if (poc == 1) { rb(&b, 1); se(&b); se(&b); unsigned k = ue(&b); for (unsigned i = 0; i < k && i < 256; i++) se(&b); }
    ue(&b); rb(&b, 1);
    int wmb = (int)ue(&b) + 1, hmu = (int)ue(&b) + 1, fmo = (int)rb(&b, 1);
    if (!fmo) rb(&b, 1);
    rb(&b, 1);
    int cl = 0, cr = 0, ct = 0, cb = 0;
    if (rb(&b, 1)) { cl = (int)ue(&b); cr = (int)ue(&b); ct = (int)ue(&b); cb = (int)ue(&b); }
    int sx = cf == 1 || cf == 2 ? 2 : 1, sy = (cf == 1 ? 2 : 1) * (2 - fmo);
    a->profile = profile; a->coded_w = wmb * 16; a->coded_h = hmu * 16 * (2 - fmo);
    a->disp_w = a->coded_w - (cl + cr) * sx; a->disp_h = a->coded_h - (ct + cb) * sy;
    a->full = 0; a->matrix = 0;
    if (rb(&b, 1)) {   // VUI
        if (rb(&b, 1) && rb(&b, 8) == 255) rb(&b, 32);   // aspect ratio (extended SAR)
        if (rb(&b, 1)) rb(&b, 1);                         // overscan
        if (rb(&b, 1)) { rb(&b, 3); a->full = (int)rb(&b, 1); if (rb(&b, 1)) { rb(&b, 16); a->matrix = (int)rb(&b, 8); } }
    }
}
static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n;
    if (t == 7) sps_info(a, nal, n);
}
// walk the NAL units of p (Annex B start codes, or else 4-byte lengths); fn gets each unit without its prefix (hle_movie_avc.c's walk, for VideoToolbox)
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {   // AVCDecoderConfigurationRecord: as hle_movie_avc.c reads it
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}
static void au_put(mavc *a, const uint8_t *nal, size_t n) {   // a NAL unit with a 4-byte start code, at the end of the access unit being built
    if (a->aulen + 4 + n > a->aucap) { a->aucap = (a->aulen + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + a->aulen; o[0] = o[1] = o[2] = 0; o[3] = 1; memcpy(o + 4, nal, n); a->aulen += 4 + n;
}
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    mavc *a = ctx; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    if (t == 5 && a->nsps && a->npps) { au_put(a, a->sps, (size_t)a->nsps); au_put(a, a->pps, (size_t)a->npps); }   // the parameter sets before every IDR
    au_put(a, nal, n);
}

static void no_decoder(mavc *a) {
    a->failed = 1;
    if (a->t) { IMFTransform_Release(a->t); a->t = NULL; }
    static int told; if (!told++) LOG("[RED] movie: no H.264 decoder (%s): black pictures at the movie's times instead of its video; on a Windows N edition, install the Media Feature Pack", a->why);
}
// The output type: NV12 among the types the transform offers now; its frame size, aperture and stride, the size of the samples it wants
static int set_output(mavc *a) {
    IMFMediaType *mt = NULL; HRESULT hr = S_OK;
    for (DWORD i = 0; SUCCEEDED(hr = IMFTransform_GetOutputAvailableType(a->t, 0, i, &mt)); i++) {
        GUID st; if (SUCCEEDED(IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, &st)) && IsEqualGUID(&st, &MFVideoFormat_NV12)) break;
        IMFMediaType_Release(mt); mt = NULL;
    }
    if (!mt) { snprintf(a->why, sizeof a->why, "the H.264 decoder offers no NV12 output (0x%08lx)", (unsigned long)hr); return 0; }
    if (FAILED(hr = IMFTransform_SetOutputType(a->t, 0, mt, 0))) { IMFMediaType_Release(mt); snprintf(a->why, sizeof a->why, "the H.264 decoder refuses its NV12 output (SetOutputType 0x%08lx)", (unsigned long)hr); return 0; }
    UINT64 fs = 0; IMFMediaType_GetUINT64(mt, &MF_MT_FRAME_SIZE, &fs);
    a->fw = (int)(fs >> 32); a->fh = (int)(UINT32)fs;
    MFVideoArea ar; a->ax = a->ay = 0; a->aw = a->fw; a->ah = a->fh;
    if (SUCCEEDED(IMFMediaType_GetBlob(mt, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&ar, sizeof ar, NULL)) && ar.Area.cx > 0 && ar.Area.cy > 0) {
        a->ax = ar.OffsetX.value; a->ay = ar.OffsetY.value; a->aw = (int)ar.Area.cx; a->ah = (int)ar.Area.cy;
        if (a->ax < 0 || a->ay < 0 || a->ax + a->aw > a->fw || a->ay + a->ah > a->fh) { a->ax = a->ay = 0; a->aw = a->fw; a->ah = a->fh; }
    }
    UINT32 st = 0; a->stride = SUCCEEDED(IMFMediaType_GetUINT32(mt, &MF_MT_DEFAULT_STRIDE, &st)) ? (int)st : 0;
    MF_TRACE("AVC output type: NV12 %d×%d, aperture %d,%d %d×%d, stride %d", a->fw, a->fh, a->ax, a->ay, a->aw, a->ah, a->stride);
    IMFMediaType_Release(mt);
    MFT_OUTPUT_STREAM_INFO oi = {0}; IMFTransform_GetOutputStreamInfo(a->t, 0, &oi);
    a->out_cb = oi.cbSize ? oi.cbSize : (DWORD)(a->fw * a->fh * 3 / 2); a->out_flags = oi.dwFlags;
    return 1;
}
static int open_decoder(mavc *a) {
    if (!(a->t = mf_transform(&k_clsid_h264, "H.264", &a->com, a->why, sizeof a->why))) return 0;
    IMFMediaType *mt = NULL; HRESULT hr = g_mf.media_type(&mt);
    if (SUCCEEDED(hr)) {
        IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video); IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
        IMFMediaType_SetUINT32(mt, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (a->coded_w > 0 && a->coded_h > 0) IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, (UINT64)a->coded_w << 32 | (UINT32)a->coded_h);
        hr = IMFTransform_SetInputType(a->t, 0, mt, 0);
        IMFMediaType_Release(mt);
    }
    if (FAILED(hr)) { snprintf(a->why, sizeof a->why, "the H.264 decoder refuses the stream (SetInputType 0x%08lx)", (unsigned long)hr); return 0; }
    if (!set_output(a)) return 0;
    IMFTransform_ProcessMessage(a->t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(a->t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    LOG("[movie] AVC: Media Foundation's H.264 decoder, %d×%d (coded %d×%d, output %d×%d), profile %d, %s %s range", a->disp_w, a->disp_h, a->coded_w, a->coded_h, a->fw, a->fh,
        a->profile, a->matrix == 1 ? "BT.709" : a->matrix == 5 || a->matrix == 6 ? "BT.601" : "an undeclared matrix,", a->full ? "full" : "video");
    return 1;
}
// a picture, its time; kept in presentation order (the transform's order already; the sort keeps a time that came early in its place)
static void q_push(mavc *a, uint8_t *nv12, int w, int h, int64_t pts) {
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest picture"); free(a->q[0].nv12); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    int k = a->nq; while (k > 0 && a->q[k - 1].pts > pts) { a->q[k] = a->q[k - 1]; k--; }
    a->q[k] = (pic_t){nv12, w, h, pts}; a->nq++;
}
// One output sample → a compact NV12 copy of its display aperture (Y rows of aw, then interleaved U V rows), video range
static void take_sample(mavc *a, IMFSample *s) {
    LONGLONG t = 0; IMFSample_GetSampleTime(s, &t);
    IMFMediaBuffer *b = NULL; if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &b))) return;
    IMF2DBuffer *b2 = NULL; BYTE *base = NULL; LONG pitch = 0; DWORD len = 0;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(b, &IID_IMF2DBuffer, (void **)&b2)) && SUCCEEDED(IMF2DBuffer_Lock2D(b2, &base, &pitch))) len = (DWORD)-1;
    else { if (b2) { IMF2DBuffer_Release(b2); b2 = NULL; } if (FAILED(IMFMediaBuffer_Lock(b, &base, NULL, &len))) { IMFMediaBuffer_Release(b); return; } pitch = a->stride > 0 ? a->stride : a->fw; }
    int w = a->aw, h = a->ah, cw = (w + 1) & ~1;
    size_t need = (size_t)pitch * a->fh * 3 / 2;
    uint8_t *o = pitch > 0 && (len == (DWORD)-1 || len >= need) ? malloc((size_t)w * h + (size_t)cw * ((h + 1) / 2)) : NULL;
    if (o) {
        const uint8_t *y = base + (size_t)a->ay * pitch + a->ax, *uv = base + (size_t)pitch * a->fh + (size_t)(a->ay / 2) * pitch + (a->ax & ~1);
        for (int r = 0; r < h; r++) memcpy(o + (size_t)r * w, y + (size_t)r * pitch, (size_t)w);
        for (int r = 0; r < (h + 1) / 2; r++) memcpy(o + (size_t)w * h + (size_t)r * cw, uv + (size_t)r * pitch, (size_t)cw);
        if (a->full) {   // full range → video range, as the NV12 VideoToolbox hands out
            for (size_t i = 0; i < (size_t)w * h; i++) o[i] = (uint8_t)(16 + (o[i] * 219 + 127) / 255);
            for (size_t i = (size_t)w * h; i < (size_t)w * h + (size_t)cw * ((h + 1) / 2); i++) o[i] = (uint8_t)(128 + ((o[i] - 128) * 224 + (o[i] >= 128 ? 127 : -127)) / 255);
        }
        if (mf_trace()) {   // the picture's mean luma and chroma over a sparse grid: a black or empty picture shows here
            unsigned long sy = 0, su = 0, n = 0;
            for (int r = 0; r < h; r += 16) for (int c = 0; c < w; c += 16) { sy += o[(size_t)r * w + c]; su += o[(size_t)w * h + (size_t)(r / 2) * cw + (c & ~1)]; n++; }
            MF_TRACE("AVC picture at %lld us: %d×%d, mean Y %lu, mean U %lu (pitch %ld)", (long long)(t / 10), w, h, n ? sy / n : 0, n ? su / n : 0, (long)pitch);
        }
        q_push(a, o, w, h, t / 10);
    } else if (!a->red++) LOG("[RED] movie: an AVC picture of %lu bytes, pitch %ld, is smaller than its %d×%d frame", (unsigned long)len, (long)pitch, a->fw, a->fh);
    if (b2) { IMF2DBuffer_Unlock2D(b2); IMF2DBuffer_Release(b2); } else IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_Release(b);
}
// Every picture the transform has ready; a format change picks the output again
static void pull(mavc *a) {
    for (int guard = 0; guard < 64 && a->t; guard++) {
        IMFSample *s = NULL;
        if (!(a->out_flags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) && !(s = mf_sample(NULL, 0, a->out_cb, -1))) return;
        MFT_OUTPUT_DATA_BUFFER ob = {0, s, 0, NULL}; DWORD st = 0;
        HRESULT hr = IMFTransform_ProcessOutput(a->t, 0, 1, &ob, &st);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        MF_TRACE("AVC ProcessOutput: 0x%08lx, status 0x%lx, sample %p", (unsigned long)hr, (unsigned long)ob.dwStatus, (void *)ob.pSample);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { if (ob.pSample && ob.pSample != s) IMFSample_Release(ob.pSample); if (s) IMFSample_Release(s); if (!set_output(a)) { no_decoder(a); return; } continue; }
        if (SUCCEEDED(hr) && ob.pSample) take_sample(a, ob.pSample);
        if (ob.pSample && ob.pSample != s) IMFSample_Release(ob.pSample);
        if (s) IMFSample_Release(s);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        if (FAILED(hr)) { if (!a->red++) LOG("[RED] movie: AVC decode error (Media Foundation ProcessOutput 0x%08lx, first case)", (unsigned long)hr); return; }
    }
}
static void apply_flush(mavc *a) { if (a->flush_pending && a->t) IMFTransform_ProcessMessage(a->t, MFT_MESSAGE_COMMAND_FLUSH, 0); a->flush_pending = 0; a->drained = 0; }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    a->aulen = 0; each_nal(p, (size_t)n, au_add, a);
    if (!a->aulen) return 0;
    if (!a->t && !a->failed && !open_decoder(a)) no_decoder(a);
    if (a->failed) { q_push(a, NULL, 0, 0, pts_us); return 0; }   // a black picture at the unit's time, as on Linux
    apply_flush(a); a->drained = 0;
    IMFSample *s = mf_sample(a->au, (DWORD)a->aulen, (DWORD)a->aulen, pts_us * 10);
    if (!s) { if (!a->red++) LOG("[RED] movie: AVC sample allocation failed"); return -1; }
    HRESULT hr = IMFTransform_ProcessInput(a->t, 0, s, 0);
    if (hr == MF_E_NOTACCEPTING) { pull(a); if (a->t) hr = IMFTransform_ProcessInput(a->t, 0, s, 0); }
    MF_TRACE("AVC ProcessInput: %zu bytes at %lld us: 0x%08lx", a->aulen, (long long)pts_us, (unsigned long)hr);
    IMFSample_Release(s);
    if (FAILED(hr)) { if (!a->red++) LOG("[RED] movie: AVC decode error (Media Foundation ProcessInput 0x%08lx, first case)", (unsigned long)hr); return -1; }
    pull(a);
    return 0;
}
// NV12 (compact) → RGBA rows of w pixels, by the SPS's matrix and range (16-bit fixed point, hle_movie.c md_rgba's coefficients); padded with opaque black or cropped
static void nv12_rgba(const mavc *a, const pic_t *pc, uint8_t *dst, int w, int h) {
    int full = a->full, m = a->matrix, b709 = m == 1 || (m != 5 && m != 6 && pc->h >= 720);
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = b709 ? 1.5748 : 1.402, gu = b709 ? 0.1873 : 0.3441, gv = b709 ? 0.4681 : 0.7141, bu = b709 ? 1.8556 : 1.772;
    int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536), BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
    int cw = pc->w < w ? pc->w : w, ch = pc->h < h ? pc->h : h, pw = (pc->w + 1) & ~1;
    if (cw < w || ch < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    #define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    for (int y = 0; y < ch; y++) {
        const uint8_t *py = pc->nv12 + (size_t)y * pc->w, *puv = pc->nv12 + (size_t)pc->w * pc->h + (size_t)(y >> 1) * pw;
        uint8_t *o = dst + (size_t)y * w * 4;
        for (int x = 0; x < cw; x++) {
            int l = (py[x] - y0) * Y, u = puv[x & ~1] - 128, v = puv[(x & ~1) + 1] - 128;
            int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, bl = (l + BU * u + 32768) >> 16;
            o[4 * x] = (uint8_t)CL(r); o[4 * x + 1] = (uint8_t)CL(g); o[4 * x + 2] = (uint8_t)CL(bl); o[4 * x + 3] = 255;
        }
    }
    #undef CL
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (a->t) apply_flush(a);
    if (flush && a->t && !a->drained) { HRESULT hr = IMFTransform_ProcessMessage(a->t, MFT_MESSAGE_COMMAND_DRAIN, 0); MF_TRACE("AVC drain: 0x%08lx", (unsigned long)hr); pull(a); a->drained = 1; }   // the end of the stream: every picture it holds
    // without a decoder, the stopgap's order: times sorted, held while 4 or fewer are queued (Baseline: none), as the B-frames VideoToolbox reorders need
    int depth = a->failed && a->profile != 66 ? 4 : 0;
    if (!a->nq || (!flush && a->nq <= depth)) return 0;
    pic_t pc = a->q[0]; memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    if (pts_us) *pts_us = pc.pts;
    if (!pc.nv12) {   // black: opaque (RGBA) or video-range (NV12)
        if (a->rgba) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
        else { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        return 1;
    }
    if (a->rgba) nv12_rgba(a, &pc, dst, w, h);
    else {   // NV12 rows of w (as hle_movie_avc.c hands VideoToolbox's out): a smaller picture padded with black, a larger one cropped
        int cw = pc.w < w ? pc.w : w, chh = pc.h < h ? pc.h : h, pw = (pc.w + 1) & ~1;
        if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, pc.nv12 + (size_t)r * pc.w, (size_t)cw);
        size_t uvb = (size_t)((cw + 1) & ~1); if (uvb > (size_t)w) uvb = (size_t)w;
        for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, pc.nv12 + (size_t)pc.w * pc.h + (size_t)r * pw, uvb);
    }
    free(pc.nv12);
    return 1;
}

// ---------- AAC ----------
struct maac { IMFTransform *t; com_t com; int ch, rate, red; DWORD out_cb, out_flags; LONGLONG t100; };
void maac_free(maac *m) {
    if (!m) return;
    if (m->t) { IMFTransform_ProcessMessage(m->t, MFT_MESSAGE_NOTIFY_END_STREAMING, 0); IMFTransform_Release(m->t); }
    com_leave(&m->com); free(m);
}
static maac *aac_fail(maac *m, const char *why) {
    static int told; if (!told++) LOG("[RED] movie: no AAC decoder (%s), outputting silence; on a Windows N edition, install the Media Feature Pack", why);
    maac_free(m); return NULL;
}
maac *maac_new(const uint8_t *asc_in, int asc_n, int *ch, int *rate) {
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    uint8_t asc[64]; int an = asc_in && asc_n > 0 && asc_n <= (int)sizeof asc ? asc_n : 0;
    if (an) memcpy(asc, asc_in, (size_t)an);
    if (an >= 2) {   // sampling frequency index and channel configuration, as hle_movie.c reads them for AudioToolbox
        int fi = (asc[0] & 7) << 1 | asc[1] >> 7, cc = asc[1] >> 3 & 15;
        if (fi < 13 && *rate <= 0) *rate = rates[fi];
        if (cc && cc < 8 && *ch <= 0) *ch = cc == 7 ? 8 : cc;
    }
    if (*rate <= 0) *rate = 48000; if (*ch <= 0) *ch = 2;
    if (!an) { int fi = 3; for (int i = 0; i < 13; i++) if (rates[i] == *rate) fi = i; asc[0] = (uint8_t)(2 << 3 | fi >> 1); asc[1] = (uint8_t)((fi & 1) << 7 | (*ch & 15) << 3); an = 2; }
    maac *m = calloc(1, sizeof *m); if (!m) die("movie: AAC decoder allocation failed");
    m->ch = *ch; m->rate = *rate;
    char why[200];
    if (!(m->t = mf_transform(&k_clsid_aac, "AAC", &m->com, why, sizeof why))) return aac_fail(m, why);
    // the input: raw AAC (payload type 0); MF_MT_USER_DATA is HEAACWAVEINFO after its WAVEFORMATEX (payload type, profile and level, struct type, reserved: 12 bytes) and then the AudioSpecificConfig
    IMFMediaType *mt = NULL; HRESULT hr = g_mf.media_type(&mt);
    if (SUCCEEDED(hr)) {
        uint8_t ud[12 + 64] = {0}; ud[2] = 0xFE; memcpy(ud + 12, asc, (size_t)an);   // wAudioProfileLevelIndication 0xFE: not specified
        IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio); IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
        IMFMediaType_SetUINT32(mt, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)m->ch); IMFMediaType_SetUINT32(mt, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)m->rate);
        IMFMediaType_SetUINT32(mt, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16); IMFMediaType_SetUINT32(mt, &MF_MT_AAC_PAYLOAD_TYPE, 0);
        IMFMediaType_SetBlob(mt, &MF_MT_USER_DATA, ud, (UINT32)(12 + an));
        hr = IMFTransform_SetInputType(m->t, 0, mt, 0);
        IMFMediaType_Release(mt); mt = NULL;
    }
    if (FAILED(hr)) { snprintf(why, sizeof why, "the AAC decoder refuses %d channels at %d Hz (SetInputType 0x%08lx)", m->ch, m->rate, (unsigned long)hr); return aac_fail(m, why); }
    // the output: 16-bit PCM with the input's channels, among the types it offers
    for (DWORD i = 0; SUCCEEDED(hr = IMFTransform_GetOutputAvailableType(m->t, 0, i, &mt)); i++) {
        GUID st; UINT32 bits = 0, c = 0;
        if (SUCCEEDED(IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, &st)) && IsEqualGUID(&st, &MFAudioFormat_PCM) && SUCCEEDED(IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) &&
            bits == 16 && SUCCEEDED(IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_NUM_CHANNELS, &c)) && (int)c == m->ch) break;
        IMFMediaType_Release(mt); mt = NULL;
    }
    if (!mt) { snprintf(why, sizeof why, "the AAC decoder offers no 16-bit PCM with %d channels (0x%08lx)", m->ch, (unsigned long)hr); return aac_fail(m, why); }
    UINT32 r = 0; if (SUCCEEDED(IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &r)) && r) m->rate = (int)r;
    hr = IMFTransform_SetOutputType(m->t, 0, mt, 0); IMFMediaType_Release(mt);
    if (FAILED(hr)) { snprintf(why, sizeof why, "the AAC decoder refuses 16-bit PCM (SetOutputType 0x%08lx)", (unsigned long)hr); return aac_fail(m, why); }
    MFT_OUTPUT_STREAM_INFO oi = {0}; IMFTransform_GetOutputStreamInfo(m->t, 0, &oi);
    m->out_cb = oi.cbSize ? oi.cbSize : (DWORD)(2048 * 2 * m->ch); m->out_flags = oi.dwFlags;
    IMFTransform_ProcessMessage(m->t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(m->t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    *ch = m->ch; *rate = m->rate;
    LOG("[movie] AAC: Media Foundation's AAC decoder, %d channels %d Hz, AudioSpecificConfig %d bytes (object type %d)", m->ch, m->rate, an, asc[0] >> 3);
    return m;
}
// the PCM the transform has ready, appended at pcm + got frames
static int aac_pull(maac *m, int16_t *pcm, int got, int cap) {
    for (int guard = 0; guard < 16; guard++) {
        IMFSample *s = NULL;
        if (!(m->out_flags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) && !(s = mf_sample(NULL, 0, m->out_cb, -1))) break;
        MFT_OUTPUT_DATA_BUFFER ob = {0, s, 0, NULL}; DWORD st = 0;
        HRESULT hr = IMFTransform_ProcessOutput(m->t, 0, 1, &ob, &st);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (SUCCEEDED(hr) && ob.pSample) {
            IMFMediaBuffer *b = NULL; BYTE *p = NULL; DWORD len = 0;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(ob.pSample, &b)) && SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &len))) {
                int fr = (int)(len / (2u * m->ch)), take = fr < cap - got ? fr : cap - got;
                if (take < fr && !m->red++) LOG("[RED] movie: AAC output of %d frames, more than the slot's %d: the rest is dropped", fr, cap - got);
                memcpy(pcm + (size_t)got * m->ch, p, (size_t)take * 2 * m->ch); got += take;
                IMFMediaBuffer_Unlock(b);
            }
            if (b) IMFMediaBuffer_Release(b);
        }
        if (ob.pSample && ob.pSample != s) IMFSample_Release(ob.pSample);
        if (s) IMFSample_Release(s);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) { if (!m->red++) LOG("[RED] movie: AAC decode error (Media Foundation ProcessOutput 0x%08lx, first case)", (unsigned long)hr); return got ? got : -1; }
    }
    return got;
}
int maac_decode(maac *m, const uint8_t *p, int n, int16_t *pcm, int cap_frames) {
    if (!m || !m->t || n <= 0) return 0;
    IMFSample *s = mf_sample(p, (DWORD)n, (DWORD)n, m->t100);
    if (!s) return -1;
    m->t100 += 10000000LL * 1024 / m->rate;
    HRESULT hr = IMFTransform_ProcessInput(m->t, 0, s, 0);
    int got = 0;
    if (hr == MF_E_NOTACCEPTING) { got = aac_pull(m, pcm, 0, cap_frames); hr = IMFTransform_ProcessInput(m->t, 0, s, 0); }
    IMFSample_Release(s);
    if (FAILED(hr)) { if (!m->red++) LOG("[RED] movie: AAC decode error (Media Foundation ProcessInput 0x%08lx, first case)", (unsigned long)hr); return got ? got : -1; }
    int r = aac_pull(m, pcm, got < 0 ? 0 : got, cap_frames);
    return r;
}
#endif
