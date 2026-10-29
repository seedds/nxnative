// Unit tests of the replay's handle resolution (nvn_resolve.c), CPU only, on every host: the handle a texture slot reads (combined slots, the image slots past 31, separate textures),
// the handle c2 holds at a byte across the table boundaries, the IDs a handle read from uniform memory names in the pools bound, deferred binds read again (directly and through a
// texture bind record), uniform memory read as the GPU will read it (unbound and out-of-range slots, a backend copy, inline updates queued in the submission that cover the read, cover
// part of it or are older than a copy or a fill), and the bytes of the driver segments c0 and c2 for a known state (every stage's storage buffer table, compute's dispatch size and
// uniform table, the point size and base instance, the handle tables with and without dynamic handle arrays). The backend's hooks and hle_nvn.c's nvn_dpool_base are stubs that record
// their calls; the memory hook reads the address itself (uniform blocks here are host arrays). Expected values are written out by hand from the layouts in nvn_resolve.h
#include "../nvn/nvn_resolve.h"
#include "../nvn/nvn_gpuwrite.h"
#include <stdio.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) {
    total++; fail += !ok;
    printf("%-80s %s\n", name, ok ? "✓" : "✗");
}

// ---------- Stubs: hle_nvn.c, nvn_state.c's hooks (as nvn_mtl.m's bind hook: the deferred handles read again first) and nvn_resolve.c's hooks ----------
int nvn_dpool_base(const void *pool, int tex) { (void)pool; return tex ? 1000 : 2000; }
uint64_t nvn_state_hook_compose(uint64_t old_serial, const prog_t *const bst[5]) { (void)old_serial; (void)bst; return 1; }
static int n_bind;
void nvn_state_hook_bind(uint32_t op, const p_bind_handles *b) { (void)op; n_bind++; nvn_bind_fresh(b); }
void nvn_state_hook_bind_bad(const void *rec, int n) { (void)rec; (void)n; }
void nvn_state_hook_img_range(const p_bind_handles *b) { (void)b; }
static int copy_on, n_copy, n_stale, stale_kind = -9, n_mem, mem_none; static uint64_t copy_a, mem_a; static size_t mem_n; static uint8_t copy_bytes[8];
int nvn_resolve_hook_ubo_copy(uint64_t a, size_t n, void *out) { n_copy++; copy_a = a; if (!copy_on) return 0; memcpy(out, copy_bytes, n); return 1; }
void nvn_resolve_hook_ubo_stale(int stage, int reg, int off, int kind) { (void)stage; (void)reg; (void)off; n_stale++; stale_kind = kind; }
void nvn_resolve_hook_mem_read(uint64_t a, size_t n, void *out) { n_mem++; mem_a = a; mem_n = n; if (!mem_none) memcpy(out, (const void *)(uintptr_t)a, n); }
static void hooks_clear(void) { n_copy = n_stale = n_mem = 0; stale_kind = -9; copy_a = mem_a = 0; mem_n = 0; }

static uint32_t rd32(const uint8_t *d, unsigned off) { uint32_t v; memcpy(&v, d + off, 4); return v; }
static uint64_t rd64(const uint8_t *d, unsigned off) { uint64_t v; memcpy(&v, d + off, 8); return v; }
static float rdf(const uint8_t *d, unsigned off) { float v; memcpy(&v, d + off, 4); return v; }
// Every byte of d outside the n ranges [lo, lo + len) is 0
static int zero_except(const uint8_t *d, const unsigned (*r)[2], int n) {
    for (unsigned i = 0; i < DRV_SIZE; i++) { int in = 0; for (int k = 0; k < n; k++) in |= i >= r[k][0] && i < r[k][0] + r[k][1]; if (!in && d[i]) return 0; }
    return 1;
}

static _Alignas(16) uint8_t ub[256];   // stage 1's uniform slot 3

