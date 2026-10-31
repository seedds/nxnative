// The arena, guest memory and packs of the x86-64 execution engine (docs/design/aot-recompiler.md 5.1 to 5.3, 9.4; aot.h).
// The arena: one reservation of DM_AOT_ARENA_SIZE at DM_AOT_ARENA_BASE, below 2^39 (the Switch's 39-bit address space, which a guest may rely on), made at start
// with plat_vm_reserve_at; nothing in it is accessible until taken. A guest address is the host address of the same byte (5.1): recompiled loads and stores
// use guest addresses directly, and host code keeps using guest pointers as C pointers. The arena's first page stays inaccessible (DM_AOT_HOST_RETURN lies
// in it); the host-function page of aot_hostfn.c follows (2.4); then whatever the host hands the guest: hle_svc.c's guest block (heap, alias and stack
// regions), module images, TLS blocks and thread-local regions (thread.c), the main thread's guest stack. Fresh memory is taken from the bottom up; ranges
// given back (dm_guest_free) are reused first and zeroed then. Pages are 4 KiB on Linux and Windows, as Horizon's, so protections are exact (5.3).
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
#define MAX_FREE 4096
static span_t g_free[MAX_FREE];  // ranges given back, sorted by address, adjacent ones joined (g_mem_lock)
static int g_nfree;

static size_t page(void) { static size_t pg; if (!pg) pg = plat_page_size(); return pg; }
static uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// Fresh arena space, aligned, inaccessible (g_mem_lock held)
static uint64_t take_fresh(uint64_t n, uint64_t align) {
    uint64_t a = round_up(g_bump, align);
    if (a > g_end || n > g_end - a)
        die("[RED] aot: the arena (%llu GiB at %#llx) is full: %#llx more bytes asked, %#llx left (DM_ALIAS_SIZE larger than the arena holds?)",
            (unsigned long long)(DM_AOT_ARENA_SIZE >> 30), (unsigned long long)DM_AOT_ARENA_BASE, (unsigned long long)n, (unsigned long long)(g_end - g_bump));
    g_bump = a + n;
    return a;
}

static void arena_init_once(void) {
    if (!plat_vm_reserve_at(DM_AOT_ARENA_BASE, DM_AOT_ARENA_SIZE))
        die("[RED] aot: cannot reserve the arena, %llu GiB at %#llx below 2^39 (errno %d): something else is mapped there",
            (unsigned long long)(DM_AOT_ARENA_SIZE >> 30), (unsigned long long)DM_AOT_ARENA_BASE, errno);
    g_bump = DM_AOT_ARENA_BASE + page();   // the first page stays inaccessible: DM_AOT_HOST_RETURN is in it
    g_end = DM_AOT_ARENA_BASE + DM_AOT_ARENA_SIZE;
    // The host-function page (2.4): a slot of 16 bytes per host entry, readable (the slots hold brk words) and never executable
    size_t n = round_up(dm_aot_hostfn_size() ? dm_aot_hostfn_size() : 1, page());
    uint64_t hp = take_fresh(n, page());
    if (plat_vm_protect((void *)(uintptr_t)hp, n, PLAT_PROT_READ | PLAT_PROT_WRITE)) die("aot: host-function page at %#llx: protection failed (errno %d)", (unsigned long long)hp, errno);
    dm_aot_hostfn_init(hp, n);
    LOG("aot: arena %#llx..%#llx (%llu GiB, below 2^39), host-function page %#llx (%zu slots)", (unsigned long long)DM_AOT_ARENA_BASE,
        (unsigned long long)(DM_AOT_ARENA_BASE + DM_AOT_ARENA_SIZE), (unsigned long long)(DM_AOT_ARENA_SIZE >> 30), (unsigned long long)hp, hle_thunks_count);
}
void dm_aot_arena_init(void) { pthread_once(&g_once, arena_init_once); }

int dm_aot_in_arena(uint64_t a, uint64_t n) {
    return a >= DM_AOT_ARENA_BASE && n <= DM_AOT_ARENA_SIZE && a - DM_AOT_ARENA_BASE <= DM_AOT_ARENA_SIZE - n;
}

void *dm_guest_reserve(size_t n) {
    dm_aot_arena_init();
    uint64_t sz = round_up(n ? n : 1, page());
    pthread_mutex_lock(&g_mem_lock);
    uint64_t a = take_fresh(sz, sz >= 0x200000 ? 0x200000 : page());   // a large block on a 2 MiB boundary, as Horizon aligns its regions
    pthread_mutex_unlock(&g_mem_lock);
    return (void *)(uintptr_t)a;
}

void *dm_guest_alloc(size_t n) {
    dm_aot_arena_init();
    uint64_t sz = round_up(n ? n : 1, page()), a = 0;
    int reused = 0;
    pthread_mutex_lock(&g_mem_lock);
    for (int i = 0; i < g_nfree; i++)
        if (g_free[i].n >= sz) {
            a = g_free[i].a; g_free[i].a += sz; g_free[i].n -= sz; reused = 1;
            if (!g_free[i].n) { memmove(g_free + i, g_free + i + 1, (size_t)(g_nfree - i - 1) * sizeof *g_free); g_nfree--; }
            break;
        }
    if (!reused) a = take_fresh(sz, page());
    pthread_mutex_unlock(&g_mem_lock);
    if (plat_vm_protect((void *)(uintptr_t)a, sz, PLAT_PROT_READ | PLAT_PROT_WRITE)) die("aot: guest memory %#llx +%#llx: protection failed (errno %d)", (unsigned long long)a, (unsigned long long)sz, errno);
    if (reused) memset((void *)(uintptr_t)a, 0, sz);   // discarded pages need not read as zero (MADV_FREE keeps them until the kernel reclaims them)
    return (void *)(uintptr_t)a;
}

