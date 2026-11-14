// Bind-time pointer sites, tested on the translator's real output: the raw-load helper of model pixel shader 0031f9442b11b682 (built by the bind-time pointer translator, branch gptr-sites), compiled
// from its MSL with a test kernel appended, called on chosen guest addresses with a pool table and a site table built here, as the host builds them (nvn_mtl.m gmem_map, NVN_RB_GPTR).
// Three buffers stand for pool memory: A and B are in the pool table; C is reachable only through a site entry that names A's range with C's delta, so a C value proves the
// site path was taken and an A value proves the table path. Usage: raw_sites <path of the .metal> <helper name>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

enum { ENTRIES = 8192, EW = 8 };   // dmsh_abi.h DMSH_GMEM_MAP_ENTRIES on main, words per entry
static const uint64_t GA = 0x100000000ull, GB = 0x100100000ull, PSZ = 65536;   // guest bases of pools A and B, their size

static void entry(uint32_t *t, int k, uint64_t lo, uint64_t hi, uint64_t delta) {
    uint32_t *e = t + (size_t)k * EW; e[0] = (uint32_t)lo; e[1] = (uint32_t)(lo >> 32); e[2] = (uint32_t)hi; e[3] = (uint32_t)(hi >> 32); e[4] = (uint32_t)delta; e[5] = (uint32_t)(delta >> 32); e[6] = 0; e[7] = 0;
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 3) { fprintf(stderr, "usage: raw_sites <shader.metal> <helper>\n"); return 2; }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue];
    NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
    NSString *h = @(argv[2]);
    src = [src stringByAppendingFormat:@"\nkernel void gptr_test(const device _31& tab [[buffer(0)]], const device _31& sites [[buffer(1)]], const device int4* c [[buffer(2)]], device int* out [[buffer(3)]], uint i [[thread_position_in_grid]])\n"
           "{ uint last = 0u; int lo = c[i].x, hi = c[i].y, s = c[i].z; if (c[i].w) { int plo = int(%llu), phi = 1, ps = -1; (void)%@(plo, phi, ps, tab, sites, last); } out[i] = %@(lo, hi, s, tab, sites, last); }\n",
           (unsigned long long)(GB & 0xffffffff) + 8, h, h];
    NSError *err = nil; MTLCompileOptions *o = [MTLCompileOptions new];
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:o error:&err];
    if (!lib) { fprintf(stderr, "compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"gptr_test"] error:&err];
    if (!ps) { fprintf(stderr, "pipeline failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLBuffer> A = [dev newBufferWithLength:PSZ options:MTLResourceStorageModeShared], B = [dev newBufferWithLength:PSZ options:MTLResourceStorageModeShared], C = [dev newBufferWithLength:PSZ options:MTLResourceStorageModeShared];
    for (uint32_t i = 0; i < PSZ / 4; i++) { ((uint32_t *)A.contents)[i] = 0xA0000000u + i; ((uint32_t *)B.contents)[i] = 0xB0000000u + i; ((uint32_t *)C.contents)[i] = 0xC0000000u + i; }
    id<MTLBuffer> tab = [dev newBufferWithLength:(size_t)ENTRIES * EW * 4 options:MTLResourceStorageModeShared];
    uint32_t *t = tab.contents; memset(t, 0, tab.length); for (int k = 1; k < ENTRIES; k++) { t[k * EW] = 0xffffffffu; t[k * EW + 1] = 0xffffffffu; }   // entry 0 empty, unused start at ~0
    uint32_t count = ENTRIES; memcpy(t + 4, &count, 4);
    entry(t, 1, GA, GA + PSZ, A.gpuAddress - GA); entry(t, 2, GB, GB + PSZ, B.gpuAddress - GB);
    id<MTLBuffer> sites = [dev newBufferWithLength:4 * EW * 4 options:MTLResourceStorageModeShared];
    uint32_t *s = sites.contents; memset(s, 0, sites.length);
    entry(s, 0, GA, GA + PSZ, C.gpuAddress - GA);   // site 0: A's range, C's delta (proves the site path)
    /* site 1 stays empty: start 0, end 0 */
    entry(s, 2, GB, GB + PSZ, B.gpuAddress - GB);   // site 2: B, right
    entry(s, 3, GB, GB + PSZ, A.gpuAddress - GB);   // site 3: B's range with a delta that maps B's base onto A's buffer (checks the site's own delta is used, in bounds)
    struct { const char *what; uint64_t addr; int site, warm; int64_t want; } T[] = {
        {"site 0 holds the address: site path",                   GA + 16,      0, 0, 0xC0000004},
        {"site 0, address in pool B: table path",                 GB + 16,      0, 0, 0xB0000004},
        {"site 1 empty, address in A: table path",                GA + 16,      1, 0, 0xA0000004},
        {"no site (-1), address in A: table path",                GA + 16,     -1, 0, 0xA0000004},
        {"site 2 = B, address in B: site path",                   GB + 32,      2, 0, 0xB0000008},
        {"site 0, last word of A's range",                        GA + PSZ - 4, 0, 0, (int64_t)(0xC0000000u + PSZ / 4 - 1)},
        {"site 0, first byte past A: table, in no pool -> 0",     GA + PSZ,     0, 0, 0},
        {"no site, address in no pool -> 0",                      0x500000000ull, -1, 0, 0},
        {"no site, address in A after a B lookup (last entry)",   GA + 40,     -1, 1, 0xA000000A},
        {"site 0, address in A after a B lookup (last entry)",    GA + 40,      0, 1, 0xC000000A},
        {"site 3's own delta is used (B range, delta to A's base)", GB + 12,    3, 0, 0xA0000003},
    };
    int n = sizeof T / sizeof T[0];
    id<MTLBuffer> cases = [dev newBufferWithLength:(size_t)n * 16 options:MTLResourceStorageModeShared], out = [dev newBufferWithLength:(size_t)n * 4 options:MTLResourceStorageModeShared];
    for (int i = 0; i < n; i++) { int32_t *c = (int32_t *)cases.contents + 4 * i; c[0] = (int32_t)(uint32_t)T[i].addr; c[1] = (int32_t)(T[i].addr >> 32); c[2] = T[i].site; c[3] = T[i].warm; }
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps]; [e setBuffer:tab offset:0 atIndex:0]; [e setBuffer:sites offset:0 atIndex:1]; [e setBuffer:cases offset:0 atIndex:2]; [e setBuffer:out offset:0 atIndex:3];
    [e useResource:A usage:MTLResourceUsageRead]; [e useResource:B usage:MTLResourceUsageRead]; [e useResource:C usage:MTLResourceUsageRead];
    [e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr, "GPU error: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
    int bad = 0;
    for (int i = 0; i < n; i++) { uint32_t got = ((uint32_t *)out.contents)[i], want = (uint32_t)T[i].want; bad += got != want;
        printf("%s %-56s got %08x want %08x\n", got == want ? "PASS" : "FAIL", T[i].what, got, want); }
    printf("== %d cases, %d failed (device %s)\n", n, bad, dev.name.UTF8String);
    return bad != 0;
} }
