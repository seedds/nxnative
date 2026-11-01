// The arena, guest memory and packs of the x86-64 execution engine (docs/design/aot-recompiler.md 5.1 to 5.3, 9.4; aot.h).
// The arena: one reservation of DM_AOT_ARENA_SIZE at DM_AOT_ARENA_BASE, below 2^39 (the Switch's 39-bit address space, which a guest may rely on), made at start
// with plat_vm_reserve_at; nothing in it is accessible until taken. A guest address is the host address of the same byte (5.1): recompiled loads and stores
// use guest addresses directly, and host code keeps using guest pointers as C pointers. The arena's first page stays inaccessible (DM_AOT_HOST_RETURN lies
// in it); the host-function page of aot_hostfn.c and the stub page of unimplemented imports follow (2.4); then whatever the host hands the guest: hle_svc.c's guest block (heap, alias and stack
// regions), module images, TLS blocks and thread-local regions (thread.c), the main thread's guest stack, and the host's own allocations the guest sees: the garena
// (aot.h, "Host memory the guest sees": small blocks in slabs of a size class, large ones in pages) with each host thread's scoped region. Fresh memory is taken from
// the bottom up; ranges given back (dm_guest_free) are reused first, the lowest that holds a request, and zeroed then. Pages are 4 KiB on Linux and Windows, as
// Horizon's, so protections are exact (5.3).
// Packs: the shared object (DLL on Windows) `nxaot build` makes for a module, loaded once, its identity checked against the image it was compiled from
// (SHA-256, sizes: a mismatch is refused), the module's base written for its base-relative code (3.3), its tables indexed for the dispatcher, the JIT's stops
// and fault reports. Its imports (dm_aot_svc, nxaot_rt_*, dm_aot_cntpct, the floating-point helpers) resolve against the host executable, which exports them
#include "aot.h"
#include "aot_hostfn.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// ---------- The arena ----------
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_mem_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_bump, g_end;   // fresh memory: [g_bump, g_end)
typedef struct { uint64_t a, n; } span_t;
#define MAX_FREE 65536
static span_t g_free[MAX_FREE];  // ranges given back, sorted by address, adjacent ones joined (g_mem_lock)
static int g_nfree;

static uint64_t g_stub_page;     // the named stubs' slots (aot.h), after the host-function page
static size_t page(void) { static size_t pg; if (!pg) pg = plat_page_size(); return pg; }
static uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
_Atomic int dm_aot_arena_check = -1;
static void garena_init(void);

// Fresh arena space, aligned, inaccessible (g_mem_lock held)
static uint64_t take_fresh(uint64_t n, uint64_t align) {
    uint64_t a = round_up(g_bump, align);
    if (a > g_end || n > g_end - a)
        die("[RED] aot: the arena (%llu GiB at %#llx) is full: %#llx more bytes asked, %#llx left (DM_ALIAS_SIZE larger than the arena holds?)",
            (unsigned long long)(DM_AOT_ARENA_SIZE >> 30), (unsigned long long)DM_AOT_ARENA_BASE, (unsigned long long)n, (unsigned long long)(g_end - g_bump));
    g_bump = a + n;
    return a;
}
static void free_remove(int i) { memmove(g_free + i, g_free + i + 1, (size_t)(g_nfree - i - 1) * sizeof *g_free); g_nfree--; }
// Arena space of n bytes aligned to align (a power of two, at least a page), inaccessible: the lowest range given back that holds it (the rest of that range stays given
// back), else fresh. *reused: it was given back before, so its pages need not read as zero (MADV_FREE keeps them until the kernel reclaims them). g_mem_lock held
static uint64_t take(uint64_t n, uint64_t align, int *reused) {
    for (int i = 0; i < g_nfree; i++) {
        const uint64_t lo = g_free[i].a, end = lo + g_free[i].n, a = round_up(lo, align);
        if (a > end || n > end - a) continue;
        *reused = 1;
        if (a == lo) { g_free[i].a += n; g_free[i].n -= n; if (!g_free[i].n) free_remove(i); return a; }
        g_free[i].n = a - lo;   // the part below the aligned start stays given back, and so does the part above the end
        if (a + n < end) {
            if (g_nfree < MAX_FREE) { memmove(g_free + i + 2, g_free + i + 1, (size_t)(g_nfree - i - 1) * sizeof *g_free); g_free[i + 1] = (span_t){a + n, end - a - n}; g_nfree++; }
            else { static int once; if (!once++) LOG("[RED] aot: the arena's free list is full (%d ranges): freed guest memory is not reused", MAX_FREE); }
        }
        return a;
    }
    *reused = 0;
    return take_fresh(n, align);
}
// The range [a, a + sz) given back (g_mem_lock held): joined with the ranges beside it
static void give_back(uint64_t a, uint64_t sz) {
    int lo = 0, hi = g_nfree;   // the first range at or above a
    while (lo < hi) { int mid = lo + (hi - lo) / 2; if (g_free[mid].a < a) lo = mid + 1; else hi = mid; }
    const int i = lo;
    if (i > 0 && g_free[i - 1].a + g_free[i - 1].n == a) {   // joins the range before it, and maybe the one after
        g_free[i - 1].n += sz;
        if (i < g_nfree && g_free[i - 1].a + g_free[i - 1].n == g_free[i].a) { g_free[i - 1].n += g_free[i].n; free_remove(i); }
    } else if (i < g_nfree && a + sz == g_free[i].a) { g_free[i].a = a; g_free[i].n += sz; }
    else if (g_nfree < MAX_FREE) { memmove(g_free + i + 1, g_free + i, (size_t)(g_nfree - i) * sizeof *g_free); g_free[i] = (span_t){a, sz}; g_nfree++; }
    else { static int once; if (!once++) LOG("[RED] aot: the arena's free list is full (%d ranges): freed guest memory is not reused", MAX_FREE); }
}

