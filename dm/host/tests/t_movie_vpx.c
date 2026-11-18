// Synthetic 16x16 red key frames encoded with ffmpeg's libvpx encoders; no game data.
// Include the real decoder, without registering unrelated SDK hooks in this test.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_movie.c"
#include <assert.h>

static const uint8_t vp8[] = {
    0xf0,0x02,0x00,0x9d,0x01,0x2a,0x10,0x00,0x10,0x00,0x00,0x47,0x08,0x85,0x85,0x88,
    0x85,0x84,0x88,0x02,0x02,0x02,0x75,0xaa,0x03,0xf8,0x03,0xfa,0x02,0x0d,0x4d,0x18,
    0x00,0xfe,0xfd,0x6e,0xf3,0xff,0xe3,0x99,0x37,0x30,0xc4,0xff,0x8e,0x6d,0xff,0xf1,
    0x61,0x3c,0x0e,0x28,0xc8,0xff,0xf1,0x51,0x00,
};
static const uint8_t vp9[] = {
    0x82,0x49,0x83,0x42,0x00,0x00,0xf0,0x00,0xf6,0x00,0x38,0x24,0x1c,0x18,0x4a,0x00,
    0x00,0x30,0x60,0x00,0x00,0x10,0xbf,0xff,0xf7,0x1d,0xaf,0xff,0xff,0xff,0x5f,0xdf,
    0xff,0xff,0xff,0xf2,0x2a,0xc0,0x00,
};
static void decode(int type, int format, const uint8_t *packet, int size) {
    mdec_t d = { .type = type, .fmt = format, .w = 16, .h = 16 };
    assert(md_vpx(&d, packet, size, 0) == 1);
    const uint8_t *p = d.out[0].frm;
    if (format == MD_FMT_NV12) {
        assert(d.out[0].cap == 16 * 16 * 3 / 2);
        for (int i = 0; i < 256; i++) assert(abs(p[i] - 81) <= 3);
        for (int i = 256; i < 384; i += 2) { assert(abs(p[i] - 90) <= 3); assert(abs(p[i+1] - 240) <= 3); }
    } else {
        assert(d.out[0].cap == 16 * 16 * 4);
        for (int i = 0; i < 1024; i += 4) assert(p[i] >= 250 && p[i+1] <= 5 && p[i+2] <= 5 && p[i+3] == 255);
    }
    vpx_codec_destroy(&d.vpx);
    free(d.out[0].frm);
}
int main(void) {
    int w = 0, h = 0;
    assert(!vp8_size(vp8, 9, &w, &h));
    assert(vp8_size(vp8, sizeof vp8, &w, &h) && w == 16 && h == 16);
    for (int format = MD_FMT_ABGR; format <= MD_FMT_NV12; format++) {
        decode(3, format, vp9, sizeof vp9);
        decode(2, format, vp8, sizeof vp8);
    }
    puts("VP8 and VP9 key frames: NV12 and RGBA pixels match");
#if !defined(__APPLE__) && !defined(_WIN32)
    // The AAC stopgap (Linux, no decoder; md_aac_silence, which Windows takes too where Media Foundation has no decoder): each packet gives one frame of silence, 1024
    // samples a channel (2048 with SBR), PCM16 interleaved, the size GetOutputBufferSize reports; an empty packet gives nothing; the channels from the configuration
    // when the MediaData gave none
    {
        static uint8_t lc[] = {0x11, 0x90}, sbr[] = {0x29, 0x90}, six[] = {0x11, 0xb0}, pkt[16] = {0xde, 0xad};   // AudioSpecificConfig: AAC-LC, 48 kHz, 2 channels; SBR; 6 channels
        mdec_t d = { .type = MD_TYPE_AAC, .ch = 2, .rate = 48000 };
        d.vhdr[0] = lc; d.vhdr_n[0] = sizeof lc;
        assert(md_aac(&d, pkt, sizeof pkt, 0) == 1 && d.out[0].bytes == 1024 * 2 * 2);
        d.out[0].frm[0] = 1; d.out[0].frm[4095] = 1;   // the slot is used again: all of the frame is written each time
        assert(md_aac(&d, pkt, sizeof pkt, 0) == 1);
        for (size_t i = 0; i < d.out[0].bytes; i++) assert(d.out[0].frm[i] == 0);
        assert(md_aac(&d, pkt, 0, 1) == 0 && d.out[1].frm == NULL);
        d.vhdr[0] = sbr;
        assert(md_aac(&d, pkt, sizeof pkt, 1) == 1 && d.out[1].bytes == 2048 * 2 * 2);
        mdec_t e = { .type = MD_TYPE_AAC };
        e.vhdr[0] = six; e.vhdr_n[0] = sizeof six;
        assert(md_aac(&e, pkt, sizeof pkt, 0) == 1 && e.ch == 6 && e.out[0].bytes == 1024 * 2 * 6);
        mdec_t f = { .type = MD_TYPE_AAC };
        assert(md_aac(&f, pkt, sizeof pkt, 0) == 1 && f.ch == 2 && f.out[0].bytes == 1024 * 2 * 2);
        free(d.out[0].frm); free(d.out[1].frm); free(e.out[0].frm); free(f.out[0].frm);
        puts("AAC without a decoder: one frame of silence per packet");
    }
#endif
}
