// What macOS records for each kind of bad GPU read (issue #1289): each case runs in a child process of its own, as macOS stops taking a process's command buffers after a
// few faults, and the parent prints the case's Metal error with the fields of the `gpuEvent-<process>-*.ips` report macOS writes for a GPU fault (address, read or
// write, sideband, requestor, page-table level, data master). A freed or evicted object faults at its own address; a binding whose address is 0 reads at 0 plus the
// offset. The M6 reads 0 with no error from a buffer at address 0, 64, a freed one and past a buffer's end, and faults at a freed texture's own address (data master
// 1 vertex, 2 fragment, 3 compute). Not in the run list (NOT_RUN): every case that faults restarts the GPU. Build: make -C dm/gfx/tests GAME=<id> t_gpu_fault_kinds;
// run: dm/gfx/tests/t_gpu_fault_kinds [case ...], or --report <process name> for that process's reports of the last 7 days (a game's: --report dmhost). Each case runs from a copy named gfkNN in a temporary folder: macOS names a report after the process, cut to 16
// characters, and wrote one report for two faults of the same name a few seconds apart
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

static const char *const CASES[][2] = {
    {"compute-load-0", "a compute shader loads through a buffer address of 0"},
    {"compute-load-64", "a compute shader loads through a buffer address of 64"},
    {"fragment-load-0", "a fragment shader loads through a buffer address of 0"},
    {"fragment-load-64", "a fragment shader loads through a buffer address of 64"},
    {"compute-load-freed", "a compute shader loads from a freed 64 MB buffer"},
    {"compute-load-past-end", "a compute shader loads 256 MB past a resident 16 KB buffer's end"},
    {"draw-index-0", "an indexed draw with an index buffer address of 0"},
    {"dispatch-indirect-0", "an indirect dispatch whose record's address is 0"},
    {"compute-texture-freed", "a compute shader reads a freed texture"},
    {"fragment-texture-freed", "a fragment shader reads a freed texture"},
};
enum { NCASE = sizeof CASES / sizeof CASES[0] };

static NSString *const SRC = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V vs(uint v [[vertex_id]]) { V r; r.p = float4(v == 1 ? 3.0 : -1.0, v == 2 ? 3.0 : -1.0, 0, 1); return r; }\n"
    "fragment float4 fs() { return float4(1); }\n"
    "fragment float4 fl(V in [[stage_in]], device const uint *p [[buffer(0)]]) { return float4(float(p[uint(in.p.x) * 16])); }\n"
    "fragment float4 ft(V in [[stage_in]], texture2d<float> t [[texture(0)]]) { return t.read(uint2(in.p.xy) * 16); }\n"
    "kernel void kl(device const uint *p [[buffer(0)]], device uint *o [[buffer(1)]], uint i [[thread_position_in_grid]]) { o[i] = p[i * 16]; }\n"
    "kernel void kt(texture2d<float> t [[texture(0)]], device uint *o [[buffer(1)]], uint2 i [[thread_position_in_grid]]) { o[i.y * 64 + i.x] = uint(t.read(i * 16).x * 255); }\n"
    "kernel void kw(device uint *o [[buffer(1)]], uint i [[thread_position_in_grid]]) { o[i] = i + 1; }\n";

