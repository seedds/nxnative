// Write-pending pages in dirty-page tracking (T-141, issue #3097): a page made writable for a faulting game write stays writable, and every reader counts it as written, until
// a whole frame has passed since that write; then the next protection protects it with a fresh generation. And a fault on a page the tracker holds as writable ("none")
// while the kernel has it read-only (someone else's protection) is not claimed, so it cannot loop.
// Expected values come from the rule, not from the implementation: in the frame of the write and the next, protection leaves the page writable, a write to it neither faults
// nor goes unseen; once a whole frame has passed it is protected again, an earlier stamp sees it as written once more, a stamp taken then sees it clean until the next write.
#include "../nvn/nvn_dirty.c"
#include <stdio.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-70s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static int kernel_writable(const void *p) { return page_writable((uintptr_t)p); }

int main(void) {
    crash_install();
    size_t pg = (size_t)getpagesize();
    volatile uint8_t *page = mmap(NULL, 8 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range((const void *)page, 8 * pg);
    nvn_dirty_frame();

    // A write faults: the page is write-pending for this frame and the next
    nvn_dirty_protect((const void *)page, pg);
    uint64_t f0 = atomic_load(&g_faults); page[8] = 1;
    EXPECT("the first write after protection faults", 1, atomic_load(&g_faults) - f0);
    uint64_t st = nvn_dirty_now(); nvn_dirty_protect((const void *)page, pg);    // a reader: stamp, protect, read (the ping-pong's re-protection)
    EXPECT("protection in the same frame leaves the page writable (kernel)", 1, kernel_writable((const void *)page));
    uint64_t f1 = atomic_load(&g_faults); page[9] = 2;
    EXPECT("so the writer's next store does not fault", 0, atomic_load(&g_faults) - f1);
    EXPECT("and the reader sees that store (its stamp counts the page as written)", 1, nvn_dirty_since((const void *)page, 16, st));
    { uint64_t a = nvn_dirty_now(), b = nvn_dirty_now();   // readers skip their check while the clock stands (nvn_mtl.m's look cache): a write that does not fault must not leave it standing
      EXPECT("while a page is write-pending the clock moves at each look", 1, b > a); }
    nvn_dirty_frame();
    nvn_dirty_protect((const void *)page, pg);
    EXPECT("one frame later (not a whole frame since the write) still writable", 1, kernel_writable((const void *)page));

    // A whole frame has passed: protected again, written seen as usual
    nvn_dirty_frame();
    uint64_t old = nvn_dirty_now() - 1; nvn_dirty_protect((const void *)page, pg);
    EXPECT("once a whole frame has passed, protection makes it read-only again", 0, kernel_writable((const void *)page));
    EXPECT("a stamp from while it was pending sees it as written once more", 1, nvn_dirty_since((const void *)page, 16, old));
    uint64_t st2 = nvn_dirty_now(); nvn_dirty_protect((const void *)page, pg);
    EXPECT("a stamp taken after that sees it clean", 0, nvn_dirty_since((const void *)page, 16, st2));
    nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_protect((const void *)page, pg);
    EXPECT("quiet frames bring no more re-reads", 0, nvn_dirty_since((const void *)page, 16, st2));
    { uint64_t a = nvn_dirty_now(), b = nvn_dirty_now(); EXPECT("with no page write-pending the clock stands", 1, a == b); }
    page[10] = 3;
    EXPECT("a later write faults and is seen", 1, nvn_dirty_since((const void *)page, 16, st2));
    EXPECT("pages made write-pending so far (two faulting writes)", 2, atomic_load(&g_pends));

    // A page never written is protected as usual
    nvn_dirty_protect((const void *)(page + pg), pg);
    EXPECT("a page with no faulting write is protected at once", 0, kernel_writable((const void *)(page + pg)));

    // A sparse tile's protection that leaves a write-pending page writable moves the sparse write count (no fault will)
    volatile uint8_t *sp = page + 3 * pg;
    nvn_dirty_protect((const void *)sp, pg); sp[0] = 1;
    uint64_t sw = nvn_dirty_sparse_writes(); nvn_dirty_protect_sparse((const void *)sp, pg);
    EXPECT("sparse protection of a write-pending page moves the sparse write count", 1, nvn_dirty_sparse_writes() - sw);

    // Hot pages: a page whose write faults keep coming back soon after it is protected again stops being protected; its readers compare at each look; once its bytes stay
    // unchanged for HOT_QUIET frames it is protected again
    {
        volatile uint8_t *hp = page + 5 * pg;
        uint64_t f0h = atomic_load(&g_faults);
        for (int k = 0; k < HOT_RUN; k++) { nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_protect((const void *)hp, pg); hp[64] = (uint8_t)k; }   // a write two frames after each protection
        EXPECT("a page written again two frames after each protection faults each time", HOT_RUN, atomic_load(&g_faults) - f0h);
        nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_protect((const void *)hp, pg);
        EXPECT("after HOT_RUN such faults the page is hot: protection leaves it writable", 1, kernel_writable((const void *)hp));
        EXPECT("one hot page", 1, g_nhot);
        uint64_t f1h = atomic_load(&g_faults), sth = nvn_dirty_now(); int still = 1;
        for (int k = 0; k < 200; k++) { hp[65] = (uint8_t)k; nvn_dirty_frame(); nvn_dirty_protect((const void *)hp, pg); still &= kernel_writable((const void *)hp); }
        EXPECT("written every frame for 200 frames, it stays writable", 1, still);
        EXPECT("and those writes take no fault", 0, atomic_load(&g_faults) - f1h);
        EXPECT("a reader's stamp sees it as written", 1, nvn_dirty_since((const void *)hp, 16, sth));
        for (int k = 0; k < HOT_QUIET + 1; k++) nvn_dirty_frame();   // its bytes unchanged
        EXPECT("quiet for HOT_QUIET frames, it is no longer hot", 0, g_nhot);
        uint64_t old = nvn_dirty_now() - 1; nvn_dirty_protect((const void *)hp, pg);
        EXPECT("so the next protection makes it read-only again", 0, kernel_writable((const void *)hp));
        EXPECT("a stamp from while it was hot sees it as written once more", 1, nvn_dirty_since((const void *)hp, 16, old));
        uint64_t st3 = nvn_dirty_now(); nvn_dirty_protect((const void *)hp, pg);
        EXPECT("a stamp taken after that sees it clean", 0, nvn_dirty_since((const void *)hp, 16, st3));
        nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_frame();
        uint64_t f2h = atomic_load(&g_faults); hp[66] = 1;
        EXPECT("a write after it went back to protection faults again", 1, atomic_load(&g_faults) - f2h);
        EXPECT("and is seen", 1, nvn_dirty_since((const void *)hp, 16, st3));
    }

    // A hot page whose texture really changes (a reader re-uploads it) goes back to protection and may not turn hot again for HOT_VETO frames
    {
        volatile uint8_t *vp = page + 6 * pg;
        for (int k = 0; k < HOT_RUN; k++) { nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_protect((const void *)vp, pg); vp[128] = (uint8_t)k; }
        nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_protect((const void *)vp, pg);
        EXPECT("veto: the page turned hot", 1, kernel_writable((const void *)vp) && atomic_load(&rg(0)->hotf[6]));
        nvn_dirty_changed((const void *)vp, 64);   // a reader found its texture really changed
        EXPECT("a real change takes it off the hot path", 0, (int)atomic_load(&rg(0)->hotf[6]));
        nvn_dirty_protect((const void *)vp, pg);
        EXPECT("and the next protection makes it read-only again", 0, kernel_writable((const void *)vp));
        for (int k = 0; k < HOT_RUN + 2; k++) { nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_protect((const void *)vp, pg); vp[129] = (uint8_t)k; }
        nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_protect((const void *)vp, pg);
        EXPECT("within HOT_VETO frames, more quick faults do not make it hot again", 0, kernel_writable((const void *)vp));
        for (int k = 0; k < HOT_VETO; k++) nvn_dirty_frame();
        for (int k = 0; k < HOT_RUN; k++) { nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_protect((const void *)vp, pg); vp[130] = (uint8_t)k; }
        nvn_dirty_frame(); nvn_dirty_frame(); nvn_dirty_protect((const void *)vp, pg);
        EXPECT("after HOT_VETO frames it can turn hot again", 1, kernel_writable((const void *)vp));
    }

    // DM_NVN_DIRTY_PENDING=0: protected again at once, as before
    g_pend_on = 0;
    volatile uint8_t *off = page + 4 * pg;
    nvn_dirty_protect((const void *)off, pg); off[0] = 1; nvn_dirty_protect((const void *)off, pg);
    EXPECT("with pending off, protection right after a write makes it read-only", 0, kernel_writable((const void *)off));
    g_pend_on = 1;

    // Somebody else's read-only protection over a page the tracker holds as writable: the fault is not the tracker's
    uintptr_t third = (uintptr_t)page + 2 * pg;
    nvn_dirty_protect((const void *)third, pg); ((volatile uint8_t *)third)[0] = 1;   // protected, then written: "none", with a generation
    mprotect((void *)third, pg, PROT_READ);                                          // another module's protection
    EXPECT("a write fault on a 'none' page the kernel has read-only is declined", 0, on_fault((void *)third, 1, NULL));
    mprotect((void *)third, pg, PROT_READ | PROT_WRITE);
    EXPECT("and claimed once the kernel has it writable", 1, on_fault((void *)third, 1, NULL));

    printf("t_dirty_hot: %s\n", g_fail ? "FAILED" : "all passed"); return g_fail != 0;
}
