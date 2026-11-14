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
    printf("%-52s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static void *page_map(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

int main(void) {
    crash_install();

    for (int i = 0; i < 65; i++) {
        void *p = page_map();
        nvn_dirty_add_range(p, 4096);
        nvn_dirty_remove_range(p);
        munmap(p, 4096);
    }

    void *reused = page_map();
    nvn_dirty_add_range(reused, 4096);
    nvn_dirty_protect(reused, 4096);
    uint64_t stamp = nvn_dirty_now();
    ((volatile uint8_t *)reused)[0] = 1;
    EXPECT("after 65 pool destroys in total, a write to a new pool is dirty", 1, nvn_dirty_since(reused, 4096, stamp));
    nvn_dirty_remove_range(reused);
    munmap(reused, 4096);

    // A pool destroyed and another registered over the same memory (the same slot): the new pool's data was written while the memory was in no pool, so no page fault and no
    // generation saw it, and the generations start again at 0. A stamp from the old pool's time must count as written (born): a kept index range of the vertex conversion
    // (nvn_mtl.m ix_range) is keyed by address and outlives its pool
    void *again = page_map();
    nvn_dirty_add_range(again, 4096);
    nvn_dirty_protect(again, 4096);
    stamp = nvn_dirty_now();
    const void *kept = nvn_dirty_span(again, 4096);
    EXPECT("a protected pool not written: clean", 0, nvn_dirty_since_span(kept, again, 4096, stamp));
    nvn_dirty_remove_range(again);
    ((volatile uint8_t *)again)[0] = 3;   // the next pool's data, written with no pool registered: no fault
    nvn_dirty_add_range(again, 4096);
    EXPECT("same memory, a new pool: an older stamp is dirty", 1, nvn_dirty_since(again, 4096, stamp));
    EXPECT("the same, through the handle kept from the old pool", 1, nvn_dirty_since_span(kept, again, 4096, stamp));
    EXPECT("the same, counted by nvn_dirty_since_except", 1, (long long)nvn_dirty_since_except(kept, again, 4096, stamp, NULL, 0, NULL));
    uint64_t stamp2 = nvn_dirty_now();
    EXPECT("a stamp taken after the new pool's registration: clean", 0, nvn_dirty_since(again, 4096, stamp2));
    nvn_dirty_remove_range(again);
    munmap(again, 4096);

    void *live[300];
    for (int i = 0; i < 300; i++) {
        live[i] = page_map();
        nvn_dirty_add_range(live[i], 4096);
    }
    void *overflow = live[299];
    nvn_dirty_protect(overflow, 4096);
    stamp = nvn_dirty_now();
    ((volatile uint8_t *)overflow)[0] = 2;
    EXPECT("pools beyond the live limit are conservatively dirty", 1, nvn_dirty_since(overflow, 4096, stamp));
    for (int i = 0; i < 300; i++) {
        nvn_dirty_remove_range(live[i]);
        munmap(live[i], 4096);
    }

    // More pools alive at once than the table's fixed 4,096 slots used to hold (Breath of the Wild reaches 4,006 in its first shrine): the table grows, and the last pool is still tracked
    enum { NMANY = 5000 };
    void **many = malloc(NMANY * sizeof *many);
    for (int i = 0; i < NMANY; i++) { many[i] = page_map(); nvn_dirty_add_range(many[i], 4096); }
    void *last = many[NMANY - 1];
    EXPECT("5000 live pools: the last one is tracked", 1, nvn_dirty_tracked(last, 4096));
    nvn_dirty_protect(last, 4096);
    stamp = nvn_dirty_now();
    EXPECT("pool 5000 not written after protect: clean", 0, nvn_dirty_since(last, 4096, stamp));
    ((volatile uint8_t *)last)[0] = 5;
    EXPECT("pool 5000 written by the game: dirty", 1, nvn_dirty_since(last, 4096, stamp));
    for (int i = 0; i < NMANY; i++) { nvn_dirty_remove_range(many[i]); munmap(many[i], 4096); }
    free(many);

    // Adjacent pools sharing a host page (pools are 4 KB aligned, host pages 16 KB): the first page of SMBW's ASTC texture pool is shared with a small preceding pool, and was judged untracked and re-uploaded on every use
    size_t pg = (size_t)getpagesize();
    uint8_t *blk = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (blk == MAP_FAILED) { perror("mmap"); exit(2); }
    uint8_t *pa = blk, *pb = blk + pg - 0x1000;           // pool A covers the start of page 0, pool B starts in the last 4 KB of page 0
    size_t na = pg - 0x1000, nb = 2 * pg, tex = pg + 0x800;   // the texture in B spans the shared page and the next one
    nvn_dirty_add_range(pa, na);
    nvn_dirty_add_range(pb, nb);
    EXPECT("texture in the later of two page-sharing pools is tracked", 1, nvn_dirty_tracked(pb, tex));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    EXPECT("page-sharing texture not written after protect: clean", 0, nvn_dirty_since(pb, tex, stamp));
    pb[0x10] = 3;
    EXPECT("game writes B's part of the shared page: B's texture dirty", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    pa[0x10] = 4;   // same physical page, A's part: the page is read-only, the fault must be claimed through B's range or it is treated as a crash
    EXPECT("game writes A's part of the shared page: B's texture conservatively dirty", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    dm_guest_write_prepare(pa, 0x20); pa[0x20] = 5;   // the host writes A on behalf of the GPU: the shared page must also release B's protection
    EXPECT("host unprotects the shared page before writing: B's texture dirty", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_remove_range(pb);
    EXPECT("A still tracked after destroying B (ranges keyed by pool start)", 1, nvn_dirty_tracked(pa, na));
    EXPECT("B no longer tracked after destroying B", 0, nvn_dirty_tracked(pb, tex));
    nvn_dirty_remove_range(pa);
    munmap(blk, 4 * pg);

    // Written pages but those holding a byte of the host's own write (nvn_dirty_since_except: a GPU copy's bytes the host wrote into a texture's storage, nvn_mtl.m pkg_copy_absorb)
    uint8_t *st8 = mmap(NULL, 8 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (st8 == MAP_FAILED) { perror("mmap"); exit(2); }
    nvn_dirty_add_range(st8, 8 * pg);
    const void *span = nvn_dirty_span(st8, 8 * pg);
    nvn_dirty_protect(st8, 8 * pg);
    stamp = nvn_dirty_now();
    uintptr_t ex[2][2] = {{(uintptr_t)st8 + pg + 0x100, (uintptr_t)st8 + 3 * pg - 0x100}, {(uintptr_t)st8 + 6 * pg + 0x800, (uintptr_t)st8 + 6 * pg + 0x900}}, first = 0;
    EXPECT("nothing written: no page but the host's", 0, (long long)nvn_dirty_since_except(span, st8, 8 * pg, stamp, (const uintptr_t (*)[2])ex, 2, &first));
    dm_guest_write_prepare(st8 + pg + 0x100, 2 * pg - 0x200); memset(st8 + pg + 0x100, 6, 2 * pg - 0x200);
    dm_guest_write_prepare(st8 + 6 * pg + 0x800, 0x100); memset(st8 + 6 * pg + 0x800, 7, 0x100);
    EXPECT("only the host's own writes: none counted", 0, (long long)nvn_dirty_since_except(span, st8, 8 * pg, stamp, (const uintptr_t (*)[2])ex, 2, &first));
    EXPECT("the same writes without the exception: 3 pages", 3, (long long)nvn_dirty_since_except(span, st8, 8 * pg, stamp, NULL, 0, &first));
    EXPECT("... the first of them is page 1", 1, (long long)((first - (uintptr_t)st8) / pg));
    st8[4 * pg + 8] = 8;   // a game write beside them
    first = 0;
    EXPECT("a game write on another page is counted", 1, (long long)nvn_dirty_since_except(span, st8, 8 * pg, stamp, (const uintptr_t (*)[2])ex, 2, &first));
    EXPECT("... and found there", 4, (long long)((first - (uintptr_t)st8) / pg));
    EXPECT("a span the write is not in: none", 0, (long long)nvn_dirty_since_except(span, st8 + 5 * pg, 3 * pg, stamp, (const uintptr_t (*)[2])ex, 2, NULL));
    EXPECT("an untracked span: SIZE_MAX", 1, nvn_dirty_since_except(NULL, &stamp, sizeof stamp, 0, NULL, 0, NULL) == SIZE_MAX);
    nvn_dirty_remove_range(st8);
    munmap(st8, 8 * pg);

    // nvn_dirty_describe (DM_NVN_DIRTYCHECK's miss report): a protected page as the tracker and the kernel hold it, and one whose protection a call outside the tracker took away
    uint8_t *dp = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (dp == MAP_FAILED) { perror("mmap"); exit(2); }
    nvn_dirty_add_range(dp, 2 * pg);
    const void *dspan = nvn_dirty_span(dp, 2 * pg);
    nvn_dirty_protect(dp, 2 * pg);
    char d[512];
    nvn_dirty_describe(dp, dspan, d, sizeof d); printf("  %s\n", d);
    EXPECT("describe: protected, kept range, kernel read-only", 1, strstr(d, "(kept) prot gen 0") && strstr(d, "kernel: r-") != NULL);
    mprotect(dp + pg, pg, PROT_READ | PROT_WRITE);   // behind the tracker's back
    nvn_dirty_describe(dp + pg, NULL, d, sizeof d); printf("  %s\n", d);
    EXPECT("describe: held protected, kernel has it writable", 1, strstr(d, " prot gen 0") && strstr(d, "kernel: rw") && !strstr(d, "(kept)"));
    nvn_dirty_remove_range(dp);
    nvn_dirty_describe(dp, NULL, d, sizeof d);
    EXPECT("describe: no live range once the pool goes", 1, strstr(d, "no live range") != NULL);
    munmap(dp, 2 * pg);
    return g_fail != 0;
}