// The graphics backend's view of the pages the arena commits and decommits for the host (aot.h dm_garena_set_commit_hooks): none until the host program sets it
typedef void (*commit_hook)(void *mem, uint64_t size);
static _Atomic(commit_hook) g_hook_map, g_hook_unmap;
void dm_garena_set_commit_hooks(void (*map)(void *mem, uint64_t size), void (*unmap)(void *mem, uint64_t size)) { atomic_store(&g_hook_map, map); atomic_store(&g_hook_unmap, unmap); }
static void commit(uint64_t a, uint64_t sz, const char *what) {
    if (plat_vm_protect((void *)(uintptr_t)a, sz, PLAT_PROT_READ | PLAT_PROT_WRITE)) die("aot: %s %#llx +%#llx: protection failed (errno %d)", what, (unsigned long long)a, (unsigned long long)sz, errno);
    commit_hook h = atomic_load(&g_hook_map);
    if (h) h((void *)(uintptr_t)a, sz);
}
static void decommit(uint64_t a, uint64_t sz) {
    commit_hook h = atomic_load(&g_hook_unmap);
    if (h) h((void *)(uintptr_t)a, sz);   // while the pages are still readable, as the memory SVCs announce theirs (an import under them goes first)
    (void)plat_vm_protect((void *)(uintptr_t)a, sz, PLAT_PROT_NONE);
    (void)plat_vm_discard((void *)(uintptr_t)a, sz);
}

static void arena_init_once(void) {
    if (!plat_vm_reserve_at(DM_AOT_ARENA_BASE, DM_AOT_ARENA_SIZE))
        die("[RED] aot: cannot reserve the arena, %llu GiB at %#llx below 2^39 (errno %d): something else is mapped there",
            (unsigned long long)(DM_AOT_ARENA_SIZE >> 30), (unsigned long long)DM_AOT_ARENA_BASE, errno);
    g_bump = DM_AOT_ARENA_BASE + page();   // the first page stays inaccessible: DM_AOT_HOST_RETURN is in it
    g_end = DM_AOT_ARENA_BASE + DM_AOT_ARENA_SIZE;
    garena_init();
    const char *chk = getenv("DM_AOT_ARENA_CHECK");
    atomic_store(&dm_aot_arena_check, chk && *chk && strcmp(chk, "0") ? 1 : 0);
    if (atomic_load(&dm_aot_arena_check)) LOG("aot: DM_AOT_ARENA_CHECK: every host-call thunk's pointers and every pointer argument of a guest call are checked against the arena");
    // The host-function page (2.4): a slot of 16 bytes per host entry, readable (the slots hold brk words) and never executable
    size_t n = round_up(dm_aot_hostfn_size() ? dm_aot_hostfn_size() : 1, page());
    uint64_t hp = take_fresh(n, page());
    if (plat_vm_protect((void *)(uintptr_t)hp, n, PLAT_PROT_READ | PLAT_PROT_WRITE)) die("aot: host-function page at %#llx: protection failed (errno %d)", (unsigned long long)hp, errno);
    dm_aot_hostfn_init(hp, n);
    // The stub page (aot.h): a slot per named stub of an unimplemented import, brk words, read-only
    const size_t sn = round_up((size_t)DM_AOT_STUBS * DM_AOT_HOSTFN_SLOT, page());
    g_stub_page = take_fresh(sn, page());
    if (plat_vm_protect((void *)(uintptr_t)g_stub_page, sn, PLAT_PROT_READ | PLAT_PROT_WRITE)) die("aot: stub page at %#llx: protection failed (errno %d)", (unsigned long long)g_stub_page, errno);
    for (size_t i = 0; i < sn / 4; i++) ((uint32_t *)(uintptr_t)g_stub_page)[i] = DM_AOT_HOSTFN_POISON;
    if (plat_vm_protect((void *)(uintptr_t)g_stub_page, sn, PLAT_PROT_READ)) die("aot: stub page at %#llx: read-only protection failed (errno %d)", (unsigned long long)g_stub_page, errno);
    LOG("aot: arena %#llx..%#llx (%llu GiB, below 2^39), host-function page %#llx (%zu slots), stub page %#llx (%d slots)", (unsigned long long)DM_AOT_ARENA_BASE,
        (unsigned long long)(DM_AOT_ARENA_BASE + DM_AOT_ARENA_SIZE), (unsigned long long)(DM_AOT_ARENA_SIZE >> 30), (unsigned long long)hp, hle_thunks_count,
        (unsigned long long)g_stub_page, DM_AOT_STUBS);
}
void dm_aot_arena_init(void) { pthread_once(&g_once, arena_init_once); }

int dm_aot_in_arena(uint64_t a, uint64_t n) {
    return a >= DM_AOT_ARENA_BASE && n <= DM_AOT_ARENA_SIZE && a - DM_AOT_ARENA_BASE <= DM_AOT_ARENA_SIZE - n;
}

void *dm_guest_reserve(size_t n) {
    dm_aot_arena_init();
    uint64_t sz = round_up(n ? n : 1, page());
    int reused;
    pthread_mutex_lock(&g_mem_lock);
    uint64_t a = take(sz, sz >= 0x200000 ? 0x200000 : page(), &reused);   // a large block on a 2 MiB boundary, as Horizon aligns its regions; a given-back range is inaccessible
    pthread_mutex_unlock(&g_mem_lock);
    return (void *)(uintptr_t)a;
}

void *dm_guest_alloc(size_t n) {
    dm_aot_arena_init();
    uint64_t sz = round_up(n ? n : 1, page());
    int reused;
    pthread_mutex_lock(&g_mem_lock);
    uint64_t a = take(sz, page(), &reused);
    pthread_mutex_unlock(&g_mem_lock);
    commit(a, sz, "guest memory");
    if (reused) memset((void *)(uintptr_t)a, 0, sz);   // discarded pages need not read as zero (MADV_FREE keeps them until the kernel reclaims them)
    return (void *)(uintptr_t)a;
}

void dm_guest_free(void *p, size_t n) {
    if (!p) return;
    uint64_t a = (uint64_t)(uintptr_t)p, sz = round_up(n ? n : 1, page());
    if (!dm_aot_in_arena(a, sz) || a % page()) die("aot: dm_guest_free(%p, %zu): not an arena range", p, n);
    decommit(a, sz);
    pthread_mutex_lock(&g_mem_lock);
    give_back(a, sz);
    pthread_mutex_unlock(&g_mem_lock);
}

uint8_t *dm_aot_image_map(const uint8_t *bytes, size_t len, size_t bss, uint64_t ro_end, size_t *mapped) {
    dm_aot_arena_init();
    uint64_t sz = round_up(len + bss, page());
    pthread_mutex_lock(&g_mem_lock);
    uint64_t a = take_fresh(sz, 0x10000);
    pthread_mutex_unlock(&g_mem_lock);
    uint8_t *b = (uint8_t *)(uintptr_t)a;
    if (plat_vm_protect(b, sz, PLAT_PROT_READ | PLAT_PROT_WRITE)) die("aot: module image at %p: protection failed (errno %d)", (void *)b, errno);
    memcpy(b, bytes, len);   // bss: fresh pages are zero
    ro_end &= ~(uint64_t)(page() - 1);
    if (ro_end > sz) ro_end = sz;
    if (ro_end && plat_vm_protect(b, ro_end, PLAT_PROT_READ)) die("aot: module image at %p: read-only protection failed (errno %d)", (void *)b, errno);
    *mapped = sz;
    return b;
}

