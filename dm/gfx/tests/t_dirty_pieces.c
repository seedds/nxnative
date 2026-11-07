// t_dirty_pieces: a span across two adjacent tracked ranges (a sparse texture's run of physical pages merged over two physical pools, hle_nvn.c vpt_walk):
// as one span it is untracked (protect does nothing, since says written); nvn_dirty_protect_pieces / nvn_dirty_since_pieces track it range by range
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

int main(void) {
    crash_install();
    size_t pg = (size_t)getpagesize(), half = 4 * pg;
    uint8_t *m = mmap(NULL, 2 * half + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(m, half);            // pool A
    nvn_dirty_add_range(m + half, half);     // pool B, adjacent in host memory
    uint8_t *span = m + half - 2 * pg; size_t len = 4 * pg;   // two pages of A, two of B

    EXPECT("the whole span is in no single range", 0, nvn_dirty_tracked(span, len));
    nvn_dirty_protect_pieces(span, len);
    uint64_t st = nvn_dirty_now();
    EXPECT("untouched after a piecewise protect: clean", 0, nvn_dirty_since_pieces(span, len, st));
    EXPECT("the old single-span look still says written (untracked)", 1, nvn_dirty_since(span, len, st));
    span[3 * pg] = 7;                          // a write into B's part
    EXPECT("a write in the second range is seen", 1, nvn_dirty_since_pieces(span, len, st));
    nvn_dirty_protect_pieces(span, len); st = nvn_dirty_now();
    EXPECT("clean again after protecting again", 0, nvn_dirty_since_pieces(span, len, st));
    span[0] = 9;                               // a write into A's part
    EXPECT("a write in the first range is seen", 1, nvn_dirty_since_pieces(span, len, st));
    nvn_dirty_protect_pieces(span, len); st = nvn_dirty_now();
    EXPECT("a span reaching past both ranges stays written (untracked tail)", 1, nvn_dirty_since_pieces(span, len + 3 * pg, st));
    nvn_dirty_remove_range(m + half);
    EXPECT("after pool B is removed its part is untracked: written", 1, nvn_dirty_since_pieces(span, len, st));
    nvn_dirty_remove_range(m);
    munmap(m, 2 * half + pg);
    printf("t_dirty_pieces: %s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
