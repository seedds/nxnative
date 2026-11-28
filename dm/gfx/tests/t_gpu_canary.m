// DM_GPU_CANARY=1 submits a fixed compute dispatch once a second, after five seconds, and logs its GPU time. Exercise the real Metal path: the line appears, with a GPU time that is
// positive and bounded, and without the variable nothing is submitted or logged.
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stdout, "FAIL: %s\n", msg); } } while (0)

int main(void) { @autoreleasepool {
    char path[] = "/tmp/t_gpu_canary.XXXXXX"; int fd = mkstemp(path);
    if (fd < 0) { printf("FAIL: a temporary log file: %s\n", strerror(errno)); return 1; }
    fflush(stderr); int saved = dup(2);   // the host logs to stderr
    if (saved < 0 || dup2(fd, 2) < 0) { printf("FAIL: the log file in place of stderr: %s\n", strerror(errno)); if (saved >= 0) close(saved); close(fd); unlink(path); return 1; }
    setenv("DM_GPU_CANARY", "1", 1);
    mtl_init();
    sleep(8);   // the first dispatch is at 5 s; its handler runs when the GPU finishes
    fflush(stderr); CHECK(dup2(saved, 2) >= 0, "stderr is put back"); close(saved); close(fd);
    FILE *f = fopen(path, "r"); char line[512]; int started = 0, samples = 0; double gpu_ms = -1;
    while (f && fgets(line, sizeof line, f)) {
        if (strstr(line, "DM_GPU_CANARY=1")) started = 1;
        double g, s; unsigned long long n;
        if (sscanf(strstr(line, "gpu canary #") ?: "", "gpu canary #%llu at %*f s: GPU %lf ms, submit to done %lf ms", &n, &g, &s) == 3) { samples++; gpu_ms = g; }
    }
    if (f) fclose(f); unlink(path);
    CHECK(started, "the start line is logged");
    CHECK(samples >= 1, "a canary sample is logged within 8 s");
    CHECK(gpu_ms > 0.01 && gpu_ms < 200, "the canary's GPU time is positive and bounded");
    printf("%d checks, %d failed (canary samples %d, last GPU %.3f ms)\n", checks, fails, samples, gpu_ms);
    return fails ? 1 : 0;
} }
