// Run-time patches of code in NROs, keyed by the NRO's build ID and checked against the bytes the rewriter leaves there.
// EA SPORTS FC 27 (1.8d.6523) builds the Frostbite NVN swapchain in its renderer NRO from whatever size the game's own settings pick (a 1280x720 default when it picks none), and
// the display sizes the SDK reports do not reach it. With Native resolution on (hle_display.c) the host takes that choice: the swapchain, its window textures, the crop and the present
// all follow the size the display holds, as a 16:9 picture that fits it.
#include "hle.h"
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>

static uint32_t movz(int x, int reg, unsigned imm) { return (x ? 0xD2800000u : 0x52800000u) | ((imm & 0xFFFFu) << 5) | ((unsigned)reg & 31); }

enum { K_CONST, K_W, K_H, K_STORE_W, K_STORE_H, K_MOV_X20_X2 };
typedef struct {
    const char *build;   // the NRO's build ID (module_t.build_id)
    uint32_t off;        // the instruction's offset in the NRO image
    uint32_t old;        // the instruction the game's code has there
    uint8_t kind;        // what the row writes
    uint8_t x, reg;      // K_W / K_H: MOVZ Xd (1) or Wd (0), destination register
} nro_patch_t;

#define FC27_NRO "d924da0d0673a87b550159a839f5f807"
// fb::SwapChainNvn::SwapChainNvn (0x7a080) copies the SwapChainDesc into the object, then calls createSwapChainResources (0x7a3b0) and createNvnWindow (0x7aa00); both read the width at
// +0xc and the height at +0x10. Its four instructions at 0x7a310 give the 1280x720 defaults when a dimension is zero; they now store the chosen size.
// resize (0x7ad00) takes the requested width in x1 and height in x2, returns early on a zero one, and does nothing when both equal the object's; it now takes the chosen size as the
// request, so an equal size still ends there and any other recreates the swapchain at the chosen size.
static const nro_patch_t k_nro_patches[] = {
    { FC27_NRO, 0x7a310, 0x34000408, K_W,           0, 8 },   // cbz w8, default width       → mov w8, #W
    { FC27_NRO, 0x7a314, 0xb9401268, K_STORE_W,     0, 0 },   // ldr w8, [x19, #0x10]        → str w8, [x19, #0xc]
    { FC27_NRO, 0x7a318, 0x34000448, K_H,           0, 8 },   // cbz w8, default height      → mov w8, #H
    { FC27_NRO, 0x7a31c, 0xaa1303e0, K_STORE_H,     0, 0 },   // mov x0, x19                 → str w8, [x19, #0x10] (x0 still holds the object)
    { FC27_NRO, 0x7ad00, 0xb40009c1, K_W,           1, 1 },   // cbz x1, return              → mov x1, #W
    { FC27_NRO, 0x7ad18, 0xaa0203f4, K_H,           1, 2 },   // mov x20, x2                 → mov x2, #H
    { FC27_NRO, 0x7ad1c, 0xb4000862, K_MOV_X20_X2,  0, 0 },   // cbz x2, return              → mov x20, x2
};

// One pass per mapped NRO: all of its rows are checked before any byte is written.
void nro_runtime_patch(void) {
    int w = 0, h = 0;
    if (!dm_display_target(&w, &h)) return;   // Native resolution is off: every NRO keeps the game's own size (this also decides the display size before any game code runs)
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->nro || !m->base || !m->build_id[0]) continue;
        uint32_t lo = UINT32_MAX, hi = 0; int n = 0, ok = 1;
        for (unsigned k = 0; k < sizeof k_nro_patches / sizeof *k_nro_patches; k++) {
            const nro_patch_t *p = &k_nro_patches[k];
            if (strcmp(p->build, m->build_id)) continue;
            n++;
            if (p->off + 4 > m->text_end || *(uint32_t *)(m->base + p->off) != p->old) { ok = 0; break; }
            if (p->off < lo) lo = p->off;
            if (p->off + 4 > hi) hi = p->off + 4;
        }
        if (!n) continue;
        if (!ok) { LOG("[RED] NRO runtime patch: %s (%s) is not the build the table expects; keeping the game's own swapchain size", m->name, m->build_id); continue; }
        uintptr_t page = (uintptr_t)(m->base + lo) & ~(uintptr_t)0x3fff;
        uintptr_t end = ((uintptr_t)(m->base + hi) + 0x3fff) & ~(uintptr_t)0x3fff;
        if (mprotect((void *)page, end - page, PROT_READ | PROT_WRITE)) { LOG("[RED] NRO runtime patch: mprotect for %dx%d failed (%s); keeping the game's own size", w, h, strerror(errno)); return; }
        for (unsigned k = 0; k < sizeof k_nro_patches / sizeof *k_nro_patches; k++) {
            const nro_patch_t *p = &k_nro_patches[k];
            if (strcmp(p->build, m->build_id)) continue;
            uint32_t v = 0;
            switch (p->kind) {
            case K_W: v = movz(p->x, p->reg, (unsigned)w); break;
            case K_H: v = movz(p->x, p->reg, (unsigned)h); break;
            case K_STORE_W: v = 0xb9000e68; break;       // str w8, [x19, #0xc]
            case K_STORE_H: v = 0xb9001268; break;       // str w8, [x19, #0x10]
            case K_MOV_X20_X2: v = 0xaa0203f4; break;    // mov x20, x2
            default: break;
            }
            *(uint32_t *)(m->base + p->off) = v;
        }
        if (mprotect((void *)page, end - page, PROT_READ | PROT_EXEC)) die("NRO runtime patch: mprotect back to RX failed");
        sys_icache_invalidate((void *)page, end - page);
        LOG("main: NRO runtime patch: the swapchain of %s (%s) is %dx%d", m->name, m->build_id, w, h);
        return;
    }
}
