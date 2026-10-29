// The host-function page (aot_hostfn.h; docs/design/aot-recompiler.md 2.4). Slot i of the page is hle_thunks[i] (gen/hle_thunks.c, sorted by mangled name);
// dm_guest_fn finds a function's slot through a copy of the table sorted by function address. Nothing binds these addresses into the game yet: on x86 the loader's
// bind_sym / force_lookup (loader.c) return dm_guest_fn(fn) where Apple returns the C function's address, a later step of the port
#include "aot_hostfn.h"
#include "dm.h"
#include <stdatomic.h>

static uint64_t g_base;                  // the page's guest address
static const hle_thunk_ent **g_by_fn;    // the entries by function address
static atomic_int g_ready;               // 1 once the page is placed (release: the fields above are written)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int by_fn_cmp(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)(*(const hle_thunk_ent *const *)a)->fn, y = (uintptr_t)(*(const hle_thunk_ent *const *)b)->fn;
    return x < y ? -1 : x > y;
}

// With g_lock held: fill the page with poison, make it read-only, index the table
static void place(uint64_t base, size_t bytes) {
    size_t need = hle_thunks_count * DM_AOT_HOSTFN_SLOT, page = plat_page_size();
    if (!base || base % page || bytes % page) die("aot: host-function page at %#llx, %zu bytes: not whole pages of %zu bytes", (unsigned long long)base, bytes, page);
    if (bytes < need) die("aot: host-function page of %zu bytes holds fewer than the %zu slots of %zu bytes it needs", bytes, hle_thunks_count, (size_t)DM_AOT_HOSTFN_SLOT);
    uint32_t *w = (uint32_t *)(uintptr_t)base;
    for (size_t i = 0; i < bytes / 4; i++) w[i] = DM_AOT_HOSTFN_POISON;
    if (plat_vm_protect((void *)(uintptr_t)base, bytes, PLAT_PROT_READ)) die("aot: host-function page at %#llx: read-only protection failed (errno %d)", (unsigned long long)base, errno);
    const hle_thunk_ent **by = malloc((hle_thunks_count ? hle_thunks_count : 1) * sizeof *by);
    if (!by) die("aot: host-function index: out of memory");
    for (size_t i = 0; i < hle_thunks_count; i++) by[i] = &hle_thunks[i];
    qsort(by, hle_thunks_count, sizeof *by, by_fn_cmp);
    for (size_t i = 1; i < hle_thunks_count; i++)
        if (by[i]->fn == by[i - 1]->fn) die("aot: %s and %s are one C function", by[i - 1]->cname, by[i]->cname);
    g_base = base; g_by_fn = by;
    atomic_store_explicit(&g_ready, 1, memory_order_release);
}

void dm_aot_hostfn_init(uint64_t guest_base, size_t bytes) {
    pthread_mutex_lock(&g_lock);
    if (atomic_load_explicit(&g_ready, memory_order_relaxed)) die("aot: host-function page already placed at %#llx", (unsigned long long)g_base);
    place(guest_base, bytes);
    pthread_mutex_unlock(&g_lock);
}

// The page in a host allocation of its own, when nothing placed it before the first use
static void ensure(void) {
    if (atomic_load_explicit(&g_ready, memory_order_acquire)) return;
    pthread_mutex_lock(&g_lock);
    if (!atomic_load_explicit(&g_ready, memory_order_relaxed)) {
        size_t page = plat_page_size(), n = (hle_thunks_count * DM_AOT_HOSTFN_SLOT + page - 1) / page * page;
        if (!n) n = page;
        void *p = plat_vm_alloc(n);
        if (!p) die("aot: host-function page: %zu bytes not allocated", n);
        place((uint64_t)(uintptr_t)p, n);
    }
    pthread_mutex_unlock(&g_lock);
}

uint64_t dm_aot_hostfn_base(void) { ensure(); return g_base; }
size_t dm_aot_hostfn_size(void) { return hle_thunks_count * DM_AOT_HOSTFN_SLOT; }

uint64_t dm_guest_fn(const void *host_fn) {
    ensure();
    size_t lo = 0, hi = hle_thunks_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uintptr_t f = (uintptr_t)g_by_fn[mid]->fn;
        if (f == (uintptr_t)host_fn) return g_base + (uint64_t)(g_by_fn[mid] - hle_thunks) * DM_AOT_HOSTFN_SLOT;
        if (f < (uintptr_t)host_fn) lo = mid + 1; else hi = mid;
    }
    die("aot: dm_guest_fn(%p): not a host function the guest can call (no HLE entry; gen_hle_thunks.py makes a slot only for entries)", host_fn);
}

// The index of the slot holding guest_addr, or -1 outside the page
static long slot_of(uint64_t guest_addr) {
    if (!atomic_load_explicit(&g_ready, memory_order_acquire) || guest_addr < g_base) return -1;
    uint64_t off = guest_addr - g_base;
    return off < hle_thunks_count * (uint64_t)DM_AOT_HOSTFN_SLOT ? (long)(off / DM_AOT_HOSTFN_SLOT) : -1;
}

const hle_thunk_ent *dm_aot_hostfn_lookup(uint64_t guest_addr) {
    long i = slot_of(guest_addr);
    return i >= 0 && guest_addr % DM_AOT_HOSTFN_SLOT == g_base % DM_AOT_HOSTFN_SLOT ? &hle_thunks[i] : NULL;
}

const char *dm_aot_hostfn_name(uint64_t guest_addr) {
    long i = slot_of(guest_addr);
    return i >= 0 ? hle_thunks[i].cname : NULL;
}

void dm_aot_hostcall(dm_guest_state *st, uint64_t guest_addr) {
    const hle_thunk_ent *e = dm_aot_hostfn_lookup(guest_addr);
    if (!e) {
        const char *in = dm_aot_hostfn_name(guest_addr);
        die("aot: host call to %#llx from guest pc %#llx: not the start of a host-function slot%s%s (page %#llx, %zu slots)", (unsigned long long)guest_addr,
            (unsigned long long)st->pc, in ? ", inside " : "", in ? in : "", (unsigned long long)g_base, hle_thunks_count);
    }
    e->thunk(st);
}
