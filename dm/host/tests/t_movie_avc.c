// Synthetic SPS prefixes and pixel buffers, no game files. Exercise the real
// parameter-set selection and output queue without requiring a hardware codec.
#include "../hle_movie_avc.c"
#include <assert.h>

typedef struct { uint8_t p[512]; unsigned bit; } writer;
static void put(writer *w, unsigned v, unsigned n) {
    while (n--) { w->p[w->bit / 8] |= ((v >> n) & 1) << (7 - w->bit % 8); w->bit++; }
}
static void ue(writer *w, unsigned v) {
    unsigned n = 0, x = ++v;
    while (x >>= 1) n++;
    put(w, 0, n); put(w, v, n + 1);
}
static size_t sps(writer *w, unsigned profile, unsigned poc, unsigned chroma, int scaling) {
    *w = (writer){0}; put(w, 0x67, 8); put(w, profile, 8); put(w, 0, 8); put(w, 41, 8); ue(w, 0);
    if (profile == 100 || profile == 244) {
        ue(w, chroma); if (chroma == 3) put(w, 0, 1);
        ue(w, 0); ue(w, 0); put(w, 0, 1); put(w, scaling, 1);
        if (scaling) for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u); i++) {
            put(w, i == 0, 1);
            if (!i) ue(w, 16); // delta_scale = -8, nextScale = 0: default matrix
        }
    }
    ue(w, 0); ue(w, poc); put(w, 1, 1); // prefix ends here; rbsp stop bit
    return (w->bit + 7) / 8;
}
static void queue_picture(mavc *a) {
    CVPixelBufferRef pb = NULL;
    assert(!CVPixelBufferCreate(NULL, 16, 16, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL, &pb));
    CVPixelBufferLockBaseAddress(pb, 0);
    for (int i = 0; i < 2; i++) memset(CVPixelBufferGetBaseAddressOfPlane(pb, i), i ? 128 : 16,
                                      CVPixelBufferGetBytesPerRowOfPlane(pb, i) * CVPixelBufferGetHeightOfPlane(pb, i));
    CVPixelBufferUnlockBaseAddress(pb, 0);
    on_frame(a, NULL, 0, 0, pb, CMTimeMake(0, 1000000), kCMTimeInvalid);
    CVPixelBufferRelease(pb);
}
int main(void) {
    writer w;
    for (unsigned profile = 100; profile <= 244; profile += 144) for (unsigned chroma = 0; chroma <= 3; chroma++) {
        for (int scaling = 0; scaling < 2; scaling++) {
            size_t n = sps(&w, profile, 2, chroma, scaling);
            assert(avc_reorder_depth(w.p, n) == 0);
            for (size_t cut = 0; cut < n; cut++) assert(avc_reorder_depth(w.p, cut) == 4);
        }
    }
    for (unsigned poc = 0; poc <= 1; poc++) {
        size_t n = sps(&w, 100, poc, 1, 0);
        assert(avc_reorder_depth(w.p, n) == 4);
    }
    size_t n = sps(&w, 66, 0, 1, 0); assert(avc_reorder_depth(w.p, n) == 0);
    memset(w.p, 0, sizeof w.p); w.p[0] = 0x67; w.p[1] = 100;
    assert(avc_reorder_depth(w.p, sizeof w.p) == 4); // unterminated Exp-Golomb code
    assert(avc_reorder_depth(w.p, sizeof w.p + 1) == 4);

    mavc *a = mavc_new(); uint8_t out[16 * 16 * 3 / 2]; int64_t pts = -1;
    n = sps(&w, 100, 2, 1, 0); keep_ps(a, w.p, n); queue_picture(a);
    assert(a->depth == 0 && mavc_take(a, 0, out, 16, 16, &pts) == 1 && pts == 0 && !mavc_queued(a));
    assert(out[0] == 16 && out[256] == 128);
    n = sps(&w, 100, 0, 1, 0); keep_ps(a, w.p, n); queue_picture(a);
    assert(a->depth == 4 && !mavc_take(a, 0, out, 16, 16, &pts) && mavc_queued(a) == 1);
    assert(mavc_take(a, 1, out, 16, 16, &pts) == 1 && !mavc_queued(a));
    mavc_free(a);
    puts("AVC: POC type 2 returns the final picture without EOS; other POC modes retain reordering and EOS drain");
}
