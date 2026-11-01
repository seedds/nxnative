// pctl free communication offline policy (hle_misc.c, from PR #38) and GameChat with no chat and no camera: the hooks are looked up by their mangled names in the real swforce registrations, so a missing or misspelled
// takeover fails; no guest module, IPC session or system applet exists in this test
#include "../hle_misc.c"
void dm_applet_enter(const char *name) { (void)name; }   // hle_oe.c: the shop and error stand-ins hold the game out of focus
void dm_applet_leave(void) {}
#include <mach-o/getsect.h>

static int unexpected, checks, fails;
void *guest_sym(const char *name) { (void)name; unexpected++; return NULL; }
module_t *module_of(const void *p) { (void)p; unexpected++; return NULL; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; unexpected++; }
// hle_nvn.c's native image table, as the chat camera storage uses it: handles are counted out and back
static uint32_t nimg_next = 0x100, nimg_live;
uint32_t nvn_native_image_new(uint32_t w, uint32_t h) { (void)w; (void)h; nimg_live++; return ++nimg_next; }
void nvn_native_image_free(uint32_t handle) { if (handle) nimg_live--; }
// The big-stack take-overs' stubs (HLE_FORCE_BIG) jump here with the implementation in x16; off a guest stack the real entry calls it straight
__asm__(".text\n.p2align 2\n.globl _dm_bigstack_enter\n_dm_bigstack_enter:\n    br x16\n");
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) {
        if (strcmp(e[i].name, name)) continue;
        count++; fn = (void *)e[i].fn;
        CHECK(e[i].why && e[i].why[0], "takeover has a reason");
    }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
// GameChat (Mario Kart World aborted in the SDK's chat::Initialize, which opens chat:u): never chatting, and the camera video storage holds one host native image per picture
static void t_chat(void) {
    void (*init)(void) = hook("_ZN2nn4chat10InitializeEv");
    unsigned (*chatting)(void) = hook("_ZN2nn4chat10IsChattingEv");
    size_t (*size)(int, int, int) = hook("_ZN2nn4chat35CalculateLocalCameraVideoBufferSizeEiii");
    void (*vinit)(void *, int, int, int, void *, size_t) = hook("_ZN2nn4chat33InitializeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeEiiiPvm");
    uint32_t (*img)(const void *, int) = hook("_ZN2nn4chat25GetLocalCameraImageHandleEPKNS0_20LocalCameraVideoTypeEi");
    void (*vfini)(void *) = hook("_ZN2nn4chat31FinalizeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeE");
    if (!init || !chatting || !size || !vinit || !img || !vfini) return;
    init(); CHECK((chatting() & 0xff) == 0, "GameChat: never chatting");
    size_t n = size(384, 216, 3); CHECK(n && !(n & 0xfff), "camera storage size: whole pages (the SDK checks the buffer's alignment)");
    uint8_t type[0xe0]; memset(type, 0xa5, sizeof type); static uint8_t buf[0x1000] __attribute__((aligned(0x1000)));
    vinit(type, 384, 216, 3, buf, n);
    uint32_t a = img(type, 0), b = img(type, 1), c = img(type, 2);
    CHECK(a && b && c && a != b && b != c && a != c && nimg_live == 3, "camera storage: one native image per picture");
    CHECK(img(type, 3) == 0 && img(type, -1) == 0, "camera storage: no image past the count");
    vfini(type); CHECK(nimg_live == 0 && img(type, 0) == 0, "camera storage finalize releases its images");
}
int main(void) {
    t_chat();
    // bool arguments are widened as hle.h requires (only the low 8 bits count); the returns are bools in w0, not nn::Result
    unsigned (*check)(unsigned) = hook("_ZN2nn4pctl32CheckFreeCommunicationPermissionEb");
    unsigned (*begin)(unsigned) = hook("_ZN2nn4pctl25TryBeginFreeCommunicationEb");
    unsigned (*available)(void) = hook("_ZN2nn4pctl28IsFreeCommunicationAvailableEv");
    void (*end)(void) = hook("_ZN2nn4pctl20EndFreeCommunicationEv");
    unsigned (*stereo)(unsigned) = hook("_ZN2nn4pctl27CheckStereoVisionPermissionEb");
    const unsigned args[] = {0, 1, 0x100, 0x101, ~0u};
    if (available) CHECK(available() == 0, "communication unavailable before any permission query");
    for (size_t i = 0; i < sizeof args / sizeof *args; i++) {
        if (check) CHECK(check(args[i]) == 0, "permission query denies communication, with or without UI");
        if (begin) CHECK(begin(args[i]) == 0, "begin agrees with the permission query");
        if (stereo) CHECK(stereo(args[i]) == 0, "stereoscopic 3D not permitted: no VR display, as pl::BeginVrMode answers");
        if (end) end();
        if (check) CHECK(check(args[i]) == 0, "ending communication does not grant permission");
        if (available) CHECK(available() == 0, "permission queries do not make communication available");
    }
    CHECK(unexpected == 0, "no guest symbol lookup or event binding");
    printf("pctl: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
