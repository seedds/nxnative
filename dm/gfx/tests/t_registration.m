// Walk the real HLE, libc, Vulkan, GL/EGL, NVN and service registrations without a GPU.
// Build under ASan without a global ignorelist: every producer must preserve its section's array layout.
#include "../vk/hle_vk.m"
#include <assert.h>
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
static const char instrumented[1] __attribute__((used)) = {0};
#endif

uint64_t resolve_import(module_t *m, const char *name);
int hle_implements(const char *name);
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
struct dm_flags g_flag;
pthread_key_t g_key18;
void svc_thread_dump(void) {}
void dm_vi_layer_size(int *w, int *h) { *w = *h = 0; }
int dm_opmode(void) { return 1; }
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
uint64_t guest_ns(void) { return 0; }
uint64_t guest_ticks(void) { return 0; }
void guest_clock_read_wait(void) {}
void thr_svc_enter(uint64_t *r, uint64_t a) { (void)r; (void)a; }
void thr_svc_leave(void) {}
void dm_spin_wait(uint64_t r, uint64_t a) { (void)r; (void)a; }
uint64_t svc_dispatch(int n, uint64_t *r, uint64_t a) { (void)n; (void)r; (void)a; return 0; }
uint64_t unimpl_stubs[4096];
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
uint32_t hle_new_event(uint32_t *write_end) { if (write_end) *write_end = 2; return 1; }

HLE("registration_hle", int, registration_hle, void) { return 41; }
HLE_FORCE("registration_force", "registration test", int, registration_force, void) { return 42; }
HLE_FORCE_DIRECT("registration_direct", "registration test", int, registration_direct, void) { return 43; }
HLE_FORCE_ALIAS("registration_alias", "registration_hle", "registration test");
static Result answer_a(sf_req *r) { (void)r; return 44; }
static Result answer_b(sf_req *r) { (void)r; return 45; }
SF_SERVICE("reg:a", answer_a);
SF_SERVICE("reg:b", answer_b);

// A 24-byte entry also catches accidental alignment stronger than the type needs.
typedef struct { const char *name; uint64_t value; void *ctx; } odd_entry;
DM_REGISTER("swtest", odd_entry, odd_a, {"a", 46, NULL});
DM_REGISTER("swtest", odd_entry, odd_b, {"b", 47, NULL});

static const void *section(const char *name, size_t size, size_t align, unsigned long *count) {
    unsigned long bytes = 0;
    const void *p = getsectiondata(&_mh_execute_header, "__DATA", name, &bytes);
    assert(p && bytes && bytes % size == 0 && (uintptr_t)p % align == 0);
    *count = bytes / size; return p;
}
#define SECTION(name, type, count) section(name, sizeof(type), _Alignof(type), count)

int main(void) {
#if __has_feature(address_sanitizer)
    assert(!__asan_address_is_poisoned(instrumented));
    assert(__asan_address_is_poisoned(instrumented + sizeof instrumented));
#endif
    unsigned long nhle, nforce, nnvn, nsf, nodd;
    const hle_ent *hle = SECTION("swhle", hle_ent, &nhle);
    void *(*gl_lookup)(const char *) = (void *)resolve_import(NULL, "eglGetProcAddress");
    unsigned nvk = 0, ngl = 0;
    for (unsigned long i = 0; i < nhle; i++) {
        assert(hle[i].name && hle[i].fn && hle_implements(hle[i].name));
        assert((void *)resolve_import(NULL, hle[i].name) == hle[i].fn);
        if (!strncmp(hle[i].name, "vk", 2)) { assert(vk_lookup(hle[i].name) == hle[i].fn); nvk++; }
        if (!strncmp(hle[i].name, "gl", 2) || !strncmp(hle[i].name, "egl", 3)) { assert(gl_lookup(hle[i].name) == hle[i].fn); ngl++; }
    }
    assert(nvk > 100 && ngl > 100);
    assert(((int (*)(void))resolve_import(NULL, "registration_hle"))() == 41);
    void *(*copy)(void *, const void *, size_t) = (void *)resolve_import(NULL, "memcpy");
    char dest[4]; assert(copy(dest, "abc", 4) == dest && !strcmp(dest, "abc"));

    const hle_force_ent *force = SECTION("swforce", hle_force_ent, &nforce);
    void *(*bootstrap)(const char *) = NULL;
    Result (*connect)(uint32_t *, const char *) = NULL;
    void (*close_session)(uint32_t) = NULL;
    unsigned found = 0;
    for (unsigned long i = 0; i < nforce; i++) {
        const hle_force_ent *e = force + i;
        assert(e->name && e->why && (!!e->fn != !!e->to));
        if (!strcmp(e->name, "registration_force")) { assert(((int (*)(void))e->fn)() == 42); found++; }
        if (!strcmp(e->name, "registration_direct")) { assert(((int (*)(void))e->fn)() == 43); found++; }
        if (!strcmp(e->name, "registration_alias")) { assert(!strcmp(e->to, "registration_hle")); found++; }
        if (!strcmp(e->name, "nvnBootstrapLoader")) bootstrap = e->fn;
        if (!strcmp(e->name, "_ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc")) connect = e->fn;
        if (!strcmp(e->name, "_ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE")) close_session = e->fn;
    }
    assert(found == 3 && bootstrap && connect && close_session);
    // NVN's private entry type has the same {name, fn} layout; copy its bytes rather than aliasing it as another struct type.
    const unsigned char *nvn = SECTION("swnvn", hle_ent, &nnvn);
    assert(nnvn > 100);
    for (unsigned long i = 0; i < nnvn; i++) {
        hle_ent entry; memcpy(&entry, nvn + i * sizeof entry, sizeof entry);
        assert(entry.name && entry.fn && bootstrap(entry.name));
    }
    const sf_service_ent *sf = SECTION("swsf", sf_service_ent, &nsf);
    assert(nsf == 2);
    for (unsigned long i = 0; i < nsf; i++) {
        uint32_t handle = 0;
        assert(connect(&handle, sf[i].name) == RES_OK && handle);
        assert(sf[i].answer(NULL) == (!strcmp(sf[i].name, "reg:a") ? 44 : 45));
        close_session(handle);
    }
    const odd_entry *odd = SECTION("swtest", odd_entry, &nodd);
    assert(nodd == 2 && odd[0].value + odd[1].value == 93);
    printf("registrations: %lu HLE (%u Vulkan, %u GL/EGL), %lu forced, %lu NVN, %lu services, %lu 24-byte entries: OK\n",
           nhle, nvk, ngl, nforce, nnvn, nsf, nodd);
    return 0;
}