// ---------- The garena: host memory the guest sees (aot.h, "Host memory the guest sees") ----------
// Small blocks: size classes up to 16 KiB, every one a multiple of 16, about four to a doubling (a block wastes at most a fifth of itself). Each class takes 64 KiB slabs
// of its own from the arena, 64 KiB-aligned and kept readable and writable for good; a slab's class is in g_ga_slab, one byte per 64 KiB of the arena, so free and size
// find a block's class from its address alone. A block lies at a multiple of its size within its slab, so it is aligned to the largest power of two dividing that size:
// an alignment above 16 takes the smallest class that is a multiple of it. Freed blocks go on their class's list (the link in the block's first word); a class's newest
// slab is handed out from its start (fresh pages, zero) until it is used up.
// Large blocks: whole pages, taken from the arena as dm_guest_alloc takes them (a range given back first), each recorded with its size in g_big (open addressing). A freed
// one of at most 1 MiB is kept, readable and writable, on the list of its page count, for the next request of that size, up to 64 MiB kept in all; any other is decommitted
// (the unmap hook first) and given back to the arena
static const uint32_t k_ga_class[] = {16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024,
                                      1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192, 10240, 12288, 14336, 16384};
enum { GA_NCLASS = sizeof k_ga_class / sizeof *k_ga_class, GA_SLAB = 0x10000, GA_SMALL_MAX = 16384, GA_CACHE_PAGES = 256 };
#define GA_CACHE_MAX (64ull << 20)
static uint8_t g_ga_slab[DM_AOT_ARENA_SIZE / GA_SLAB];   // class index + 1 of the slab at that 64 KiB of the arena, 0: none (bss: untouched parts cost nothing)
static struct { pthread_mutex_t mu; void *free; uint64_t bump, end; uint64_t slabs; } g_ga[GA_NCLASS];
typedef struct { uint64_t a, n; } big_t;
static pthread_mutex_t g_big_mu = PTHREAD_MUTEX_INITIALIZER;
static big_t *g_big; static size_t g_big_cap, g_big_n;          // live large blocks, keyed by address (g_big_mu)
static void *g_big_cache[GA_CACHE_PAGES + 1]; static uint64_t g_big_cached;   // freed large blocks kept, by page count (g_big_mu)

static void garena_init(void) { for (int i = 0; i < GA_NCLASS; i++) pthread_mutex_init(&g_ga[i].mu, NULL); }
static int ga_class_of(size_t n, size_t align) {
    for (int i = 0; i < GA_NCLASS; i++) if (k_ga_class[i] >= n && k_ga_class[i] % align == 0) return i;
    return -1;
}
static int ga_slab_class(uint64_t a) { return dm_aot_in_arena(a, 1) ? (int)g_ga_slab[(a - DM_AOT_ARENA_BASE) / GA_SLAB] - 1 : -1; }
static size_t big_slot(uint64_t a) { uint64_t h = (a >> 12) * 0x9E3779B97F4A7C15ull; return (size_t)(h >> 20) & (g_big_cap - 1); }
static void big_put(uint64_t a, uint64_t n) {   // g_big_mu held
    if (2 * (g_big_n + 1) > g_big_cap) {
        size_t old = g_big_cap; big_t *t = g_big;
        g_big_cap = old ? 2 * old : 1024;
        if (!(g_big = calloc(g_big_cap, sizeof *g_big))) die("aot: the garena's table of large blocks: out of host memory");
        g_big_n = 0;
        for (size_t i = 0; i < old; i++) if (t[i].a) big_put(t[i].a, t[i].n);
        free(t);
    }
    size_t i = big_slot(a);
    while (g_big[i].a) i = (i + 1) & (g_big_cap - 1);
    g_big[i] = (big_t){a, n}; g_big_n++;
}
static uint64_t big_find(uint64_t a, int remove) {   // the block's size, 0 when a starts none; removed when asked (g_big_mu held)
    if (!g_big_cap) return 0;
    size_t i = big_slot(a);
    while (g_big[i].a && g_big[i].a != a) i = (i + 1) & (g_big_cap - 1);
    if (!g_big[i].a) return 0;
    const uint64_t n = g_big[i].n;
    if (remove) {   // no tombstones: the rest of the cluster (up to its first empty slot) is put again, each entry landing at or before where it was
        size_t end = i;
        do end = (end + 1) & (g_big_cap - 1); while (g_big[end].a);
        g_big[i].a = 0; g_big_n--;
        for (size_t j = (i + 1) & (g_big_cap - 1); j != end; j = (j + 1) & (g_big_cap - 1)) {
            const big_t e = g_big[j]; g_big[j].a = 0; g_big_n--; big_put(e.a, e.n);
        }
    }
    return n;
}

