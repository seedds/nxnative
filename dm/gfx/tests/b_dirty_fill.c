// CPU time of a sequential fill of protected pages through the real fault path (nvn_dirty.c + crash.c), with DM_NVN_FAULT_AHEAD as set: faults and µs of CPU per page
#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <sys/mman.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
module_t *g_mod; int g_nmod; module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);
static double cpu_s(void) { struct rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6; }
int main(int argc, char **argv) {
    int np = argc > 1 ? atoi(argv[1]) : 4096, ranges = argc > 2 ? atoi(argv[2]) : 1;
    crash_install(); size_t pg = (size_t)getpagesize();
    for (int i = 1; i < ranges; i++) { void *o = mmap(NULL, pg * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); nvn_dirty_add_range(o, pg * 4); }   // other pools the fault scan walks
    uint8_t *m = mmap(NULL, (size_t)np * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    nvn_dirty_add_range(m, (size_t)np * pg);
    for (int rep = 0; rep < 3; rep++) {
        nvn_dirty_protect(m, (size_t)np * pg);
        uint64_t f0, p0, f1, p1; nvn_dirty_stats(&f0, &p0); double c0 = cpu_s();
        for (int i = 0; i < np; i++) ((volatile uint8_t *)m)[(size_t)i * pg] = (uint8_t)i;
        double c = cpu_s() - c0; nvn_dirty_stats(&f1, &p1);
        printf("%d pages, %d ranges: %llu faults, %.1f us of CPU a page (%.1f us a fault)\n", np, ranges, (unsigned long long)(f1 - f0), c / np * 1e6, f1 > f0 ? c / (double)(f1 - f0) * 1e6 : 0);
    }
    return 0;
}

