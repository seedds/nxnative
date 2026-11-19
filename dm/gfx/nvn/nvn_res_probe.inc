// What an Apple GPU does when a shader reads or writes through a pointer to a buffer outside the residency set (docs/reference/memory.md, "A read through a pointer to a buffer outside
// the residency set returns zeros"). The raw pool table (nvn_mtl.m gmem_map) hands shaders the GPU addresses of memory pools, which they reach through pointers loaded from memory, out
// of Metal's sight; whether a pool could stay out of the residency set until a shader needs it depends on what such an access does. Each case: a no-copy shared buffer over page-aligned
// memory (as a pool wraps the game's pages), one word per 4 KiB page filled, its GPU address in a resident argument buffer; a kernel copies the words through that pointer into a
// resident output buffer (or writes through it). A Metal 4 queue with the residency set on the queue, as the host has it. Its own queue, set and allocator: it runs before the game, or
// alone (dm/gfx/tests/t_res_absent.m; the host with DM_RES_PROBE=1, mtl_init). say() gets one line per case; returns 0 when a resident control went wrong
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
static const char *const kResProbeSrc =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct P { device uint *p; };\n"
    "kernel void rd(constant P &a [[buffer(0)]], device uint *out [[buffer(1)]], constant uint &n [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
    "    if (i < n) out[i] = a.p[i * 1024u];\n"
    "}\n"
    "kernel void wr(constant P &a [[buffer(0)]], device uint *out [[buffer(1)]], constant uint &n [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
    "    if (i < n) { a.p[i * 1024u] = 0xC0DE0000u | i; out[i] = 1u; }\n"
    "}\n";
