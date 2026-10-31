// DM_NVN_POOLPEAK's page set (nvn_poolpeak.h): pages marked once, partial pages rounded out, the pool's end respected, the largest unused range with allocations from both ends
#include "../nvn/nvn_poolpeak.h"
#include <stdio.h>

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) {
    nvn_poolpeak p;
    CHECK(nvn_poolpeak_init(&p, 115ull << 20), "a 115 MB pool");
    CHECK(p.pages == 1840 && p.used == 0 && nvn_poolpeak_gap(&p) == 1840, "1,840 pages of 64 KB, none used, all of it one range");
    CHECK(nvn_poolpeak_add(&p, 0, 0x10000) == 1 && p.used == 1, "one page at the start");
    CHECK(nvn_poolpeak_add(&p, 0, 0x10000) == 0 && p.used == 1, "the same page again: the set does not grow");
    CHECK(nvn_poolpeak_add(&p, 0x8000, 0x10000) == 1 && p.used == 2, "a texture across a page boundary takes both pages");
    CHECK(nvn_poolpeak_gap(&p) == 1838, "the unused range after them");
    // From the other end: the top 10 pages
    CHECK(nvn_poolpeak_add(&p, (1840 - 10) * 0x10000ull, 10 * 0x10000ull) == 10 && p.used == 12, "ten pages at the pool's end");
    CHECK(nvn_poolpeak_gap(&p) == 1828, "the range between both ends");
    // One page in the middle splits the range
    CHECK(nvn_poolpeak_add(&p, 900 * 0x10000ull, 1) == 1, "one byte marks its page");
    CHECK(nvn_poolpeak_gap(&p) == 929, "the larger part: pages 901 to 1829");
    CHECK(nvn_poolpeak_add(&p, (1840 - 1) * 0x10000ull, 4 * 0x10000ull) == 0, "a texture past the pool's end marks nothing beyond it");
    CHECK(nvn_poolpeak_add(&p, 0, 0) == 0, "a texture of no storage marks nothing");
    nvn_poolpeak q; CHECK(nvn_poolpeak_init(&q, 0x18000) && q.pages == 2, "a pool of 1.5 pages has two");
    free(p.map); free(q.map);
    printf("t_poolpeak: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