static void *ga_small(int c, int zero) {
    const uint32_t size = k_ga_class[c];
    pthread_mutex_lock(&g_ga[c].mu);
    void *p = g_ga[c].free;
    int fresh = 0;
    if (p) g_ga[c].free = *(void **)p;
    else {
        if (g_ga[c].bump + size > g_ga[c].end) {   // a new slab
            int reused;
            pthread_mutex_lock(&g_mem_lock);
            const uint64_t s = take(GA_SLAB, GA_SLAB, &reused);
            g_ga_slab[(s - DM_AOT_ARENA_BASE) / GA_SLAB] = (uint8_t)(c + 1);
            pthread_mutex_unlock(&g_mem_lock);
            commit(s, GA_SLAB, "garena slab");
            if (reused) memset((void *)(uintptr_t)s, 0, GA_SLAB);
            g_ga[c].bump = s; g_ga[c].end = s + GA_SLAB / size * size; g_ga[c].slabs++;
        }
        p = (void *)(uintptr_t)g_ga[c].bump; g_ga[c].bump += size; fresh = 1;
    }
    pthread_mutex_unlock(&g_ga[c].mu);
    if (zero && !fresh) memset(p, 0, size);
    return p;
}
static void *ga_big(size_t n, size_t align, int zero) {
    const uint64_t sz = round_up(n ? n : 1, page()), al = align > page() ? align : page();
    void *p = NULL;
    pthread_mutex_lock(&g_big_mu);
    if (al == page() && sz / page() <= GA_CACHE_PAGES && (p = g_big_cache[sz / page()])) { g_big_cache[sz / page()] = *(void **)p; g_big_cached -= sz; }
    pthread_mutex_unlock(&g_big_mu);
    if (p) { if (zero) memset(p, 0, sz); }
    else {
        int reused;
        pthread_mutex_lock(&g_mem_lock);
        const uint64_t a = take(sz, al, &reused);
        pthread_mutex_unlock(&g_mem_lock);
        commit(a, sz, "garena block");
        if (reused && zero) memset((void *)(uintptr_t)a, 0, sz);
        p = (void *)(uintptr_t)a;
    }
    pthread_mutex_lock(&g_big_mu);
    big_put((uint64_t)(uintptr_t)p, sz);
    pthread_mutex_unlock(&g_big_mu);
    return p;
}
static void *ga_alloc(size_t n, size_t align, int zero) {
    dm_aot_arena_init();
    if (align < 16) align = 16;
    if (align & (align - 1)) die("aot: garena alignment %zu is not a power of two", align);
    const int c = n <= GA_SMALL_MAX && align <= GA_SMALL_MAX ? ga_class_of(n ? n : 1, align) : -1;
    return c >= 0 ? ga_small(c, zero) : ga_big(n, align, zero);
}
void *dm_garena_alloc(size_t n, size_t align) { return ga_alloc(n, align, 1); }
void *dm_garena_malloc(size_t n, size_t align) { return ga_alloc(n, align, 0); }

size_t dm_garena_size(const void *p) {
    const uint64_t a = (uint64_t)(uintptr_t)p;
    const int c = p ? ga_slab_class(a) : -1;
    if (c >= 0) {   // a block's start (a slab's tail past its last block is none)
        const uint64_t off = (a - DM_AOT_ARENA_BASE) % GA_SLAB;
        return off % k_ga_class[c] || off >= GA_SLAB / k_ga_class[c] * k_ga_class[c] ? 0 : k_ga_class[c];
    }
    if (!p || !dm_aot_in_arena(a, 1)) return 0;
    pthread_mutex_lock(&g_big_mu);
    const uint64_t n = big_find(a, 0);
    pthread_mutex_unlock(&g_big_mu);
    return (size_t)n;
}

void dm_garena_free(void *p) {
    if (!p) return;
    const uint64_t a = (uint64_t)(uintptr_t)p;
    const int c = ga_slab_class(a);
    if (c >= 0) {
        if (!dm_garena_size(p)) die("aot: dm_garena_free(%p): inside a slab of %u-byte blocks, not a block's start", p, k_ga_class[c]);
        pthread_mutex_lock(&g_ga[c].mu);
        *(void **)p = g_ga[c].free; g_ga[c].free = p;
        pthread_mutex_unlock(&g_ga[c].mu);
        return;
    }
    pthread_mutex_lock(&g_big_mu);
    const uint64_t sz = dm_aot_in_arena(a, 1) ? big_find(a, 1) : 0;
    if (!sz) { pthread_mutex_unlock(&g_big_mu); die("aot: dm_garena_free(%p): no garena block starts there", p); }
    const int keep = sz / page() <= GA_CACHE_PAGES && g_big_cached + sz <= GA_CACHE_MAX;
    if (keep) { *(void **)p = g_big_cache[sz / page()]; g_big_cache[sz / page()] = p; g_big_cached += sz; }
    pthread_mutex_unlock(&g_big_mu);
    if (keep) return;
    decommit(a, sz);
    pthread_mutex_lock(&g_mem_lock);
    give_back(a, sz);
    pthread_mutex_unlock(&g_mem_lock);
}

void *dm_garena_realloc(void *p, size_t n, size_t align) {
    if (!p) return dm_garena_malloc(n, align);
    if (!n) { dm_garena_free(p); return NULL; }
    const size_t old = dm_garena_size(p);
    if (!old) die("aot: dm_garena_realloc(%p): no garena block starts there", p);
    if (old >= n && !((uintptr_t)p & ((align < 16 ? 16 : align) - 1))) return p;   // fits where it is
    void *q = dm_garena_malloc(n, align);
    memcpy(q, p, old < n ? old : n);
    dm_garena_free(p);
    return q;
}

char *dm_garena_strdup(const char *s) {
    if (!s) return NULL;
    const size_t n = strlen(s) + 1;
    char *d = dm_garena_malloc(n, 16);
    memcpy(d, s, n);
    return d;
}

// Copies of strings with static storage, found again by the host string's address (open addressing, g_lit_mu)
typedef struct { const char *host, *guest; } lit_t;
static pthread_mutex_t g_lit_mu = PTHREAD_MUTEX_INITIALIZER;
static lit_t *g_lit; static size_t g_lit_cap, g_lit_n;
static size_t lit_slot(const char *s) { return (size_t)(((uint64_t)(uintptr_t)s * 0x9E3779B97F4A7C15ull) >> 24) & (g_lit_cap - 1); }
const char *dm_garena_literal(const char *s) {
    if (!s || dm_aot_in_arena((uint64_t)(uintptr_t)s, 1)) return s;   // already the guest's
    pthread_mutex_lock(&g_lit_mu);
    if (2 * (g_lit_n + 1) > g_lit_cap) {
        size_t old = g_lit_cap; lit_t *t = g_lit;
        g_lit_cap = old ? 2 * old : 256;
        if (!(g_lit = calloc(g_lit_cap, sizeof *g_lit))) die("aot: the garena's string table: out of host memory");
        for (size_t i = 0; i < old; i++) if (t[i].host) { size_t k = lit_slot(t[i].host); while (g_lit[k].host) k = (k + 1) & (g_lit_cap - 1); g_lit[k] = t[i]; }
        free(t);
    }
    size_t k = lit_slot(s);
    while (g_lit[k].host && g_lit[k].host != s) k = (k + 1) & (g_lit_cap - 1);
    if (!g_lit[k].host) { g_lit[k].host = s; g_lit[k].guest = dm_garena_strdup(s); g_lit_n++; }
    const char *g = g_lit[k].guest;
    pthread_mutex_unlock(&g_lit_mu);
    return g;
}

void *dm_garena_static(void *_Atomic *slot, const void *init, size_t n) {
    void *p = atomic_load_explicit(slot, memory_order_acquire);
    if (p) return p;
    void *q = dm_garena_alloc(n, 16), *expect = NULL;
    if (init) memcpy(q, init, n);
    if (atomic_compare_exchange_strong_explicit(slot, &expect, q, memory_order_acq_rel, memory_order_acquire)) return q;
    dm_garena_free(q);   // another thread made it first
    return expect;
}

