// Kept vertex conversions (vk_get): a conversion is kept from a key's second submission, holds what vconv_elem gives each element (and the conversion of zeros past the bound
// range), is bound again while its source is unchanged, and goes when the source is written: inside its protected pages, in the bytes it shares a page with, or by the GPU.
// A write beside the stream in a shared page leaves it; a range below the smallest kept one, or a stream rewritten between its sightings, is never kept
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; printf("FAIL: %s\n", label); } } while (0)
static int same(uint64_t gpu, const vk_key *k, const uint8_t *src) {   // the kept buffer against vconv_elem
    for (int i = 0; i < g_vk_n; i++) if (g_vk_list[i]->gpu == gpu) {
        const float *got = ((__bridge id<MTLBuffer>)g_vk_list[i]->buf).contents; static const uint8_t zeros[16];
        for (uint64_t v = 0; v < k->nt; v++) { float want[4]; vconv_elem((int)k->fmt, v < k->nv ? src + v * k->stride : zeros, want); if (memcmp(got + 4 * v, want, 16)) return 0; }
        return 1; }
    return 0;
}
static void wr(uint8_t *p, uint8_t v) { dm_guest_write_prepare(p, 1); *p = v; }   // a host write, as the game's own goes through the fault hook
int main(void) { @autoreleasepool {
    mtl_init();
    size_t pg = (size_t)getpagesize(), poolsize = 64 * pg; uint8_t *mem = NULL; if (posix_memalign((void **)&mem, pg, poolsize)) abort();
    srand(20261111); for (size_t i = 0; i < poolsize; i++) mem[i] = (uint8_t)rand();
    mtl_pool_add(mem, poolsize); nvn_dirty_add_range(mem, poolsize);
    static const int formats[] = { 0x14, 0x15, 0x10, 0x11, 0x26, 0x57 };   // RG16UI, RG16I, RG8I, RG16F, RGBA8 snorm, RG8_UI2F
    for (size_t fi = 0; fi < sizeof formats / sizeof *formats; fi++) {
        int f = formats[fi]; uint64_t es = (uint64_t)vconv_size(f); uint32_t stride = (uint32_t)es + 4, off = 100;
        uint64_t nv = 100000, nt = nv + 37; uint8_t *src = mem + off; uint64_t sn = (nv - 1) * stride + es;
        vk_key k = {(uint64_t)(uintptr_t)mem, off + sn, 0, nv, nt, (uint32_t)f, off, stride, 0};
        uint64_t a = (uint64_t)(uintptr_t)src;
        g_sub_serial += 100000;   // past every earlier back-off
        CHECK("first submission: noted only", !vk_get(&k, a, es) && !g_vk_n);
        g_sub_serial++; uint64_t g1 = vk_get(&k, a, es);
        CHECK("second submission: kept", g1 && g_vk_n == 1);
        CHECK("the kept conversion is vconv_elem's, zeros past the range", same(g1, &k, src));
        CHECK("the same submission binds it again", vk_get(&k, a, es) == g1);
        g_sub_serial++; CHECK("unchanged: bound again in the next submission", vk_get(&k, a, es) == g1);
        // a neighbour in the stream's first page, before its first byte, and one after its last
        wr(mem + 3, (uint8_t)(mem[3] + 1)); wr(src + sn + 5, (uint8_t)(src[sn + 5] + 1));
        g_sub_serial++; CHECK("a write beside the stream in a shared page leaves it", vk_get(&k, a, es) == g1);
        // inside the protected pages
        wr(src + 5 * pg, (uint8_t)(src[5 * pg] + 1));
        g_sub_serial++; CHECK("a write inside drops it", !vk_get(&k, a, es) && !g_vk_n);
        g_sub_serial++; CHECK("not tried again at once", !vk_get(&k, a, es));
        g_sub_serial += 64; uint64_t g2 = vk_get(&k, a, es);
        CHECK("kept again after the back-off, with the new bytes", g2 && same(g2, &k, src));
        // the stream's own bytes in the shared first page
        wr(src + 1, (uint8_t)(src[1] + 1));
        g_sub_serial++; CHECK("a write to its bytes in a shared page drops it", !vk_get(&k, a, es) && !g_vk_n);
        g_sub_serial += 4096; uint64_t g3 = vk_get(&k, a, es); CHECK("kept again", g3 && same(g3, &k, src));
        vk_gpu_write((uint64_t)(uintptr_t)mem + poolsize - 64, 64); CHECK("a GPU write elsewhere leaves it", g_vk_n == 1);
        vk_gpu_write(a + sn - 1, 8); CHECK("a GPU write on it drops it", !g_vk_n);
        g_sub_serial++; CHECK("and it is not bound", !vk_get(&k, a, es));
        g_sub_serial += 100000; uint64_t g4 = vk_get(&k, a, es); CHECK("kept again", g4 != 0);
        vk_gpu_write(0, 0); CHECK("a GPU write that may be anywhere drops it", !g_vk_n);
        // a range inside one page: no page of its own to protect, the whole of it is compared
        vk_key s = {(uint64_t)(uintptr_t)mem, poolsize, 0, 4096, 4096, 0x03, (uint32_t)(20 * pg + 16), 2, 0}; uint8_t *ss = mem + s.off; uint64_t sa = (uint64_t)(uintptr_t)ss;
        g_sub_serial += 100000; vk_get(&s, sa, 1); g_sub_serial++; uint64_t g5 = vk_get(&s, sa, 1);
        CHECK("a range within one page is kept", g5 && same(g5, &s, ss));
        g_sub_serial++; CHECK("and bound again", vk_get(&s, sa, 1) == g5);
        wr(ss + 4000, (uint8_t)(ss[4000] + 1)); g_sub_serial++; CHECK("and dropped when written", !vk_get(&s, sa, 1) && !g_vk_n);
        vk_key t = s; t.nv = t.nt = 4095; g_sub_serial++; vk_get(&t, sa, 1); g_sub_serial++;
        CHECK("a range below the smallest kept one is not kept", !vk_get(&t, sa, 1) && !g_vk_n);
    }
    // a stream rewritten between its sightings is not kept: its samples differ each time it is due
    { vk_key d = {(uint64_t)(uintptr_t)mem, poolsize, 0, 50000, 50000, 0x14, (uint32_t)(30 * pg + 8), 4, 0}; uint8_t *ds = mem + d.off; uint64_t da = (uint64_t)(uintptr_t)ds; uint64_t made = g_vk_made; int kept = 0;
      g_sub_serial += 100000;
      for (int i = 0; i < 2000; i++) { dm_guest_write_prepare(ds, 200000); for (int j = 0; j < 200000; j += 64) ds[j]++; g_sub_serial++; kept += vk_get(&d, da, 4) != 0; }
      CHECK("a stream rewritten every submission is never kept", !kept && g_vk_made == made);
      for (int i = 0; i < 40000 && !kept; i++) { g_sub_serial++; kept += vk_get(&d, da, 4) != 0; }
      CHECK("and is kept once it stays unchanged", kept == 1 && same(g_vk_list[g_vk_n - 1]->gpu, &d, ds)); }
    // forgotten when not drawn
    uint32_t before = g_vk.n; g_sub_serial += VK_IDLE + 100; for (int i = 0; i < 64 && g_vk.n; i++) { g_vk_swept = 0; vk_sweep(); }
    CHECK("keys not drawn are forgotten", before && !g_vk.n && !g_vk_bytes);
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
