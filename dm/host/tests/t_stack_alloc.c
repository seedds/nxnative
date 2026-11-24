// Forced takeover registrations and routing for AddressSpaceAllocator[Base] (hle_stack_alloc.c):
// stack-region allocators route to dm_stack_alloc across all three SDK eras, while non-stack
// allocators (AslrSpaceManager) and allocators with forbidden regions forward to the SDK unchanged.
#include "../hle_stack_alloc.c"
#include <assert.h>
#include <stdio.h>

static module_t fake_sdk = {.name = "sdk"};
module_t *g_mod = &fake_sdk;
int g_nmod = 1;

static const uint64_t STACK_BASE = 0x7000000000ull, STACK_END = 0x7010000000ull;
static const uint64_t ASLR_BASE  = 0x6000000000ull, ASLR_END  = 0x7000000000ull;
static int host_alloc_calls, orig_ctor_calls, orig_alloc_calls;
static size_t last_size, last_align, last_off, last_guard;

int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
int dm_stack_region_matches(uint64_t start, uint64_t end) { return start == STACK_BASE && end == STACK_END; }
uint64_t dm_stack_alloc(size_t size, size_t align, size_t align_offset, size_t guard_size) {
    host_alloc_calls++;
    last_size = size; last_align = align; last_off = align_offset; last_guard = guard_size;
    return STACK_BASE + guard_size;
}

static void fake_ctor_c1(void *self, uint64_t start, uint64_t end, uint64_t guard) { (void)self; (void)start; (void)end; (void)guard; orig_ctor_calls++; }
static void fake_ctor_c2(void *self, uint64_t start, uint64_t end, uint64_t guard) { (void)self; (void)start; (void)end; (void)guard; orig_ctor_calls++; }
static void fake_base_ctor(void *self, uint64_t start, uint64_t end, uint64_t guard, const void *forb, size_t nforb) {
    (void)self; (void)start; (void)end; (void)guard; (void)forb; (void)nforb; orig_ctor_calls++;
}
static uint64_t fake_alloc_v1(void *self, size_t size, size_t align) { (void)self; (void)size; (void)align; orig_alloc_calls++; return 0x11110000; }
static uint64_t fake_alloc_v2(void *self, size_t size, size_t align, size_t off) { (void)self; (void)size; (void)align; (void)off; orig_alloc_calls++; return 0x22220000; }
static uint64_t fake_alloc_v3(void *self, size_t size, size_t align, size_t off, uint64_t (*rng)(uint64_t)) {
    (void)self; (void)size; (void)align; (void)off; (void)rng; orig_alloc_calls++; return 0x33330000;
}

void *module_sym(module_t *m, const char *name) {
    assert(m == &fake_sdk);
    if (!strcmp(name, "_ZN2nn2os6detail21AddressSpaceAllocatorC1Emmm")) return (void *)fake_ctor_c1;
    if (!strcmp(name, "_ZN2nn2os6detail21AddressSpaceAllocatorC2Emmm")) return (void *)fake_ctor_c2;
    if (!strcmp(name, "_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmEC2EmmmPKNS1_36AddressSpaceAllocatorForbiddenRegionEm")) return (void *)fake_base_ctor;
    if (!strcmp(name, "_ZN2nn2os6detail21AddressSpaceAllocator13AllocateSpaceEmm")) return (void *)fake_alloc_v1;
    if (!strcmp(name, "_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmE13AllocateSpaceEmmm")) return (void *)fake_alloc_v2;
    if (!strcmp(name, "_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmE13AllocateSpaceEmmmPFmmE")) return (void *)fake_alloc_v3;
    return NULL;
}

static uint64_t dummy_rng(uint64_t max) { return max; }

