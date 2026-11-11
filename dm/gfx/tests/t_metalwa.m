// t_metalwa <lib.metallib>: runs t_metalwa.metal's kernels on this Mac's GPU and checks them on the CPU. One line per check: "<name>: bad <lanes with a wrong result> of <lanes>".
// Bounded: the waterfall stops at 4096 rounds a lane; the whole run is a few milliseconds of GPU time. test_dmsh_metalwa.py (DMSH_TEST_METAL=1) builds and runs it.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 64 * 32 };

static id<MTLComputePipelineState> pipe_of(id<MTLDevice> d, id<MTLLibrary> lib, NSString *name) {
    NSError *e = nil; id<MTLFunction> f = [lib newFunctionWithName:name];
    if (f.functionConstantsDictionary.count) f = [lib newFunctionWithName:name constantValues:[MTLFunctionConstantValues new] error:&e];   // defaults
    id<MTLComputePipelineState> p = f ? [d newComputePipelineStateWithFunction:f error:&e] : nil;
    if (!p) { fprintf(stderr, "%s: %s\n", name.UTF8String, e ? e.description.UTF8String : "no function"); exit(1); }
    return p;
}

static id<MTLBuffer> _Nonnull buf(id<MTLDevice> d, const void *bytes, NSUInteger len) {   // a shared buffer, copied from bytes when given
    id<MTLBuffer> b = bytes ? [d newBufferWithBytes:bytes length:len options:0] : [d newBufferWithLength:len options:0];
    if (!b) { fprintf(stderr, "no buffer of %lu bytes\n", (unsigned long)len); exit(1); }
    return b;
}

static uint32_t rnd(void) { static uint32_t x = 7; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }   // xorshift32: the same patterns on every run

static void run(id<MTLCommandQueue> q, id<MTLComputePipelineState> p, NSArray<id<MTLBuffer>> *bufs) {
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:p];
    for (NSUInteger i = 0; i < bufs.count; i++) [ce setBuffer:bufs[i] offset:0 atIndex:i];
    [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr, "%s\n", cb.error.description.UTF8String); exit(1); }
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 2) { fprintf(stderr, "usage: t_metalwa <lib.metallib>\n"); return 2; }
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [d newCommandQueue]; NSError *e = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
    if (!lib) { fprintf(stderr, "%s\n", e.description.UTF8String); return 1; }
    id<MTLBuffer> out = buf(d, NULL, N * 4); const uint32_t *r = out.contents;
    {   // kk3_branch: the odd lanes
        uint32_t in[2] = {0xaaaaaaaau, 0}; id<MTLBuffer> ib = buf(d, in, sizeof in);
        run(q, pipe_of(d, lib, @"kk3_branch"), @[ib, out]);
        int bad = 0; for (int i = 0; i < N; i++) bad += r[i] != ((i & 1) ? 0xaaaaaaaau : 2u);
        printf("kk3_branch: bad %d of %d\n", bad, N);
    }
    {   // kk3_waterfall on lane patterns
        const char *names[] = {"one key", "2 keys", "3 keys", "32 keys", "lane 0 inactive, 2 keys", "half inactive, 4 keys", "random 1-4 keys, random inactive"};
        id<MTLComputePipelineState> p = pipe_of(d, lib, @"kk3_waterfall");
        for (int pat = 0; pat < 7; pat++) {
            static uint32_t v[N], a[N];
            for (int i = 0; i < N; i++) {
                int l = i & 31; a[i] = 1;
                switch (pat) {
                case 0: v[i] = 5; break;
                case 1: v[i] = l < 7 ? 0x11 : 0x22; break;
                case 2: v[i] = 0x100 + (l % 3); break;
                case 3: v[i] = 0x1000 + l; break;
                case 4: v[i] = l & 1 ? 0x11 : 0x22; a[i] = l != 0; break;
                case 5: v[i] = l % 4; a[i] = l & 1; break;
                default: v[i] = rnd() % (1 + (i / 32) % 4); a[i] = (rnd() % 8) != 0; break;
                }
            }
            id<MTLBuffer> vb = buf(d, v, sizeof v), ab = buf(d, a, sizeof a);
            run(q, p, @[vb, out, ab]);
            int bad = 0, capped = 0, lanes = 0;
            for (int g = 0; g < N / 32; g++) {
                uint32_t seen[32]; uint32_t distinct = 0;
                for (int l = 0; l < 32; l++) {
                    int i = g * 32 + l; uint32_t k; if (!a[i]) continue;
                    for (k = 0; k < distinct; k++) if (seen[k] == v[i]) break;
                    if (k == distinct) seen[distinct++] = v[i];
                }
                for (int l = 0; l < 32; l++) { int i = g * 32 + l; if (!a[i]) continue; lanes++; capped += r[i] >= 4096; bad += r[i] != distinct; }
            }
            printf("kk3_waterfall %s: bad %d of %d (%d at the 4096-round cap)\n", names[pat], bad, lanes, capped);
        }
    }
    {   // kk9_break: the first active lane of each group breaks at once
        uint32_t in[2] = {0, 64}; id<MTLBuffer> ib = buf(d, in, sizeof in);
        run(q, pipe_of(d, lib, @"kk9_break"), @[ib, out]);
        int bad = 0; for (int i = 0; i < N; i++) bad += r[i] != (1u << (i & 31));
        printf("kk9_break: bad %d of %d\n", bad, N);
    }
} return 0; }
