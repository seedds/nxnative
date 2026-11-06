// nvn_dirty_discard (nvn_mtl.m rel_*, DM_TEX_RELEASE): the whole pages inside a range become zero pages. In a tracked range only pages still protected since the upload go, and they stay
// protected, so a later game write is seen as one; a page written since keeps its bytes. Memory outside every tracked range (a CPU_NO_ACCESS pool) goes only with tracked = 0.
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
    printf("%-60s expected %-6lld → got %-6lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

int main(void) {
    crash_install();
    size_t pg = (size_t)getpagesize(), len = 4 * pg;
    uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 2; }
    memset(p, 0x5a, len);
    nvn_dirty_add_range(p, len);
    nvn_dirty_protect(p, len);
    uint64_t stamp = nvn_dirty_now();
    p[2 * pg] = 7;   // written after the upload: its page is unprotected and must keep its bytes
    // the range starts half a page in: only whole pages go (pages 1 and 3; page 2 was written; page 0 is partial)
    size_t n = nvn_dirty_discard(p + pg / 2, len - pg / 2, 1, 0);
    EXPECT("tracked: bytes discarded (two whole protected pages)", 2 * pg, n);
    EXPECT("the partial first page keeps its bytes", 0x5a, p[0]);
    EXPECT("a discarded page reads zero", 0, p[pg]);
    EXPECT("the page written since keeps its bytes", 7, p[2 * pg]);
    EXPECT("the last page is discarded too", 0, p[3 * pg + 5]);
    stamp = nvn_dirty_now();
    EXPECT("no write since: not dirty", 0, nvn_dirty_since(p + 3 * pg, pg, stamp));
    ((volatile uint8_t *)p)[3 * pg] = 9;   // a discarded page stays protected: the game's write is seen
    EXPECT("a write to a discarded page is seen", 1, nvn_dirty_since(p + 3 * pg, pg, stamp));
    EXPECT("and lands", 9, p[3 * pg]);
    nvn_dirty_remove_range(p);

    memset(p, 0x33, len);
    EXPECT("untracked memory, tracked = 1: nothing", 0, nvn_dirty_discard(p, len, 1, 0));
    EXPECT("untracked memory, tracked = 0: every whole page", len, nvn_dirty_discard(p, len, 0, 0));
    EXPECT("and it reads zero", 0, p[len - 1]);
    munmap(p, len);
    printf("t_dirty_discard: %s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
