// nv::GetGraphicsMemoryInfo writes exactly the record its SDK defines: 0x20 bytes on SDK 10 and older, 0x40 from SDK 11 on (and when the version is unknown)
#include "../../gfx/hle_nv.c"
#include <assert.h>

static int sdk;
int dm_sdk_major(void) { return sdk; }

// SDK-side dispatch is independent of the host's g_nv_alloc. Movie's
// NvOsAlloc calls this table, so recording only the host copy is insufficient.
static struct nv_allocator sdk_alloc, sdk_dev_alloc;
static int setters, have_sdk = 1, allocations, frees, reallocations;
static void sdk_set(nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) {
    sdk_alloc = (struct nv_allocator){a, f, r, u}; setters++;
}
static void sdk_dev_set(nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) {
    sdk_dev_alloc = (struct nv_allocator){a, f, r, u}; setters++;
}
void *guest_sym(const char *name) {
    if (!have_sdk) return NULL;
    if (!strcmp(name, hle_force_h_nv_SetAlloc.name)) return (void *)sdk_set;
    if (!strcmp(name, hle_force_h_nv_SetDevAlloc.name)) return (void *)sdk_dev_set;
    assert(!"unexpected SDK lookup"); return NULL;
}
static int allocator_user;
static void *client_alloc(unsigned long size, unsigned long align, void *user) {
    assert(user == &allocator_user && align == 16); allocations++;
    return malloc(size);
}
static void client_free(void *p, void *user) {
    assert(user == &allocator_user); frees++; free(p);
}
static void *client_realloc(void *p, unsigned long size, void *user) {
    assert(user == &allocator_user); reallocations++; return realloc(p, size);
}
static void check_allocators(void) {
    h_nv_SetAlloc(client_alloc, client_free, client_realloc, &allocator_user);
    h_nv_SetDevAlloc(client_alloc, client_free, client_realloc, &allocator_user);
    assert(setters == 2);
    struct nv_allocator *tables[] = {&sdk_alloc, &sdk_dev_alloc, &g_nv_alloc, &g_nv_devtools_alloc};
    for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        struct nv_allocator *t = tables[i];
        assert(t->alloc && t->free && t->realloc);
        void *p = t->alloc(224, 16, t->user); assert(p);
        memset(p, 0x5a, 224);
        p = t->realloc(p, 65536, t->user); assert(p);
        for (int j = 0; j < 224; j++) assert(((unsigned char *)p)[j] == 0x5a);
        t->free(p, t->user);
    }
    assert(allocations == 4 && frees == 4 && reallocations == 4);
    // A module with no SDK definition still gets the host-side allocator.
    have_sdk = 0;
    h_nv_SetAlloc(client_alloc, client_free, client_realloc, NULL);
    h_nv_SetDevAlloc(client_alloc, client_free, client_realloc, NULL);
    assert(setters == 2 && !g_nv_alloc.user && !g_nv_devtools_alloc.user);
}

// The record between two guard words; after it, the old record again in an allocation of exactly its size, for AddressSanitizer
static void check(int major, uint64_t pool) {
    sdk = major;
    unsigned n = DM_SDK_OLD(major) ? 4 : 8;
    const uint64_t guard = UINT64_C(0xa5a5a5a5a5a5a5a5);
    struct { uint64_t before, out[8], after; } block;
    memset(&block, 0xa5, sizeof(block));
    h_nv_MemInfo(block.out);
    assert(block.before == guard && block.after == guard);
    for (unsigned i = 0; i < 8; i++) assert(block.out[i] == (i >= n ? guard : i == 0 || i == 2 ? pool : 0));
    uint64_t *exact = malloc(n * 8); assert(exact);
    h_nv_MemInfo(exact);
    free(exact);
}
int main(void) {
    check_allocators();
    static const int majors[] = {3, 10, 11, 20, 22, 0};
    unsigned char storage[4096];
    for (unsigned i = 0; i < sizeof(majors) / sizeof(majors[0]); i++) {
        check(majors[i], 0);
        h_nv_Init(storage, sizeof(storage));
        check(majors[i], sizeof(storage));
        h_nv_Fin();
        check(majors[i], 0);
    }
    puts("NV memory: SDK and host allocator dispatch; 0x20 / 0x40-byte memory records and pool figures");
    return 0;
}