// The scoped regions: one per host thread, made at its first use, given back at its exit (g_gs_key's destructor). Overflow blocks (taken from the garena when an allocation
// does not fit) are a stack of their own, popped by the release of a mark taken before them
typedef struct gs_ovf { struct gs_ovf *next; void *p; } gs_ovf;
typedef struct { uint8_t *base; uint32_t top, novf; gs_ovf *ovf; int red; } gs_region;
static _Thread_local gs_region *t_gs;
static pthread_key_t g_gs_key;
static pthread_once_t g_gs_once = PTHREAD_ONCE_INIT;
static void gs_pop_to(gs_region *r, uint32_t novf) {
    while (r->novf > novf && r->ovf) { gs_ovf *o = r->ovf; r->ovf = o->next; r->novf--; dm_garena_free(o->p); free(o); }
}
static void gs_destroy(void *arg) {   // on the exiting thread itself
    gs_region *r = arg;
    gs_pop_to(r, 0);
    dm_guest_free(r->base, DM_GSCOPE_SIZE);
    free(r);
    t_gs = NULL;   // a later destructor of this thread that takes a scoped buffer gets a region of its own again
}
static void gs_key_make(void) { if (pthread_key_create(&g_gs_key, gs_destroy)) die("aot: the scoped regions' thread key"); }
static gs_region *gs_self(void) {
    if (t_gs) return t_gs;
    pthread_once(&g_gs_once, gs_key_make);
    gs_region *r = calloc(1, sizeof *r);
    if (!r) die("aot: a scoped region: out of host memory");
    r->base = dm_guest_alloc(DM_GSCOPE_SIZE);
    pthread_setspecific(g_gs_key, r);
    return t_gs = r;
}
dm_gscope_t dm_gscope_mark(void) { gs_region *r = gs_self(); return (uint64_t)r->top | (uint64_t)r->novf << 32; }
void *dm_gscope_alloc(size_t n) {
    gs_region *r = gs_self();
    const size_t k = n ? (n + 15) & ~(size_t)15 : 16;
    if (k <= DM_GSCOPE_SIZE - r->top) {
        uint8_t *p = r->base + r->top;
        r->top += (uint32_t)k;
        memset(p, 0, k);
        return p;
    }
    if (!r->red++)
        LOG("[RED] aot: a scoped guest buffer of %zu bytes does not fit in this thread's %u KiB region (%u bytes in use): taken from the garena instead, and from now on for "
            "every such buffer of this thread (aot.h DM_GSCOPE_SIZE)", n, DM_GSCOPE_SIZE >> 10, r->top);
    gs_ovf *o = malloc(sizeof *o);
    if (!o) die("aot: a scoped buffer: out of host memory");
    o->p = dm_garena_alloc(n ? n : 1, 16); o->next = r->ovf; r->ovf = o; r->novf++;
    return o->p;
}
void dm_gscope_release(dm_gscope_t mark) {
    gs_region *r = t_gs;
    if (!r) return;
    const uint32_t top = (uint32_t)mark, novf = (uint32_t)(mark >> 32);
    if (top < r->top) r->top = top;   // an inner mark released late (a longjmp skipped its block) never moves the top up
    gs_pop_to(r, novf);
}

// ---------- The guard (aot.h, DM_AOT_ARENA_CHECK) ----------
// The guest-call side: DM_GUEST_CALL's pointer arguments (dm_gc_arg). dm_guest_call's own arguments are uint64_t at its sites, so the guard cannot tell a pointer there;
// those sites pass arena memory or integers only: hle_guestthread.c gcall (the ThreadType, the stack and the entry's argument, all garena or the SDK's malloc), hle_ro.c
// (an NRO's init and fini: no argument), loader.c's DT_INIT calls (none), main_x86.c (the main thread's handle, argc, and argv in the arena), hle_glslc.c (x8: a
// dm_guest_alloc block) and hle_audren.c's GenerateCommand (the SDK's own render system and buffers)
void dm_aot_arena_flag(const char *site, unsigned line, int pos, uint64_t v, const char *what) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static struct { const char *site; unsigned line; int pos; } seen[512];
    static int nseen;
    pthread_mutex_lock(&mu);
    int first = 1;
    for (int i = 0; i < nseen && first; i++) if (seen[i].site == site && seen[i].line == line && seen[i].pos == pos) first = 0;
    if (first && nseen < (int)(sizeof seen / sizeof *seen)) seen[nseen++] = (typeof(seen[0])){site, line, pos};
    pthread_mutex_unlock(&mu);
    if (!first) return;
    const char *sym = plat_addr_symbol((const void *)(uintptr_t)v, NULL);
    LOG("[RED] aot: arena check: %s %d at %s:%u is %#llx%s%s%s, outside the arena %#llx..%#llx: host memory handed to the guest (aot.h, \"Host memory the guest sees\")",
        what, pos, site, line, (unsigned long long)v, sym ? " (" : "", sym ? sym : "", sym ? ")" : "", (unsigned long long)DM_AOT_ARENA_BASE,
        (unsigned long long)(DM_AOT_ARENA_BASE + DM_AOT_ARENA_SIZE));
}

// ---------- Imports: host functions and unimplemented imports at guest-visible addresses (design 2.4) ----------
uint64_t unimpl_stubs[DM_AOT_STUBS];   // hle.c resolve_import hands out &unimpl_stubs[k]; on Apple entry.S assembles them, here they are identities only

uint64_t dm_aot_stub_addr(unsigned k) {
    dm_aot_arena_init();
    if (k >= DM_AOT_STUBS) die("aot: stub %u of %d", k, DM_AOT_STUBS);
    return g_stub_page + (uint64_t)k * DM_AOT_HOSTFN_SLOT;
}

long dm_aot_stub_index(uint64_t guest) {
    if (!g_stub_page || guest < g_stub_page || (guest - g_stub_page) % DM_AOT_HOSTFN_SLOT) return -1;
    const uint64_t k = (guest - g_stub_page) / DM_AOT_HOSTFN_SLOT;
    return k < DM_AOT_STUBS ? (long)k : -1;
}

