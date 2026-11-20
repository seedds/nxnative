// pctl free communication offline policy (hle_misc.c, from PR #38): the hooks are looked up by their mangled names in the real swforce registrations, so a missing or misspelled
// takeover fails; no guest module, IPC session or system applet exists in this test
#include "../hle_misc.c"
void dm_applet_enter(const char *name) { (void)name; }   // hle_oe.c: the shop and error stand-ins hold the game out of focus
void dm_applet_leave(void) {}
#include <mach-o/getsect.h>

static int unexpected, checks, fails;
void *guest_sym(const char *name) { (void)name; unexpected++; return NULL; }
module_t *module_of(const void *p) { (void)p; unexpected++; return NULL; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; unexpected++; }
// hle_codec_video.c: GameChat's camera images, recorded
static int blanks_live, blank_w, blank_h; static uint32_t blank_next = 0x80000000u;
uint32_t dm_native_image_blank(uint32_t w, uint32_t h) { blanks_live++; blank_w = (int)w; blank_h = (int)h; return ++blank_next; }
void dm_native_image_blank_free(uint32_t handle) { if (handle >> 31) blanks_live--; }
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
int main(void) {
    // bool arguments are widened as hle.h requires (only the low 8 bits count); the returns are bools in w0, not nn::Result
    unsigned (*check)(unsigned) = hook("_ZN2nn4pctl32CheckFreeCommunicationPermissionEb");
    unsigned (*begin)(unsigned) = hook("_ZN2nn4pctl25TryBeginFreeCommunicationEb");
    unsigned (*available)(void) = hook("_ZN2nn4pctl28IsFreeCommunicationAvailableEv");
    void (*end)(void) = hook("_ZN2nn4pctl20EndFreeCommunicationEv");
    unsigned (*stereo)(unsigned) = hook("_ZN2nn4pctl27CheckStereoVisionPermissionEb");
    unsigned (*agc)(const void *) = hook("_ZN2nn4pctl33CheckAgcPermissionUnderMinimumAgeERKNS_7account3UidE");
    const uint64_t agc_uid[2] = {1, 2};
    CHECK(agc(agc_uid) == 0, "a user under the minimum age: not permitted, as the free communication queries answer");
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
    // GameChat's camera video storage with no camera: blank images of the size, their handles in the 0xe0-byte storage type, released at Finalize; the game's buffer needs a page
    size_t (*lsize)(int, int, int) = hook("_ZN2nn4chat35CalculateLocalCameraVideoBufferSizeEiii");
    void (*linit)(void *, int, int, int, void *, size_t) = hook("_ZN2nn4chat33InitializeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeEiiiPvm");
    uint32_t (*limg)(const void *, int) = hook("_ZN2nn4chat25GetLocalCameraImageHandleEPKNS0_20LocalCameraVideoTypeEi");
    void (*lfini)(void *) = hook("_ZN2nn4chat31FinalizeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeE");
    void (*rinit)(void *, int, int, int, void *, size_t) = hook("_ZN2nn4chat34InitializeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeEiiiPvm");
    uint32_t (*rimg)(const void *, int) = hook("_ZN2nn4chat26GetRemoteCameraImageHandleEPKNS0_21RemoteCameraVideoTypeEi");
    void (*rfini)(void *) = hook("_ZN2nn4chat32FinalizeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeE");
    CHECK(hook("_ZN2nn4chat36CalculateRemoteCameraVideoBufferSizeEiii") && hook("_ZN2nn4chat28AttachToRoomInfoChangedEventEPNS_2os11SystemEventE"), "the other GameChat takeovers");
    if (lsize && linit && limg && lfini && rinit && rimg && rfini) {
        uint8_t local[0xe0 + 16], remote[0xe0];
        memset(local, 0xcc, sizeof local);
        size_t n = lsize(384, 216, 3);
        CHECK(n && n % 0x1000 == 0, "the buffer size is page-aligned, as the SDK checks");
        linit(local, 384, 216, 3, NULL, n);
        CHECK(blanks_live == 3 && blank_w == 384 && blank_h == 216, "three blank images of the size");
        CHECK(limg(local, 0) && limg(local, 2) && limg(local, 0) != limg(local, 2) && !limg(local, 3) && !limg(local, -1), "an image handle for each of the three, none past them");
        CHECK(local[0xe0] == 0xcc, "nothing written past the 0xe0-byte type");
        rinit(remote, 32, 32, 4, NULL, 0x1000);
        CHECK(blanks_live == 7 && rimg(remote, 3) && rimg(remote, 3) != limg(local, 0), "remote images apart from the local ones");
        lfini(local); rfini(remote);
        CHECK(blanks_live == 0 && !limg(local, 0) && !rimg(remote, 0), "Finalize releases the images");
        linit(local, 4000, 216, 3, NULL, n);
        CHECK(blanks_live == 0 && !limg(local, 0), "a size out of the SDK's range makes no image");
    }
    CHECK(unexpected == 0, "no guest symbol lookup or event binding");
    printf("pctl: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