int main(void) {
    nvn_state_reset();

    // ---------- The handle a texture slot reads ----------
    S.tex[1][0] = 0x00100001; S.tex[1][1] = 0x500000002ull; S.tex[1][2] = 0x00000003; S.tex[1][31] = 0x0010001f; S.tex[1][32] = 0x00200020; S.tex[1][33] = 0x00200021;
    S.tex[1][40] = 0x00200028; S.tex[1][128] = 0x00300080; S.tex[1][255] = 0x003000ff;
    S.img[1][0] = 0x500; S.img[1][7] = 0x507; S.img[1][8] = 0x508;
    S.samp[1][0] = 0x00400000; S.samp[1][127] = 0x07f00000;
    const uint64_t *tx = S.tex[1], *im = S.img[1];
    t("combined slot 0: its texture", nvn_tex_slot_handle(tx, im, 0) == 0x00100001);
    t("combined slot 31: its texture (images are not consulted below 32)", nvn_tex_slot_handle(tx, im, 31) == 0x0010001f);
    t("combined slot 32 with image 0 bound: the image", nvn_tex_slot_handle(tx, im, 32) == 0x500);
    t("combined slot 33, image 1 unbound: the texture", nvn_tex_slot_handle(tx, im, 33) == 0x00200021);
    t("combined slot 39: image 7, the last shared slot", nvn_tex_slot_handle(tx, im, 39) == 0x507);
    t("combined slot 40: past the 8 shared slots, the texture (not image 8)", nvn_tex_slot_handle(tx, im, 40) == 0x00200028);
    t("combined slot 5, nothing bound: 0", nvn_tex_slot_handle(tx, im, 5) == 0);
    t("separate texture 0 (slot 128) and 127 (slot 255)", nvn_tex_slot_handle(tx, im, 128) == 0x00300080 && nvn_tex_slot_handle(tx, im, 255) == 0x003000ff);
    t("slots 256 and 1000: past the table, 0", nvn_tex_slot_handle(tx, im, 256) == 0 && nvn_tex_slot_handle(tx, im, 1000) == 0);

    // ---------- The handle c2 holds at a byte ----------
    t("c2 0x20: combined slot 0", nvn_c2_handle(1, 0x20) == 0x00100001);
    t("c2 0x118: combined slot 31", nvn_c2_handle(1, 0x118) == 0x0010001f);
    t("c2 0x120: slot 32, the image table's first word: image 0", nvn_c2_handle(1, 0x120) == 0x500);
    t("c2 0x128: slot 33, image unbound: the texture", nvn_c2_handle(1, 0x128) == 0x00200021);
    t("c2 0x160: slot 40, the last combined slot", nvn_c2_handle(1, 0x160) == 0x00200028);
    t("c2 0x168 and 0x560: separate textures 0 and 127", nvn_c2_handle(1, 0x168) == 0x00300080 && nvn_c2_handle(1, 0x560) == 0x003000ff);
    t("c2 0x568 and 0x570: the sampler table gives no handle", nvn_c2_handle(1, 0x568) == 0 && nvn_c2_handle(1, 0x570) == 0);
    t("c2 0x24 and 0x16c: not on an 8-byte word, 0", nvn_c2_handle(1, 0x24) == 0 && nvn_c2_handle(1, 0x16c) == 0);
    t("c2 below the tables (0, 0x18): 0", nvn_c2_handle(1, 0) == 0 && nvn_c2_handle(1, 0x18) == 0);
    t("c2 of another stage, nothing bound: 0", nvn_c2_handle(0, 0x20) == 0);

    // ---------- IDs a handle read from uniform memory names ----------
    t("handle 0: no texture, no sampler", nvn_tid_from_handle(0) == 0 && nvn_sid_from_handle(0) == 0);
    S.tex_base = 100; S.samp_base = 200;
    t("texture 5, sampler 7 with bases 100 / 200: 105, 207", nvn_tid_from_handle(5 | 7u << 20) == 105 && nvn_sid_from_handle(5 | 7u << 20) == 207);
    t("a sampler-only handle: no texture", nvn_tid_from_handle(7u << 20) == 0 && nvn_sid_from_handle(7u << 20) == 207);
    static dpool_t tpool; p_set_pool sp = {&tpool};
    nvn_state_apply(OP_SET_TEX_POOL, &sp);
    t("after binding a texture pool (base 1000): texture 5 is 1005", nvn_tid_from_handle(5) == 1005 && nvn_sid_from_handle(1u << 20) == 201);
    S.tex_base = S.samp_base = 0;

    // ---------- Deferred binds read again ----------
    uint64_t src[32]; for (int i = 0; i < 32; i++) src[i] = 0x00700000u + (uint64_t)i;
    p_bind_handles b = {.stage = 2, .first = 0, .n = 2, .defer = 1, .src = (uint64_t)(uintptr_t)src, .h = {1, 2}};
    nvn_bind_fresh(&b);
    t("deferred: the handles read again from src", b.h[0] == 0x00700000 && b.h[1] == 0x00700001 && !b.h[2]);
    b = (p_bind_handles){.stage = 2, .first = 0, .n = 2, .defer = 0, .src = (uint64_t)(uintptr_t)src, .h = {1, 2}}; nvn_bind_fresh(&b);
    t("not deferred: the recorded handles stay", b.h[0] == 1 && b.h[1] == 2);
    b = (p_bind_handles){.stage = 2, .first = 0, .n = 2, .defer = 1, .src = 0, .h = {1, 2}}; nvn_bind_fresh(&b);
    t("deferred without a source: the recorded handles stay", b.h[0] == 1 && b.h[1] == 2);
    b = (p_bind_handles){.stage = 2, .first = 0, .n = 0, .defer = 1, .src = (uint64_t)(uintptr_t)src, .h = {1}}; nvn_bind_fresh(&b);
    p_bind_handles b33 = {.stage = 2, .first = 0, .n = 33, .defer = 1, .src = (uint64_t)(uintptr_t)src, .h = {1}}; nvn_bind_fresh(&b33);
    t("deferred with count 0 or 33: nothing read", b.h[0] == 1 && b33.h[0] == 1);
    b = (p_bind_handles){.stage = 2, .first = 0, .n = 32, .defer = 1, .src = (uint64_t)(uintptr_t)src}; nvn_bind_fresh(&b);
    t("deferred with count 32: all 32 read", b.h[0] == 0x00700000 && b.h[31] == 0x0070001f);
    p_bind_handles bt = {.stage = 3, .first = 4, .n = 2, .defer = 1, .src = (uint64_t)(uintptr_t)src, .h = {9, 9}};
    n_bind = 0; uint32_t d = nvn_state_apply(OP_BIND_TEX, &bt);
    t("a deferred texture bind record: the bound handles are src's, the record updated", S.tex[3][4] == 0x00700000 && S.tex[3][5] == 0x00700001 && bt.h[0] == 0x00700000
      && n_bind == 1 && d == (NVN_STATE_APPLIED | NVN_DIRTY_ARGS(3)));
    src[0] = 0x00710000; d = nvn_state_apply(OP_BIND_TEX, &bt);
    t("the same record replayed after the game rewrote src: the new handle", S.tex[3][4] == 0x00710000 && d == (NVN_STATE_APPLIED | NVN_DIRTY_ARGS(3)));

    // ---------- Uniform memory as the GPU will read it ----------
    for (int i = 0; i < 256; i++) ub[i] = (uint8_t)i;
    const uint64_t ua = (uint64_t)(uintptr_t)ub;
    p_bind_buf ubb = {.stage = 1, .index = 3, .addr = ua, .size = 64};
    nvn_state_apply(OP_BIND_UBO, &ubb);
    nvn_gw_reset(); hooks_clear();
    uint8_t o8[8]; memset(o8, 0xff, 8); nvn_ubo_read(6, 3, 0, 4, o8); uint32_t h0 = 1; nvn_ubo_read(1, 16, 0, 4, &h0); uint32_t h1 = 1; nvn_ubo_read(-1, 3, 0, 4, &h1);
    t("stage 6, slot 16, stage -1: 0, no hook asked", rd32(o8, 0) == 0 && !h0 && !h1 && !n_copy && !n_mem);
    t("an unbound slot: 0, no hook asked", nvn_ubo_handle(1, 4, 0) == 0 && !n_copy && !n_mem);
    t("a read past the binding's size (62 + 4 > 64): 0, no hook asked", nvn_ubo_handle(1, 3, 62) == 0 && !n_copy && !n_mem);
    uint32_t h = nvn_ubo_handle(1, 3, 8);
    t("memory: the word at +8, the backend's copy asked first at the same address", h == 0x0b0a0908 && n_copy == 1 && copy_a == ua + 8 && n_mem == 1 && mem_a == ua + 8 && mem_n == 4);
    uint64_t sa; nvn_ubo_read(1, 3, 16, 8, &sa);
    t("an 8-byte read (a storage buffer address, SBUFU)", sa == 0x1716151413121110ull && mem_n == 8);
    t("the last word of the binding (+60)", nvn_ubo_handle(1, 3, 60) == 0x3f3e3d3c);
    hooks_clear(); copy_on = 1; memcpy(copy_bytes, "\x11\x22\x33\x44\x55\x66\x77\x88", 8);
    t("the backend's copy answers: its bytes, memory not read", nvn_ubo_handle(1, 3, 8) == 0x44332211 && !n_mem && !n_stale);
    copy_on = 0;
    static const uint8_t upd[8] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7};
    int e = nvn_gw_pend_add(ua + 8, 8, 1); nvn_gw_pend_data(e, ua + 8, upd);
    hooks_clear();
    t("an inline update queued over the read: its bytes, memory not read", nvn_ubo_handle(1, 3, 8) == 0xa3a2a1a0 && !n_mem && !n_stale);
    t("its second word", nvn_ubo_handle(1, 3, 12) == 0xa7a6a5a4 && !n_mem);
    hooks_clear(); nvn_ubo_read(1, 3, 12, 8, &sa);
    t("a read the update covers in part: reported (kind 1), memory read", sa == 0x131211100f0e0d0cull && n_stale == 1 && stale_kind == 1 && n_mem == 1);
    hooks_clear();
    t("a read beside the update: memory, nothing reported", nvn_ubo_handle(1, 3, 32) == 0x23222120 && !n_stale && n_mem == 1);
    static const uint8_t upd2[4] = {0xb0, 0xb1, 0xb2, 0xb3};
    e = nvn_gw_pend_add(ua + 8, 4, 1); nvn_gw_pend_data(e, ua + 8, upd2);
    hooks_clear();
    t("a newer update over the first word: the newer bytes", nvn_ubo_handle(1, 3, 8) == 0xb3b2b1b0 && !n_mem);
    t("the second word: the older update, the newest over it", nvn_ubo_handle(1, 3, 12) == 0xa7a6a5a4 && !n_mem);
    nvn_gw_pend_add(ua + 8, 16, 2);
    hooks_clear();
    t("a buffer copy queued after the updates: reported (kind 2), memory read", nvn_ubo_handle(1, 3, 8) == 0x0b0a0908 && n_stale == 1 && stale_kind == 2 && n_mem == 1);
    nvn_gw_pend_add(ua + 40, 4, 3);
    hooks_clear();
    t("a fill queued over the read: reported (kind 3)", nvn_ubo_handle(1, 3, 40) == 0x2b2a2928 && stale_kind == 3);
    nvn_gw_reset(); hooks_clear();
    t("the next submission: memory again, nothing reported", nvn_ubo_handle(1, 3, 8) == 0x0b0a0908 && !n_stale && n_mem == 1);
    mem_none = 1;
    t("an address in no pool: 0", nvn_ubo_handle(1, 3, 8) == 0);
    mem_none = 0;

    // ---------- The driver segments ----------
    static uint8_t dd[DRV_SIZE];
    nvn_state_reset();
    S.ssbo[0][0] = (p_bind_buf){0, 0, 0x1122334455667788ull, 0x100}; S.ssbo[0][15] = (p_bind_buf){0, 15, 0x99, 0x123456789ull};
    S.ssbo[1][2] = (p_bind_buf){1, 2, 0x5000, 0x40}; S.ssbo[2][1] = (p_bind_buf){2, 1, 0x6000, 0x80}; S.ssbo[5][3] = (p_bind_buf){5, 3, 0x7000, 0x30};
    S.ubo[5][7] = (p_bind_buf){5, 7, 0x8000000010ull, 144};
    const uint32_t dim[3] = {4, 5, 6};
    memset(dd, 0xaa, sizeof dd); int ok = nvn_fill_driver_bank(0, 0, dd, 0, 61, dim);
    t("c0 vertex: filled", ok == 1);
    t("c0 vertex: storage buffer 0 at 0x110 (address, size)", rd64(dd, 0x110) == 0x1122334455667788ull && rd32(dd, 0x118) == 0x100 && rd32(dd, 0x11c) == 0);
    t("c0 vertex: storage buffer 15 at 0x200, its size cut to 32 bits", rd64(dd, 0x200) == 0x99 && rd32(dd, 0x208) == 0x23456789);
    t("c0 vertex: point size 2047 at 0x610, base instance at 0x644", rdf(dd, 0x610) == 2047.0f && rd32(dd, 0x644) == 61);
    t("c0 vertex: every other byte 0", zero_except(dd, (const unsigned[][2]){{0x110, 12}, {0x200, 12}, {0x610, 4}, {0x644, 4}}, 4));
    memset(dd, 0xaa, sizeof dd); ok = nvn_fill_driver_bank(1, 0, dd, 0, 0, dim);
    t("c0 pixel: the table at 0x510 (slot 2 at 0x530)", ok == 1 && rd64(dd, 0x530) == 0x5000 && rd32(dd, 0x538) == 0x40 && rdf(dd, 0x610) == 2047.0f
      && zero_except(dd, (const unsigned[][2]){{0x530, 12}, {0x610, 4}}, 2));
    memset(dd, 0xaa, sizeof dd); ok = nvn_fill_driver_bank(2, 0, dd, 0, 0, dim);
    t("c0 geometry: the table at 0x410 (slot 1 at 0x420)", ok == 1 && rd64(dd, 0x420) == 0x6000 && rd32(dd, 0x428) == 0x80 && zero_except(dd, (const unsigned[][2]){{0x420, 12}, {0x610, 4}}, 2));
    memset(dd, 0xaa, sizeof dd); ok = nvn_fill_driver_bank(5, 0, dd, 0, 61, dim);
    t("c0 compute: the table at 0x310 (slot 3 at 0x340)", ok == 1 && rd64(dd, 0x340) == 0x7000 && rd32(dd, 0x348) == 0x30);
    t("c0 compute: the dispatch size at 0x200", rd32(dd, 0x200) == 4 && rd32(dd, 0x204) == 5 && rd32(dd, 0x208) == 6);
    t("c0 compute: uniform buffer 7 at 0x280 (Link's Awakening's light parameters)", rd64(dd, 0x280) == 0x8000000010ull && rd32(dd, 0x288) == 144);
    t("c0 compute: no point size, no base instance, the rest 0", zero_except(dd, (const unsigned[][2]){{0x340, 12}, {0x200, 12}, {0x280, 12}}, 3));
    memset(dd, 0xaa, sizeof dd); ok = nvn_fill_driver_bank(3, 0, dd, 0, 61, dim);
    t("c0 hull: base never measured (0), no table, point size and base instance still written", ok == 0 && rdf(dd, 0x610) == 2047.0f && rd32(dd, 0x644) == 61
      && zero_except(dd, (const unsigned[][2]){{0x610, 4}, {0x644, 4}}, 2));
    t("c0 domain: base never measured (0)", nvn_fill_driver_bank(4, 0, dd, 0, 0, dim) == 0);

    S.tex[1][0] = 0x00100001; S.tex[1][1] = 0x500000002ull; S.tex[1][2] = 0x00000003; S.tex[1][31] = 0x0010001f; S.tex[1][32] = 0x00200020; S.tex[1][33] = 0x00200021;
    S.tex[1][40] = 0x00200028; S.tex[1][128] = 0x00300080; S.tex[1][255] = 0x003000ff;
    S.img[1][0] = 0x500; S.img[1][7] = 0x507; S.img[1][8] = 0x508;
    S.samp[1][0] = 0x00400000; S.samp[1][127] = 0x07f00000;
    memset(dd, 0xaa, sizeof dd); ok = nvn_fill_driver_bank(1, 2, dd, 0, 61, dim);
    t("c2: filled", ok == 1);
    t("c2: images 0 and 7 at 0x120 and 0x158, image 8 not in the table", rd32(dd, 0x120) == 0x500 && rd32(dd, 0x158) == 0x507);
    t("c2: combined slots 0, 2 and 31 at 0x20 + 8 r", rd32(dd, 0x20) == 0x00100001 && rd32(dd, 0x30) == 3 && rd32(dd, 0x118) == 0x0010001f);
    t("c2: only a handle's low 32 bits (slot 1), the word's upper half 0", rd32(dd, 0x28) == 2 && rd32(dd, 0x2c) == 0 && rd32(dd, 0x24) == 0);
    t("c2: slot 32 with image 0 bound: the image's word", rd32(dd, 0x120) == 0x500);
    t("c2: slot 33, image 1 unbound: the texture; slot 34 unbound: 0", rd32(dd, 0x128) == 0x00200021 && rd32(dd, 0x130) == 0);
    t("c2: slot 40 at 0x160", rd32(dd, 0x160) == 0x00200028);
    t("c2: separate textures 0 and 127 at 0x168 and 0x560", rd32(dd, 0x168) == 0x00300080 && rd32(dd, 0x560) == 0x003000ff);
    t("c2: samplers 0 and 127 at 0x568 and 0x960", rd32(dd, 0x568) == 0x00400000 && rd32(dd, 0x960) == 0x07f00000);
    t("c2: every other byte 0", zero_except(dd, (const unsigned[][2]){{0x20, 4}, {0x28, 4}, {0x30, 4}, {0x118, 4}, {0x120, 4}, {0x128, 4}, {0x158, 4}, {0x160, 4},
                                                                          {0x168, 4}, {0x560, 4}, {0x568, 4}, {0x960, 4}}, 12));
    int same = 1; for (unsigned bb = 0x20; bb < 0x568; bb += 8) same &= rd32(dd, bb) == (uint32_t)nvn_c2_handle(1, bb);
    t("c2: every handle word is what nvn_c2_handle gives for its byte", same);
    memset(dd, 0xaa, sizeof dd); ok = nvn_fill_driver_bank(1, 2, dd, 1, 61, dim);
    t("c2 dynamic: slot 0 is index 0 (its sampler bits give 0 << 20)", ok == 1 && rd32(dd, 0x20) == 0);
    t("c2 dynamic: slot 31 with a sampler: 31 | 31 << 20; slot 2 without: 2", rd32(dd, 0x118) == (31u | 31u << 20) && rd32(dd, 0x30) == 2);
    t("c2 dynamic: slot 1's high handle bits count as a sampler", rd32(dd, 0x28) == (1u | 1u << 20));
    t("c2 dynamic: the image table keeps the handles; slot 33: 33 | 33 << 20", rd32(dd, 0x120) == 0x500 && rd32(dd, 0x128) == (33u | 33u << 20));
    t("c2 dynamic: separate textures 0 and 127 by index, an unbound one 0", rd32(dd, 0x168) == 0 && rd32(dd, 0x560) == 127 && rd32(dd, 0x170) == 0);
    t("c2 dynamic: sampler 127 is 127 << 20, sampler 0 is 0, an unbound one 0", rd32(dd, 0x960) == 127u << 20 && rd32(dd, 0x568) == 0 && rd32(dd, 0x570) == 0);

    printf("== t_resolve: %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
