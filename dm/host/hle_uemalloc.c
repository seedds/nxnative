// Unreal Engine's allocator on a host malloc zone. Octopath Traveler 0's engine allocator is an FMallocBinned3 constructed in place in nninitStartup; its constructor reserves 37 GB of
// address space (74 pools of 512 MB), which an iPhone cannot give (about 5 GB without the extended virtual addressing entitlement), and its pools are too uneven to shrink (CUTS in
// the game's patches.py: the constructor call is cut). The host puts this vtable in the object instead: every FMemory allocation reaches it through the object (FPlatformMemory::BaseAllocator returns it,
// and the thread-local storage allocator calls its Malloc / Free slots), and the guest's own trivial functions stay in the slots that do nothing.
// The FMalloc slots (the engine's FMalloc interface, measured from the vtable and the thread-local callbacks): 0-1 destructors, 2-5 FExec, 6 Malloc, 7 TryMalloc, 8 Realloc, 9 TryRealloc, 10 Free, 11 QuantizeSize, 12 GetAllocationSize, 13 Trim, 14 SetupTLSCachesOnCurrentThread,
// 15 ClearAndDisableTLSCachesOnCurrentThread, 16 InitializeStatsMetadata, 17 UpdateStats, 18 GetAllocatorStats, 19 DumpAllocatorStats, 20 IsInternallyThreadSafe, 21 ValidateHeap,
// 22 GetDescriptiveName, 23-25 OnMallocInitialized / OnPreFork / OnPostFork (the thread-local callbacks main+0x12e80 / main+0x12ec0 call +0x30 and +0x50).
// Unreal Engine 4.27 (DRAGON QUEST VII Reimagined) has one FExec slot and no fork callbacks: 0-1 destructors, 2 Exec, 3 Malloc, 4 TryMalloc, 5 Realloc, 6 TryRealloc, 7 Free, 8 QuantizeSize,
// 9 GetAllocationSize, 10 Trim, 11 SetupTLSCachesOnCurrentThread, 12 ClearAndDisableTLSCachesOnCurrentThread, 13 InitializeStatsMetadata, 14 UpdateStats, 15 GetAllocatorStats,
// 16 DumpAllocatorStats, 17 IsInternallyThreadSafe, 18 ValidateHeap, 19 GetDescriptiveName, 20 OnMallocInitialized (main+0x5e4c0e8: TryMalloc / TryRealloc call +0x18 / +0x28, GCreateMalloc
// calls +0x88 and +0xa0; Free is main+0x10, which reads the pool base as GetAllocationSize does). Its allocator object is made at run time (GCreateMalloc mallocs it), so the host puts the
// vtable in at an observation gate right after the cut constructor call (OBS in the game's patches.py, OBS_UE_MALLOC: x19 = the object)
// Unreal Engine 5.6 (FINAL FANTASY RESONANCE) adds MallocZeroed / TryMallocZeroed after Free and two TLS cache calls after the setup, and two cached-memory queries at the end: 0-1 destructors,
// 2-5 FExec, 6 Malloc, 7 TryMalloc, 8 Realloc, 9 TryRealloc, 10 Free, 11 MallocZeroed, 12 TryMallocZeroed, 13 QuantizeSize, 14 GetAllocationSize, 15 Trim, 16 SetupTLSCachesOnCurrentThread,
// 17 MarkTLSCachesAsUsedOnCurrentThread, 18 MarkTLSCachesAsUnusedOnCurrentThread, 19 ClearAndDisableTLSCachesOnCurrentThread, 20 InitializeStatsMetadata, 21 UpdateStats, 22 GetAllocatorStats,
// 23 DumpAllocatorStats, 24 IsInternallyThreadSafe, 25 ValidateHeap, 26 GetDescriptiveName, 27-29 OnMallocInitialized / OnPreFork / OnPostFork, 30-31 the free cached memory sizes (main+0x67df800;
// the base FMalloc vtable main+0x67dff48 has 0 for the three pure slots 6 / 8 / 10, its 7 / 9 call +0x30 / +0x40, and its 11 and 12 are one function: +0x30, then memset). The zeroed slots
// keep the guest's function, which calls Malloc through the vtable
// Rocksteady's Unreal Engine 3 (Batman: Arkham Knight) carries an FMallocBinned3 too (74 pools of 512 MB, 37 GB, plus a 1 GB region for its small-block metadata) behind Unreal Engine 3's
// FMalloc interface, whose sizes and alignments are 32-bit (DWORD): 0-1 destructors, 2 Exec (returns 0), 3 QuantizeSize(DWORD, DWORD) → DWORD, 4 Malloc(DWORD, DWORD), 5 Realloc(void *, DWORD, DWORD),
// 6 Free, 7 returns 0, 8 does nothing, 9 a second Malloc entry that calls slot 4, 10-11 return 0, 12 does nothing, 13 Tick (flushes the thread caches when slot 19 set the flag at +8),
// 14 IsInternallyThreadSafe (1), 15 does nothing, 16 an Exec, 17 ValidateHeap (1), 18 DumpAllocatorStats, 19 sets the flag at +8, 20 GetAllocationSize(void *, DWORD &),
// 21 SetupTLSCachesOnCurrentThread (every new thread calls it before anything else), 22 ClearAndDisableTLSCachesOnCurrentThread, 23 does nothing (main+0x329a408; the thread-safe proxy main+0x3450158
// forwards slots 2 and 4-8 under its lock, so it names them). There is no TryMalloc / TryRealloc. The object is static (main+0x51818c0) and constructed in the guarded getter main+0x192eb30,
// inline: the cut jumps from the guard over the construction to the guard's release, so the object keeps the host's vtable
// Fixed addresses: each row runs only on the main build it was found in (AGENTS.md)
#include "hle.h"
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <os/lock.h>
#include <unistd.h>

