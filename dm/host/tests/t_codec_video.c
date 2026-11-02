// The Switch 2's video decoder API as the host serves it (hle_codec_video.c), without a guest: the buffer protocol, results, sync handles, sync point events and the
// readback, on synthetic red key frames: 16x16 VP9 and VP8 (the ones t_movie_vpx.c decodes), a 64x64 H.264 baseline frame (ffmpeg's libx264, SEI removed) and a 64x64 AV1 frame
// (SVT-AV1 through PyAV, preset 12, CRF 30); no game data. The SystemEvent functions are stubs that record each event's state, and the NVN entry points native images use are
// stubs that record what the host made.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_movie.c"
#include "../hle_codec_video.c"
#include "../hle_movie_avc.c"
#include "../hle_movie_av1.c"
#include <assert.h>

uint64_t now_ns(void) { return 1; }
__thread _Atomic int dm_host_work;   // hle_thread.c
static struct { void *ev; int bound, set; } g_ev[16];
static int ev_slot(void *e) { for (int i = 0; i < 16; i++) if (g_ev[i].ev == e) return i; for (int i = 0; i < 16; i++) if (!g_ev[i].ev) { g_ev[i].ev = e; return i; } abort(); }
void hle_bind_system_event(void *e, int autoclear) { (void)autoclear; g_ev[ev_slot(e)].bound = 1; }
void hle_signal_system_event(void *e) { g_ev[ev_slot(e)].set = 1; }
void hle_clear_system_event(void *e) { g_ev[ev_slot(e)].set = 0; }
static int ev_set(void *e) { return g_ev[ev_slot(e)].set; }
static void destroy_ev(void *e) { g_ev[ev_slot(e)].bound = g_ev[ev_slot(e)].set = 0; }   // nn::os::DestroySystemEvent: closing the handle unbinds the event
void *guest_sym(const char *n) { return !strcmp(n, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE") ? (void *)destroy_ev : NULL; }

static const uint8_t vp8[] = {
    0xf0,0x02,0x00,0x9d,0x01,0x2a,0x10,0x00,0x10,0x00,0x00,0x47,0x08,0x85,0x85,0x88,
    0x85,0x84,0x88,0x02,0x02,0x02,0x75,0xaa,0x03,0xf8,0x03,0xfa,0x02,0x0d,0x4d,0x18,
    0x00,0xfe,0xfd,0x6e,0xf3,0xff,0xe3,0x99,0x37,0x30,0xc4,0xff,0x8e,0x6d,0xff,0xf1,
    0x61,0x3c,0x0e,0x28,0xc8,0xff,0xf1,0x51,0x00,
};
static const uint8_t h264[] = {   // Annex B: SPS, PPS, one IDR slice
    0x00,0x00,0x00,0x01,0x67,0x42,0xc0,0x0a,0xdc,0x42,0x6c,0x04,0x40,0x00,0x00,0x03,0x00,0x40,0x00,0x00,0x0c,0xa3,0xc4,0x89,
    0xe0,0x00,0x00,0x00,0x01,0x68,0xce,0x0f,0xc8,0x00,0x00,0x01,0x65,0x88,0x84,0x3a,0x11,0x8a,0x00,0x02,0x18,0xf1,0xc0,0x00,
    0x40,0xf6,0x38,0x00,0x08,0x79,0x49,0xc9,0xc9,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xe0,
};
static const uint8_t av1[] = {   // one temporal unit: temporal delimiter, sequence header (profile 0, 64x64), frame
    0x12,0x00,0x0a,0x0b,0x02,0x00,0x00,0x04,0x95,0x7f,0xfc,0x0a,0xf9,0x00,0x40,0x32,0x15,0x10,0x00,0xbc,0x00,0x49,0x22,0x08,0x08,0x21,0x10,0x00,0xad,0x3b,0x67,0x00,
    0x1e,0xb5,0xdb,0x79,0x52,0x90,
};
// The host NVN entry points native images use (nvn_host_proc), recording what the host made
static struct { int pool_flags, pool_init, pool_fin, target, fmt, tw, th, tflags, tex_init; void *mem; size_t size; ptrdiff_t stride, off; const void *storage, *dev; } g_nv;
static void nv_mpdef(NVNmemoryPoolBuilder *b) { (void)b; }
static void nv_mpdev(NVNmemoryPoolBuilder *b, NVNdevice *d) { (void)b; g_nv.dev = d; }
static void nv_mpflags(NVNmemoryPoolBuilder *b, int f) { (void)b; g_nv.pool_flags = f; }
static void nv_mpstorage(NVNmemoryPoolBuilder *b, void *m, size_t n) { (void)b; g_nv.mem = m; g_nv.size = n; }
static NVNboolean nv_mpinit(NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) { (void)p; (void)b; g_nv.pool_init++; return 1; }
static void nv_mpfin(NVNmemoryPool *p) { (void)p; g_nv.pool_fin++; }
static void nv_tbdef(NVNtextureBuilder *b) { (void)b; }
static void nv_tbdev(NVNtextureBuilder *b, NVNdevice *d) { (void)b; (void)d; }
static void nv_tbtarget(NVNtextureBuilder *b, NVNtextureTarget t) { (void)b; g_nv.target = t; }
static void nv_tbfmt(NVNtextureBuilder *b, NVNformat f) { (void)b; g_nv.fmt = f; }
static void nv_tbsize(NVNtextureBuilder *b, int w, int h) { (void)b; g_nv.tw = w; g_nv.th = h; }
static void nv_tbflags(NVNtextureBuilder *b, int f) { (void)b; g_nv.tflags = f; }
static void nv_tbstride(NVNtextureBuilder *b, ptrdiff_t s) { (void)b; g_nv.stride = s; }
static void nv_tbstorage(NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t o) { (void)b; g_nv.storage = p; g_nv.off = o; }
static NVNboolean nv_texinit(NVNtexture *t, const NVNtextureBuilder *b) { (void)t; (void)b; g_nv.tex_init++; return 1; }
void *nvn_host_proc(const char *n) {
    static const struct { const char *name; void *fn; } k[] = {
        {"nvnMemoryPoolBuilderSetDefaults", nv_mpdef}, {"nvnMemoryPoolBuilderSetDevice", nv_mpdev}, {"nvnMemoryPoolBuilderSetFlags", nv_mpflags},
        {"nvnMemoryPoolBuilderSetStorage", nv_mpstorage}, {"nvnMemoryPoolInitialize", nv_mpinit}, {"nvnMemoryPoolFinalize", nv_mpfin},
        {"nvnTextureBuilderSetDefaults", nv_tbdef}, {"nvnTextureBuilderSetDevice", nv_tbdev}, {"nvnTextureBuilderSetTarget", nv_tbtarget}, {"nvnTextureBuilderSetFormat", nv_tbfmt},
        {"nvnTextureBuilderSetSize2D", nv_tbsize}, {"nvnTextureBuilderSetFlags", nv_tbflags}, {"nvnTextureBuilderSetStride", nv_tbstride},
        {"nvnTextureBuilderSetStorage", nv_tbstorage}, {"nvnTextureInitialize", nv_texinit}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(k[i].name, n)) return k[i].fn;
    return NULL;
}
static const uint8_t vp9[] = {
    0x82,0x49,0x83,0x42,0x00,0x00,0xf0,0x00,0xf6,0x00,0x38,0x24,0x1c,0x18,0x4a,0x00,
    0x00,0x30,0x60,0x00,0x00,0x10,0xbf,0xff,0xf7,0x1d,0xaf,0xff,0xff,0xff,0x5f,0xdf,
    0xff,0xff,0xff,0xf2,0x2a,0xc0,0x00,
};

int main(void) {
    _Alignas(16) static uint8_t dec[0x400], pool[3][0x40], spt[0x80], mem[16 * 16 * 4];
    uint64_t size, align, id; uint32_t idx; cv_sync sync; uint16_t w, h, cw, ch; int64_t ts; bool eos;

    // Pools: one picture each, NV12 (format 3, as Bananza asks)
    cv_pref pref = h_cv_pref(16, 16, 3, 0);
    assert(!h_cv_poolsize(&size, &align, &pref, 1) && size >= 16 * 16 * 4 && align == 0x1000);
    for (int i = 0; i < 3; i++) assert(!h_cv_pool(pool[i], mem, size, &pref, 1, 0) && pool_of(pool[i]));

    // Decoder: the submission-ready event is bound in its type and signalled
    assert(!h_cv_vp9init2(dec, 0x10000, 16, 16, 1, 1, 3));
    cv_dec *d = cv_of(dec); assert(d && d->vpx_ok);
    void *ev = NULL; assert(!h_cv_vp9event(&ev, dec) && ev == dec + CV_EVENT_OFF && g_ev[ev_slot(ev)].bound && ev_set(ev));

    // Registering makes a buffer acquirable at once, its sync handle signalled; an empty queue answers 3604
    for (uint32_t i = 0; i < 2; i++) assert(!h_cv_vp9reg(dec, i, pool[i], 0));
    for (uint32_t i = 0; i < 2; i++) { assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == i && sync.magic == CV_SYNC && sync.gen == 0 && h_gs_trywait(&sync)); }
    assert(h_cv_vp9acq(&idx, &sync, dec) == CV_NONE);
    assert(h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) == CV_NONE);

    // Released: acquirable again with a new handle that signals once a picture is in the buffer; the picture's properties and its NV12 / RGBA readback
    h_cv_vp9rel(dec, 0, sync);
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 0 && sync.gen == 1 && !h_gs_trywait(&sync));
    assert(h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) == CV_NONE);
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 1000, false) && id == 1);
    assert(h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) && w == 16 && h == 16 && cw == 16 && ch == 16 && ts == 1000 && !eos);
    uint8_t nv12[16 * 16 * 3 / 2], rgba[16 * 16 * 4];
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 16, 16, nv12, 16, 256, nv12 + 256, 16, 128));
    for (int i = 0; i < 256; i++) assert(abs(nv12[i] - 81) <= 3);
    for (int i = 256; i < 384; i += 2) { assert(abs(nv12[i] - 90) <= 3); assert(abs(nv12[i + 1] - 240) <= 3); }
    uint8_t y[256], uv[128];   // separate planes
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 16, 16, y, 16, 256, uv, 16, 128) && !memcmp(y, nv12, 256) && !memcmp(uv, nv12 + 256, 128));
    assert(!h_cv_rgba(pool[0], 0, 0, 0, 16, 16, rgba, 64, sizeof rgba));
    for (int i = 0; i < 1024; i += 4) assert(rgba[i] >= 250 && rgba[i + 1] <= 5 && rgba[i + 2] <= 5 && rgba[i + 3] == 255);

    // No released buffer: pictures wait (4), then packets are refused (3603) and the event is cleared; a release takes the oldest and signals again
    for (int i = 0; i < CV_HOLD; i++) assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 2000 + i, false));
    assert(d->nheld == CV_HOLD && !ev_set(ev));
    assert(h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 9999, false) == CV_BUSY);
    h_cv_vp9rel(dec, 1, sync);
    assert(d->nheld == CV_HOLD - 1 && ev_set(ev));
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1 && h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 2000);

    // Sync point event: armed on an unfilled buffer, signalled when the picture lands; one armed on a filled buffer is signalled at once
    assert(!h_spt_create(spt) && g_ev[ev_slot(spt)].bound);
    h_cv_vp9rel(dec, 0, sync);   // buffer 0 takes the next waiting picture at once
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 0);
    assert(!h_spt_request(spt, &sync) && ev_set(spt) && !SPT(spt)->armed);
    assert(!h_cv_vp9reg(dec, 2, pool[2], 0) && !h_cv_vp9acq(&idx, &sync, dec) && idx == 2);
    for (int i = 0; i < 2; i++) { h_cv_vp9rel(dec, (uint32_t)i, sync); assert(!h_cv_vp9acq(&idx, &sync, dec)); }   // the last two waiting pictures go out
    h_cv_vp9rel(dec, 2, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 2 && !h_gs_trywait(&sync));
    assert(!h_spt_request(spt, &sync) && !ev_set(spt) && SPT(spt)->armed);
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 3000, false) && ev_set(spt) && !SPT(spt)->armed && h_gs_trywait(&sync));

    // End of stream with no data: the next released buffer carries it, with no picture
    assert(!h_cv_vp9submit(&id, dec, NULL, 0, 0, true) && d->eos_pending);
    h_cv_vp9rel(dec, 2, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 2 && h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 2) && eos && w == 0 && h == 0);

    // A pool finalized while its buffer is still registered: the buffer keeps working, its picture has nowhere to go (ASan checks no freed pool is written)
    h_cv_poolfin(pool[1]); assert(!pool_of(pool[1]) && d->buf[1].pool == NULL);
    h_cv_vp9rel(dec, 1, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1 && !h_gs_trywait(&sync));
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 4000, false) && h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 4000 && w == 16);
    // A flush drops the waiting pictures and restarts the decoder; finalize frees it and destroys its event
    assert(!h_cv_vp9flush(dec) && d->nheld == 0 && d->vpx_ok && ev_set(ev));
    assert(!h_cv_vp9fin(dec) && !cv_of(dec) && !g_ev[ev_slot(ev)].bound);
    // A decoder initialised again in the same memory, cleared in between (Donkey Kong Bananza's second movie): its event is bound anew
    memset(dec, 0, sizeof dec);
    assert(!h_cv_vp9init2(dec, 0x10000, 16, 16, 1, 1, 3) && cv_of(dec) && g_ev[ev_slot(ev)].bound && ev_set(ev));
    assert(!h_cv_vp9fin(dec) && !cv_of(dec) && !g_ev[ev_slot(ev)].bound);
    for (int i = 0; i < 3; i += 2) { h_cv_poolfin(pool[i]); assert(!pool_of(pool[i])); }
    // VP8 (libvpx) and H.264 (VideoToolbox) take the same protocol: one released buffer, one key frame, its picture read back red
    {   _Alignas(16) static uint8_t d8[0x400], p8[0x40], m8[16 * 16 * 4];
        assert(!h_cv_poolsize(&size, &align, &pref, 1) && !h_cv_pool(p8, m8, size, &pref, 1, 0));
        assert(!h_cv_vp8init2(d8, 0x10000, 16, 16, 1, 1, 3) && cv_of(d8) && cv_of(d8)->codec == CV_VP8 && cv_of(d8)->vpx_ok);
        assert(!h_cv_vp8reg(d8, 0, p8, 0) && !h_cv_vp8acq(&idx, &sync, d8) && idx == 0);
        h_cv_vp8rel(d8, 0, sync); assert(!h_cv_vp8acq(&idx, &sync, d8) && !h_gs_trywait(&sync));
        assert(!h_cv_vp8submit(&id, d8, vp8, sizeof vp8, 7000, false) && h_gs_trywait(&sync));
        assert(!h_cv_vp8props(&w, &h, &cw, &ch, &ts, &eos, d8, 0) && w == 16 && h == 16 && ts == 7000);
        assert(!h_cv_nv12(p8, 0, 0, 0, 16, 16, nv12, 16, 256, nv12 + 256, 16, 128));
        for (int i = 0; i < 256; i++) assert(abs(nv12[i] - 81) <= 3);
        for (int i = 256; i < 384; i += 2) { assert(abs(nv12[i] - 90) <= 4); assert(abs(nv12[i + 1] - 240) <= 4); }
        // Unregistered: no longer acquirable or filled
        h_cv_vp8rel(d8, 0, sync); assert(!h_cv_vp8unreg(d8, 0) && h_cv_vp8acq(&idx, &sync, d8) == CV_NONE && !cv_of(d8)->buf[0].reg);
        assert(!h_cv_vp8fin(d8) && !cv_of(d8)); h_cv_poolfin(p8);
    }
    {   _Alignas(16) static uint8_t dh[0x400], ph[0x40], mh[64 * 64 * 4]; static uint8_t yuv[64 * 64 * 3 / 2];
        cv_pref p64 = h_cv_pref(64, 64, 3, 0);
        assert(!h_cv_poolsize(&size, &align, &p64, 1) && !h_cv_pool(ph, mh, size, &p64, 1, 0));
        assert(!h_cv_h264init2(dh, 0x10000, 64, 64, 1, 1, 3) && cv_of(dh) && cv_of(dh)->codec == CV_H264 && cv_of(dh)->avc);
        assert(!h_cv_h264reg(dh, 0, ph, 0) && !h_cv_h264acq(&idx, &sync, dh));
        h_cv_h264rel(dh, 0, sync); assert(!h_cv_h264acq(&idx, &sync, dh) && !h_gs_trywait(&sync));
        assert(!h_cv_h264submit(&id, dh, h264, sizeof h264, 5000000, true) && h_gs_trywait(&sync));   // end of stream: VideoToolbox gives its picture out
        assert(!h_cv_h264props(&w, &h, &cw, &ch, &ts, &eos, dh, 0) && w == 64 && h == 64 && ts == 5000000 && eos);
        assert(!h_cv_nv12(ph, 0, 0, 0, 64, 64, yuv, 64, 4096, yuv + 4096, 64, 2048));
        for (int i = 0; i < 4096; i++) assert(abs(yuv[i] - 81) <= 4);
        for (int i = 4096; i < 6144; i += 2) { assert(abs(yuv[i] - 90) <= 6); assert(abs(yuv[i + 1] - 240) <= 6); }
        assert(!h_cv_h264fin(dh) && !cv_of(dh)); h_cv_poolfin(ph);
    }
    // AV1 through SDK 22's calls (Initialize…ByTransferMemory, the event it returns, a preference with a tiling mode, a pool without a codec mode), on each backend this Mac has
    // (VideoToolbox where the chip decodes AV1, and dav1d), read back red; and the GPU route: the pool image as a native image whose host memory pool holds the RGBA picture
    for (int soft = VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1) ? 0 : 1; soft < 2; soft++) {
        setenv("DM_AV1_DAV1D", soft ? "1" : "0", 1);
        _Alignas(16) static uint8_t da[0x400], pa[0x40], ma[64 * 64 * 4], nimg[0x1a0], tex[0x200]; static uint8_t yuv[64 * 64 * 3 / 2], rgb[64 * 64 * 4];
        cv_pref p64 = h_cv_pref22(64, 64, 2, 1);
        assert(!h_cv_poolsize(&size, &align, &p64, 1) && !h_cv_pool22(pa, ma, size, &p64, 1) && pool_of(pa));
        assert(!h_cv_av1tm(da, ma, 0x1000, 0x10000, 64, 64, 1, 3, 2, 0) && cv_of(da) && cv_of(da)->codec == CV_AV1 && cv_of(da)->av1);
        assert(!strcmp(mav1_backend(cv_of(da)->av1), soft ? "dav1d" : "VideoToolbox"));
        void *ev = h_cv_av1ev22(da); assert(ev == da + CV_EVENT_OFF && g_ev[ev_slot(ev)].bound && ev_set(ev));
        // The native image of pool image 0, made before any picture: a handle, a CPU-uncached host pool of RGBA rows, a linear RGBA8 texture over it
        uint32_t nh = h_cv_native(pa, 0); assert(nh == 0x100);
        uint8_t nb[0x20] = {0}; *(void **)nb = (void *)0x1234; memcpy(nb + 8, &nh, 4); uint32_t perm = 1, fmt = NVN_FORMAT_RGBA8_SRGB; memcpy(nb + 0xc, &perm, 4); memcpy(nb + 0x10, &fmt, 4);
        memset(&g_nv, 0, sizeof g_nv);
        assert(h_nvn_nimg_init(nimg, nb) && g_nv.pool_init == 1 && g_nv.dev == (void *)0x1234 && g_nv.size >= 64 * 64 * 4);
        assert(g_nv.pool_flags == (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED));
        assert(h_nvn_nimg_tex((NVNtexture *)tex, nimg) && g_nv.tex_init == 1 && g_nv.target == NVN_TEXTURE_TARGET_2D && g_nv.fmt == NVN_FORMAT_RGBA8_SRGB);
        assert(g_nv.tw == 64 && g_nv.th == 64 && g_nv.tflags == NVN_TEXTURE_FLAGS_LINEAR && g_nv.stride == 256 && g_nv.off == 0 && g_nv.storage == &nimg_of(nimg)->pool);
        assert(!h_cv_av1reg(da, 0, pa, 0) && !h_cv_av1acq(&idx, &sync, da));
        h_cv_av1rel(da, 0, sync); assert(!h_cv_av1acq(&idx, &sync, da) && !h_gs_trywait(&sync));
        assert(!h_cv_av1submit(&id, da, av1, sizeof av1, 6000000, true) && h_gs_trywait(&sync));
        assert(!h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, da, 0) && w == 64 && h == 64 && ts == 6000000 && eos);
        assert(!h_cv_nv12(pa, 0, 0, 0, 64, 64, yuv, 64, 4096, yuv + 4096, 64, 2048));
        for (int i = 0; i < 4096; i++) assert(abs(yuv[i] - 81) <= 4);
        for (int i = 4096; i < 6144; i += 2) { assert(abs(yuv[i] - 90) <= 6); assert(abs(yuv[i + 1] - 240) <= 6); }
        assert(!h_cv_rgbahdr(pa, 0, 0, 0, 64, 64, rgb, 256, sizeof rgb));   // SDK 22's HDR readback is its RGBA one
        for (int i = 0; i < 64 * 64 * 4; i += 4) assert(rgb[i] >= 245 && rgb[i + 1] <= 10 && rgb[i + 2] <= 10 && rgb[i + 3] == 255);
        assert(!memcmp(g_nv.mem, rgb, sizeof rgb));   // the picture went into the native image's pool too (rows of 256 bytes: tight)
        h_nvn_nimg_fin(nimg); assert(!nimg_of(nimg) && g_nv.pool_fin == 1 && pool_of(pa) && !pool_of(pa)->nimg[0]);
        assert(!h_cv_av1fin(da) && !cv_of(da)); h_cv_poolfin(pa);
    }
    printf("t_codec_video: all passed\n");
    return 0;
}
