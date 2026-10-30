// nv:: (the GPU driver glue layer beneath NVN: creates transfer memory, connects to the nvdrv service). NVN has been replaced wholesale with a host null device, so this layer has nothing to do.
// The allocator callbacks are recorded in the host global g_nv_alloc; the graphics layer takes them from here later when it needs to allocate memory with the game's allocator.
#include "../host/hle.h"

typedef void *(*nv_alloc_fn)(unsigned long size, unsigned long align, void *user);
typedef void (*nv_free_fn)(void *p, void *user);
typedef void *(*nv_realloc_fn)(void *p, unsigned long size, void *user);
struct nv_allocator { nv_alloc_fn alloc; nv_free_fn free; nv_realloc_fn realloc; void *user; };
struct nv_allocator g_nv_alloc, g_nv_devtools_alloc;
// The graphics pool the game installs (nv::MemoryInfo reports on it). Nothing is allocated from it on the host: the graphics layer allocates its own Metal objects
static struct { void *mem; unsigned long size; int inited; } g_nv_pool;

#define WHY "NVN has been replaced wholesale with a host null device, so the driver glue layer has nothing to do"
HLE_FORCE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "; allocator recorded in g_nv_alloc",
          void, h_nv_SetAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "; allocator recorded in g_nv_devtools_alloc",
          void, h_nv_SetDevAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_devtools_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY "; pool recorded for nv::GetGraphicsMemoryInfo", void, h_nv_Init, void *mem, unsigned long size) {
    g_nv_pool = (typeof(g_nv_pool)){mem, size, 1};
    LOG("nv::InitializeGraphics: pool %.2f MB, recorded (the host's graphics layer allocates its own)", size / 1048576.0);
}
HLE_FORCE("_ZN2nv16FinalizeGraphicsEv", WHY, void, h_nv_Fin, void) { g_nv_pool = (typeof(g_nv_pool)){0}; }
// The same export also returns four counters on older SDKs. Recognize the wrapper's data flow, not a title/SDK version or a copy of its instructions:
// save the output pointer from x0, call a helper with stack scratch, widen four consecutive s32s into four u64 output slots, then only restore the stack and return.
// Anything outside this small straight-line form retains the modern contract below.
static int nv_memory_info_legacy(void) {
    const void *fn = guest_sym("_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE");
    module_t *m = fn ? module_of(fn) : NULL;
    uintptr_t at = (uintptr_t)fn, base = m ? (uintptr_t)m->base : 0;
    if (!m || (at & 3) || at < base || m->text_end > m->size) return 0;
    uintptr_t off = at - base;
    if (off >= m->text_end) return 0;
    uint32_t code[32];
    size_t n = (m->text_end - off) / 4;
    if (n > sizeof(code) / sizeof(code[0])) n = sizeof(code) / sizeof(code[0]);
    memcpy(code, fn, n * 4);
    if (!n || (code[0] & 0xffc003ffu) != 0xd10003ffu) return 0; // sub sp, sp, #frame
    unsigned frame = (code[0] >> 10) & 0xfff, output = 31, scratch = 0;
    if (!frame || frame > 512 || (frame & 15)) return 0;
    size_t i = 1;
    for (; i < n; i++) {
        uint32_t w = code[i]; unsigned imm = (w >> 10) & 0xfff;
        if ((w & 0xffffffe0u) == 0xaa0003e0u) { // mov x19..x28, x0: survives the helper
            if (output != 31 || (w & 31) < 19 || (w & 31) > 28) return 0;
            output = w & 31;
        } else if ((w & 0xffc003ffu) == 0x910003e0u) { // add x0, sp, #scratch; immediately call the helper
            if (output == 31 || imm + 16 > frame || i + 1 >= n || (code[i + 1] & 0xfc000000u) != 0x94000000u) return 0;
            scratch = imm; i += 2; break;
        } else if ((w & 0xffc003ffu) == 0x910003fdu) { // add x29, sp, #offset
            if (imm >= frame) return 0;
        } else if ((w & 0xffc003e0u) == 0xf90003e0u) { // str xt, [sp, #offset]
            if (imm * 8 + 8 > frame) return 0;
        } else if ((w & 0xffc003e0u) == 0xa90003e0u) { // stp xt, xt2, [sp, #offset], nonnegative and in the frame
            unsigned offset = (w >> 15) & 0x7f;
            if (offset >= 64 || offset * 8 + 16 > frame) return 0;
        } else return 0;
    }
    if (output == 31 || i + 8 > n) return 0;
    for (unsigned field = 0; field < 4; field++, i += 2) {
        uint32_t load = code[i], store = code[i + 1]; unsigned tmp = load & 31;
        if ((load & 0xffc003e0u) != 0xb98003e0u || tmp > 18 || ((load >> 10) & 0xfff) * 4 != scratch + field * 4) return 0;
        if ((store & 0xffc00000u) != 0xf9000000u || (store & 31) != tmp || ((store >> 5) & 31) != output || ((store >> 10) & 0xfff) != field) return 0;
    }
    int restored_lr = 0;
    for (; i < n; i++) {
        uint32_t w = code[i]; unsigned reg = w & 31, imm = (w >> 10) & 0xfff;
        if ((w & 0xffc003ffu) == 0x910003ffu) // add sp, sp, #frame; ret (no further output writes)
            return imm == frame && restored_lr && i + 1 < n && code[i + 1] == 0xd65f03c0u;
        if ((w & 0xffc003e0u) == 0xf94003e0u) { // ldr a saved register from this frame
            if (reg < 19 || reg > 30 || imm * 8 + 8 > frame) return 0;
            restored_lr |= reg == 30;
        } else if ((w & 0xffc003e0u) == 0xa94003e0u) { // ldp saved registers from this frame
            unsigned reg2 = (w >> 10) & 31, offset = (w >> 15) & 0x7f;
            if (reg < 19 || reg > 30 || reg2 < 19 || reg2 > 30 || offset >= 64 || offset * 8 + 16 > frame) return 0;
            restored_lr |= reg == 30 || reg2 == 30;
        } else return 0;
    }
    return 0;
}
// Modern nv::MemoryInfo is 0x40 bytes, eight 64-bit counters (the SDK fills it from eight sign-extended 32-bit driver counters). Its own implementation reads the NV driver object, which the host never
// creates, so it faults: Well Dweller crashed there (sdk+0x4656c8, x0 = 0) while reporting video memory at startup. Report the pool the game installed, with nothing used from it
HLE_FORCE("_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE", WHY "; pool figures from nv::InitializeGraphics, no driver statistics", void, h_nv_MemInfo, uint64_t *out) {
    // Old free/allocatable/minimum-free/minimum-allocatable counters describe the absent nvdrv allocator, not the host's pool.
    if (nv_memory_info_legacy()) { memset(out, 0, 0x20); return; }
    memset(out, 0, 0x40);
    if (g_nv_pool.inited) { out[0] = g_nv_pool.size; out[2] = g_nv_pool.size; }   // [0] total, [1] used (0), [2] free
}
HLE_FORCE("_ZN2nv22DumpGraphicsMemoryInfoEv", WHY "; the dump goes to the host log", void, h_nv_Dump, void) {
    LOG("nv::DumpGraphicsMemoryInfo: pool %.2f MB, 0 used (host graphics layer)", g_nv_pool.size / 1048576.0);
}