// Slot layouts: which slots the host fills and which do nothing (up to 6; 0 ends the list); the others keep the guest's own functions
enum { UE5, UE4_27, UE5_6, UE3 };
static const struct { int nslot, malloc_, try_malloc, realloc_, try_realloc, free_, quantize, get_size, nothing[6]; bool dword; } k_lay[] = {   // try slots -1: none; dword: 32-bit sizes
    [UE5] = { 26, 6, 7, 8, 9, 10, 11, 12, { 13, 14, 15, 19 } },      // Trim, the TLS cache setup and teardown, DumpAllocatorStats
    [UE4_27] = { 21, 3, 4, 5, 6, 7, 8, 9, { 10, 11, 12, 16 } },
    [UE5_6] = { 32, 6, 7, 8, 9, 10, 13, 14, { 15, 16, 17, 18, 19, 23 } },   // Trim, the four TLS cache calls, DumpAllocatorStats
    [UE3] = { 24, 4, -1, 5, -1, 6, 3, 20, { 13, 21, 22 } , true },          // Tick, the TLS cache setup and teardown
};
static const struct { const char *build, *what; int layout; uint64_t obj, vtable, cut; uint32_t cut_word; uint64_t late_vt; } k_ue[] = {   // cut_word: what the rewriter put at cut (0: a nop); late_vt: see release()
    // Octopath Traveler 0 1.0.5: the FMallocBinned3 object (main+0x68022c8, bss), its vtable (main+0x5b06f38, 26 slots), the cut constructor call (main+0x12e40, a nop after the rewrite)
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Octopath Traveler 0 1.0.5", UE5, 0x68022c8, 0x5b06f38, 0x12e40 },
    // DRAGON QUEST VII Reimagined 1.2.0: no fixed object (0: made by GCreateMalloc, given at the gate), its vtable (main+0x5e4c0e8, 21 slots), the cut constructor call (main+0x32758dc)
    { "f7ec03e4cb9de3bd8319d4e899d2e9cc", "DRAGON QUEST VII Reimagined 1.2.0", UE4_27, 0, 0x5e4c0e8, 0x32758dc },
    // Pikmin 4 1.1.1 (Unreal Engine 4.26.2, the 4.27 slot layout: TryMalloc / TryRealloc call +0x18 / +0x28, slot 17 returns 1): made by GCreateMalloc, its vtable (main+0x8da4758, 21 slots),
    // the cut constructor call (main+0x353f788)
    { "060f827bd2b38703e9ea11e3210d89bc", "Pikmin 4 1.1.1", UE4_27, 0, 0x8da4758, 0x353f788 },
    // OCTOPATH TRAVELER II 1.1.1 (Unreal Engine 4.27): made by GCreateMalloc, its vtable (main+0x64710b0: slots 0-19 as the 4.27 layout, TryMalloc / TryRealloc call +0x18 / +0x28,
    // slots 17 and 18 return 1, 19 the name; there is no OnMallocInitialized, so the 21st word copied is the next vtable's offset to top, which nothing calls), the cut constructor call (main+0xbecc08)
    { "0d9649011312f62e93f6a5673a09d7e9", "OCTOPATH TRAVELER II 1.1.1", UE4_27, 0, 0x64710b0, 0xbecc08 },
    // SHIN MEGAMI TENSEI V: Vengeance 1.0.3: GCreateMalloc makes the object in x19; the constructor reserves 37 GB at main+0x15790b0. Its vtable has the UE4 layout:
    // TryMalloc / TryRealloc call +0x18 / +0x28, slots 17 and 18 return 1, 19 returns "Binned3"; the following word is 0 (no OnMallocInitialized).
    { "0a4f14a5c12f470a4d3168a8f2ae6005", "SHIN MEGAMI TENSEI V: Vengeance 1.0.3", UE4_27, 0, 0x5a7f0a8, 0x1f2f63c },
    // FINAL FANTASY RESONANCE 1.0.1: the FMallocBinned3 object (main+0x77c6210, bss), its vtable (main+0x67df800, 32 slots), the cut constructor call (main+0x137f0)
    { "84e182f72ee1da4f477804bd05b54c15", "FINAL FANTASY RESONANCE 1.0.1", UE5_6, 0x77c6210, 0x67df800, 0x137f0 },
    // Asian release: object, 32-slot vtable and constructor cut verified on this main build (PRs #126 and #304).
    { "b7f965b9d7227db8e29f04e84ea7331b", "FINAL FANTASY RESONANCE 1.0.1 (Asian release)", UE5_6, 0x77c6210, 0x67df800, 0x137f0 },
    // Batman: Arkham Knight 1.0.5 (Rocksteady's Unreal Engine 3): the static FMallocBinned3 object (main+0x51818c0, bss), its vtable (main+0x329a408, 24 slots), the cut in its guarded getter
    // (main+0x192ec14: b main+0x192fea4, past the inline construction to the guard's release); blocks of Scaleform Video's player (vtable main+0x35f0448) wait before the zone gets them
    { "feae83dc9e9fb34c77d0a62f1fa9ae9f", "Batman: Arkham Knight 1.0.5", UE3, 0x51818c0, 0x329a408, 0x192ec14, 0x140004a4, 0x35f0448 },
};
enum { NSLOT = 32 };

