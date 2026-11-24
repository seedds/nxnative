// Deterministic thread/fiber stack placement in the guest stack region (hle_svc.c dm_stack_alloc).
// The SDK's AddressSpaceAllocator[Base]::AllocateSpace probes 512 random addresses via QueryMemory with no fallback,
// which fragments a 256 MB stack region once several 8 MB Unity worker stacks are live (issues #1809, #2012; Super Monkey Ball Banana Rumble).
// Across SDK 1.3.1 through 22.2.0 (Switch 1 and Switch 2), both the constructor and AllocateSpace are invoked via PLT,
// so forced takeovers capture the stack-region allocator instance and its exact guard size without assuming any internal C++ struct layout.
// Non-stack allocators (AslrSpaceManager, RemoteRegionManager) and any allocator with forbidden regions pass straight through to the SDK.
#include "hle.h"

#define STACK_ALLOC_MAX 8
static struct { void *self; size_t guard; } g_stack_allocs[STACK_ALLOC_MAX];
static int g_nstack_allocs;
static pthread_mutex_t g_stack_alloc_mu = PTHREAD_MUTEX_INITIALIZER;

static void stack_alloc_track(void *self, uint64_t start, uint64_t end, size_t guard, size_t forbidden_count) {
    int match = !forbidden_count && dm_stack_region_matches(start, end);
    pthread_mutex_lock(&g_stack_alloc_mu);
    int idx = -1;
    for (int i = 0; i < g_nstack_allocs; i++) if (g_stack_allocs[i].self == self) { idx = i; break; }
    if (match) {
        if (idx >= 0) g_stack_allocs[idx].guard = guard;
        else if (g_nstack_allocs < STACK_ALLOC_MAX) g_stack_allocs[g_nstack_allocs++] = (typeof(g_stack_allocs[0])){self, guard};
    } else if (idx >= 0) {
        g_stack_allocs[idx] = g_stack_allocs[--g_nstack_allocs];
    }
    pthread_mutex_unlock(&g_stack_alloc_mu);
}

static int stack_alloc_lookup(void *self, size_t *guard_out) {
    pthread_mutex_lock(&g_stack_alloc_mu);
    for (int i = 0; i < g_nstack_allocs; i++) {
        if (g_stack_allocs[i].self == self) { *guard_out = g_stack_allocs[i].guard; pthread_mutex_unlock(&g_stack_alloc_mu); return 1; }
    }
    pthread_mutex_unlock(&g_stack_alloc_mu);
    return 0;
}

static void *sdk_sym(const char *name) {
    for (int i = 0; i < g_nmod; i++)
        if (!strcmp(g_mod[i].name, "sdk")) return module_sym(&g_mod[i], name);
    return NULL;
}

static void (*g_orig_ctor_c1)(void *, uint64_t, uint64_t, uint64_t);
static void (*g_orig_ctor_c2)(void *, uint64_t, uint64_t, uint64_t);
static void (*g_orig_base_ctor)(void *, uint64_t, uint64_t, uint64_t, const void *, size_t);
static uint64_t (*g_orig_alloc_v1)(void *, size_t, size_t);
static uint64_t (*g_orig_alloc_v2)(void *, size_t, size_t, size_t);
static uint64_t (*g_orig_alloc_v3)(void *, size_t, size_t, size_t, uint64_t (*)(uint64_t));
static pthread_once_t g_sa_once = PTHREAD_ONCE_INIT;

static void stack_alloc_resolve(void) {
    g_orig_ctor_c1   = sdk_sym("_ZN2nn2os6detail21AddressSpaceAllocatorC1Emmm");
    g_orig_ctor_c2   = sdk_sym("_ZN2nn2os6detail21AddressSpaceAllocatorC2Emmm");
    g_orig_base_ctor = sdk_sym("_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmEC2EmmmPKNS1_36AddressSpaceAllocatorForbiddenRegionEm");
    g_orig_alloc_v1  = sdk_sym("_ZN2nn2os6detail21AddressSpaceAllocator13AllocateSpaceEmm");
    g_orig_alloc_v2  = sdk_sym("_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmE13AllocateSpaceEmmm");
    g_orig_alloc_v3  = sdk_sym("_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmE13AllocateSpaceEmmmPFmmE");
}

