// Crash address formatting (preserving image+offset+raw PC for redacted/unnamed symbols) and VM census formatting.
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "../dm.h"

int crash_fmt_addr(char *buf, size_t cap, const char *tag, uint64_t a, const module_t *m, const Dl_info *di);
void dm_vm_census(FILE *f);

void svc_guest_range(uint8_t **base, size_t *size) { *base = NULL; *size = 0; }
int thr_name_of(pthread_t pt, char *b, size_t n) { (void)pt; (void)b; (void)n; return 0; }
uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }

static int checks, failed;
static void expect_str(const char *name, const char *want, const char *got) {
    checks++;
    if (strcmp(want, got) != 0) {
        failed++;
        fprintf(stderr, "%s: expected '%s', got '%s'\n", name, want, got);
    }
}
static void expect_has(const char *name, const char *haystack, const char *needle) {
    checks++;
    if (!strstr(haystack, needle)) {
        failed++;
        fprintf(stderr, "%s: missing '%s' in:\n%s\n", name, needle, haystack);
    }
}

int main(void) { @autoreleasepool {
    char buf[256];

    // 1. Guest module offset
    module_t mod;
    memset(&mod, 0, sizeof mod);
    snprintf(mod.name, sizeof mod.name, "main");
    mod.base = (uint8_t *)0x115800000ull;
    crash_fmt_addr(buf, sizeof buf, "pc", 0x115801234ull, &mod, NULL);
    expect_str("guest module", " pc=main+0x1234", buf);

    // 2. Named host symbol
    Dl_info di = {0};
    di.dli_fname = "/private/var/containers/Bundle/DmIOS.app/DmIOS";
    di.dli_fbase = (void *)0x100998000ull;
    di.dli_sname = "pkg_edges_take";
    di.dli_saddr = (void *)0x100a425bcull;
    crash_fmt_addr(buf, sizeof buf, "lr", 0x100a42728ull, NULL, &di);
    expect_str("named symbol", " lr=pkg_edges_take+0x16c", buf);

    // 3. Redacted iOS framework symbol preserves image basename, image-relative offset, and raw PC
    memset(&di, 0, sizeof di);
    di.dli_fname = "/System/Library/Frameworks/Metal.framework/Metal";
    di.dli_fbase = (void *)0x1f8000000ull;
    di.dli_sname = "<redacted>";
    di.dli_saddr = (void *)0x1f8012000ull;
    crash_fmt_addr(buf, sizeof buf, "", 0x1f8012340ull, NULL, &di);
    expect_str("redacted framework", " Metal+0x12340(0x1f8012340)", buf);

    // 4. Missing symbol name with image filename
    di.dli_sname = NULL;
    di.dli_fname = "/usr/lib/system/libdispatch.dylib";
    di.dli_fbase = (void *)0x19d000000ull;
    crash_fmt_addr(buf, sizeof buf, "pc", 0x19d0040e4ull, NULL, &di);
    expect_str("unnamed dylib", " pc=libdispatch.dylib+0x40e4(0x19d0040e4)", buf);

    // 5. Redacted with no image filename falls back to raw hex
    memset(&di, 0, sizeof di);
    di.dli_sname = "<redacted>";
    crash_fmt_addr(buf, sizeof buf, "", 0x123456780ull, NULL, &di);
    expect_str("redacted without image", " 0x123456780", buf);

    // 6. No dladdr hit
    crash_fmt_addr(buf, sizeof buf, "addr", 0xdeadbeef0ull, NULL, NULL);
    expect_str("no dladdr", " addr=0xdeadbeef0", buf);

    // 7. VM census emits sub-GB/GB formatted free gaps and top file/other region breakdown
    void *anon = mmap(NULL, 4 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    FILE *tf = tmpfile();
    if (!tf) return 2;
    dm_vm_census(tf);
    fflush(tf);
    fseek(tf, 0, SEEK_END);
    long sz = ftell(tf);
    fseek(tf, 0, SEEK_SET);
    char *out = calloc(1, (size_t)sz + 1);
    if (!out) return 2;
    if (sz > 0) (void)fread(out, 1, (size_t)sz, tf);
    fclose(tf);
    if (anon != MAP_FAILED) munmap(anon, 4 << 20);

    expect_has("census header", out, "[dm] address space:");
    expect_has("census largest free", out, "largest free ");
    expect_has("census class line", out, "[dm] address space by class (GB):");
    expect_has("census top regions line", out, "[dm] address space regions:");
    expect_has("census top file", out, "top file:");
    expect_has("census top other", out, "top other(!31):");
    free(out);

    printf("t_crash_diag: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