static malloc_zone_t *g_zone;
static _Atomic int64_t g_used, g_peak;   // bytes in use (usable sizes) and their high-water mark, for the [mem] line (one per 256 MB of new peak)
static void used(int64_t d) {
    int64_t after = atomic_fetch_add(&g_used, d) + d, peak = atomic_load(&g_peak);
    while (after > peak && !atomic_compare_exchange_weak(&g_peak, &peak, after)) {}
    if (after > peak && after >> 28 != peak >> 28) LOG("[mem] Unreal heap (host malloc zone) up to %.2f GB in use", after / 1073741824.0);
}
static size_t align_of(uint32_t a) { return a > 16 ? a : 16; }   // 0 = DEFAULT_ALIGNMENT, 16 bytes as Binned3 gives
// DM_UE_HEAPLOG=<bytes>: one line for each allocation and free of at least that many bytes, with the block, its size and the guest frames that called (the frame-pointer chain
// from the host function the guest called; the engine's thread-safe proxy is the first frame). To match a stale object a crash reads with the block it lies in, and who freed it
static size_t g_heaplog;
static void heaplog(const char *what, void *p, size_t n, size_t a) {
    char b[512]; int k = snprintf(b, sizeof b, "[dm] [ueheap] %s %p %#zx align %zu t=%.3f by", what, p, n, a, now_ns() / 1e9);
    uint64_t fp = (uint64_t)__builtin_frame_address(0);
    for (int i = 0; i < 8 && fp && !(fp & 7) && k < (int)sizeof b - 40; i++) {
        uint64_t w[2]; vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS || got != sizeof w) break;
        module_t *m = module_of((void *)w[1]);
        if (m) k += snprintf(b + k, sizeof b - k, " %s+%#llx", m->name, (unsigned long long)(w[1] - (uint64_t)m->base));
        if (w[0] <= fp) break;
        fp = w[0];
    }
    b[k++] = '\n'; write(2, b, k);
}
static void *alloc(size_t n, uint32_t a) {
    void *p = align_of(a) == 16 ? malloc_zone_malloc(g_zone, n ? n : 1) : malloc_zone_memalign(g_zone, align_of(a), n ? n : 1);
    if (p) used((int64_t)malloc_size(p));
    if (g_heaplog && n >= g_heaplog) heaplog("alloc", p, n, align_of(a));
    return p;
}
// Some freed blocks wait in a short FIFO before they go back to the zone. A macOS zone writes its free-list pointer into a block's first word at once, where an object keeps its
// vtable. Batman: Arkham Knight's Scaleform Video frees its player (0x550 bytes, vtable main+0x35f0448) on the main thread in the video provider's destructor (main+0x202a700: it
// clears the video image's player pointer, image +0x38, then destroys the player) without the lock under which the rendering thread updates the image (main+0xeaaa8 → main+0x203171c:
// load image +0x38, then call the player's slot 0x100); a rendering thread that loaded the pointer just before it was cleared called through the zone's pointer (main+0x2031740,
// about once in 30 boots, always at the end of the logo movie). A guest race: only blocks whose first word is the row's late_vt (the player's vtable, which its destructor has
// just put back) wait 250 ms (DM_UE_QUARANTINE=<ms> makes every freed block wait that long, up to 64 MB; 0 = none). The memory accounting counts them as freed
#define QUAR_N 8192
#define QUAR_BYTES (64ull << 20)
static struct { void *p; uint64_t t; size_t n; } g_q[QUAR_N];
static unsigned g_qh, g_qt; static size_t g_qbytes; static uint64_t g_quar_ns, g_late_vt; static bool g_quar_all; static os_unfair_lock g_ql = OS_UNFAIR_LOCK_INIT;
static void release(void *p) {
    if (!p) return;
    size_t n = malloc_size(p); used(-(int64_t)n);
    if (g_heaplog && n >= g_heaplog) heaplog("free", p, n, 0);
    bool hold = g_quar_ns && (g_quar_all || (n >= 8 && *(uint64_t *)p == g_late_vt));
    if (!hold && __atomic_load_n(&g_qt, __ATOMIC_RELAXED) == __atomic_load_n(&g_qh, __ATOMIC_RELAXED)) { malloc_zone_free(g_zone, p); return; }   // nothing waiting: no lock
    if (hold && !g_quar_all) { static _Atomic int seen; if (!atomic_fetch_add(&seen, 1)) LOG("Unreal heap: a freed block %p (%#zx bytes) of the object with the listed vtable waits before it goes back to the zone", p, n); }
    void *out[32]; int k = 0; uint64_t now = now_ns();
    os_unfair_lock_lock(&g_ql);
    while (g_qt != g_qh && k < 32 && (g_qt - g_qh >= QUAR_N || g_qbytes + n > QUAR_BYTES || now - g_q[g_qh % QUAR_N].t > g_quar_ns)) {
        g_qbytes -= g_q[g_qh % QUAR_N].n; out[k++] = g_q[g_qh % QUAR_N].p; g_qh++;
    }
    if (hold && g_qt - g_qh < QUAR_N) { g_q[g_qt % QUAR_N].p = p; g_q[g_qt % QUAR_N].t = now; g_q[g_qt % QUAR_N].n = n; g_qt++; g_qbytes += n; p = NULL; }
    os_unfair_lock_unlock(&g_ql);
    for (int i = 0; i < k; i++) malloc_zone_free(g_zone, out[i]);
    if (p) malloc_zone_free(g_zone, p);
}
static void *realloc_aligned(void *p, size_t n, uint32_t a) {
    if (!p) return alloc(n, a);
    if (!n) { release(p); return NULL; }
    size_t old = malloc_size(p);
    if (align_of(a) == 16) { void *q = malloc_zone_realloc(g_zone, p, n); if (q) used((int64_t)malloc_size(q) - (int64_t)old); return q; }
    if (!((uintptr_t)p & (align_of(a) - 1)) && old >= n) return p;
    void *q = alloc(n, a); if (!q) return NULL;
    memcpy(q, p, old < n ? old : n); release(p); return q;
}

