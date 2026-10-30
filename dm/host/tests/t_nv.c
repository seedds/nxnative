// Synthetic AArch64 wrappers exercise the old four-counter ABI without copying a game's instruction stream.
#include "../../gfx/hle_nv.c"
#include <assert.h>

static uint32_t code[64];
static module_t module;
static void *symbol;
static int have_module = 1;
void *guest_sym(const char *name) {
    assert(!strcmp(name, "_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE"));
    return symbol;
}
module_t *module_of(const void *p) { assert(p == symbol); return have_module ? &module : NULL; }

// Instruction encoders for an independently constructed wrapper. Offsets are bytes, register 31 is SP or ZR as appropriate.
static uint32_t add(unsigned dst, unsigned src, unsigned bytes) { return 0x91000000u | bytes << 10 | src << 5 | dst; }
static uint32_t mov(unsigned dst, unsigned src) { return 0xaa0003e0u | src << 16 | dst; }
static uint32_t str(unsigned reg, unsigned base, unsigned bytes) { return 0xf9000000u | (bytes / 8) << 10 | base << 5 | reg; }
static uint32_t ldr(unsigned reg, unsigned base, unsigned bytes) { return 0xf9400000u | (bytes / 8) << 10 | base << 5 | reg; }
static uint32_t ldrsw(unsigned reg, unsigned base, unsigned bytes) { return 0xb9800000u | (bytes / 4) << 10 | base << 5 | reg; }
static uint32_t pair(int load, unsigned a, unsigned b, unsigned base, unsigned bytes) {
    return (load ? 0xa9400000u : 0xa9000000u) | (bytes / 8) << 15 | b << 10 | base << 5 | a;
}
static size_t wrapper(unsigned fields, unsigned output, unsigned tmp, unsigned frame, unsigned scratch) {
    size_t n = 0;
    code[n++] = add(31, 31, frame) | 0x40000000u; // sub sp, sp, #frame
    code[n++] = pair(0, 29, 30, 31, frame - 16);
    code[n++] = str(output, 31, frame - 24);
    code[n++] = mov(output, 0);
    code[n++] = add(29, 31, frame - 16);
    code[n++] = pair(0, 31, 31, 31, scratch);
    code[n++] = add(0, 31, scratch);
    code[n++] = 0x94000010u; // helper call: its relocation does not determine the output ABI
    for (unsigned f = 0; f < fields; f++) {
        code[n++] = ldrsw(tmp, 31, scratch + f * 4);
        code[n++] = str(tmp, output, f * 8);
    }
    code[n++] = ldr(output, 31, frame - 24);
    code[n++] = pair(1, 29, 30, 31, frame - 16);
    code[n++] = add(31, 31, frame);
    code[n++] = 0xd65f03c0u;
    module = (module_t){.base = (uint8_t *)code, .size = n * 4, .text_end = n * 4};
    symbol = code; have_module = 1;
    return n;
}
static size_t legacy(void) { return wrapper(4, 22, 11, 96, 16); }
static void reject_word(size_t at, uint32_t instruction) {
    legacy(); code[at] = instruction; assert(!nv_memory_info_legacy());
}
static void check_recognition(void) {
    // Different saved/scratch registers and stack layouts prove that selection follows the ABI, not a byte fingerprint.
    for (unsigned saved = 19; saved <= 28; saved++)
        for (unsigned tmp = 0; tmp <= 18; tmp++) {
            wrapper(4, saved, tmp, 128, 32); assert(nv_memory_info_legacy());
        }
    legacy(); code[7] = 0x97fffff0u; assert(nv_memory_info_legacy()); // backward BL is equally valid
    for (unsigned fields = 3; fields <= 8; fields++) {
        wrapper(fields, 22, 11, 128, 16); assert(nv_memory_info_legacy() == (fields == 4));
    }
    reject_word(3, mov(22, 1)); // output pointer did not originate in x0
    reject_word(3, mov(18, 0)); // caller-saved pointer cannot survive the helper
    reject_word(4, add(22, 31, 16)); // output pointer overwritten before the call
    reject_word(6, add(0, 0, 16)); // helper is not filling stack scratch
    reject_word(6, add(0, 31, 88)); // 16-byte scratch extends beyond this frame
    reject_word(7, 0x14000010u); // branch, not a returning helper call
    reject_word(8, ldr(11, 31, 16)); // eight-byte load, not sign-extension from a driver counter
    reject_word(8, ldrsw(11, 22, 16)); // counters are not from the helper's stack output
    reject_word(8, ldrsw(11, 31, 20)); // wrong first counter
    reject_word(9, str(10, 22, 0)); // store does not use the sign-extended result
    reject_word(9, str(11, 21, 0)); // store is not to the saved output pointer
    reject_word(9, str(11, 22, 8)); // missing first output field
    reject_word(13, str(11, 22, 8)); // duplicate output field
    reject_word(15, str(11, 22, 32)); // a fifth-field write, not four consecutive fields
    reject_word(16, str(11, 22, 32)); // an extra write after the apparent four-field prefix
    reject_word(17, pair(1, 29, 28, 31, 80)); // no restored return address
    reject_word(18, add(31, 31, 80)); // incomplete frame restoration
    reject_word(19, 0xd65f0260u); // return via x19, not the restored LR
    reject_word(0, add(31, 31, 0) | 0x40000000u);
    reject_word(0, add(31, 31, 100) | 0x40000000u);
    reject_word(1, pair(0, 29, 30, 31, 504)); // stack save outside the frame
    reject_word(2, str(22, 31, 96));
    reject_word(16, ldr(22, 31, 96));
    reject_word(17, pair(1, 29, 30, 31, 504));
    // Every truncated instruction range, including a one-byte-short RET, must reject before reading beyond text.
    size_t n = legacy();
    for (size_t bytes = 0; bytes < n * 4; bytes++) {
        legacy(); module.text_end = bytes; assert(!nv_memory_info_legacy());
    }
    legacy(); module.size--; assert(!nv_memory_info_legacy());
    legacy(); module.base += 4; assert(!nv_memory_info_legacy());
    legacy(); symbol = (uint8_t *)code + 1; assert(!nv_memory_info_legacy());
    legacy(); symbol = (uint8_t *)code + module.size; assert(!nv_memory_info_legacy());
    legacy(); have_module = 0; assert(!nv_memory_info_legacy());
    legacy(); symbol = NULL; assert(!nv_memory_info_legacy());
    // Real allocation bounds, rather than only logical text bounds, for the sanitizer to police.
    n = legacy(); uint32_t *bounded = malloc(n * 4); assert(bounded);
    memcpy(bounded, code, n * 4); symbol = bounded; module.base = (uint8_t *)bounded;
    assert(nv_memory_info_legacy()); free(bounded);
    legacy(); bounded = malloc(4); assert(bounded); bounded[0] = code[0];
    symbol = bounded; module.base = (uint8_t *)bounded; module.text_end = module.size = 4;
    assert(!nv_memory_info_legacy()); free(bounded);
}