// The child: one case on a Metal 4 queue, as the host runs. Prints one line: the commit's error (the command buffer's own, under the top-level one) and the first word read.
// Exits 10 when the command buffer failed, 0 when it did not
static int run_case(const char *c) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    if (![d supportsFamily:MTLGPUFamilyMetal4]) { printf("no Metal 4\n"); return 2; }
    id<MTLLibrary> lib = [d newLibraryWithSource:SRC options:nil error:&e]; if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 2; }
    id<MTLResidencySet> rs = [d newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&e]; if (!rs) { printf("residency set: %s\n", e.localizedDescription.UTF8String); return 2; }
    id<MTL4CommandQueue> q = [d newMTL4CommandQueue]; [q addResidencySet:rs];
    id<MTL4CommandAllocator> al = [d newCommandAllocator]; if (!q || !al) { printf("no Metal 4 queue or allocator\n"); return 2; }
    MTL4ArgumentTableDescriptor *atd = [MTL4ArgumentTableDescriptor new]; atd.maxBufferBindCount = 2; atd.maxTextureBindCount = 1;
    id<MTL4ArgumentTable> at = [d newArgumentTableWithDescriptor:atd error:&e]; if (!at) { printf("argument table: %s\n", e.localizedDescription.UTF8String); return 2; }
    id<MTLBuffer> out = [d newBufferWithLength:65536 * 4 options:MTLResourceStorageModeShared]; if (!out) { printf("no output buffer\n"); return 2; } [rs addAllocation:out];
    MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    rd.usage = MTLTextureUsageRenderTarget; rd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [d newTextureWithDescriptor:rd]; if (!rt) { printf("no render target\n"); return 2; } [rs addAllocation:rt];
    id<MTLBuffer> keep = [d newBufferWithLength:16384 options:MTLResourceStorageModeShared]; if (!keep) { printf("no buffer\n"); return 2; } [rs addAllocation:keep];
    [rs commit];
    int frag = !strncmp(c, "fragment-", 9) || !strcmp(c, "draw-index-0"), tex = strstr(c, "texture") != NULL;
    uint64_t addr = 0;
    if (!strcmp(c, "compute-load-64") || !strcmp(c, "fragment-load-64")) addr = 64;
    else if (!strcmp(c, "compute-load-past-end")) addr = keep.gpuAddress + (256ull << 20);
    else if (!strcmp(c, "compute-load-freed")) @autoreleasepool { id<MTLBuffer> b = [d newBufferWithLength:64 << 20 options:MTLResourceStorageModePrivate]; addr = b.gpuAddress; b = nil; }
    id<MTL4CommandBuffer> cb = [d newCommandBuffer]; if (!cb) { printf("no command buffer\n"); return 2; } [cb beginCommandBufferWithAllocator:al];
    @autoreleasepool {
        id<MTLTexture> t = nil;
        if (tex) { MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1024 height:1024 mipmapped:NO]; td.storageMode = MTLStorageModePrivate;
            t = [d newTextureWithDescriptor:td]; [at setTexture:t.gpuResourceID atIndex:0]; }
        else if (strcmp(c, "draw-index-0") && strcmp(c, "dispatch-indirect-0")) [at setAddress:addr atIndex:0];
        [at setAddress:out.gpuAddress atIndex:1];
        if (frag) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"];
            pd.fragmentFunction = [lib newFunctionWithName:tex ? @"ft" : !strcmp(c, "draw-index-0") ? @"fs" : @"fl"]; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithDescriptor:pd error:&e]; if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 2; }
            MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new]; rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTL4RenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:ps]; [re setArgumentTable:at atStages:MTLRenderStageFragment];
            if (!strcmp(c, "draw-index-0")) [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3000 indexType:MTLIndexTypeUInt32 indexBuffer:0 indexBufferLength:12000 instanceCount:1 baseVertex:0 baseInstance:0];
            else [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
        } else {
            id<MTLFunction> kf = [lib newFunctionWithName:tex ? @"kt" : !strcmp(c, "dispatch-indirect-0") ? @"kw" : @"kl"]; if (!kf) { printf("no kernel\n"); return 2; }
            id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:kf error:&e];
            if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 2; }
            id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce setArgumentTable:at]; [ce setComputePipelineState:ps];
            if (!strcmp(c, "dispatch-indirect-0")) [ce dispatchThreadgroupsWithIndirectBuffer:0 threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            else if (tex) [ce dispatchThreads:MTLSizeMake(64, 64, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            else [ce dispatchThreads:MTLSizeMake(4096, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding];
        }
        if (t) { [rs addAllocation:t]; [rs commit]; [rs removeAllocation:t]; [rs commit]; t = nil; }   // resident once, then out of the set and freed before the commit (Metal 4 holds no reference)
    }
    [cb endCommandBuffer];
    dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block NSError *fe = nil;
    MTL4CommitOptions *co = [MTL4CommitOptions new]; [co addFeedbackHandler:^(id<MTL4CommitFeedback> f) { fe = f.error; dispatch_semaphore_signal(sem); }];
    [q commit:&cb count:1 options:co];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) { printf("no feedback in 10 s\n"); return 3; }
    NSError *u = fe.userInfo[NSUnderlyingErrorKey];
    printf("%s; first word read %#x\n", u ? u.localizedDescription.UTF8String : fe ? fe.localizedDescription.UTF8String : "no error", ((const uint32_t *)out.contents)[0]);
    return fe ? 10 : 0;
} }