static void *m_Malloc(void *self, size_t n, uint32_t a) {
    (void)self; void *p = alloc(n, a);
    if (!p) die("Unreal heap: out of memory (%zu bytes, alignment %u, %.2f GB in use)", n, a, atomic_load(&g_used) / 1073741824.0);
    return p;
}
static void *m_TryMalloc(void *self, size_t n, uint32_t a) { (void)self; return alloc(n, a); }
static void *m_Realloc(void *self, void *p, size_t n, uint32_t a) {
    (void)self; void *q = realloc_aligned(p, n, a);
    if (!q && n) die("Unreal heap: out of memory in Realloc (%zu bytes, alignment %u, %.2f GB in use)", n, a, atomic_load(&g_used) / 1073741824.0);
    return q;
}
static void *m_TryRealloc(void *self, void *p, size_t n, uint32_t a) { (void)self; return realloc_aligned(p, n, a); }
static void m_Free(void *self, void *p) { (void)self; release(p); }
static size_t m_QuantizeSize(void *self, size_t n, uint32_t a) { (void)self; (void)a; return malloc_good_size(n); }
static bool m_GetAllocationSize(void *self, void *p, size_t *out) { (void)self; if (!p) return false; *out = malloc_size(p); return true; }
static void m_Nothing(void *self) { (void)self; }   // Trim (its bool argument is ignored), the thread-local cache setup and teardown, DumpAllocatorStats
// Unreal Engine 3's slots: sizes and alignments are DWORDs, so only the low 32 bits of their registers are the argument
static void *m3_Malloc(void *self, uint32_t n, uint32_t a) { return m_Malloc(self, n, a); }
static void *m3_Realloc(void *self, void *p, uint32_t n, uint32_t a) { return m_Realloc(self, p, n, a); }
static uint32_t m3_QuantizeSize(void *self, uint32_t n, uint32_t a) { (void)self; (void)a; return (uint32_t)malloc_good_size(n); }
static bool m3_GetAllocationSize(void *self, void *p, uint32_t *out) { (void)self; if (!p) return false; *out = (uint32_t)malloc_size(p); return true; }
static void *g_vt[NSLOT];
static const char *g_what;

