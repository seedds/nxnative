// The memory SVCs announce guest memory to the graphics backend (docs/design/vulkan-backend.md 4.2; hle_svc.c, x86-64 only): SetHeapSize growing, MapMemory and
// MapPhysicalMemory call be_guest_map with the host range they made accessible, once it is; SetHeapSize shrinking and UnmapPhysicalMemory call be_guest_unmap with the
// range they are about to decommit, while it is still accessible, so that an import of it goes first; a request that commits or decommits nothing calls neither, and
// UnmapMemory, which leaves its pages accessible, calls neither. The production dispatcher on the small layout of t_svc_memory (memory_profile/game_profile.h), with a stub
// backend that records every call and whether its range was readable then (plat_mem_read, which a decommitted page fails)
#include "../dm.h"
#include <stdarg.h>

// hle_svc.c's guest block: a plain reservation here, as t_heap_size has it (the arena's dm_guest_reserve in the host)
void *dm_guest_reserve(size_t n) { return plat_vm_reserve(n); }
#include "../hle_svc.c"
void *dm_guest_alloc(size_t n) { (void)n; abort(); }
int dm_aot_main_stack(uint64_t *lo, uint64_t *size) { (void)lo; (void)size; return 0; }

static module_t test_modules[1];
module_t *g_mod = test_modules;
int g_nmod;
struct dm_flags g_flag;
void hlecount_svc(int num) { (void)num; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
void gate_obs_set(int id, void (*fn)(uint64_t *, int)) { (void)id; (void)fn; }
void dm_guest_write_prepare(void *addr, size_t size) { (void)addr; (void)size; }
module_t *module_of(const void *p) { (void)p; return NULL; }

// ---------- The stub backend ----------
typedef struct { int unmap; uint64_t a, n; int first, last; } call_t;   // first / last: the range's first and last byte readable at the call
static call_t g_calls[64];
static int g_ncalls;
static int readable(uint64_t a) { uint8_t b; size_t got = 0; return !plat_mem_read(a, &b, 1, &got) && got == 1; }
static void record(int unmap, void *mem, uint64_t size) {
    if (g_ncalls >= 64) { puts("FAIL: more than 64 backend calls"); exit(1); }
    const uint64_t a = (uint64_t)(uintptr_t)mem;
    g_calls[g_ncalls++] = (call_t){unmap, a, size, size && readable(a), size && readable(a + size - 1)};
}
void be_guest_map(void *mem, uint64_t size) { record(0, mem, size); }
void be_guest_unmap(void *mem, uint64_t size) { record(1, mem, size); }

static int checks, fails;
static void check(int ok, const char *what) { checks++; if (!ok) { fails++; printf("FAIL: %s\n", what); } else printf("ok   %s\n", what); }
static uint64_t call(unsigned svc, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t *x1_out) {
    uint64_t r[31] = {x0, x1, x2};
    svc_dispatch((int)svc, r, 0);
    if (x1_out) *x1_out = r[1];
    return r[0];
}
// The calls since the last expect() are exactly these: (unmap, address, size, readable at the call) each, 0 for "no call"
static void expect(const char *what, int n, ...) {
    va_list ap; va_start(ap, n);
    int ok = g_ncalls == n;
    for (int i = 0; i < n && ok; i++) {
        int unmap = va_arg(ap, int); uint64_t a = va_arg(ap, uint64_t), sz = va_arg(ap, uint64_t); int rd = va_arg(ap, int);
        const call_t *c = &g_calls[i];
        ok = c->unmap == unmap && c->a == a && c->n == sz && (rd < 0 || (c->first == rd && c->last == rd));
        if (!ok) printf("     call %d: %s %#llx +%#llx (first %d last %d), expected %s %#llx +%#llx (readable %d)\n", i, c->unmap ? "unmap" : "map", (unsigned long long)c->a,
                        (unsigned long long)c->n, c->first, c->last, unmap ? "unmap" : "map", (unsigned long long)a, (unsigned long long)sz, rd);
    }
    va_end(ap);
    if (g_ncalls != n) printf("     %d calls, expected %d\n", g_ncalls, n);
    check(ok, what);
    g_ncalls = 0;
}

int main(void) {
    enum { P = 0x1000 };
    if (plat_page_size() != P) { printf("note: host pages of %zu bytes, not 4 KiB: the expected ranges are written for 4 KiB pages: skipped\n", (size_t)plat_page_size()); return 0; }
    mem_init();
    const uint64_t heap = (uint64_t)(uintptr_t)g_heap, alias = (uint64_t)(uintptr_t)g_alias, stack = (uint64_t)(uintptr_t)g_stack;
    uint64_t base = 0;
    check(!call(0x01, 0, 0x200000, 0, &base) && base == heap, "SetHeapSize 2 MiB");
    expect("  grows: be_guest_map(heap, 2 MiB) after the pages are open", 1, 0, heap, (uint64_t)0x200000, 1);
    check(!call(0x01, 0, 0x200000, 0, NULL), "SetHeapSize 2 MiB again");
    expect("  no change: no call", 0);
    check(!call(0x01, 0, 0x100000, 0, NULL), "SetHeapSize 1 MiB");
    expect("  shrinks: be_guest_unmap(heap + 1 MiB, 1 MiB) while the pages are still readable", 1, 1, heap + 0x100000, (uint64_t)0x100000, 1);
    check(!readable(heap + 0x100000) && readable(heap + 0xFFFFF), "  and then decommitted, the rest still open");
    check(!call(0x2C, alias + 4 * P, 4 * P, 0, NULL), "MapPhysicalMemory alias +4 pages, 4 pages");
    expect("  commits: be_guest_map of the 4 pages, open", 1, 0, alias + 4 * P, (uint64_t)4 * P, 1);
    check(!call(0x2C, alias + 4 * P, 4 * P, 0, NULL), "MapPhysicalMemory the same pages again");
    expect("  nothing new: no call", 0);
    check(!call(0x2C, alias + 6 * P, 4 * P, 0, NULL), "MapPhysicalMemory alias +6 pages, 4 pages (2 already mapped)");
    expect("  commits: be_guest_map of the request's pages", 1, 0, alias + 6 * P, (uint64_t)4 * P, 1);
    check(!call(0x2D, alias + 5 * P, P, 0, NULL), "UnmapPhysicalMemory alias +5 pages, 1 page");
    expect("  decommits: be_guest_unmap of the page while it is readable", 1, 1, alias + 5 * P, (uint64_t)P, 1);
    check(!readable(alias + 5 * P) && readable(alias + 4 * P) && readable(alias + 6 * P), "  and then that page alone is closed");
    check(!call(0x2D, alias + 5 * P, P, 0, NULL), "UnmapPhysicalMemory the same page again");
    expect("  nothing to decommit: no call", 0);
    check(!call(0x04, stack, heap, 2 * P, NULL), "MapMemory stack <- heap, 2 pages");
    expect("  commits the destination: be_guest_map(stack, 2 pages), open", 1, 0, stack, (uint64_t)2 * P, 1);
    check(!call(0x05, stack, heap, 2 * P, NULL), "UnmapMemory stack <- heap, 2 pages");
    expect("  leaves the pages open: no call", 0);
    check(call(0x2C, alias + 3, P, 0, NULL) == 0xD401 && call(0x01, 0, (uint64_t)1 << 40, 0, NULL) == 0xD001, "refused requests (unaligned, larger than the heap region)");
    expect("  no call", 0);
    check(!call(0x2D, alias + 4 * P, 6 * P, 0, NULL), "UnmapPhysicalMemory alias +4 pages, 6 pages (5 mapped)");
    expect("  decommits: be_guest_unmap of the request's pages", 1, 1, alias + 4 * P, (uint64_t)6 * P, -1);
    check(!call(0x01, 0, 0, 0, NULL), "SetHeapSize 0");
    expect("  shrinks to nothing: be_guest_unmap(heap, 1 MiB) while readable", 1, 1, heap, (uint64_t)0x100000, 1);
    printf("t_guest_map: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