// One gpuEvent report as a line of its fault fields
static NSString *gpu_event_line(NSString *p) {
    NSString *s = [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil]; NSRange nl = s ? [s rangeOfString:@"\n"] : NSMakeRange(NSNotFound, 0);
    if (nl.location == NSNotFound) return [NSString stringWithFormat:@"unreadable (%@)", p.lastPathComponent];
    NSDictionary *j = [NSJSONSerialization JSONObjectWithData:[[s substringFromIndex:nl.location + 1] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    NSDictionary *a = [j isKindOfClass:[NSDictionary class]] ? j[@"analysis"] : nil, *f = a[@"bif0_fault"];
    if (!f) return [NSString stringWithFormat:@"%@ (%@)", a[@"restart_reason_desc"] ?: @"no analysis", p.lastPathComponent];
    return [NSString stringWithFormat:@"%@ at %#llx, %@, sideband %@, requestor %@, level %@, data master %@ (%@)", a[@"restart_reason_desc"], [f[@"address"] unsignedLongLongValue],
            [f[@"is_read"] boolValue] ? @"read" : @"write", f[@"sideband"], f[@"requestor"], f[@"level"], a[@"guilty_dm"], p.lastPathComponent];
}
// The gpuEvent reports of process `name` written after `since`, newest first
static NSArray<NSString *> *gpu_events(NSString *name, NSDate *since) {
    NSFileManager *fm = [NSFileManager defaultManager]; NSMutableArray *r = [NSMutableArray array]; NSString *pre = [NSString stringWithFormat:@"gpuEvent-%@-", name];
    for (NSString *dir in @[@"/Library/Logs/DiagnosticReports", [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/DiagnosticReports"]])
        for (NSString *f in [fm contentsOfDirectoryAtPath:dir error:nil]) {
            if (![f hasPrefix:pre]) continue;
            NSString *p = [dir stringByAppendingPathComponent:f]; NSDate *m = [fm attributesOfItemAtPath:p error:nil].fileModificationDate;
            if ([m compare:since] == NSOrderedDescending) [r addObject:@[m, p]]; }
    [r sortUsingComparator:^NSComparisonResult(NSArray *x, NSArray *y) { return [y[0] compare:x[0]]; }];
    NSMutableArray *out = [NSMutableArray array]; for (NSArray *e in r) [out addObject:gpu_event_line(e[1])];
    return out;
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc == 3 && !strcmp(argv[1], "--child")) return run_case(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--report")) {   // a process's reports of the last 7 days, newest first (for a game: --report dmhost, or the name the app's host runs under, cut to 16 characters)
        NSArray<NSString *> *evs = gpu_events(@(argv[2]), [NSDate dateWithTimeIntervalSinceNow:-7 * 86400]);
        for (NSString *ev in evs) printf("%s\n", ev.UTF8String);
        if (!evs.count) printf("no gpuEvent report of %s in the last 7 days\n", argv[2]);
        return 0;
    }
    char self[4096]; uint32_t sz = sizeof self; if (_NSGetExecutablePath(self, &sz)) return 2;
    NSFileManager *fm = [NSFileManager defaultManager]; NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_gpu_fault_kinds.%d", getpid()]];
    if (![fm createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:nil]) return 2;
    printf("%s, %s; each case in a process of its own\n", MTLCreateSystemDefaultDevice().name.UTF8String, [NSProcessInfo processInfo].operatingSystemVersionString.UTF8String);
    int bad = 0;
    for (int i = 0; i < NCASE; i++) {
        int want = argc < 2; for (int k = 1; k < argc; k++) if (!strcmp(argv[k], CASES[i][0])) want = 1;
        if (!want) continue;
        NSDate *t0 = [NSDate dateWithTimeIntervalSinceNow:-1];
        printf("%-24s %s\n  Metal: ", CASES[i][0], CASES[i][1]); fflush(stdout);
        NSString *name = [NSString stringWithFormat:@"gfk%02d", i + 1], *copy = [tmp stringByAppendingPathComponent:name];
        if (![fm copyItemAtPath:@(self) toPath:copy error:nil]) { printf("cannot copy the program to %s\n", copy.UTF8String); bad++; continue; }
        char *args[] = {(char *)copy.fileSystemRepresentation, "--child", (char *)CASES[i][0], NULL}; pid_t pid; int st = 0;
        if (posix_spawn(&pid, copy.fileSystemRepresentation, NULL, NULL, args, environ) || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) { bad++; continue; }
        if (WEXITSTATUS(st) != 10) { if (WEXITSTATUS(st)) bad++; continue; }   // no fault: no report to wait for
        // macOS writes the report a moment after the GPU's restart, and only so many of them: on an M6 none came after about nine in 25 minutes, for any process name
        NSString *ev = nil; for (int w = 0; w < 24 && !ev; w++) { usleep(250000); ev = gpu_events(name, t0).firstObject; }
        printf("  macOS: %s\n", ev ? ev.UTF8String : "no gpuEvent report written (macOS writes a limited number of them; run again later for the fault's address)");
        sleep(2);   // the GPU's restart settles before the next case
    }
    [fm removeItemAtPath:tmp error:nil];
    return bad != 0;
} }