// Observation gate OBS_UE_MALLOC: the allocator object GCreateMalloc just made (x19), before anything calls it
static void obj_made(uint64_t *regs, int param) {
    (void)param;
    *(void **)(uintptr_t)regs[19] = g_vt;
    static int once; if (!once++) LOG("%s: Unreal's allocator (FMallocBinned3, made at run time at %#llx) runs on a host malloc zone", g_what, (unsigned long long)regs[19]);
}

// Whether [a, a + n) may be Unreal heap memory: with the host allocator installed, a range outside the guest block that the host has mapped readable and writable.
// Unreal allocates the stacks of the threads it creates with FMemory (OCTOPATH TRAVELER II's game thread: nnMain at main+0x544 mallocs it with 4 KB alignment; its
// FRunnableThreads take theirs from inside larger allocations, so a stack is not always a block start, and malloc has no cheap lookup from an interior pointer), and
// nn::os::CreateThread moves such a stack into the stack region with svcMapMemory, whose source must be backed memory (hle_svc.c map_source_backed): this heap lives
// outside the guest block, so the check refused it and the SDK aborted in MapAliasStack before the game started (the caller has already checked the guest block's own regions)
int dm_ue_heap_owns(uint64_t a, uint64_t n) {
    if (!g_zone || !a || !n) return 0;
    int dm_guest_as_contains(uintptr_t a);   // hle_svc.c
    if (dm_guest_as_contains((uintptr_t)a) || dm_guest_as_contains((uintptr_t)(a + n - 1))) return 0;   // inside the guest block: not this heap
    vm_address_t at = (vm_address_t)a; vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (vm_region_64(mach_task_self(), &at, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return 0;   // vm_region_64: iOS has no mach_vm.h
    return at <= a && a + n <= at + sz && (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE);
}

// Called once the modules are loaded and relocated, before any guest code runs
void dm_ue_malloc_install(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        for (int k = 0; k < (int)(sizeof k_ue / sizeof k_ue[0]); k++) if (!strcmp(g_mod[i].build_id, k_ue[k].build)) {
            uint8_t *b = g_mod[i].base; int L = k_ue[k].layout;
            if (*(uint32_t *)(b + k_ue[k].cut) != (k_ue[k].cut_word ? k_ue[k].cut_word : 0xd503201f)) { LOG("[RED] %s: main is rewritten without the allocator cut (its patches.py): Unreal's own FMallocBinned3 runs", k_ue[k].what); return; }
            if (!k_ue[k].obj && !gate_obs_present(OBS_UE_MALLOC)) { LOG("[RED] %s: main is rewritten without the allocator gate (OBS of its patches.py): the allocator would have no vtable", k_ue[k].what); return; }
            memcpy(g_vt, b + k_ue[k].vtable, sizeof g_vt[0] * k_lay[L].nslot);   // the relocated vtable: the trivial slots stay the guest's
            int dw = k_lay[L].dword;
            g_vt[k_lay[L].malloc_] = dw ? (void *)m3_Malloc : (void *)m_Malloc; g_vt[k_lay[L].realloc_] = dw ? (void *)m3_Realloc : (void *)m_Realloc; g_vt[k_lay[L].free_] = (void *)m_Free;
            if (k_lay[L].try_malloc >= 0) g_vt[k_lay[L].try_malloc] = (void *)m_TryMalloc;
            if (k_lay[L].try_realloc >= 0) g_vt[k_lay[L].try_realloc] = (void *)m_TryRealloc;
            g_vt[k_lay[L].quantize] = dw ? (void *)m3_QuantizeSize : (void *)m_QuantizeSize; g_vt[k_lay[L].get_size] = dw ? (void *)m3_GetAllocationSize : (void *)m_GetAllocationSize;
            for (int s = 0; s < 6 && k_lay[L].nothing[s]; s++) g_vt[k_lay[L].nothing[s]] = (void *)m_Nothing;
            { const char *e = getenv("DM_UE_HEAPLOG"); g_heaplog = e ? strtoull(e, NULL, 0) : 0; }
            g_zone = malloc_create_zone(0, 0); malloc_set_zone_name(g_zone, "guest Unreal heap"); g_what = k_ue[k].what;
            { const char *e = getenv("DM_UE_QUARANTINE"); int ms = e ? atoi(e) : k_ue[k].late_vt ? 250 : 0; g_quar_ns = ms > 0 ? (uint64_t)ms * 1000000 : 0;
              g_quar_all = e && ms > 0; g_late_vt = k_ue[k].late_vt ? (uint64_t)(b + k_ue[k].late_vt) : 0;
              if (ms > 0 && g_quar_all) LOG("%s: freed Unreal heap blocks wait %d ms (up to 64 MB) before they go back to the zone (DM_UE_QUARANTINE)", k_ue[k].what, ms);
              else if (ms > 0) LOG("%s: freed blocks of the object with vtable main+%#llx wait %d ms before they go back to the zone", k_ue[k].what, (unsigned long long)k_ue[k].late_vt, ms); }
            if (!k_ue[k].obj) { gate_obs_set(OBS_UE_MALLOC, obj_made); continue; }
            *(void **)(b + k_ue[k].obj) = g_vt;
            LOG("%s: Unreal's allocator (FMallocBinned3 at main+%#llx) runs on a host malloc zone", k_ue[k].what, (unsigned long long)k_ue[k].obj);
        }
}
