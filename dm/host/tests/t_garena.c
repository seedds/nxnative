// The garena (dm/host/aot.h, "Host memory the guest sees"; aot_arena.c): the one rule's allocator for host memory the guest sees on the x86-64 engine, checked alone over the
// real arena below 2^39: small blocks of every size class at every alignment they serve, zero, in the arena, reused once freed and zero again; large blocks in pages, at
// alignments up to 1 MiB, kept for reuse up to 1 MiB and decommitted above it (the commit hooks see both); realloc keeping the data; strdup, the literal copies found again by
// the host string's address, the statics made once; the scoped region of a host thread (mark, release, nesting, DM_GLOCAL released at the end of its block, a thread's own
// region) and its exhaustion (a buffer larger than the region: a [RED] line once per thread, the buffer taken from the garena and released with the mark); the arena's
// given-back ranges reused, the lowest first; and the guest-call side of the arena guard (DM_AOT_ARENA_CHECK: DM_GUEST_CALL's pointer arguments, aot.h dm_gc_arg), which flags
// a host pointer passed on purpose once per site and leaves integers, null and arena pointers alone. The host-call side (the generated thunks) is checked by t_aot_hostfn.
// No guest code runs here; the engine's other files are not linked (the stubs below stand for the host-function table and the loader)
#include "../aot.h"
#include "../aot_hostfn.h"
#include <stdarg.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

// What aot_arena.c and aot_hostfn.c take from the rest of the host: no host-call thunks, no loader
const hle_thunk_ent hle_thunks[1] = {{0}};
const size_t hle_thunks_count = 0;
uint64_t loader_named_stub(const char *name) { (void)name; return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }

static int checks, fails;
static void t(int ok, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    checks++; fails += !ok;
    printf("%-5s ", ok ? "ok" : "FAIL"); vprintf(fmt, ap); putchar('\n');
    va_end(ap);
    fflush(stdout);
}
static int in_arena(const void *p, size_t n) { return dm_aot_in_arena((uint64_t)(uintptr_t)p, n) && (uint64_t)(uintptr_t)p + n <= (1ull << 39); }
static int zero(const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) if (b[i]) return 0; return 1; }

// stderr into a file for a while, to read the [RED] lines a step prints
static int g_saved = -1;
static void capture_begin(void) { fflush(stderr); g_saved = dup(2); FILE *f = fopen("t_garena.stderr", "w"); if (f) { dup2(fileno(f), 2); fclose(f); } }
static char *capture_end(void) {
    fflush(stderr); dup2(g_saved, 2); close(g_saved);
    static char buf[16384]; buf[0] = 0;
    FILE *f = fopen("t_garena.stderr", "r");
    if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); remove("t_garena.stderr"); }
    fputs(buf, stderr);   // and into the test's own output
    return buf;
}
static int count(const char *s, const char *needle) { int n = 0; for (const char *p = strstr(s, needle); p; p = strstr(p + 1, needle)) n++; return n; }

// The commit hooks (be_guest_map / be_guest_unmap in the host program): every range they were told of
static uint64_t g_map_a, g_map_n, g_unmap_a, g_unmap_n; static int g_maps, g_unmaps;
static void on_map(void *mem, uint64_t size) { g_map_a = (uint64_t)(uintptr_t)mem; g_map_n = size; g_maps++; }
static void on_unmap(void *mem, uint64_t size) { g_unmap_a = (uint64_t)(uintptr_t)mem; g_unmap_n = size; g_unmaps++; }

static void *thread_region(void *arg) {   // another host thread: a scoped region of its own
    uint64_t *out = arg;
    dm_gscope_t m = dm_gscope_mark();
    out[0] = (uint64_t)(uintptr_t)dm_gscope_alloc(64);
    dm_gscope_release(m);
    return NULL;
}

