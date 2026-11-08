// Synthetic GPTR quad loads: CPU oracle for every output word, scalar cache parity,
// boundary and seeded in-pool addresses. No game-derived shader inputs.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

enum { ENTRIES = 16384, EW = 8 };   // dmsh_abi.h DMSH_GMEM_MAP_ENTRIES on main, words per entry
static const uint64_t GA = 0x100000000ull, GB = 0x100100000ull, PSZ = 65536, GD = 0x123400000000ull;   // guest bases of pools A and B, their size

static void entry(uint32_t *t, int k, uint64_t lo, uint64_t hi, uint64_t delta) {
    uint32_t *e = t + (size_t)k * EW; e[0] = (uint32_t)lo; e[1] = (uint32_t)(lo >> 32); e[2] = (uint32_t)hi; e[3] = (uint32_t)(hi >> 32); e[4] = (uint32_t)delta; e[5] = (uint32_t)(delta >> 32); e[6] = 0; e[7] = 0;
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 2) { fprintf(stderr, "usage: t_gptr_quads <synthetic-gpu.metallib>\n"); return 2; }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];
    if (!lib) { fprintf(stderr, "compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"gptr_test"] error:&err];
    if (!ps) { fprintf(stderr, "pipeline failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLBuffer> A = [dev newBufferWithLength:PSZ options:MTLResourceStorageModeShared], B = [dev newBufferWithLength:PSZ options:MTLResourceStorageModeShared], C = [dev newBufferWithLength:PSZ options:MTLResourceStorageModeShared];
    for (uint32_t i = 0; i < PSZ / 4; i++) { ((uint32_t *)A.contents)[i] = 0xA0000000u + i; ((uint32_t *)B.contents)[i] = 0xB0000000u + i; ((uint32_t *)C.contents)[i] = 0xC0000000u + i; }
    id<MTLBuffer> tab = [dev newBufferWithLength:(size_t)ENTRIES * EW * 4 options:MTLResourceStorageModeShared];
    uint32_t *t = tab.contents; memset(t, 0, tab.length); for (int k = 1; k < ENTRIES; k++) { t[k * EW] = 0xffffffffu; t[k * EW + 1] = 0xffffffffu; }   // entry 0 empty, unused start at ~0
    uint32_t count = ENTRIES; memcpy(t + 4, &count, 4);
    entry(t, 1, GA, GA + PSZ, A.gpuAddress - GA); entry(t, 2, GB, GB + PSZ, B.gpuAddress - GB);
    // Preserve sorted disjoint ranges while placing a real pool beyond 8192.
    for (int k = 3; k < 9000; k++) { uint64_t v = GB + PSZ + (uint64_t)k * 16; entry(t, k, v, v, 0); }
    entry(t, 9000, GD, GD + PSZ, C.gpuAddress - GD);
    uint32_t tag = (uint32_t)(GD >> 21), hs = tag & 4095u;
    t[hs * EW + 6] = tag; t[hs * EW + 7] = 9000; // full 32-bit tag and separate index, matching this host
    // Wrong-tag hint for A must be ignored in favor of the dynamic search.
    uint32_t as = (uint32_t)(GA >> 21) & 4095u;
    t[as * EW + 6] = 0xdeadbeefu; t[as * EW + 7] = 9000;
    id<MTLBuffer> sites = [dev newBufferWithLength:6 * EW * 4 options:MTLResourceStorageModeShared];
    uint32_t *s = sites.contents; memset(s, 0, sites.length);
    entry(s, 0, GA, GA + PSZ, C.gpuAddress - GA);   // site 0: A's range, C's delta (proves the site path)
    /* site 1 stays empty: start 0, end 0 */
    entry(s, 2, GB, GB + PSZ, B.gpuAddress - GB);   // site 2: B, right
    entry(s, 3, GB, GB + PSZ, A.gpuAddress - GB);   // site 3: B's range with a delta that maps B's base onto A's buffer (checks the site's own delta is used, in bounds)
    entry(s, 4, GA, GA+8, C.gpuAddress-GA);
    entry(s, 5, GA+4, GA+64, C.gpuAddress-GA);
    struct { const char *what; uint64_t addr; int site, warm; int64_t want; } T[20000] = {
        {"32-bit low-word carry into A", GA-4, 0, 0, 0},
        {"64-bit address wrap", UINT64_MAX-3, 0, 0, 0},
        {"last complete quad", GA+PSZ-16, 0, 0, 0xC0003FFC},
        {"quad with one out-of-range word", GA+PSZ-12, 0, 0, 0xC0003FFD},
        {"quad with two out-of-range words", GA+PSZ-8, 0, 0, 0xC0003FFE},
        {"site ends halfway through quad", GA, 4, 0, 0xC0000000},
        {"site begins inside quad", GA, 5, 0, 0xA0000000},
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
        {"full-tag hint reaches pool at entry 9000", GD + 16, -1, 0, 0xC0000004},
        {"wrong site falls back to high-address pool", GD + 20, 0, 0, 0xC0000005},
        {"high-address pool upper boundary is excluded", GD + PSZ, -1, 0, 0},
    };
    int n = 21;
    const uint64_t boundaries[] = {0, GA, GA+4, GA+8, GA+64, GA+PSZ, GB, GB+PSZ, GD, GD+PSZ};
    uint32_t seed=0x51a7c0de;
    for (int site=-1;site<6;site++) for(int warm=0;warm<2;warm++) {
      for(unsigned b=0;b<sizeof boundaries/sizeof boundaries[0];b++) for(int off=-32;off<=32;off+=4) {
        T[n].what="boundary sweep";T[n].addr=boundaries[b]+(uint64_t)(int64_t)off;T[n].site=site;T[n].warm=warm;n++;
      }
      for(int j=0;j<512;j++) {
        seed=seed*1664525u+1013904223u;
        uint64_t base=j%3==0?GA:j%3==1?GB:GD;
        T[n].what="seeded pool sweep";T[n].addr=base+(seed%(PSZ/4))*4;T[n].site=site;T[n].warm=warm;n++;
      }
    }
    if(n>20000)return 3;
    id<MTLBuffer> cases = [dev newBufferWithLength:(size_t)n * 16 options:MTLResourceStorageModeShared], out = [dev newBufferWithLength:(size_t)n * 40 options:MTLResourceStorageModeShared];
    for (int i = 0; i < n; i++) { int32_t *c = (int32_t *)cases.contents + 4 * i; c[0] = (int32_t)(uint32_t)T[i].addr; c[1] = (int32_t)(T[i].addr >> 32); c[2] = T[i].site; c[3] = T[i].warm; }
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps]; [e setBuffer:tab offset:0 atIndex:0]; [e setBuffer:sites offset:0 atIndex:1]; [e setBuffer:cases offset:0 atIndex:2]; [e setBuffer:out offset:0 atIndex:3];
    [e useResource:A usage:MTLResourceUsageRead]; [e useResource:B usage:MTLResourceUsageRead]; [e useResource:C usage:MTLResourceUsageRead];
    [e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr, "GPU error: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
    int bad = 0;
    for(int i=0;i<n;i++) for(int lane=0;lane<4;lane++) {
      uint64_t addr=T[i].addr+4ull*lane;uint32_t want=0;
      int site=T[i].site;uint64_t lo=0,hi=0,base=0;uint32_t tag=0;
      if(site==0){lo=GA;hi=GA+PSZ;base=GA;tag=0xC0000000u;}
      if(site==2){lo=GB;hi=GB+PSZ;base=GB;tag=0xB0000000u;}
      if(site==3){lo=GB;hi=GB+PSZ;base=GB;tag=0xA0000000u;}
      if(site==4){lo=GA;hi=GA+8;base=GA;tag=0xC0000000u;}
      if(site==5){lo=GA+4;hi=GA+64;base=GA;tag=0xC0000000u;}
      if(addr>=lo && addr<hi) want=tag+(uint32_t)((addr-base)/4);
      else if(addr>=GA && addr<GA+PSZ)want=0xA0000000u+(uint32_t)((addr-GA)/4);
      else if(addr>=GB && addr<GB+PSZ)want=0xB0000000u+(uint32_t)((addr-GB)/4);
      else if(addr>=GD && addr<GD+PSZ)want=0xC0000000u+(uint32_t)((addr-GD)/4);
      uint32_t *r=(uint32_t*)out.contents+i*10;
      if(r[lane]!=want || r[4+lane]!=want){bad++;if(bad<20)printf("oracle mismatch case %d lane %d address %llx site %d got %x/%x want %x\n",i,lane,addr,site,r[lane],r[4+lane],want);}
    }
    for(int i=0;i<n;i++){ uint32_t *r=(uint32_t*)out.contents+i*10;for(int k=0;k<4;k++) {if(r[k]!=r[4+k]) {bad++;printf("quad mismatch case %d word %d\n",i,k);}} if(r[8]!=r[9]){bad++;printf("cache mismatch case %d\n",i);} }
    printf("== %d cases, %d failed (device %s)\n", n, bad, dev.name.UTF8String);
    return bad != 0;
} }
