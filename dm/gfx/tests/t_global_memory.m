// Execute a translated synthetic compute shader that reaches one 4 KB buffer through global addresses (test_global_memory.py), on one thread, and print the buffer's
// 1,024 words in hex for the caller to check. The words start as 0xabcd0000 + index; c0 holds the given words.
// Usage: t_global_memory <metallib> [c0=<member>] [s=<member>,...] [sblen=<member>[:<slot>,...]] [map=<member> log=<member> guest=<address>] [<c0 byte offset>=<word>]... [w<index>=<word>]...
//   s: the storage buffer members of the c0 descriptor (the translator may give one descriptor several: a constant offset folded, a runtime one range-checked), each
//   bound to the buffer. sblen: the storage-buffer length table (dmsh_abi.h DMSH_SBLEN_BANK), giving {0, 1024} words for each bounded storage slot (nvn_mtl.m sblen_put).
//   map / log: the memory pool table and its write log (dmsh_abi.h DMSH_GMEM_MAP_*, DMSH_GMEM_LOG_*), for addresses the translator traced to no
//   storage buffer: the table maps the buffer at guest address <address> in entry 1, as the host's (nvn_mtl.m gmem_map) does a pool
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn/dmsh_abi.h"

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc >= 2);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); assert(dev);
    NSError *error = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error]; assert(lib && !error);
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"]; assert(fn);
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&error]; assert(pso && !error);
    id<MTLBuffer> data = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> c0 = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    id<MTLBuffer> map = [dev newBufferWithLength:DMSH_GMEM_MAP_ENTRIES * DMSH_GMEM_MAP_ENTRY options:MTLResourceStorageModeShared];
    id<MTLBuffer> log = [dev newBufferWithLength:DMSH_GMEM_MAP_ENTRIES / 8 options:MTLResourceStorageModeShared];
    id<MTLBuffer> sblen = [dev newBufferWithLength:DMSH_SBLEN_SLOTS * 8 options:MTLResourceStorageModeShared];
    uint32_t *words = data.contents, *cb = c0.contents;
    for (int i = 0; i < 1024; i++) words[i] = 0xabcd0000u + (uint32_t)i;
    memset(log.contents, 0, log.length);
    memset(sblen.contents, 0, sblen.length);
    id<MTLArgumentEncoder> args = [fn newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> ab = [dev newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
    [args setArgumentBuffer:ab offset:0];
    for (int a = 2; a < argc; a++) {
        const char *s = argv[a], *eq = strchr(s, '='); assert(eq); char *end;
        if (!strncmp(s, "c0=", 3)) [args setBuffer:c0 offset:0 atIndex:strtoul(eq + 1, NULL, 10)];
        else if (!strncmp(s, "s=", 2)) for (s = eq + 1; *s; s = *end == ',' ? end + 1 : end) { [args setBuffer:data offset:0 atIndex:strtoul(s, &end, 10)]; assert(end > s); }
        else if (!strncmp(s, "sblen=", 6)) {
            [args setBuffer:sblen offset:0 atIndex:strtoul(eq + 1, &end, 10)]; assert(end > eq + 1);
            if (*end == ':') for (s = end + 1; *s; s = *end == ',' ? end + 1 : end) {
                unsigned long slot = strtoul(s, &end, 10); assert(end > s && (!*end || *end == ',') && slot < (unsigned long)DMSH_SBLEN_SLOTS);
                ((uint32_t *)sblen.contents)[2 * slot] = 0; ((uint32_t *)sblen.contents)[2 * slot + 1] = (uint32_t)(data.length / 4);
            } else assert(!*end);
        }
        else if (!strncmp(s, "map=", 4)) [args setBuffer:map offset:0 atIndex:strtoul(eq + 1, NULL, 10)];
        else if (!strncmp(s, "log=", 4)) [args setBuffer:log offset:0 atIndex:strtoul(eq + 1, NULL, 10)];
        else if (!strncmp(s, "guest=", 6)) {   // entry 0 empty, entry 1 the buffer, the others unused (start ~0); no hints, so lookups search the table
            uint64_t guest = strtoull(eq + 1, NULL, 0), *e = map.contents;
            memset(e, 0, map.length);
            for (int i = 2; i < DMSH_GMEM_MAP_ENTRIES; i++) e[i * 4] = ~0ull;
            e[4] = guest; e[5] = guest + data.length; e[6] = data.gpuAddress - guest;
        }
        else if (*s == 'w') { long i = strtol(s + 1, &end, 0); assert(end == eq && i >= 0 && i < 1024); words[i] = (uint32_t)strtoul(eq + 1, NULL, 0); }
        else { long o = strtol(s, &end, 0); assert(end == eq && o >= 0 && o < 65536 && !(o & 3)); cb[o / 4] = (uint32_t)strtoul(eq + 1, NULL, 0); }
    }
    id<MTLCommandQueue> queue = [dev newCommandQueue]; id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [command computeCommandEncoder];
    [enc setComputePipelineState:pso]; [enc setBuffer:ab offset:0 atIndex:0];
    [enc useResource:data usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc useResource:c0 usage:MTLResourceUsageRead];
    [enc useResource:map usage:MTLResourceUsageRead];
    [enc useResource:log usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc useResource:sblen usage:MTLResourceUsageRead];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding]; [command commit]; [command waitUntilCompleted];
    assert(command.status == MTLCommandBufferStatusCompleted);
    for (int i = 0; i < 1024; i++) printf("%08x%c", words[i], i % 16 == 15 ? '\n' : ' ');
    return 0;
}}