void dm_guest_free(void *p, size_t n) {
    if (!p) return;
    uint64_t a = (uint64_t)(uintptr_t)p, sz = round_up(n ? n : 1, page());
    if (!dm_aot_in_arena(a, sz) || a % page()) die("aot: dm_guest_free(%p, %zu): not an arena range", p, n);
    (void)plat_vm_protect(p, sz, PLAT_PROT_NONE);
    (void)plat_vm_discard(p, sz);
    pthread_mutex_lock(&g_mem_lock);
    int i = 0;
    while (i < g_nfree && g_free[i].a < a) i++;
    if (i > 0 && g_free[i - 1].a + g_free[i - 1].n == a) {   // joins the range before it, and maybe the one after
        g_free[i - 1].n += sz;
        if (i < g_nfree && g_free[i - 1].a + g_free[i - 1].n == g_free[i].a) {
            g_free[i - 1].n += g_free[i].n; memmove(g_free + i, g_free + i + 1, (size_t)(g_nfree - i - 1) * sizeof *g_free); g_nfree--;
        }
    } else if (i < g_nfree && a + sz == g_free[i].a) { g_free[i].a = a; g_free[i].n += sz; }
    else if (g_nfree < MAX_FREE) { memmove(g_free + i + 1, g_free + i, (size_t)(g_nfree - i) * sizeof *g_free); g_free[i] = (span_t){a, sz}; g_nfree++; }
    else { static int once; if (!once++) LOG("[RED] aot: the arena's free list is full (%d ranges): freed guest memory is not reused", MAX_FREE); }
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
    const char *bad = NULL; char why[160];
    if (!d) bad = "it has no nxaot_module descriptor (not a pack of nxaot build)";
    else if (d->abi != NXAOT_MODULE_ABI) { snprintf(why, sizeof why, "its descriptor has ABI %u, this host reads %u", d->abi, NXAOT_MODULE_ABI); bad = why; }
    else if (memcmp(d->sha256, id->sha256, 32) || d->image_size != id->image_size || d->text_end != id->text_end || d->bss_size != id->bss_size) {
        snprintf(why, sizeof why, "it was compiled from another image than %s (SHA-256 or layout differ: image %#llx / %#llx, text %#llx / %#llx, bss %#llx / %#llx)", m->name,
                 (unsigned long long)d->image_size, (unsigned long long)id->image_size, (unsigned long long)d->text_end, (unsigned long long)id->text_end,
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
    // The function map and the line table (3.5), image-relative in the library, as absolute native addresses sorted for the fault reports
    p->nfns = (size_t)d->fn_count;
    p->fns = malloc((p->nfns ? p->nfns : 1) * sizeof *p->fns);
    for (size_t i = 0; i < p->nfns; i++) {
        const nxaot_fn_entry *f = &d->fns[i];
        p->fns[i] = (map_t){(uint64_t)(uintptr_t)&f->body + (uint64_t)(int64_t)f->body, f->offset};
    }
    p->nlines = d->lines && d->lines_end > d->lines ? (size_t)(d->lines_end - d->lines) : 0;
    p->lines = malloc((p->nlines ? p->nlines : 1) * sizeof *p->lines);
    for (size_t i = 0; i < p->nlines; i++) {
        const nxaot_line_entry *e = &d->lines[i];
        p->lines[i] = (map_t){(uint64_t)(uintptr_t)&e->native + (uint64_t)(int64_t)e->native, e->offset};
    }
    if (!p->fns || !p->lines) die("aot: no memory for a pack's tables");
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
    LOG("aot: pack %s for %s at %p: %llu functions, %u lookup slots, %zu line entries, %llu returns, code %#llx..%#llx; %s", path, m->name, (void *)m->base,
        (unsigned long long)d->fn_count, d->lookup_mask + 1, p->nlines, (unsigned long long)d->return_count, (unsigned long long)p->code_lo, (unsigned long long)p->code_hi,
        d->tool ? d->tool : "");
    return p;
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

uint64_t dm_aot_code_lo(void) {
    int n = atomic_load_explicit(&g_npacks, memory_order_acquire); uint64_t lo = UINT64_MAX;
    for (int i = 0; i < n; i++) if (g_packs[i]->base < lo) lo = g_packs[i]->base;
    return n ? lo : 0;
}
uint64_t dm_aot_code_hi(void) {
    int n = atomic_load_explicit(&g_npacks, memory_order_acquire); uint64_t hi = 0;
    for (int i = 0; i < n; i++) if (g_packs[i]->base + g_packs[i]->text_end > hi) hi = g_packs[i]->base + g_packs[i]->text_end;
    return hi;
}
