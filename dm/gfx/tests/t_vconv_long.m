// vconv_fill converts a long range in chunks on every core: the result is the one vconv_elem gives each element, for the lengths around a chunk and a half, a chunk's multiples and
// a range as long as a football stadium's mesh (915,138 vertices), at aligned and unaligned sources and strides larger and smaller than the element
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
int main(void) { @autoreleasepool {
    // the 16 8 / 16-bit integer formats (vconv_int), unorm / snorm 8 and 16, half 1-4 components, float 1-4 components
    static const int formats[] = { 0x03, 0x04, 0x08, 0x09, 0x0F, 0x10, 0x14, 0x15, 0x1B, 0x1C, 0x20, 0x21, 0x27, 0x28, 0x2C, 0x2D,
                                   0x01, 0x02, 0x06, 0x0E, 0x12, 0x1A, 0x26, 0x05, 0x11, 0x1D, 0x29, 0x0A, 0x16, 0x22 };
    static const uint64_t counts[] = { 1, 7, 32767, 32768, 65535, 65536, 65537, 98304, 100001, 262145, 915138 };
    static const uint32_t extra_strides[] = { 0, 1, 4, 5, 12, 40 };   // added to the element's size
    srand(20261108);
    int failed = 0, checks = 0;
    for (size_t k = 0; k < sizeof formats / sizeof *formats; k++) {
        int f = formats[k], size = vconv_size(f);
        for (size_t c = 0; c < sizeof counts / sizeof *counts; c++) for (size_t s = 0; s < sizeof extra_strides / sizeof *extra_strides; s++) {
            uint64_t nv = counts[c]; uint32_t stride = (uint32_t)size + extra_strides[s];
            if (nv > 262145 && s > 1) continue;   // the longest range at the two smallest strides is enough
            size_t bytes = (size_t)stride * nv + 64; uint8_t *raw = malloc(bytes);
            for (size_t i = 0; i < bytes; i++) raw[i] = (uint8_t)rand();
            const uint8_t *src = raw + (c & 3);   // unaligned sources too
            float *got = malloc(nv * 16), *want = malloc(nv * 16);
            vconv_fill(f, (uint64_t)(uintptr_t)src, stride, nv, got);
            for (uint64_t i = 0; i < nv; i++) vconv_elem(f, src + i * stride, want + 4 * i);
            checks++;
            if (memcmp(got, want, nv * 16)) { failed++; printf("FAIL format %#x, %llu vertices, stride %u\n", f, (unsigned long long)nv, stride); }
            free(raw); free(got); free(want);
        }
    }
    // the zero fill the replay appends past the bound range (stride 0, a 16-byte source)
    { static const uint8_t zeros[16]; uint64_t nv = 300000; float *got = malloc(nv * 16), *want = malloc(nv * 16);
      for (size_t k = 0; k < sizeof formats / sizeof *formats; k++) {
          vconv_fill(formats[k], (uint64_t)(uintptr_t)zeros, 0, nv, got);
          for (uint64_t i = 0; i < nv; i++) vconv_elem(formats[k], zeros, want + 4 * i);
          checks++; if (memcmp(got, want, nv * 16)) { failed++; printf("FAIL zero fill, format %#x\n", formats[k]); } }
      free(got); free(want); }
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