enum { RP_WORDS = 4096 };   // pages touched per case: 16 MiB of buffer
typedef struct { id<MTLDevice> dev; id<MTL4CommandQueue> q; id<MTLResidencySet> res; id<MTL4CommandAllocator> alloc; id<MTL4ArgumentTable> at; id<MTLComputePipelineState> rd, wr; } rp_ctx;
typedef struct { int faulted; char err[400]; } rp_run;
typedef enum { RP_ANON, RP_FILE, RP_FILE_CACHE } rp_mem;
static uint32_t rp_pat(uint32_t i) { return 0xA5000000u | i; }
static rp_run rp_dispatch(rp_ctx *c, id<MTLComputePipelineState> pso, id<MTLBuffer> target, id<MTLBuffer> out) {
    rp_run r = {0};
    id<MTLBuffer> args = [c->dev newBufferWithLength:16 options:MTLResourceStorageModeShared], nb = [c->dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    *(uint64_t *)args.contents = target.gpuAddress; *(uint32_t *)nb.contents = RP_WORDS;
    memset(out.contents, 0xEE, out.length);
    [c->res addAllocation:args]; [c->res addAllocation:nb]; [c->res addAllocation:out]; [c->res commit];
    [c->alloc reset];
    id<MTL4CommandBuffer> cb = [c->dev newCommandBuffer];
    [cb beginCommandBufferWithAllocator:c->alloc];
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [c->at setAddress:args.gpuAddress atIndex:0]; [c->at setAddress:out.gpuAddress atIndex:1]; [c->at setAddress:nb.gpuAddress atIndex:2];
    [e setArgumentTable:c->at]; [e setComputePipelineState:pso];
    [e dispatchThreads:MTLSizeMake(RP_WORDS, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [e endEncoding]; [cb endCommandBuffer];
    dispatch_semaphore_t done = dispatch_semaphore_create(0); rp_run *rp = &r;
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        if (f.error) { NSError *u = f.error.userInfo[NSUnderlyingErrorKey]; rp->faulted = 1;
            snprintf(rp->err, sizeof rp->err, "%s%s%s", f.error.description.UTF8String, u ? " | underlying: " : "", u ? u.description.UTF8String : ""); }
        dispatch_semaphore_signal(done);
    }];
    const id<MTL4CommandBuffer> cbs[1] = {cb};
    [c->q commit:cbs count:1 options:o];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) { r.faulted = 2; snprintf(r.err, sizeof r.err, "no feedback within 10 s"); }
    [c->res removeAllocation:args]; [c->res removeAllocation:nb]; [c->res removeAllocation:out]; [c->res commit];
    return r;
}
static void *rp_mem_new(rp_mem k, size_t n, int *fd_out) {
    *fd_out = -1;
    if (k == RP_ANON) { void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); return p == MAP_FAILED ? NULL : p; }
    char path[1024]; snprintf(path, sizeof path, "%s/dm_res_probe.XXXXXX", NSTemporaryDirectory().UTF8String);   // 16 MiB, removed at once
    int fd = mkstemp(path); if (fd < 0) return NULL;
    unlink(path);
    if (ftruncate(fd, (off_t)n)) { close(fd); return NULL; }
    if (k == RP_FILE_CACHE) for (uint32_t i = 0; i < RP_WORDS; i++) { uint32_t v = rp_pat(i); if (pwrite(fd, &v, 4, (off_t)i * 4096) != 4) { close(fd); return NULL; } }   // in the page cache only
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);   // the game's memory is a shared file mapping too where it goes into files (iOS)
    if (p == MAP_FAILED) { close(fd); return NULL; }
    *fd_out = fd; return p;
}
// history: 0 never resident; 1 resident for a commit that read it, then removed; 2 resident (a control); 3 never resident, another buffer over the same pages resident;
// 4 as 1, then 32 other commits; 5 as 1, then 2 s; 6 read while not resident, then made resident and read again
static int rp_case(rp_ctx *c, const char *name, rp_mem k, int history, int write, void (*say)(const char *)) {
    char line[700]; size_t n = (size_t)RP_WORDS * 4096; int fd; void *p = rp_mem_new(k, n, &fd);
    if (!p) { snprintf(line, sizeof line, "%-34s could not map memory", name); say(line); return 1; }
    if (k != RP_FILE_CACHE) for (uint32_t i = 0; i < RP_WORDS; i++) ((uint32_t *)p)[i * 1024u] = rp_pat(i);
    id<MTLBuffer> b = [c->dev newBufferWithBytesNoCopy:p length:n options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLBuffer> out = [c->dev newBufferWithLength:RP_WORDS * 4 options:MTLResourceStorageModeShared], twin = nil;
    char pre[160] = "";
    if (history == 1 || history == 4 || history == 5) {
        [c->res addAllocation:b]; [c->res commit]; rp_dispatch(c, c->rd, b, out); [c->res removeAllocation:b]; [c->res commit];
        if (history == 4) { id<MTLBuffer> o2 = [c->dev newBufferWithLength:n options:MTLResourceStorageModeShared]; [c->res addAllocation:o2]; [c->res commit]; for (int j = 0; j < 32; j++) rp_dispatch(c, c->rd, o2, out); [c->res removeAllocation:o2]; [c->res commit]; }
        if (history == 5) sleep(2);
    } else if (history == 2) { [c->res addAllocation:b]; [c->res commit]; }
    else if (history == 3) { twin = [c->dev newBufferWithBytesNoCopy:p length:n options:MTLResourceStorageModeShared deallocator:nil]; [c->res addAllocation:twin]; [c->res commit]; }
    else if (history == 6) {
        rp_run cold = rp_dispatch(c, c->rd, b, out); int got = 0; for (uint32_t i = 0; i < RP_WORDS; i++) got += ((uint32_t *)out.contents)[i] == rp_pat(i);
        snprintf(pre, sizeof pre, " (before it was resident: %s, data %d)", cold.faulted ? "FAULT" : "ok", got);
        [c->res addAllocation:b]; [c->res commit];
    }
    rp_run r = rp_dispatch(c, write ? c->wr : c->rd, b, out);
    int good = 0, zero = 0, other = 0, untouched = 0;
    for (uint32_t i = 0; i < RP_WORDS; i++) {
        uint32_t v = ((uint32_t *)out.contents)[i];
        if (write) { uint32_t m = ((uint32_t *)p)[i * 1024u]; if (m == (0xC0DE0000u | i)) good++; else if (m == 0) zero++; else other++; }
        else if (v == 0xEEEEEEEEu) untouched++; else if (v == rp_pat(i)) good++; else if (v == 0) zero++; else other++;
    }
    snprintf(line, sizeof line, "%-34s %-5s %s %d/%d, zero %d, other %d, output unwritten %d%s%s%s", name, r.faulted == 1 ? "FAULT" : r.faulted == 2 ? "HANG" : "ok",
             write ? "landed" : "data", good, RP_WORDS, zero, other, untouched, pre, r.faulted ? " | error: " : "", r.faulted ? r.err : "");
    say(line);
    if (history == 2 || history == 6) [c->res removeAllocation:b];
    if (twin) [c->res removeAllocation:twin];
    [c->res commit];
    b = nil; twin = nil; out = nil; munmap(p, n); if (fd >= 0) close(fd);
    return history != 2 || (!r.faulted && good == RP_WORDS);
}
static int res_probe(id<MTLDevice> dev, int rounds, void (*say)(const char *)) { @autoreleasepool {
    char line[300];
    if (!dev || ![dev supportsFamily:MTLGPUFamilyMetal4]) { say("no Metal 4 device: skipped"); return 1; }
    rp_ctx c = {.dev = dev}; NSError *err = nil;
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new]; rd.initialCapacity = 64;
    c.res = [dev newResidencySetWithDescriptor:rd error:&err];
    MTL4CommandQueueDescriptor *qd = [MTL4CommandQueueDescriptor new]; static dispatch_queue_t fq; if (!fq) fq = dispatch_queue_create("dm.res-probe.feedback", DISPATCH_QUEUE_SERIAL);   // kept for good, as the host keeps its own (g_feedback_q): freed with the probe's queue it was released once too often
    qd.feedbackQueue = fq;
    c.q = [dev newMTL4CommandQueueWithDescriptor:qd error:&err];
    if (c.q && c.res) [c.q addResidencySet:c.res];
    c.alloc = [dev newCommandAllocator];
    MTL4ArgumentTableDescriptor *atd = [MTL4ArgumentTableDescriptor new]; atd.maxBufferBindCount = 4;
    c.at = [dev newArgumentTableWithDescriptor:atd error:&err];
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kResProbeSrc) options:nil error:&err];
    if (lib) { c.rd = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rd"] error:&err]; c.wr = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"wr"] error:&err]; }
    if (!c.q || !c.res || !c.alloc || !c.at || !c.rd || !c.wr) { snprintf(line, sizeof line, "could not set up Metal 4: %s", err.localizedDescription.UTF8String); say(line); return 0; }
    snprintf(line, sizeof line, "%s, working set %llu MB, %d pages touched per case", dev.name.UTF8String, (unsigned long long)(dev.recommendedMaxWorkingSetSize >> 20), RP_WORDS); say(line);
    int ok = 1;
    for (int round = 0; round < rounds; round++) {
        snprintf(line, sizeof line, "round %d", round + 1); say(line);
        ok &= rp_case(&c, "control: resident, read", RP_ANON, 2, 0, say);
        ok &= rp_case(&c, "anon, never resident, read", RP_ANON, 0, 0, say);
        ok &= rp_case(&c, "anon, evicted, read", RP_ANON, 1, 0, say);
        ok &= rp_case(&c, "anon, twin resident, read", RP_ANON, 3, 0, say);
        ok &= rp_case(&c, "anon, evicted + 32 commits, read", RP_ANON, 4, 0, say);
        ok &= rp_case(&c, "anon, evicted + 2 s, read", RP_ANON, 5, 0, say);
        ok &= rp_case(&c, "anon, made resident late, read", RP_ANON, 6, 0, say);
        ok &= rp_case(&c, "file, never resident, read", RP_FILE, 0, 0, say);
        ok &= rp_case(&c, "file via pwrite, never res., read", RP_FILE_CACHE, 0, 0, say);
        ok &= rp_case(&c, "anon, never resident, write", RP_ANON, 0, 1, say);
        ok &= rp_case(&c, "file, never resident, write", RP_FILE, 0, 1, say);
        ok &= rp_case(&c, "control: resident, write", RP_ANON, 2, 1, say);
    }
    ok &= rp_case(&c, "control after the cases", RP_ANON, 2, 0, say);   // does the queue still work (a fault may revoke the process's GPU access)
    [c.q removeResidencySet:c.res];   // Metal traps when a set still attached to a queue is freed
    say(ok ? "done: controls correct" : "FAIL: a resident control went wrong");
    return ok;
} }
