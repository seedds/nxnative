// Pages released ahead of a sequential write (nvn_dirty.c fa_release, DM_NVN_FAULT_AHEAD): a thread that fills a protected range page after page takes a handful of faults
// instead of one a page, every page it wrote counts as written, a single write releases nothing ahead, a range's last page is released only by its own fault, and a page
// protected again faults again. Run with DM_NVN_FAULT_AHEAD=0 too: one fault a page, as before
#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-64s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)
enum { NP = 64 };
static uint64_t faults(void) { uint64_t f, p; nvn_dirty_stats(&f, &p); return f; }

int main(void) {
    const char *e = getenv("DM_NVN_FAULT_AHEAD"); int on = !e || atoi(e) != 0;
    crash_install();
    size_t pg = (size_t)getpagesize();
    uint8_t *m = mmap(NULL, NP * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(m, NP * pg);
    nvn_dirty_protect(m, NP * pg);

    // Page after page, as a decompressor fills a texture's storage: faults at pages 0, 1, 3, 6, 11, 20, 37 (the window doubling to 32) and the last page, 63, on its own
    uint64_t stamp = nvn_dirty_now(), f0 = faults();
    for (int i = 0; i < NP; i++) ((volatile uint8_t *)m)[(size_t)i * pg] = (uint8_t)(i + 1);
    EXPECT("a sequential fill of 64 protected pages: faults", on ? 8 : NP, faults() - f0);
    int dirty = 0; for (int i = 0; i < NP; i++) dirty += nvn_dirty_since(m + (size_t)i * pg, pg, stamp);
    EXPECT("every page written counts as written", NP, dirty);
    int same = 1; for (int i = 0; i < NP; i++) same &= m[(size_t)i * pg] == (uint8_t)(i + 1);
    EXPECT("and holds what was written", 1, same);

    // Protected again: one write in the middle releases nothing ahead
    nvn_dirty_protect(m, NP * pg);
    stamp = nvn_dirty_now(); f0 = faults();
    ((volatile uint8_t *)m)[10 * pg] = 0x55;
    EXPECT("a single write: one fault", 1, faults() - f0);
    EXPECT("its page is written", 1, nvn_dirty_since(m + 10 * pg, pg, stamp));
    EXPECT("the page after it is not", 0, nvn_dirty_since(m + 11 * pg, pg, stamp));

    // Two in a row: the second releases one page ahead, which counts as written though nothing wrote it; the next one does not
    ((volatile uint8_t *)m)[20 * pg] = 1; ((volatile uint8_t *)m)[21 * pg] = 1;
    EXPECT("pages 20 and 21 written in order: two faults", 2, faults() - f0 - 1);
    EXPECT("page 22, released ahead, counts as written", on ? 1 : 0, nvn_dirty_since(m + 22 * pg, pg, stamp));
    EXPECT("page 23 does not", 0, nvn_dirty_since(m + 23 * pg, pg, stamp));
    f0 = faults(); ((volatile uint8_t *)m)[22 * pg] = 1;
    EXPECT("writing page 22 then faults only without the release ahead", on ? 0 : 1, faults() - f0);

    // A page released ahead and protected again (an upload) faults when written
    nvn_dirty_protect(m, NP * pg);
    stamp = nvn_dirty_now(); f0 = faults();
    ((volatile uint8_t *)m)[22 * pg] = 2;
    EXPECT("protected again, page 22 faults on its write", 1, faults() - f0);
    EXPECT("and is written", 1, nvn_dirty_since(m + 22 * pg, pg, stamp));

    nvn_dirty_remove_range(m);
    munmap(m, NP * pg);
    printf("dirty ahead (DM_NVN_FAULT_AHEAD %s): %s\n", on ? "on" : "off", g_fail ? "FAIL" : "ok");
    return g_fail != 0;
}