static unsigned callback_calls;
static void *alloc_cb(unsigned long n, unsigned long align, void *u) { (void)n; (void)align; callback_calls++; return u; }
static void free_cb(void *p, void *u) { (void)p; (void)u; callback_calls++; }
static void *realloc_cb(void *p, unsigned long n, void *u) { (void)p; (void)n; callback_calls++; return u; }
static void check_info(unsigned fields, uint64_t pool) {
    struct { uint64_t before, out[8], after; } block;
    memset(&block, 0xa5, sizeof(block));
    wrapper(fields, 22, 11, 128, 16);
    h_nv_MemInfo(block.out);
    const uint64_t guard = UINT64_C(0xa5a5a5a5a5a5a5a5);
    assert(block.before == guard && block.after == guard);
    for (unsigned i = 0; i < 8; i++) {
        uint64_t want = fields == 4 ? (i < 4 ? 0 : guard) : (i == 0 || i == 2 ? pool : 0);
        assert(block.out[i] == want);
    }
    if (fields == 4) {
        uint64_t *small = malloc(32); assert(small); memset(small, 0xa5, 32);
        h_nv_MemInfo(small); // ASan's redzone begins exactly after the old output record.
        for (unsigned i = 0; i < 4; i++) assert(small[i] == 0);
        free(small);
    }
}
int main(void) {
    check_recognition();
    assert(hle_force_h_nv_MemInfo.fn == (void *)h_nv_MemInfo);
    assert(!strcmp(hle_force_h_nv_MemInfo.name, "_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE"));
    int normal, devtools;
    h_nv_SetAlloc(alloc_cb, free_cb, realloc_cb, &normal);
    h_nv_SetDevAlloc(alloc_cb, free_cb, realloc_cb, &devtools);
    check_info(4, 0); check_info(8, 0); h_nv_Dump();
    unsigned char storage[4096]; memset(storage, 0x6d, sizeof(storage));
    for (unsigned round = 0; round < 2; round++) {
        h_nv_Init(storage, sizeof(storage));
        check_info(4, 0); check_info(8, sizeof(storage)); h_nv_Dump();
        symbol = NULL; uint64_t modern[8]; memset(modern, 0xa5, sizeof(modern)); h_nv_MemInfo(modern);
        for (unsigned i = 0; i < 8; i++) assert(modern[i] == (i == 0 || i == 2 ? sizeof(storage) : 0));
        h_nv_Fin(); check_info(4, 0); check_info(8, 0); h_nv_Dump();
    }
    for (size_t i = 0; i < sizeof(storage); i++) assert(storage[i] == 0x6d);
    assert(g_nv_alloc.alloc == alloc_cb && g_nv_alloc.free == free_cb && g_nv_alloc.realloc == realloc_cb && g_nv_alloc.user == &normal);
    assert(g_nv_devtools_alloc.alloc == alloc_cb && g_nv_devtools_alloc.free == free_cb && g_nv_devtools_alloc.realloc == realloc_cb && g_nv_devtools_alloc.user == &devtools);
    assert(!callback_calls);
    puts("NV memory: generated legacy/modern wrappers, data-flow rejection, text/allocation bounds, exact output writes and unchanged pool lifecycle passed");
    return 0;
}