int main(void) {
    // Verify all 6 forced takeover registrations exist with exact mangled SDK symbol names.
    assert(!strcmp(hle_force_h_asa_ctor_c1.name, "_ZN2nn2os6detail21AddressSpaceAllocatorC1Emmm") && hle_force_h_asa_ctor_c1.fn);
    assert(!strcmp(hle_force_h_asa_ctor_c2.name, "_ZN2nn2os6detail21AddressSpaceAllocatorC2Emmm") && hle_force_h_asa_ctor_c2.fn);
    assert(!strcmp(hle_force_h_asa_base_ctor.name, "_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmEC2EmmmPKNS1_36AddressSpaceAllocatorForbiddenRegionEm") && hle_force_h_asa_base_ctor.fn);
    assert(!strcmp(hle_force_h_asa_alloc_v1.name, "_ZN2nn2os6detail21AddressSpaceAllocator13AllocateSpaceEmm") && hle_force_h_asa_alloc_v1.fn);
    assert(!strcmp(hle_force_h_asa_alloc_v2.name, "_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmE13AllocateSpaceEmmm") && hle_force_h_asa_alloc_v2.fn);
    assert(!strcmp(hle_force_h_asa_alloc_v3.name, "_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmE13AllocateSpaceEmmmPFmmE") && hle_force_h_asa_alloc_v3.fn);

    int stack_v1, aslr_v1, stack_v2, stack_v3, stack_forb;

    // SDK 1-11: C1 registers stack allocator (guard 0x4000); ASLR allocator forwards to SDK.
    ((void (*)(void *, uint64_t, uint64_t, uint64_t))hle_force_h_asa_ctor_c1.fn)(&stack_v1, STACK_BASE, STACK_END, 0x4000);
    ((void (*)(void *, uint64_t, uint64_t, uint64_t))hle_force_h_asa_ctor_c1.fn)(&aslr_v1, ASLR_BASE, ASLR_END, 0x4000);
    assert(orig_ctor_calls == 2);
    assert(((uint64_t (*)(void *, size_t, size_t))hle_force_h_asa_alloc_v1.fn)(&stack_v1, 0x800000, 0x1000) == STACK_BASE + 0x4000);
    assert(host_alloc_calls == 1 && last_size == 0x800000 && last_align == 0x1000 && last_off == 0 && last_guard == 0x4000);
    assert(((uint64_t (*)(void *, size_t, size_t))hle_force_h_asa_alloc_v1.fn)(&aslr_v1, 0x10000, 0x1000) == 0x11110000);
    assert(orig_alloc_calls == 1);

    // SDK 12-14: Base::C2 + 3-arg AllocateSpace.
    ((void (*)(void *, uint64_t, uint64_t, uint64_t, const void *, size_t))hle_force_h_asa_base_ctor.fn)(&stack_v2, STACK_BASE, STACK_END, 0x4000, NULL, 0);
    assert(((uint64_t (*)(void *, size_t, size_t, size_t))hle_force_h_asa_alloc_v2.fn)(&stack_v2, 0x100000, 0x4000, 0x1000) == STACK_BASE + 0x4000);
    assert(host_alloc_calls == 2 && last_size == 0x100000 && last_align == 0x4000 && last_off == 0x1000 && last_guard == 0x4000);
    assert(((uint64_t (*)(void *, size_t, size_t, size_t))hle_force_h_asa_alloc_v2.fn)(&aslr_v1, 0x10000, 0x1000, 0) == 0x22220000);
    assert(orig_alloc_calls == 2);

    // SDK 15-22: Base::C2 (guard 0x10000) + 4-arg AllocateSpace; forbidden regions stay on SDK.
    int dummy_forb = 0;
    ((void (*)(void *, uint64_t, uint64_t, uint64_t, const void *, size_t))hle_force_h_asa_base_ctor.fn)(&stack_v3, STACK_BASE, STACK_END, 0x10000, NULL, 0);
    ((void (*)(void *, uint64_t, uint64_t, uint64_t, const void *, size_t))hle_force_h_asa_base_ctor.fn)(&stack_forb, STACK_BASE, STACK_END, 0x10000, &dummy_forb, 1);
    assert(((uint64_t (*)(void *, size_t, size_t, size_t, uint64_t (*)(uint64_t)))hle_force_h_asa_alloc_v3.fn)(&stack_v3, 0x800000, 0x1000, 0, dummy_rng) == STACK_BASE + 0x10000);
    assert(host_alloc_calls == 3 && last_guard == 0x10000);
    assert(((uint64_t (*)(void *, size_t, size_t, size_t, uint64_t (*)(uint64_t)))hle_force_h_asa_alloc_v3.fn)(&stack_forb, 0x800000, 0x1000, 0, dummy_rng) == 0x33330000);
    assert(orig_alloc_calls == 3);

    // Reconstructing an existing instance with a non-stack range untracks it.
    ((void (*)(void *, uint64_t, uint64_t, uint64_t))hle_force_h_asa_ctor_c2.fn)(&stack_v1, ASLR_BASE, ASLR_END, 0x4000);
    assert(((uint64_t (*)(void *, size_t, size_t))hle_force_h_asa_alloc_v1.fn)(&stack_v1, 0x10000, 0x1000) == 0x11110000);
    assert(orig_alloc_calls == 4 && host_alloc_calls == 3);

    puts("t_stack_alloc: ok");
    return 0;
}