uint64_t dm_aot_bind(const module_t *m, const char *name, uint64_t v) {
    if (!m || !dm_aot_in_arena((uint64_t)(uintptr_t)m->base, 1)) return v;                  // a module the engine did not map: host addresses, as Apple binds them
    if (!v || module_of((const void *)(uintptr_t)v) || dm_aot_in_arena(v, 1)) return v;      // undefined weak (0), a loaded module's definition, guest memory
    const uint64_t s0 = (uint64_t)(uintptr_t)unimpl_stubs;
    if (v >= s0 && v < s0 + sizeof unimpl_stubs) return dm_aot_stub_addr((unsigned)((v - s0) / sizeof *unimpl_stubs));   // hle.c's named stub k
    const uint64_t slot = dm_aot_hostfn_find((const void *)(uintptr_t)v);
    if (slot) return slot;   // an entry's C function (HLE, HLE_FORCE, NVN, VK, GL, EGL, PASS): its slot
    const char *sym = plat_addr_symbol((const void *)(uintptr_t)v, NULL);
    die("[RED] aot: %s imports %s, which binds to the host function at %#llx (%s), and that function has no host-call thunk (dm/host/tools/gen_hle_thunks.py makes one per "
        "entry of the import tables and per HLE_LINKAGE function of the sources nxhost compiles): compiled code could not call it", m->name, name, (unsigned long long)v,
        sym ? sym : "no symbol");
}

// ---------- Host functions handed to the guest (aot.h) ----------
uint64_t loader_named_stub(const char *name);   // loader.c: hle.c's named stub for name (resolve_import under the loader's lock), as &unimpl_stubs[k]

// A stub slot for name: hle.c's named stub (the one an import of that name nothing implements would bind to), at its slot of the stub page
static uint64_t stub_slot(const char *name) {
    const uint64_t v = loader_named_stub(name), s0 = (uint64_t)(uintptr_t)unimpl_stubs;
    if (v >= s0 && v < s0 + sizeof unimpl_stubs) return dm_aot_stub_addr((unsigned)((v - s0) / sizeof *unimpl_stubs));
    const uint64_t slot = dm_aot_hostfn_find((const void *)(uintptr_t)v);   // the import table implements the name: that entry
    if (slot) return slot;
    die("[RED] aot: the stub for %s is the host function at %#llx, which has no host-call thunk", name, (unsigned long long)v);
}

static void *fnptr(const void *fn, const char *name) {
    const uint64_t v = (uint64_t)(uintptr_t)fn;
    if (!v || dm_aot_in_arena(v, 1) || module_of(fn)) return (void *)fn;   // nothing, or already the guest's: guest memory, a loaded module, a slot or a stub slot
    const uint64_t s0 = (uint64_t)(uintptr_t)unimpl_stubs;
    if (v >= s0 && v < s0 + sizeof unimpl_stubs) return (void *)(uintptr_t)dm_aot_stub_addr((unsigned)((v - s0) / sizeof *unimpl_stubs));   // hle.c's named stub k
    const uint64_t slot = dm_aot_hostfn_find(fn);
    if (slot) return (void *)(uintptr_t)slot;
    if (name) return (void *)(uintptr_t)stub_slot(strdup(name));   // a lookup by name the host does not implement: that name's stub, quietly (its first call reports
                                                                   // it); hle.c keeps the name, and the guest's string may not live as long
    // A host function with no thunk: a site that hands the guest a function no generator rule covers. Loud, once per function, and a stub that answers 0
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static const void *seen[64]; static int nseen;
    const char *sym = plat_addr_symbol(fn, NULL);
    char buf[160];
    snprintf(buf, sizeof buf, "host function %s", sym ? sym : "?");
    pthread_mutex_lock(&mu);
    int first = 1;
    for (int i = 0; i < nseen; i++) if (seen[i] == fn) { first = 0; break; }
    if (first && nseen < 64) seen[nseen++] = fn;
    pthread_mutex_unlock(&mu);
    if (first)
        LOG("[RED] aot: the host hands the guest the host function at %p (%s), which has no host-call thunk (gen_hle_thunks.py: an import-table entry or an HLE_LINKAGE "
            "function): the guest gets a stub that answers 0", fn, sym ? sym : "no symbol");
    return (void *)(uintptr_t)stub_slot(strdup(buf));   // hle.c keeps the name
}

void *dm_guest_fnptr(const void *fn) { return fnptr(fn, NULL); }

void *dm_guest_fnptr_named(const void *fn, const char *name) { return fnptr(fn, name); }

// ---------- Packs ----------
typedef struct { uint64_t native; uint64_t off; } map_t;   // a native address and the module offset of its guest function or instruction
struct dm_aot_pack {
    void *lib;
    const nxaot_module_desc *desc;
    module_t *mod;
    uint64_t base, size, text_end;   // the image's guest range: [base, base + size), its text [base, base + text_end)
    uint64_t code_lo, code_hi;       // the library's code in the host
    map_t *fns, *lines;              // sorted by native address
    size_t nfns, nlines;
    char name[16];
};
#define MAX_PACKS 4096   // FINAL FANTASY X/X-2 registers 1,432 NROs (services.md); one pack per module for now
static dm_aot_pack *g_packs[MAX_PACKS];
static _Atomic int g_npacks;     // published with release after g_packs[n] is written; packs are never removed
static pthread_mutex_t g_pack_lock = PTHREAD_MUTEX_INITIALIZER;

static int map_cmp(const void *a, const void *b) {
    uint64_t x = ((const map_t *)a)->native, y = ((const map_t *)b)->native;
    return x < y ? -1 : x > y;
}

#if defined(_WIN32)
static void *lib_open(const char *path, char *err, size_t errn) {
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, (int)(sizeof w / sizeof *w))) { snprintf(err, errn, "%s: path not convertible", path); return NULL; }
    HMODULE h = LoadLibraryW(w);
    if (!h) snprintf(err, errn, "%s: LoadLibrary failed (error %lu)", path, (unsigned long)GetLastError());
    return (void *)h;
}
static void *lib_sym(void *lib, const char *name) { return (void *)GetProcAddress((HMODULE)lib, name); }
static void lib_close(void *lib) { FreeLibrary((HMODULE)lib); }
#else
static void *lib_open(const char *path, char *err, size_t errn) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) snprintf(err, errn, "%s", dlerror());
    return h;
}
static void *lib_sym(void *lib, const char *name) { return dlsym(lib, name); }
static void lib_close(void *lib) { dlclose(lib); }
#endif
// The library's code in the host: the executable mapping that holds its enter trampoline (one per library: lld puts the text in one segment), widened to
// every function the map names
static void lib_code_range(const dm_aot_pack *p, uint64_t *lo, uint64_t *hi) {
    uint64_t base = 0, size = 0; int prot = 0;
    const uint64_t at = (uint64_t)(uintptr_t)p->desc->enter;
    *lo = UINT64_MAX; *hi = 0;
    if (!plat_vm_query(at, &base, &size, &prot) && at >= base && at - base < size && (prot & PLAT_PROT_EXEC)) { *lo = base; *hi = base + size; }
    for (size_t i = 0; i < p->nfns; i++) {
        if (p->fns[i].native < *lo) *lo = p->fns[i].native;
        if (p->fns[i].native >= *hi) *hi = p->fns[i].native + 1;
    }
}