int main(void) {
    dm_aot_arena_init();
    dm_garena_set_commit_hooks(on_map, on_unmap);

    // ---------- Small blocks: each size class at each alignment it serves ----------
    static const size_t sizes[] = {1, 15, 16, 17, 48, 100, 129, 300, 1000, 1500, 4096, 5000, 9000, 16384};
    static const size_t aligns[] = {0, 16, 32, 64, 256, 4096, 16384};
    int ok_align = 1, ok_zero = 1, ok_where = 1, ok_size = 1, n_alloc = 0;
    void *keep[sizeof sizes / sizeof *sizes][sizeof aligns / sizeof *aligns];
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++)
        for (size_t j = 0; j < sizeof aligns / sizeof *aligns; j++) {
            uint8_t *p = dm_garena_alloc(sizes[i], aligns[j]); keep[i][j] = p; n_alloc++;
            const size_t al = aligns[j] < 16 ? 16 : aligns[j], got = dm_garena_size(p);
            ok_align &= !((uintptr_t)p & (al - 1));
            ok_where &= in_arena(p, sizes[i]);
            ok_zero &= zero(p, got);
            ok_size &= got >= sizes[i] && (sizes[i] > 16384 || aligns[j] > 16384 || got <= 16384);
            memset(p, 0xab, sizes[i]);
        }
    t(ok_align, "%d small allocations (1 to 16384 bytes, alignments 16 to 16384): each aligned as asked", n_alloc);
    t(ok_where, "each in the arena below 2^39");
    t(ok_zero, "each zero, over its whole usable size");
    t(ok_size, "dm_garena_size: at least the size asked, a class of at most 16 KiB");
    t(dm_garena_size((uint8_t *)keep[5][0] + 16) == 0 && dm_garena_size(&checks) == 0, "dm_garena_size of an address inside a block, or of host memory: 0");
    void *again = keep[6][0]; dm_garena_free(again);
    uint8_t *re = dm_garena_alloc(129, 0);
    t(re == again && zero(re, dm_garena_size(re)), "a freed block is the next one of its class, zero again (%p)", (void *)re);
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) for (size_t j = 0; j < sizeof aligns / sizeof *aligns; j++) if (keep[i][j] != again) dm_garena_free(keep[i][j]);
    dm_garena_free(re);
    uint8_t *m = dm_garena_malloc(200, 0);
    t(m && in_arena(m, 200), "dm_garena_malloc: a block in the arena (not zeroed: the Unreal heap's form)");
    dm_garena_free(m);

    // ---------- Large blocks ----------
    int maps0 = g_maps;
    uint8_t *big = dm_garena_alloc(100000, 0);
    t(big && in_arena(big, 100000) && !((uintptr_t)big & 4095) && dm_garena_size(big) == 102400 && zero(big, 102400),
      "100000 bytes: whole pages (102400), page-aligned, zero, in the arena");
    t(g_maps > maps0 && g_map_a == (uint64_t)(uintptr_t)big && g_map_n == 102400, "the map hook was told of its pages (%#llx + %#llx)", (unsigned long long)g_map_a,
      (unsigned long long)g_map_n);
    uint8_t *al64k = dm_garena_alloc(20000, 0x10000), *al1m = dm_garena_alloc(5000, 0x100000);
    t(!((uintptr_t)al64k & 0xffff) && !((uintptr_t)al1m & 0xfffff) && in_arena(al64k, 20000) && in_arena(al1m, 5000), "alignments 64 KiB and 1 MiB: %p, %p", (void *)al64k,
      (void *)al1m);
    memset(big, 0x5a, 100000);
    dm_garena_free(big);
    uint8_t *big2 = dm_garena_alloc(100000, 0);
    t(big2 == big && zero(big2, 102400), "a freed block of up to 1 MiB is kept for its size: the next one is the same pages, zero again");
    int unmaps0 = g_unmaps;
    uint8_t *huge = dm_garena_alloc(3u << 20, 0);
    const uint64_t huge_a = (uint64_t)(uintptr_t)huge;
    dm_garena_free(huge);
    t(g_unmaps == unmaps0 + 1 && g_unmap_a == huge_a && g_unmap_n == 3u << 20, "a freed block above 1 MiB is decommitted: the unmap hook was told of it before");
    uint8_t *r = dm_garena_realloc(NULL, 40, 0);
    for (int i = 0; i < 40; i++) r[i] = (uint8_t)i;
    r = dm_garena_realloc(r, 30000, 0);
    int kept = 1; for (int i = 0; i < 40; i++) kept &= r[i] == i;
    t(kept && dm_garena_size(r) >= 30000 && in_arena(r, 30000), "dm_garena_realloc from a small block to a large one keeps the data");
    t(dm_garena_realloc(r, 0, 0) == NULL, "dm_garena_realloc to 0 frees");
    dm_garena_free(big2); dm_garena_free(al64k); dm_garena_free(al1m);

    // ---------- Strings and statics ----------
    static const char host_lit[] = "channel-count";
    const char *l1 = dm_garena_literal(host_lit), *l2 = dm_garena_literal(host_lit);
    t(l1 && l1 == l2 && l1 != host_lit && !strcmp(l1, host_lit) && in_arena(l1, sizeof host_lit), "dm_garena_literal: one arena copy per host string (%p), found again by its address",
      (const void *)l1);
    t(dm_garena_literal(l1) == l1 && dm_garena_literal(NULL) == NULL, "a string already in the arena (and null) is returned as it is");
    char *d = dm_garena_strdup("game environment");
    t(d && !strcmp(d, "game environment") && in_arena(d, 17), "dm_garena_strdup");
    dm_garena_free(d);
    static void *_Atomic slot;
    static const uint32_t init[4] = {0x50534944, 1, 2, 3};
    uint32_t *s1 = dm_garena_static(&slot, init, sizeof init), *s2 = dm_garena_static(&slot, NULL, sizeof init);
    t(s1 && s1 == s2 && !memcmp(s1, init, sizeof init) && in_arena(s1, sizeof init), "dm_garena_static: made once (a copy of its initial value), the same object after");

    // ---------- The scoped region ----------
    dm_gscope_t m0 = dm_gscope_mark();
    uint8_t *a = dm_gscope_alloc(40), *b = dm_gscope_alloc(1);
    t(in_arena(a, 40) && !((uintptr_t)a & 15) && !((uintptr_t)b & 15) && b == a + 48 && zero(a, 48), "dm_gscope_alloc: 16-byte steps, zero, in the arena");
    memset(a, 0xcc, 40);
    dm_gscope_t m1 = dm_gscope_mark();
    uint8_t *c = dm_gscope_alloc(100);
    dm_gscope_release(m1);
    uint8_t *c2 = dm_gscope_alloc(100);
    t(c2 == c && zero(c2, 100), "a released mark gives its space back: the next buffer is the same, zero again");
    dm_gscope_release(m0);
    t(dm_gscope_alloc(8) == a, "the outer mark released: back to where it began");
    dm_gscope_release(m0);
    uint8_t *inside;
    {
        DM_GLOCAL(uint64_t, loc, 16);   // as GUEST_LOCAL: Apple's uint64_t loc[16]
        inside = (uint8_t *)loc;
        t(in_arena(loc, 128) && zero(loc, 128), "DM_GLOCAL: 16 zero words in the arena at %p", (void *)loc);
    }
    t(dm_gscope_alloc(8) == inside, "DM_GLOCAL's block ended: its space was released (cleanup attribute)");
    dm_gscope_release(m0);
    uint64_t other = 0; pthread_t th;
    if (!pthread_create(&th, NULL, thread_region, &other)) pthread_join(th, NULL);
    t(other && in_arena((void *)(uintptr_t)other, 64) && (other < (uint64_t)(uintptr_t)a || other >= (uint64_t)(uintptr_t)a + DM_GSCOPE_SIZE),
      "another host thread has a region of its own (%#llx)", (unsigned long long)other);
    // Exhaustion: a buffer larger than the region
    dm_gscope_t mx = dm_gscope_mark();
    (void)dm_gscope_alloc(DM_GSCOPE_SIZE - 64);
    capture_begin();
    uint8_t *ovf = dm_gscope_alloc(100000), *ovf2 = dm_gscope_alloc(200);
    char *err = capture_end();
    t(in_arena(ovf, 100000) && zero(ovf, 100000) && in_arena(ovf2, 200) && dm_garena_size(ovf) >= 100000, "a buffer that does not fit is taken from the garena, zero, in the arena");
    t(count(err, "[RED] aot: a scoped guest buffer of 100000 bytes does not fit") == 1 && count(err, "[RED]") == 1, "exhaustion: one [RED] line for the thread, naming the size");
    dm_gscope_release(mx);
    t(dm_garena_size(ovf) == 0, "the overflow buffer went back with the mark (no block starts at %p any more)", (void *)ovf);
    dm_gscope_release(m0);

    // ---------- The arena's given-back ranges ----------
    uint8_t *g1 = dm_guest_alloc(3 * 4096), *g2 = dm_guest_alloc(4096);
    memset(g1, 0x77, 3 * 4096);
    dm_guest_free(g1, 3 * 4096);
    uint8_t *g3 = dm_guest_alloc(2 * 4096), *g4 = dm_guest_reserve(4096);   // fresh space lies above g2: a range given back lies below it
    t(g3 < g2 && g4 < g2 && in_arena(g3, 2 * 4096) && in_arena(g4, 4096), "ranges given back are taken again before fresh space, by dm_guest_alloc and dm_guest_reserve alike");
    t(zero(g3, 2 * 4096), "and read as zero again");
    dm_guest_free(g2, 4096); dm_guest_free(g3, 2 * 4096); dm_guest_free(g4, 4096);

    // ---------- The guard, the guest-call side (DM_AOT_ARENA_CHECK; aot.h DM_GC_A, as DM_GUEST_CALL expands it) ----------
    int host_local = 7; static char host_static[16];
    uint8_t *arena_ptr = dm_garena_alloc(16, 0);
    atomic_store(&dm_aot_arena_check, 1);
    capture_begin();
    uint64_t v1 = DM_GC_A(&host_local, 2), v1b = DM_GC_A(&host_local, 2);   // a host stack pointer: flagged, and at the same site again (one line): not twice
    uint64_t v2 = DM_GC_A(host_static, 3);   // an array: a pointer too
    uint64_t v3 = DM_GC_A(arena_ptr, 1);     // the arena's: not flagged
    uint64_t v4 = DM_GC_A((uint64_t)0x7fff00001000ull, 4);   // an integer, whatever its value: not flagged
    uint64_t v5 = DM_GC_A((void *)NULL, 5);  // null: not flagged
    err = capture_end();
    atomic_store(&dm_aot_arena_check, 0);
    t(v1 == (uint64_t)(uintptr_t)&host_local && v1b == v1 && v2 == (uint64_t)(uintptr_t)host_static && v3 == (uint64_t)(uintptr_t)arena_ptr && v4 == 0x7fff00001000ull && !v5,
      "DM_GC_A passes every value on unchanged");
    t(count(err, "[RED] aot: arena check: guest call argument 2 at ") == 1 && count(err, "[RED] aot: arena check: guest call argument 3 at ") == 1 && count(err, "[RED]") == 2,
      "the guard: a [RED] line for the host stack pointer and one for the host array, once per site; none for an arena pointer, an integer or null");
    t(strstr(err, "t_garena.c:") != NULL, "the line names the call site's file and line");
    dm_garena_free(arena_ptr);

    printf("t_garena: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
