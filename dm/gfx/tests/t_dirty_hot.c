// Hot pages in dirty-page tracking (T-141): a page written again and again while a reader keeps protecting it cools (left writable, always counted as written) instead of
// faulting at every write, and comes back under protection once its time is up; and a fault on a page the tracker holds as writable ("none") while the kernel has it read-only
// (someone else's protection) is not claimed, so it cannot loop.
// Expected values come from the rule, not from the implementation: after HOT_N quick write-faults the next protection leaves the page writable and a write to it neither faults
// nor goes unseen (nvn_dirty_since says written for any stamp); after the cooling time the page is protected again, a stamp taken then sees it clean until the next write.
#include "../nvn/nvn_dirty.c"
#include <stdio.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-66s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static int kernel_writable(const void *p) { return page_writable((uintptr_t)p); }

int main(void) {
    crash_install();
    g_cool_ms = 300;   // short, so the test does not wait 2 s
    size_t pg = (size_t)getpagesize();
    volatile uint8_t *page = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range((const void *)page, 4 * pg);

    // A ping-pong: protect, write (faults), HOT_N times within HOT_MS of each other
    uint64_t f0 = atomic_load(&g_faults);
    for (int k = 0; k < HOT_N; k++) { nvn_dirty_protect((const void *)page, pg); page[8] = (uint8_t)k; }
    EXPECT("each of the quick writes faulted once", HOT_N, atomic_load(&g_faults) - f0);
    EXPECT("the page cooled", 1, atomic_load(&g_cools));

    // Cooling: protection leaves it writable, writes do not fault, and every stamp sees it as written
    uint64_t st = nvn_dirty_now(); nvn_dirty_protect((const void *)page, pg);
    EXPECT("protection while cooling leaves the page writable (kernel)", 1, kernel_writable((const void *)page));
    uint64_t f1 = atomic_load(&g_faults); page[9] = 0x77;
    EXPECT("a write while cooling does not fault", 0, atomic_load(&g_faults) - f1);
    EXPECT("a stamp taken just before that protection sees the page as written", 1, nvn_dirty_since((const void *)page, 16, st));
    EXPECT("so does a stamp taken now", 1, nvn_dirty_since((const void *)page, 16, nvn_dirty_now()));
    nvn_dirty_protect((const void *)(page + pg), pg);
    EXPECT("the next page, never hot, is protected as usual", 0, kernel_writable((const void *)(page + pg)));

    // The time is up: protected again, writes seen as usual
    usleep((useconds_t)(g_cool_ms + 100) * 1000);
    uint64_t old = st; nvn_dirty_protect((const void *)page, pg);
    EXPECT("after the cooling time protection makes it read-only again", 0, kernel_writable((const void *)page));
    EXPECT("a stamp from while it cooled still sees it as written", 1, nvn_dirty_since((const void *)page, 16, old));
    uint64_t st2 = nvn_dirty_now(); nvn_dirty_protect((const void *)page, pg);
    EXPECT("a stamp taken after it is protected again sees it clean", 0, nvn_dirty_since((const void *)page, 16, st2));
    usleep(150 * 1000);   // past HOT_MS: this write is a single fault, not a hot run
    page[10] = 1;
    EXPECT("a later write is seen", 1, nvn_dirty_since((const void *)page, 16, st2));
    EXPECT("one slow write does not cool it again", 1, atomic_load(&g_cools));

    // Somebody else's read-only protection over a page the tracker holds as writable: the fault is not the tracker's
    uintptr_t third = (uintptr_t)page + 2 * pg;
    nvn_dirty_protect((const void *)third, pg); ((volatile uint8_t *)third)[0] = 1;   // protected, then written: "none", with a generation
    mprotect((void *)third, pg, PROT_READ);                                          // another module's protection
    EXPECT("a write fault on a 'none' page the kernel has read-only is declined", 0, on_fault((void *)third, 1, NULL));
    mprotect((void *)third, pg, PROT_READ | PROT_WRITE);
    EXPECT("and claimed once the kernel has it writable", 1, on_fault((void *)third, 1, NULL));

    printf("t_dirty_hot: %s\n", g_fail ? "FAILED" : "all passed"); return g_fail != 0;
}
