// Execute the production emitter's tiny AArch64 UICamera postlude, without a
// game, a render loop or game assets. Called through the native x8 result ABI.
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct { uint32_t mode, padding; float aspect, bounds[4], near_z, far_z; } camera;
typedef struct { float simd[8][4]; uint64_t before_flags, after_flags, x9, x8; } trace;
static void invoke(void *fn, camera *out, float input[8][4], trace *t) {
    __asm__ volatile(
        "mov x16, %[fn]\n mov x17, %[trace]\n mov x8, %[out]\n"
        "ldp q0, q1, [%[in]]\n ldp q2, q3, [%[in], #32]\n"
        "ldp q4, q5, [%[in], #64]\n ldp q6, q7, [%[in], #96]\n"
        "cmp xzr, xzr\n mrs x10, nzcv\n str x10, [x17, #128]\n"
        "blr x16\n"
        "stp q0, q1, [x17]\n stp q2, q3, [x17, #32]\n"
        "stp q4, q5, [x17, #64]\n stp q6, q7, [x17, #96]\n"
        "mrs x10, nzcv\n str x10, [x17, #136]\n str x9, [x17, #144]\n str x8, [x17, #152]\n"
        : : [fn] "r" (fn), [trace] "r" (t), [out] "r" (out), [in] "r" (input)
        : "x8", "x9", "x10", "x16", "x17", "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "cc", "memory");
}
static int checks;
#define CHECK(x) do { assert(x); checks++; } while(0)
int main(int argc, char **argv) {
    CHECK(argc == 2);
    FILE *f = fopen(argv[1], "rb"); CHECK(f);
    uint8_t code[512]; size_t n = fread(code, 1, sizeof code, f); fclose(f); CHECK(n > 256 && n < sizeof code);
    uint8_t *jit = mmap(NULL, 16384, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0); CHECK(jit != MAP_FAILED);
    pthread_jit_write_protect_np(0); memcpy(jit, code, n); __builtin___clear_cache((char *)jit, (char *)jit+n); pthread_jit_write_protect_np(1);
    for (int mode = 0; mode < 2; mode++) for (int sample = 0; sample < 128; sample++) {
        float in[8][4]; for (int i=0; i<8; i++) for(int j=0; j<4; j++) in[i][j] = 0.125f*(i*4+j+1);
        float cx = sample ? (sample-64)*0.0625f : 0, cy = sample ? (sample-64)*0.03125f : 0;
        in[0][0] = cx+8; in[1][0] = cx-8; in[2][0] = cy-4.5f; in[3][0] = cy+4.5f;
        in[4][0] = 0.644420981f; in[5][0] = 31.6444206f;
        struct { uint8_t prefix[16]; camera c; uint8_t suffix[16]; } out; memset(&out, 0xa5, sizeof out);
        trace t = {0}; invoke(jit+(mode?128:0), &out.c, in, &t);
        CHECK(!memcmp(t.simd, in, sizeof in)); CHECK(t.before_flags == t.after_flags); CHECK(t.x9 == 2); CHECK(t.x8 == (uintptr_t)&out.c);
        CHECK(out.c.mode == 2 && out.c.padding == 0 && out.c.aspect == 1);
        CHECK(!memcmp(&out.c.near_z, &in[4][0], 4)); CHECK(!memcmp(&out.c.far_z, &in[5][0], 4));
        for (int i=0; i<16; i++) CHECK(out.prefix[i] == 0xa5 && out.suffix[i] == 0xa5);
        for (int i=0; i<4; i++) {
            float centre = i<2 ? cx : cy;
            float expected = centre+(in[i][0]-centre)*(mode?10.f/9.f:1.f);
            CHECK(fabsf(out.c.bounds[i]-expected) < 0.000004f);
        }
        CHECK(fabsf((out.c.bounds[0]+out.c.bounds[1])*.5f-cx) < .000004f);
        CHECK(fabsf((out.c.bounds[2]+out.c.bounds[3])*.5f-cy) < .000004f);
        // Equal scale in actual canvas pixels keeps UI circles round. The
        // selected scene and final presentation crop are independent of this.
        float sx=320.f/(out.c.bounds[0]-out.c.bounds[1]), sy=180.f/(out.c.bounds[3]-out.c.bounds[2]);
        CHECK(fabsf(sx-sy) < .000004f);
        if (mode && !sample) {
            float left=(in[1][0]-out.c.bounds[1])*sx, right=(in[0][0]-out.c.bounds[1])*sx;
            CHECK(fabsf(left-16) < .0001f && fabsf(right-304) < .0001f); // all UI inside crop [16,304]
            CHECK(fabsf((in[2][0]-out.c.bounds[2])*sy-9) < .0001f); // symmetric vertical safe area
        }
    }
    munmap(jit, 16384); printf("PASS: %d native UICamera checks; default/16:10, symmetric/asymmetric bounds, SIMD/NZCV, depth, edge fit\n", checks);
    return 0;
}