HLE_FORCE("_ZN2nn2os6detail21AddressSpaceAllocatorC1Emmm",
          "record stack-region allocator instance and guard size for deterministic stack placement (hle_svc.c dm_stack_alloc)",
          void, h_asa_ctor_c1, void *self, uint64_t start, uint64_t end, uint64_t guard) {
    pthread_once(&g_sa_once, stack_alloc_resolve);
    if (!g_orig_ctor_c1) die("AddressSpaceAllocator::C1 not found in sdk");
    g_orig_ctor_c1(self, start, end, guard);
    stack_alloc_track(self, start, end, (size_t)guard, 0);
}

HLE_FORCE("_ZN2nn2os6detail21AddressSpaceAllocatorC2Emmm",
          "record stack-region allocator instance and guard size for deterministic stack placement (hle_svc.c dm_stack_alloc)",
          void, h_asa_ctor_c2, void *self, uint64_t start, uint64_t end, uint64_t guard) {
    pthread_once(&g_sa_once, stack_alloc_resolve);
    if (!g_orig_ctor_c2) die("AddressSpaceAllocator::C2 not found in sdk");
    g_orig_ctor_c2(self, start, end, guard);
    stack_alloc_track(self, start, end, (size_t)guard, 0);
}

HLE_FORCE("_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmEC2EmmmPKNS1_36AddressSpaceAllocatorForbiddenRegionEm",
          "record stack-region allocator instance and guard size for deterministic stack placement (hle_svc.c dm_stack_alloc)",
          void, h_asa_base_ctor, void *self, uint64_t start, uint64_t end, uint64_t guard, const void *forbidden, size_t forbidden_count) {
    pthread_once(&g_sa_once, stack_alloc_resolve);
    if (!g_orig_base_ctor) die("AddressSpaceAllocatorBase::C2 not found in sdk");
    g_orig_base_ctor(self, start, end, guard, forbidden, forbidden_count);
    stack_alloc_track(self, start, end, (size_t)guard, forbidden_count);
}

HLE_FORCE("_ZN2nn2os6detail21AddressSpaceAllocator13AllocateSpaceEmm",
          "deterministic stack placement in the guest stack region; non-stack allocators pass through to the SDK",
          uint64_t, h_asa_alloc_v1, void *self, size_t size, size_t align) {
    pthread_once(&g_sa_once, stack_alloc_resolve);
    size_t guard = 0;
    if (stack_alloc_lookup(self, &guard)) return dm_stack_alloc(size, align, 0, guard);
    if (!g_orig_alloc_v1) die("AddressSpaceAllocator::AllocateSpace not found in sdk");
    return g_orig_alloc_v1(self, size, align);
}

HLE_FORCE("_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmE13AllocateSpaceEmmm",
          "deterministic stack placement in the guest stack region; non-stack allocators pass through to the SDK",
          uint64_t, h_asa_alloc_v2, void *self, size_t size, size_t align, size_t align_offset) {
    pthread_once(&g_sa_once, stack_alloc_resolve);
    size_t guard = 0;
    if (stack_alloc_lookup(self, &guard)) return dm_stack_alloc(size, align, align_offset, guard);
    if (!g_orig_alloc_v2) die("AddressSpaceAllocatorBase::AllocateSpace(3) not found in sdk");
    return g_orig_alloc_v2(self, size, align, align_offset);
}

HLE_FORCE("_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmE13AllocateSpaceEmmmPFmmE",
          "deterministic stack placement in the guest stack region; non-stack allocators pass through to the SDK",
          uint64_t, h_asa_alloc_v3, void *self, size_t size, size_t align, size_t align_offset, uint64_t (*random_fn)(uint64_t)) {
    pthread_once(&g_sa_once, stack_alloc_resolve);
    size_t guard = 0;
    if (stack_alloc_lookup(self, &guard)) return dm_stack_alloc(size, align, align_offset, guard);
    if (!g_orig_alloc_v3) die("AddressSpaceAllocatorBase::AllocateSpace(4) not found in sdk");
    return g_orig_alloc_v3(self, size, align, align_offset, random_fn);
}