dm_aot_pack *dm_aot_pack_load(const char *path, module_t *m, const dm_aot_identity *id, char *err, size_t errn) {
    void *lib = lib_open(path, err, errn);
    if (!lib) return NULL;
    const nxaot_module_desc *d = lib_sym(lib, "nxaot_module");
    const char *bad = NULL; char why[400];
    if (!d) bad = "it has no nxaot_module descriptor (not a pack of nxaot build)";
    else if (d->abi != NXAOT_MODULE_ABI) { snprintf(why, sizeof why, "its descriptor has ABI %u, this host reads %u", d->abi, NXAOT_MODULE_ABI); bad = why; }
    else if (memcmp(d->sha256, id->sha256, 32) || d->image_size != id->image_size || d->text_end != id->text_end || d->bss_size != id->bss_size) {
        char hp[65], hi[65];   // the pack's image and the mapped one, by SHA-256
        for (int i = 0; i < 32; i++) { snprintf(hp + 2 * i, 3, "%02x", d->sha256[i]); snprintf(hi + 2 * i, 3, "%02x", id->sha256[i]); }
        snprintf(why, sizeof why, "it was compiled from another image than %s: the pack's SHA-256 %s, the image's %s (image %#llx / %#llx bytes, text %#llx / %#llx, bss %#llx / %#llx)",
                 m->name, hp, hi, (unsigned long long)d->image_size, (unsigned long long)id->image_size, (unsigned long long)d->text_end, (unsigned long long)id->text_end,
                 (unsigned long long)d->bss_size, (unsigned long long)id->bss_size);
        bad = why;
    }
    if (bad) { snprintf(err, errn, "%s: %s", path, bad); lib_close(lib); return NULL; }
    dm_aot_pack *p = calloc(1, sizeof *p);
    if (!p) die("aot: no memory for a pack");
    p->lib = lib; p->desc = d; p->mod = m;
    p->base = (uint64_t)(uintptr_t)m->base; p->size = round_up(id->image_size + id->bss_size, page()); p->text_end = id->text_end;
    snprintf(p->name, sizeof p->name, "%s", m->name);
    *d->base = p->base;   // 3.3: return addresses and every pc-built address are base + module offset
    // The function map and the line table (3.5; ABI 3: per function a run of {native - body, guest offset}), image-relative in the library, as absolute native
    // addresses sorted for the fault reports
    p->nfns = (size_t)d->fn_count;
    p->fns = malloc((p->nfns ? p->nfns : 1) * sizeof *p->fns);
    p->nlines = 0;
    for (size_t i = 0; d->fn_lines && d->lines && i < p->nfns; i++) p->nlines += d->fn_lines[i].count;
    p->lines = malloc((p->nlines ? p->nlines : 1) * sizeof *p->lines);
    if (!p->fns || !p->lines) die("aot: no memory for a pack's tables");
    for (size_t i = 0, k = 0; i < p->nfns; i++) {
        const nxaot_fn_entry *f = &d->fns[i];
        const uint64_t body = (uint64_t)(uintptr_t)&f->body + (uint64_t)(int64_t)f->body;
        p->fns[i] = (map_t){body, f->offset};
        for (uint32_t j = 0; d->fn_lines && d->lines && j < d->fn_lines[i].count; j++, k++) {
            const nxaot_line_entry *e = &d->lines[d->fn_lines[i].first + j];
            p->lines[k] = (map_t){body + e->native, e->offset};
        }
    }
    qsort(p->fns, p->nfns, sizeof *p->fns, map_cmp);
    qsort(p->lines, p->nlines, sizeof *p->lines, map_cmp);
    lib_code_range(p, &p->code_lo, &p->code_hi);
    pthread_mutex_lock(&g_pack_lock);
    int n = atomic_load_explicit(&g_npacks, memory_order_relaxed);
    if (n >= MAX_PACKS) die("aot: more than %d packs", MAX_PACKS);
    if (n && (d->fpcr & 0x07C80000u) != (g_packs[0]->desc->fpcr & 0x07C80000u))
        LOG("[RED] aot: %s is specialised for FPCR %#x, the first pack for %#x: its code runs in the JIT fallback under the other (design 7.1)", path, d->fpcr, g_packs[0]->desc->fpcr);
    g_packs[n] = p;
    atomic_store_explicit(&g_npacks, n + 1, memory_order_release);
    pthread_mutex_unlock(&g_pack_lock);
    LOG("aot: pack %s for %s at %p: %llu functions, %u lookup slots, %zu line entries, %llu returns, %llu gate sites, code %#llx..%#llx; %s", path, m->name,
        (void *)m->base, (unsigned long long)d->fn_count, d->lookup_mask + 1, p->nlines, (unsigned long long)d->return_count, (unsigned long long)d->gate_count,
        (unsigned long long)p->code_lo, (unsigned long long)p->code_hi, d->tool ? d->tool : "");
    return p;
}

