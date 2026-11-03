// Runtime patches of code in NROs, keyed by the NRO's build ID. The offline rewriter (dm/games/<id>/patches.py) cuts the default bytes; a patch that must follow a launcher
// setting is rewritten here, after loader.c has mapped the NROs and before the game loads them. Every row checks the build and the bytes the offline cut left before writing.
#include "hle.h"
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>

// MOVZ Wd/Xd, #imm16 (the instruction forms the table uses)
static uint32_t movz(int x, int reg, unsigned imm) { return (x ? 0xD2800000u : 0x52800000u) | ((imm & 0xFFFFu) << 5) | ((unsigned)reg & 31); }

typedef struct {
    const char *build;   // the NRO's build ID (module_t.build_id)
    uint32_t off;        // the instruction's offset in the NRO image
    uint32_t old;        // the instruction the offline cut left there
    uint8_t x;           // 1 = MOVZ Xd (64-bit), 0 = MOVZ Wd (32-bit)
    uint8_t reg;         // destination register
    uint8_t value;       // 0 = width, 1 = height
} nro_patch_t;

// EA SPORTS FC 27 1.8d.6523: fb::SwapChainNvn's constructor (0x7a310, 0x7a318) and resize (0x7ad18, 0x7ad24) are cut offline to 2560x1440; these four
// instructions take the launcher's DM_DISPLAY_SIZE instead, so the swapchain object, NVN window textures and crop are the selected size.
static const nro_patch_t k_nro_patches[] = {
    { "d924da0d0673a87b550159a839f5f807", 0x7a310, 0x52814008, 0, 8, 0 },   // mov w8, #0xa00 (width)
    { "d924da0d0673a87b550159a839f5f807", 0x7a318, 0x5280b408, 0, 8, 1 },   // mov w8, #0x5a0 (height)
    { "d924da0d0673a87b550159a839f5f807", 0x7ad18, 0xd280b414, 1, 20, 1 },  // mov x20, #0x5a0 (height)
    { "d924da0d0673a87b550159a839f5f807", 0x7ad24, 0xd2814015, 1, 21, 0 },  // mov x21, #0xa00 (width)
};

// The NROs the table patches: one pass per mapped NRO, all of its rows checked before any byte is written.
void nro_runtime_patch(void) {
    const char *e = getenv("DM_DISPLAY_SIZE"); if (!e || !*e) e = DM_GAME_DISPLAY_SIZE;
    int w = 0, h = 0;
    if (!e || !*e || sscanf(e, "%dx%d", &w, &h) != 2 || w < 320 || h < 240 || w > 7680 || h > 4320) return;
    if (w == 2560 && h == 1440) return;   // the offline cuts already hold this
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->nro || !m->base || !m->build_id[0]) continue;
        uint32_t lo = UINT32_MAX, hi = 0; int n = 0, ok = 1;
        for (unsigned k = 0; k < sizeof k_nro_patches / sizeof *k_nro_patches; k++) {
            const nro_patch_t *p = &k_nro_patches[k];
            if (strcmp(p->build, m->build_id)) continue;
            n++;
            if (p->off + 4 > m->text_end) { ok = 0; break; }
            if (*(uint32_t *)(m->base + p->off) != p->old) { ok = 0; break; }
            if (p->off < lo) lo = p->off;
            if (p->off + 4 > hi) hi = p->off + 4;
        }
        if (!n) continue;
        if (!ok) { LOG("[RED] NRO runtime patch: %s (%s) is not the build the table expects; keeping the offline bytes", m->name, m->build_id); continue; }
        uintptr_t page = (uintptr_t)(m->base + lo) & ~(uintptr_t)0x3fff;
        uintptr_t end = ((uintptr_t)(m->base + hi) + 0x3fff) & ~(uintptr_t)0x3fff;
        if (mprotect((void *)page, end - page, PROT_READ | PROT_WRITE)) { LOG("[RED] NRO runtime patch: mprotect for %dx%d failed (%s); keeping the offline bytes", w, h, strerror(errno)); return; }
        for (unsigned k = 0; k < sizeof k_nro_patches / sizeof *k_nro_patches; k++) {
            const nro_patch_t *p = &k_nro_patches[k];
            if (strcmp(p->build, m->build_id)) continue;
            *(uint32_t *)(m->base + p->off) = movz(p->x, p->reg, p->value ? (unsigned)h : (unsigned)w);
        }
        if (mprotect((void *)page, end - page, PROT_READ | PROT_EXEC)) die("NRO runtime patch: mprotect back to RX failed");
        sys_icache_invalidate((void *)page, end - page);
        LOG("main: NRO runtime patch %dx%d in %s (%s)", w, h, m->name, m->build_id);
        return;
    }
}
