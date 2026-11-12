// GPU keep-busy (DM_NVN_GPU_BUSY, off by default; the policy and its reasons in nvn_gpu_busy.h): a host thread of its own dispatches short, compute-only work on a queue of
// its own while the game's present interval is 2 or more, and sleeps otherwise. It touches no game state: one pipeline, one 16-byte buffer, a classic command queue
#import <Metal/Metal.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "nvn_gpu_busy.h"
#include "host_msl.h"
#include "../../host/dm.h"

#define BLOG(...) dm_log("[dm] [nvn-mtl] " __VA_ARGS__)
#define BUSY_LOG_S 10.0      // a summary line this often while the work runs
#define BUSY_IDLE_US 50000   // how often the thread looks at the interval while the work is off

__attribute__((weak)) int nvn_game_interval(void) { return 1; }   // hle_nvn.c: the present interval the game sets (unit tests without it: never active)

static nvn_gpu_busy_cfg g_busy;

static double busy_now_s(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * (double)tb.numer / tb.denom / 1e9; }

typedef struct { id<MTLCommandQueue> q; id<MTLComputePipelineState> ps; id<MTLBuffer> sink; } busy_gpu;

// One dispatch of iters multiply-adds a thread, waited for; its GPU time in ms, or -1 when it failed (an iPhone app in the background, a lost device)
static double busy_dispatch(const busy_gpu *g, uint32_t iters, int groups) {
    id<MTLCommandBuffer> cb = [g->q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:g->ps]; [ce setBuffer:g->sink offset:0 atIndex:0]; [ce setBytes:&iters length:sizeof iters atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake((NSUInteger)groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted ? (cb.GPUEndTime - cb.GPUStartTime) * 1e3 : -1;
}

static void *busy_thread(void *arg) {
    pthread_setname_np("dm.nvn.gpu-busy");
    busy_gpu g = {0}; uint32_t iters = GPU_BUSY_ITERS0; int was = 0; double t0 = 0, gpu_ms = 0, it_sum = 0; unsigned n = 0, failed = 0;
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)arg; NSError *e = nil;
        id<MTLLibrary> lib = host_msl_lib(dev, nil, "gpu_busy", &e);
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"gpu_busy"] : nil;
        g.ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        g.q = [dev newCommandQueue]; g.q.label = @"dm GPU keep-busy";
        g.sink = [dev newBufferWithLength:16 options:MTLResourceStorageModePrivate];
        if (!g.ps || !g.q || !g.sink) { BLOG("[RED] DM_NVN_GPU_BUSY: the keep-busy pipeline could not be made (%s); the switch stays off", e ? e.localizedDescription.UTF8String : "no queue or buffer"); return NULL; }
    }
    for (;;) @autoreleasepool {
        int on = nvn_gpu_busy_active(&g_busy, nvn_game_interval());
        if (on != was) { BLOG("GPU keep-busy %s (game present interval %d)", on ? "on" : "off", nvn_game_interval()); was = on; t0 = busy_now_s(); gpu_ms = it_sum = 0; n = failed = 0; }
        if (!on) { usleep(BUSY_IDLE_US); continue; }
        double t1 = busy_now_s(), ms = busy_dispatch(&g, iters, g_busy.groups), wall = (busy_now_s() - t1) * 1e3;
        if (ms < 0) { failed++; usleep(BUSY_IDLE_US); continue; }
        n++; gpu_ms += ms; it_sum += iters;
        iters = nvn_gpu_busy_iters(iters, ms, GPU_BUSY_CHUNK_MS);
        double gap = nvn_gpu_busy_gap_ms(ms, wall, g_busy.duty); if (gap > 0) usleep((useconds_t)(gap * 1000));
        double t = busy_now_s();
        if (t - t0 >= BUSY_LOG_S) {
            BLOG("GPU keep-busy over %.1f s: %u dispatches of %d threadgroups, %.2f ms each (%.0f multiply-adds a thread), their GPU time %.0f%% of the wall time; failed %u",
                 t - t0, n, g_busy.groups, gpu_ms / n, it_sum / n, gpu_ms / 10.0 / (t - t0), failed);
            t0 = t; gpu_ms = it_sum = 0; n = failed = 0;
        }
    }
    return NULL;
}

void nvn_gpu_busy_start(void *dev) {   // nvn_mtl.m mtl_init: reads the switch once and starts the thread when it is on
    const char *s = getenv("DM_NVN_GPU_BUSY");
    if (!nvn_gpu_busy_parse(s, &g_busy)) { BLOG("[RED] DM_NVN_GPU_BUSY=%s not understood (1, <duty 2-100> or <duty>:<threadgroups 1-%d>): off", s, GPU_BUSY_GROUPS_MAX); return; }
    if (!g_busy.duty) return;
    BLOG("DM_NVN_GPU_BUSY: while the game's present interval is %d or more, the GPU is kept busy %d%% of the time by dispatches of %d threadgroups of about %.1f ms",
         GPU_BUSY_ITV_MIN, g_busy.duty, g_busy.groups, GPU_BUSY_CHUNK_MS);
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, busy_thread, dev)) BLOG("[RED] DM_NVN_GPU_BUSY: no thread for the keep-busy work; off");
    pthread_attr_destroy(&a);
}