int dm_aot_pack_check_gates(const module_t *m, const dm_aot_gate_row *rows, int n, int *unserved, char *err, size_t errn) {
    const dm_aot_pack *p = NULL;
    dm_aot_find((uint64_t)(uintptr_t)m->base, &p);
    *unserved = 0;
    if (!p) { snprintf(err, errn, "%s has no pack loaded", m->name); return 1; }
    const nxaot_module_desc *d = p->desc;
    // Every site of the pack's is a row of the manifest, with its kind and argument: the code calls the host there, and the gate table must hold the gate
    for (uint64_t i = 0; i < d->gate_count; i++) {
        const nxaot_gate_site *g = &d->gates[i];
        int found = 0;
        for (int k = 0; k < n && !found; k++) found = rows[k].offset == g->offset && rows[k].kind == g->kind && rows[k].arg == g->arg;
        if (!found) {
            snprintf(err, errn, "the pack of %s calls a gate at %s+%#x (%s, argument %#llx) that its manifest has no row for: a pack of another manifest", m->name, m->name,
                     g->offset, g->kind == NXAOT_GATE_OBS ? "an observation gate" : "a spin wait's back-edge", (unsigned long long)g->arg);
            return 1;
        }
    }
    // Every row of the manifest the pack serves: the ones it does not are a [RED] line each (nxaot build said why: no discovered code holds them)
    for (int k = 0; k < n; k++) {
        const nxaot_gate_site *g = rows[k].offset <= UINT32_MAX ? nxaot_gate_at(d, rows[k].offset) : NULL;
        if (g && g->kind == rows[k].kind && g->arg == rows[k].arg) continue;
        (*unserved)++;
        LOG("[RED] aot: %s's manifest has %s at %s+%#llx that its pack does not call (no compiled code holds it: see nxaot build's log): %s", m->name,
            rows[k].kind == NXAOT_GATE_OBS ? "an observation gate" : "a spin wait's back-edge", m->name, (unsigned long long)rows[k].offset,
#if NXHOST_JIT
            "served by the JIT fallback if it reaches that code (aot_run.c dm_aot_gate_rows), by nothing in compiled code");
#else
            rows[k].kind == NXAOT_GATE_OBS ? "its observer does not run" : "the loop spins without the back-off");
#endif
    }
    return 0;
}

int dm_aot_load_module_pack(module_t *m, const char *dir, const uint8_t sha256[32], uint64_t image_size, uint64_t text_end, uint64_t bss, char *err, size_t errn) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s" DM_AOT_PACK_SUFFIX, dir, m->name);
    dm_aot_identity id;
    memcpy(id.sha256, sha256, 32); id.image_size = image_size; id.text_end = text_end; id.bss_size = bss;
    if (dm_aot_pack_load(path, m, &id, err, errn)) return 0;
    size_t k = strlen(err);   // how to make it, after the reason: from the manifest beside it, or for an NRO (loader.c loader_nro_map) from the NRO file the game loads
    if (m->nro) snprintf(err + k, errn > k ? errn - k : 0, " (nxaot build <the NRO file, %s.nro> -o <object> --shared %s makes the pack of this NRO)", m->name, path);
    else snprintf(err + k, errn > k ? errn - k : 0, " (nxaot build %s/%s.aot.tsv -o <object> --shared %s makes the pack of this image)", dir, m->name, path);
    return 1;
}

const nxaot_module_desc *dm_aot_pack_desc(const dm_aot_pack *p) { return p->desc; }
const void *dm_aot_pack_jit_entry(const dm_aot_pack *p) { return p->desc->jit_entry; }
uint64_t dm_aot_pack_base(const dm_aot_pack *p) { return p->base; }

uint32_t dm_aot_module_fpcr(void) {
    return atomic_load_explicit(&g_npacks, memory_order_acquire) ? g_packs[0]->desc->fpcr : 0;
}

static const dm_aot_pack *pack_of_guest(uint64_t guest) {
    int n = atomic_load_explicit(&g_npacks, memory_order_acquire);
    for (int i = 0; i < n; i++) if (guest - g_packs[i]->base < g_packs[i]->size) return g_packs[i];
    return NULL;
}

const nxaot_lookup_entry *dm_aot_find(uint64_t guest, const dm_aot_pack **pack) {
    const dm_aot_pack *p = pack_of_guest(guest);
    if (pack) *pack = p;
    if (!p) return NULL;
    const nxaot_module_desc *d = p->desc;
    const uint64_t off = guest - p->base;
    const uint32_t mask = d->lookup_mask;
    for (uint32_t h = nxaot_lookup_hash(off, mask), k = 0; k <= mask; h = (h + 1) & mask, k++) {
        const nxaot_lookup_entry *e = &d->lookup[h];
        if (e->offset == off) return e;
        if (e->offset == NXAOT_LOOKUP_EMPTY) return NULL;
    }
    return NULL;
}

const void *dm_aot_entry_of(const nxaot_lookup_entry *e) { return (const uint8_t *)&e->entry + e->entry; }

int dm_aot_is_return(uint64_t guest) {
    const dm_aot_pack *p = pack_of_guest(guest);
    if (!p) return 0;
    uint64_t off = guest - p->base;
    if (off >= UINT32_MAX) return 0;
    size_t lo = 0, hi = (size_t)p->desc->return_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t v = p->desc->returns[mid];
        if (v == off) return 1;
        if (v < off) lo = mid + 1; else hi = mid;
    }
    return 0;
}

const dm_aot_pack *dm_aot_pack_of_code(uint64_t host_pc) {
    int n = atomic_load_explicit(&g_npacks, memory_order_acquire);
    for (int i = 0; i < n; i++) if (host_pc >= g_packs[i]->code_lo && host_pc < g_packs[i]->code_hi) return g_packs[i];
    return NULL;
}

void dm_aot_guest_name(uint64_t guest, char *out, size_t n) {
    const module_t *m = module_of((const void *)(uintptr_t)guest);
    if (m) snprintf(out, n, "%s+%#llx", m->name, (unsigned long long)(guest - (uint64_t)(uintptr_t)m->base));
    else snprintf(out, n, "%#llx", (unsigned long long)guest);
}

// The last entry at or before pc (the tables are sorted), NULL when none
static const map_t *last_at(const map_t *t, size_t n, uint64_t pc) {
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (t[mid].native <= pc) lo = mid + 1; else hi = mid; }
    return lo ? &t[lo - 1] : NULL;
}

int dm_aot_where(uint64_t host_pc, char *out, size_t n) {
    const dm_aot_pack *p = dm_aot_pack_of_code(host_pc);
    if (!p) return 0;
    const map_t *f = last_at(p->fns, p->nfns, host_pc), *l = last_at(p->lines, p->nlines, host_pc);
    if (!f) { snprintf(out, n, "host pc %#llx, in %s's library before its first function", (unsigned long long)host_pc, p->name); return 1; }
    if (l && l->native >= f->native)
        snprintf(out, n, "host pc %#llx, in the function at %s+%#llx, guest instruction %s+%#llx%s", (unsigned long long)host_pc, p->name, (unsigned long long)f->off,
                 p->name, (unsigned long long)l->off, l->native == host_pc ? "" : " (the nearest line entry before it)");
    else snprintf(out, n, "host pc %#llx, in the function at %s+%#llx", (unsigned long long)host_pc, p->name, (unsigned long long)f->off);
    return 1;
}

