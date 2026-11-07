// The Switch 2's video decoder API as the host serves it (hle_codec_video.c), without a guest: the buffer protocol, results, sync handles, sync point events and the
// readback, on synthetic red key frames: 16x16 VP9 and VP8 (the ones t_movie_vpx.c decodes), a 64x64 H.264 baseline frame (ffmpeg's libx264, SEI removed) and a 64x64 AV1 frame
// (SVT-AV1 through PyAV, preset 12, CRF 30); no game data. The SystemEvent functions are stubs that record each event's state.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_movie.c"
#include "../hle_codec_video.c"
#include "../hle_movie_avc.c"
#include "../hle_movie_av1.c"
#include <assert.h>

static uint64_t g_test_wall = 1, g_test_frames, g_test_focus_in; static int g_test_focus = 1;
uint64_t now_ns(void) { return g_test_wall; }
uint64_t guest_ns(void) { return g_test_wall; }
uint64_t dm_guest_frame(void) { return g_test_frames; }   // hle_os.c: frames presented
int dm_focus_now(void) { return g_test_focus; }           // hle_oe.c: the window's focus
uint64_t dm_focus_in_since(void) { return g_test_focus_in; }
__thread _Atomic int dm_host_work;   // hle_thread.c
static struct { void *ev; int bound, set; } g_ev[16];
static int ev_slot(void *e) { for (int i = 0; i < 16; i++) if (g_ev[i].ev == e) return i; for (int i = 0; i < 16; i++) if (!g_ev[i].ev) { g_ev[i].ev = e; return i; } abort(); }
void hle_bind_system_event(void *e, int autoclear) { (void)autoclear; g_ev[ev_slot(e)].bound = 1; }
void hle_signal_system_event(void *e) { g_ev[ev_slot(e)].set = 1; }
void hle_clear_system_event(void *e) { g_ev[ev_slot(e)].set = 0; }
static int ev_set(void *e) { return g_ev[ev_slot(e)].set; }
static void destroy_ev(void *e) { g_ev[ev_slot(e)].bound = g_ev[ev_slot(e)].set = 0; }   // nn::os::DestroySystemEvent: closing the handle unbinds the event
void *guest_sym(const char *n) { return !strcmp(n, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE") ? (void *)destroy_ev : NULL; }
static vpx_image_t *black_i420(unsigned w, unsigned h) {
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, w, h, 1); if (!c) return NULL;
    memset(c->planes[0], 16, (size_t)c->stride[0] * h);
    memset(c->planes[1], 128, (size_t)c->stride[1] * ((h + 1) / 2)); memset(c->planes[2], 128, (size_t)c->stride[2] * ((h + 1) / 2));
    return c;
}

static const uint8_t vp8[] = {
    0xf0,0x02,0x00,0x9d,0x01,0x2a,0x10,0x00,0x10,0x00,0x00,0x47,0x08,0x85,0x85,0x88,
    0x85,0x84,0x88,0x02,0x02,0x02,0x75,0xaa,0x03,0xf8,0x03,0xfa,0x02,0x0d,0x4d,0x18,
    0x00,0xfe,0xfd,0x6e,0xf3,0xff,0xe3,0x99,0x37,0x30,0xc4,0xff,0x8e,0x6d,0xff,0xf1,
    0x61,0x3c,0x0e,0x28,0xc8,0xff,0xf1,0x51,0x00,
};
static const uint8_t av1[] = {   // one temporal unit: temporal delimiter, sequence header (profile 0, 64x64), frame
    0x12,0x00,0x0a,0x0b,0x02,0x00,0x00,0x04,0x95,0x7f,0xfc,0x0a,0xf9,0x00,0x40,0x32,0x15,0x10,0x00,0xbc,0x00,0x49,0x22,0x08,0x08,0x21,0x10,0x00,0xad,0x3b,0x67,0x00,
    0x1e,0xb5,0xdb,0x79,0x52,0x90,
};
static const uint8_t h264[] = {   // Annex B: SPS, PPS, one IDR slice
    0x00,0x00,0x00,0x01,0x67,0x42,0xc0,0x0a,0xdc,0x42,0x6c,0x04,0x40,0x00,0x00,0x03,0x00,0x40,0x00,0x00,0x0c,0xa3,0xc4,0x89,
    0xe0,0x00,0x00,0x00,0x01,0x68,0xce,0x0f,0xc8,0x00,0x00,0x01,0x65,0x88,0x84,0x3a,0x11,0x8a,0x00,0x02,0x18,0xf1,0xc0,0x00,
    0x40,0xf6,0x38,0x00,0x08,0x79,0x49,0xc9,0xc9,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xe0,
};
static const uint8_t vp9[] = {
    0x82,0x49,0x83,0x42,0x00,0x00,0xf0,0x00,0xf6,0x00,0x38,0x24,0x1c,0x18,0x4a,0x00,
    0x00,0x30,0x60,0x00,0x00,0x10,0xbf,0xff,0xf7,0x1d,0xaf,0xff,0xff,0xff,0x5f,0xdf,
    0xff,0xff,0xff,0xf2,0x2a,0xc0,0x00,
};

static double g_test_now;
static double test_clock(void) { return g_test_now; }

int main(void) {
    _Alignas(16) static uint8_t dec[0x400], pool[3][0x40], spt[0x80], mem[16 * 16 * 4];
    uint64_t size, align, id; uint32_t idx; cv_sync sync; uint16_t w, h, cw, ch; int64_t ts; bool eos;

    // Pools: one picture each, NV12 (format 3, as Bananza asks)
    cv_pref pref = h_cv_pref(16, 16, 3, 0);
    assert(!h_cv_poolsize(&size, &align, &pref, 1) && size >= 16 * 16 * 4 && align == CV_NATIVE_ALIGN);
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
    uint32_t cs = 7; w = h = 0;   // SDK 19's form: the same properties, and colour space 0
    assert(!h_cv_vp9props19(&w, &h, &cw, &ch, &cs, &ts, &eos, dec, 0) && w == 16 && h == 16 && ts == 1000 && !eos && cs == 0);
    uint8_t nv12[16 * 16 * 3 / 2], rgba[16 * 16 * 4];
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 16, 16, nv12, 16, 256, nv12 + 256, 16, 128));
    for (int i = 0; i < 256; i++) assert(abs(nv12[i] - 81) <= 3);
    for (int i = 256; i < 384; i += 2) { assert(abs(nv12[i] - 90) <= 3); assert(abs(nv12[i + 1] - 240) <= 3); }
    uint8_t y[256], uv[128];   // separate planes
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 16, 16, y, 16, 256, uv, 16, 128) && !memcmp(y, nv12, 256) && !memcmp(uv, nv12 + 256, 128));
    assert(!h_cv_rgba(pool[0], 0, 0, 0, 16, 16, rgba, 64, sizeof rgba));
    for (int i = 0; i < 1024; i += 4) assert(rgba[i] >= 250 && rgba[i + 1] <= 5 && rgba[i + 2] <= 5 && rgba[i + 3] == 255);

    // No released buffer: pictures wait; the event goes down at CV_READY, with room left for a client's signal in flight, and packets are refused (3603) at CV_HOLD
    for (int i = 0; i < CV_READY; i++) assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 2000 + i, false));
    assert(d->nheld == CV_READY && !ev_set(ev));
    for (int i = CV_READY; i < CV_HOLD; i++) assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 2000 + i, false));
    assert(d->nheld == CV_HOLD && !ev_set(ev));
    assert(h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 9999, false) == CV_BUSY);
    // A release takes the oldest; the event comes back below CV_READY
    for (int i = CV_HOLD; i > CV_READY; i--) { h_cv_vp9rel(dec, 1, sync); assert(d->nheld == i - 1 && !ev_set(ev)); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1); }
    h_cv_vp9rel(dec, 1, sync);
    assert(d->nheld == CV_READY - 1 && ev_set(ev));
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1 && h_gs_trywait(&sync));
    // Start signals it again: a client may drop the signal Initialize gave
    hle_clear_system_event(ev); assert(!ev_set(ev));
    assert(!h_cv_vp9start(dec) && ev_set(ev));
    // A release below CV_READY with no packet taken signals nothing: a client counts each signal as one packet it may send. A packet taken does
    hle_clear_system_event(ev);
    h_cv_vp9rel(dec, 1, sync); assert(d->nheld == CV_READY - 2 && !ev_set(ev));
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1);
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 2000 + CV_HOLD, false) && d->nheld == CV_READY - 1 && ev_set(ev));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 2000 + CV_HOLD - CV_READY + 1);

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
    {   // AV1: a decoder of its own codec through SDK 22's entry point, whose last two arguments lie on the stack (the guest's 8-byte slots), and a packet with no sequence header yet gives no picture
        uint8_t da[0x400]; memset(da, 0, sizeof da);
        assert(!h_cv_av1tm(da, NULL, 0, 0x10000, 0xabcd0080, 0xabcd0040, 1, 3, 2, 2) && cv_of(da) && cv_of(da)->codec == CV_AV1 && cv_of(da)->w == 0x80 && cv_of(da)->h == 0x40);
        assert(h_cv_av1ev22(da) == da + CV_EVENT_OFF && ev_set(da + CV_EVENT_OFF));
        static const uint8_t delimiter[] = {0x12, 0x00};   // a temporal delimiter OBU alone
        uint64_t n; assert(!h_cv_av1submit(&n, da, delimiter, sizeof delimiter, 0, false) && !cv_of(da)->nheld);
        assert(!h_cv_av1fin(da) && !cv_of(da));
        // The av1C record from a sequence header: profile 0, one operating point at level 12 (tier 0), 2560x1440, 8-bit 4:2:0 with chroma position 0, written field by field
        uint8_t obu[32] = {0x0a, 0}; size_t bit = 16;
        #define PUT(v, nb) do { for (int k_ = (nb) - 1; k_ >= 0; k_--, bit++) if (((uint32_t)(v) >> k_) & 1) obu[bit / 8] |= (uint8_t)(0x80 >> (bit % 8)); } while (0)
        PUT(0, 3); PUT(0, 1); PUT(0, 1);            // profile, still picture, reduced header
        PUT(0, 1); PUT(0, 1); PUT(0, 5);            // no timing info, no display delay, one operating point
        PUT(0, 12); PUT(12, 5); PUT(0, 1);          // idc, level 12, tier
        PUT(11, 4); PUT(10, 4); PUT(2559, 12); PUT(1439, 11);   // frame size bits and frame size
        PUT(0, 1); PUT(0, 3); PUT(0, 4);            // no frame ids, block tools, inter tools
        PUT(1, 1); PUT(0, 2);                       // order hint, jnt_comp and ref_frame_mvs
        PUT(1, 1); PUT(1, 1);                       // screen content tools: choose; integer mv: choose
        PUT(6, 3); PUT(0, 3);                       // order hint bits, superres / cdef / restoration
        PUT(0, 1); PUT(0, 1); PUT(0, 1);            // not high bit depth, not monochrome, no colour description
        PUT(0, 1); PUT(0, 2); PUT(0, 1);            // colour range, chroma position, separate delta q
        #undef PUT
        obu[1] = (uint8_t)((bit + 7) / 8 - 2);
        uint8_t c[4]; int aw = 0, ah = 0;
        assert(mav1_av1c(obu, (bit + 7) / 8, c, &aw, &ah) && aw == 2560 && ah == 1440);
        assert(c[0] == 0x81 && c[1] == 12 && c[2] == 0x0c && c[3] == 0);
        size_t seqn = 0; assert(find_seq(obu, (bit + 7) / 8, &seqn) == obu && seqn == (bit + 7) / 8);
        assert(!mav1_av1c(delimiter, sizeof delimiter, c, &aw, &ah) && !find_seq(delimiter, sizeof delimiter, &seqn));
        // The red frame on each backend this Mac has (VideoToolbox where the chip decodes AV1, and dav1d, which DM_AV1_DAV1D=1 picks), as NV12: Y 81, Cb 90, Cr 240 (BT.601, video range)
        for (int soft = VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1) ? 0 : 1; soft < 2; soft++) {
            setenv("DM_AV1_DAV1D", soft ? "1" : "0", 1);
            mav1 *a = mav1_new(); assert(!strcmp(mav1_backend(a), soft ? "dav1d" : "VideoToolbox"));
            mav1_picture pic; assert((mav1_decode(a, av1, sizeof av1, &pic) == 1 || mav1_drain(a, &pic)) && pic.w == 64 && pic.h == 64 && pic.matrix == 1 && !pic.full_range);   // no matrix in the stream: BT.601 at 64 lines; dav1d's frame threads give a lone frame's picture at the end of the stream
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) assert(abs(pic.y[(size_t)y * pic.ystride + x] - 81) <= 4);
            for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x += 2) { assert(abs(pic.uv[(size_t)y * pic.uvstride + x] - 90) <= 6); assert(abs(pic.uv[(size_t)y * pic.uvstride + x + 1] - 240) <= 6); }
            mav1_reset(a); assert(mav1_decode(a, delimiter, sizeof delimiter, &pic) == 0);   // a new stream: nothing until its sequence header
            mav1_free(a);
        }
        {   // dav1d decodes frames in parallel: a picture can come some packets after its own and two can come together, the end of the stream gives the rest, and each keeps its
            // packet's timestamp, in order
            setenv("DM_AV1_DAV1D", "1", 1);
            mav1 *a = mav1_new(); mav1_picture pic; int64_t seen[24]; int ns = 0;
            for (int i = 0; i < 20; i++)
                if (mav1_decode_ts(a, av1, sizeof av1, 1000 * (int64_t)i, &pic) == 1) do { assert(ns < 24 && pic.w == 64); seen[ns++] = pic.ts; } while (mav1_next(a, &pic));
            while (mav1_drain(a, &pic)) { assert(ns < 24 && pic.w == 64); seen[ns++] = pic.ts; }
            assert(ns == 20); for (int i = 0; i < 20; i++) assert(seen[i] == 1000 * (int64_t)i);
            mav1_free(a);
        }
        unsetenv("DM_AV1_DAV1D");
    }
    {   // A native image: SDK 22's preference and pool, the handle names the pool's memory, and a picture lands there as RGBA in NVN's linear layout
        cv_pref p = h_cv_pref22(0xffff0020, 0xffff0010, 2, 2); assert(p.w == 32 && p.h == 16);
        uint64_t sz, al; assert(!h_cv_poolsize(&sz, &al, &p, 2) && al == CV_NATIVE_ALIGN && sz == 2 * CV_NATIVE_ALIGN);
        uint8_t pt[0x40] = {0}; void *m; assert(!posix_memalign(&m, al, sz)); memset(m, 0, sz);
        assert(!h_cv_pool22(pt, m, sz, &p, 2));
        uint32_t ha = h_cv_native(pt, 1), nw, nh, ns; uint8_t *nm = NULL; uint64_t nz;
        assert(ha && dm_native_image(ha, &nm, &nz, &nw, &nh, &ns) && nm == (uint8_t *)m + CV_NATIVE_ALIGN && nz == CV_NATIVE_ALIGN && nw == 32 && nh == 16 && ns == 128);
        vpx_image_t *im = black_i420(32, 16); im->range = VPX_CR_STUDIO_RANGE;
        pool_native_write(pool_of(pt), 1, im); vpx_img_free(im);
        assert(nm[0] == 0 && nm[3] == 255 && nm[ns * 15 + 31 * 4 + 3] == 255);
        h_cv_poolfin(pt); assert(!dm_native_image(ha, &nm, &nz, &nw, &nh, &ns)); free(m);
    }
    {   // SDK 19's Initialize by remote memory (two sizes before the picture size) and its preference: a VP9 decoder as the other forms make
        _Alignas(16) static uint8_t d19[0x400];
        cv_pref p19 = h_cv_pref19(0xffff0000u | 64, 32, 3, 1);   // the 16-bit sizes come unextended
        assert(p19.w == 64 && p19.h == 32 && p19.fmt == 3 && p19.magic == CV_MAGIC);
        assert(!h_cv_vp9rm19(d19, 0x10000, 0x200000, 0xabcd0000u | 16, 16, 1, 1, 3) && cv_of(d19) && cv_of(d19)->w == 16);
        assert(!h_cv_vp9fin(d19));
    }
    {   // Blank native images (GameChat's camera pictures with no camera): zeroed storage with room for 16 bytes a pixel, apart from the decoder pools' handles, gone once freed
        uint32_t a = dm_native_image_blank(384, 216), b = dm_native_image_blank(384, 216), nw, nh, ns; uint8_t *nm = NULL; uint64_t nz;
        assert(a && b && a != b && a >> 31 && b >> 31);
        assert(dm_native_image(a, &nm, &nz, &nw, &nh, &ns) && nw == 384 && nh == 216 && ns >= 384 * 16 && ns % 64 == 0 && nz >= (uint64_t)ns * 216);
        for (uint64_t k = 0; k < nz; k++) assert(!nm[k]);
        dm_native_image_blank_free(a); assert(!dm_native_image(a, &nm, &nz, &nw, &nh, &ns)); assert(dm_native_image(b, &nm, &nz, &nw, &nh, &ns));
        dm_native_image_blank_free(b); dm_native_image_blank_free(b); dm_native_image_blank_free(0x100);   // twice, and a decoder pool's handle: nothing happens
        uint32_t k[BLANK_MAX + 1]; for (int i = 0; i < BLANK_MAX; i++) assert((k[i] = dm_native_image_blank(32, 32)));
        assert(!(k[BLANK_MAX] = dm_native_image_blank(32, 32)));   // full
        for (int i = 0; i < BLANK_MAX; i++) dm_native_image_blank_free(k[i]);
    }

    {   // AV1 pictures by the movie's clock (cv_skip_late), on a clock the test sets: 50 fps timestamps (20 ms apart), host seconds from 100
        cv_dec *d = calloc(1, sizeof *d); d->codec = CV_AV1; double (*real)(void) = cv_clock; cv_clock = test_clock;
        #define HOLD(n) do { d->nheld = 0; for (int i_ = 0; i_ < (n); i_++) { d->held[i_] = black_i420(16, 16); d->held_ts[i_] = 20000000LL * (i_ + d->clk_last / 20000000LL); d->held_eos[i_] = 0; } d->nheld = (n); } while (0)
        #define CLEAR() do { for (int i_ = 0; i_ < d->nheld; i_++) vpx_img_free(d->held[i_]); d->nheld = 0; } while (0)
        g_test_now = 100.0; HOLD(6); cv_skip_late(d); assert(d->clk_on && d->nheld == 6 && d->clk_t0 == 100.0);   // the first picture starts the clock: nothing dropped
        g_test_now = 100.1; cv_skip_late(d);   // 100 ms on: pictures at 0..100 ms are due; the next must be 50 ms overdue for one to go
        assert(d->nheld == 4 && d->held_ts[0] == 40000000);   // 0 and 20 ms dropped (20 and 40 ms over 50 ms overdue), 40 kept: its next (60) is only 40 ms overdue
        assert(d->skipped == 2);
        g_test_now = 100.1; d->held_eos[0] = 1; cv_skip_late(d); assert(d->nheld == 4);   // a picture that ends the stream is never dropped
        d->held_eos[0] = 0; CLEAR();
        // over a second behind while the game presented frames without taking a picture (a pause menu, a load screen): the clock starts again, nothing dropped
        d->clk_last = 200000000; HOLD(3); d->take_frame = 10; g_test_frames = 10 + CV_PAUSE_FRAMES; g_test_now = 106.0; cv_skip_late(d);
        assert(d->nheld == 3 && d->clk_t0 == 106.0 && d->clk_ts0 == d->held_ts[0] && d->restarts == 1 && !d->catching);
        CLEAR();
        // 1.5 s behind after the whole game stood still in the host (a pipeline built on first use) with its sound going on: the clock stays, the late pictures go
        d->clk_last = 300000000; HOLD(5); d->take_frame = g_test_frames - 1; g_test_wall = 50; d->take_wall = 40; g_test_now = 107.6; cv_skip_late(d);
        assert(d->nheld == 1 && d->held_ts[0] == 380000000 && d->clk_t0 == 106.0 && d->restarts == 1 && d->catching && d->catchups == 1);
        g_test_now = 107.61; cv_skip_late(d); assert(d->catchups == 1);   // still behind: the same stall, noted once
        CLEAR();
        // over a second behind, nothing presented, but the window went out of focus and came back meanwhile (the game may have paused itself): the clock starts again
        d->clk_last = 400000000; HOLD(3); d->take_frame = g_test_frames; d->take_wall = 60; g_test_focus_in = 70; g_test_now = 110.0; cv_skip_late(d);
        assert(d->nheld == 3 && d->clk_t0 == 110.0 && d->restarts == 2 && !d->catching);
        CLEAR(); g_test_focus = 2;   // and out of focus now
        d->clk_last = 500000000; HOLD(3); d->take_wall = 80; g_test_now = 115.0; cv_skip_late(d); assert(d->nheld == 3 && d->clk_t0 == 115.0 && d->restarts == 3);
        g_test_focus = 1;
        CLEAR(); d->clk_last = 900000000; d->nheld = 0;
        for (int i = 0; i < 3; i++) { d->held[i] = black_i420(16, 16); d->held_ts[i] = 20000000LL * i; d->held_eos[i] = 0; } d->nheld = 3;   // a timestamp going back (a loop)
        g_test_now = 106.5; cv_skip_late(d); assert(d->nheld == 3 && d->clk_t0 == 106.5 && d->clk_ts0 == 0);
        CLEAR(); cv_clock = real; free(d);
        #undef HOLD
        #undef CLEAR
    }
    printf("t_codec_video: all passed\n");
    return 0;
}
