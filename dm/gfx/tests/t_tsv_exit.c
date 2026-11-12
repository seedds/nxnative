// The real graceful exit bypasses atexit; its graphics hook must flush the TSV.
#include "../nvn/hle_nvn.c"
#include <sys/stat.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
int main(void) {
    const char *path = getenv("DM_NVN_TSV"); assert(path);
    setenv("DM_NVN_TSV_DEFER", "1", 1);
    setenv("DM_NVN_TSV_CALLS", "0", 1);
    setenv("DM_NVN_FRAMES", "0", 1);
    dump_frame();
    trow(123, "@deferred_exit_probe", 8.333);
    struct stat st; assert(!stat(path, &st) && st.st_size == 0);
    dm_gfx_quit(0);
    assert(!stat(path, &st) && st.st_size > 0);
    _Exit(0);
}
