// Threads that return before a threadgroup_barrier: Metal's specification wants every thread of the threadgroup at each barrier, NVIDIA's BAR counts an exited thread
// as arrived, and Switch compute shaders rely on that ("if (lid * 4 >= count) return;" before a barrier: 156 of PRAGMATA's). The translator emits the same shape in MSL.
// This test runs those shapes on the Mac's GPU and checks that every dispatch completes and every thread that stays sees the others' writes: whole simdgroups returning,
// all but the last simdgroup returning, half the lanes of every simdgroup, threads returning between two barriers, a barrier loop after the returns, threads leaving a
// barrier loop at different iterations, and half the lanes before a simdgroup_barrier. A GPU that waited for the returned threads would hang the command buffer (Metal
// ends it with kIOGPUCommandBufferCallbackErrorHang), which is a failure here. Passes on an Apple M6 (docs/reference/shaders.md, "Compute threads that return before a barrier").
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static const char *src =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define ARGS device uint *out [[buffer(0)]], constant uint &count [[buffer(1)]], uint lid [[thread_position_in_threadgroup]], uint tg [[threadgroup_position_in_grid]]\n"
    "#define OUT out[tg * 1024 + lid]\n"
    // simdgroup 0 stays, the other 31 return whole
    "kernel void whole(ARGS) {\n"
    "    threadgroup uint s[1024];\n"
    "    if (lid >= count) return;\n"
    "    s[lid] = lid;\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    OUT = s[(lid + 1) % count];\n"
    "}\n"
    // only the last simdgroup stays
    "kernel void last(ARGS) {\n"
    "    threadgroup uint s[1024];\n"
    "    if (lid < 1024 - count) return;\n"
    "    s[lid] = lid;\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    OUT = s[1024 - count + (lid + 1) % count];\n"
    "}\n"
    // the odd lanes of every simdgroup return
    "kernel void lanes(ARGS) {\n"
    "    threadgroup uint s[1024];\n"
    "    if ((lid & 1) != 0 && count != 0) return;\n"
    "    s[lid] = lid;\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    OUT = s[(lid + 2) % 1024];\n"
    "}\n"
    // half the threads return before the first barrier, all but simdgroup 0 between the two
    "kernel void staged(ARGS) {\n"
    "    threadgroup uint s[1024], t[1024];\n"
    "    if (lid >= 512) return;\n"
    "    s[lid] = lid;\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    if (lid >= count) return;\n"
    "    t[lid] = s[lid] + s[(lid + 1) % count];\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    OUT = t[(lid + 1) % count];\n"
    "}\n"
    // simdgroup 0 stays and runs count / 2 iterations of a loop with two barriers (trip count from the buffer)
    "kernel void loop(ARGS) {\n"
    "    threadgroup uint s[1024];\n"
    "    if (lid >= count) return;\n"
    "    s[lid] = 0;\n"
    "    for (uint i = 0; i < count / 2; i++) {\n"
    "        threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "        uint v = s[(lid + 1) % count];\n"
    "        threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "        s[lid] = v + 1;\n"
    "    }\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    OUT = s[(lid + 1) % count];\n"
    "}\n"
    // simdgroup k returns inside a barrier loop at iteration k - 1; simdgroup 0 runs all count iterations
    "kernel void loopexit(ARGS) {\n"
    "    threadgroup uint s[1024];\n"
    "    s[lid] = 1;\n"
    "    for (uint i = 0; i < count; i++) {\n"
    "        threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "        if ((lid >> 5) == i + 1) return;\n"
    "        s[lid] += 1;\n"
    "    }\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    OUT = s[(lid + 1) % 32];\n"
    "}\n"
    // the odd lanes return before a simdgroup barrier
    "kernel void simdbar(ARGS) {\n"
    "    threadgroup uint s[1024];\n"
    "    if ((lid & 1) != 0 && count != 0) return;\n"
    "    s[lid] = lid;\n"
    "    simdgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    OUT = s[lid ^ 2];\n"
    "}\n";

enum { GROUPS = 4096, THREADS = 1024, COUNT = 32, REPEAT = 5, UNSET = 0xffffffffu };

// The value thread lid of every threadgroup writes, or UNSET for a thread that returned
static uint32_t expect(const char *name, uint32_t lid) {
    if (!strcmp(name, "whole")) return lid < COUNT ? (lid + 1) % COUNT : UNSET;
    if (!strcmp(name, "last")) return lid >= THREADS - COUNT ? THREADS - COUNT + (lid + 1) % COUNT : UNSET;
    if (!strcmp(name, "lanes")) return lid & 1 ? UNSET : (lid + 2) % THREADS;
    if (!strcmp(name, "staged")) return lid < COUNT ? (lid + 1) % COUNT + (lid + 2) % COUNT : UNSET;
    if (!strcmp(name, "loop")) return lid < COUNT ? COUNT / 2 : UNSET;
    if (!strcmp(name, "loopexit")) return lid < 32 ? 1 + COUNT : UNSET;
    if (!strcmp(name, "simdbar")) return lid & 1 ? UNSET : lid ^ 2;
    return 0;
}

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("FAIL: no Metal device\n"); return 1; }
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:nil error:&err];
    if (!lib) { printf("FAIL: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> out = [dev newBufferWithLength:GROUPS * THREADS * 4 options:MTLResourceStorageModeShared];
    uint32_t count = COUNT;
    int fails = 0;
    const char *names[] = {"whole", "last", "lanes", "staged", "loop", "loopexit", "simdbar"};
    for (size_t k = 0; k < sizeof names / sizeof *names; k++) {
        id<MTLFunction> fn = [lib newFunctionWithName:@(names[k])];
        id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
        if (!ps || ps.maxTotalThreadsPerThreadgroup < THREADS) { printf("FAIL: %s: no 1024-thread pipeline\n", names[k]); fails++; continue; }
        int bad = 0;
        for (int r = 0; r < REPEAT && !bad; r++) {
            memset(out.contents, 0xff, GROUPS * THREADS * 4);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:ps];
            [e setBuffer:out offset:0 atIndex:0];
            [e setBytes:&count length:4 atIndex:1];
            [e dispatchThreadgroups:MTLSizeMake(GROUPS, 1, 1) threadsPerThreadgroup:MTLSizeMake(THREADS, 1, 1)];
            [e endEncoding];
            [cb commit];
            [cb waitUntilCompleted];   // a GPU that waited at the barrier for the returned threads ends here with Metal's hang error
            if (cb.status != MTLCommandBufferStatusCompleted) {
                printf("FAIL: %s: command buffer status %d: %s\n", names[k], (int)cb.status, cb.error.localizedDescription.UTF8String);
                bad = 1; break;
            }
            const uint32_t *o = out.contents;
            for (uint32_t i = 0; i < GROUPS * THREADS && !bad; i++)
                if (o[i] != expect(names[k], i % THREADS)) {
                    printf("FAIL: %s: threadgroup %u thread %u wrote %u, expected %u\n", names[k], i / THREADS, i % THREADS, o[i], expect(names[k], i % THREADS));
                    bad = 1;
                }
        }
        fails += bad;
    }
    printf("%s: %d shapes of threads returning before a barrier, %d x %d threads x %d dispatches each, %d failed\n",
           dev.name.UTF8String, (int)(sizeof names / sizeof *names), GROUPS, THREADS, REPEAT, fails);
    return fails != 0;
} }
