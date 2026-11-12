// Public NFC hooks must stop an older SDK from opening nfc:user, while preserving
// the existing NFP no-tag path. Exercise the real forced registrations, no guest SDK.
#include "../hle_sdk16.c"

static int checks, fails, lookups;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
void *guest_sym(const char *name) { (void)name; lookups++; return NULL; }
module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
int dm_sdk_major(void) { return 3; }
int dm_game_imports(const char *prefix) { (void)prefix; return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; }
static int signals; static void *signalled;
void hle_signal_system_event(void *ev) { signals++; signalled = ev; }
// The included SDK source also registers vsync hooks; NFC must never enter them.
uint64_t hle_system_event_binding(void *ev) { (void)ev; abort(); }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; abort(); }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
static void *forced(const hle_force_ent *e, const char *name) {
    // ASan places redzones between section globals; inspect the emitted registration
    // objects individually instead of treating the Mach-O section as a packed array.
    CHECK(!strcmp(e->name, name) && e->fn, "forced registration has the exact public symbol and a function");
    CHECK(e->why && e->why[0], "forced hook has a reason");
    return e->fn;
}

int main(void) {
    void (*init)(void) = forced(&hle_force_h_nfc_Init, "_ZN2nn3nfc10InitializeEv");
    void (*fini)(void) = forced(&hle_force_h_nfc_Fini, "_ZN2nn3nfc8FinalizeEv");
    int (*state)(void) = forced(&hle_force_h_nfc_State, "_ZN2nn3nfc8GetStateEv");
    unsigned (*enabled)(void) = forced(&hle_force_h_nfc_Enabled, "_ZN2nn3nfc12IsNfcEnabledEv");
    void (*nfp_init)(void) = forced(&hle_force_h_nfp_Init, "_ZN2nn3nfp10InitializeEv");
    void (*nfp_fini)(void) = forced(&hle_force_h_nfp_Fini, "_ZN2nn3nfp8FinalizeEv");
    Result (*list)(NfpDeviceHandle *, int *, int) = forced(&hle_force_h_nfp_List, "_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii");
    Result (*start)(const NfpDeviceHandle *) = forced(&hle_force_h_nfp_Start, "_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE");
    Result (*stop)(const NfpDeviceHandle *) = forced(&hle_force_h_nfp_Stop, "_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE");
    int (*device_state)(const NfpDeviceHandle *) = forced(&hle_force_h_nfp_State, "_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE");
    Result (*tag_info)(void *, const NfpDeviceHandle *) = forced(&hle_force_h_nfp_TagInfo, "_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE");
    int (*nfp_lib)(void) = forced(&hle_force_h_nfp_LibState, "_ZN2nn3nfp8GetStateEv");
    void (*nfp_init_ext)(void) = forced(&hle_force_h_nfp_InitExt, "_ZN2nn3nfp37InitializeWithExtendedApplicationAreaEv");
    if (fails) return 1;
    // The amiibo library's own state: Breath of the Wild and Tears of the Kingdom initialize it and attach their availability event only when it is not 1 (#613).
    CHECK(nfp_lib() == 0, "NFP begins uninitialized");
    nfp_init_ext();
    CHECK(nfp_lib() == 1, "NFP initialization with the extended application area is observable");
    nfp_fini();
    CHECK(nfp_lib() == 0, "NFP finalization ends it");
    CHECK(state() == 0, "NFC begins uninitialized");
    CHECK(enabled() == 0, "NFC has no physical reader before initialization");
    nfp_init();
    CHECK(nfp_lib() == 1, "NFP initialization is observable");
    struct { uint64_t before; NfpDeviceHandle handle; uint64_t after; } device;
    memset(&device, 0xa5, sizeof device);
    int count = -1;
    CHECK(list(&device.handle, &count, 1) == RES_OK && count == 1, "existing NFP device remains available");
    CHECK(device.before == UINT64_C(0xa5a5a5a5a5a5a5a5) && device.after == UINT64_C(0xa5a5a5a5a5a5a5a5), "device enumeration respects output bounds");
    CHECK(start(&device.handle) == RES_OK && device_state(&device.handle) == NFP_STATE_SEARCH, "existing NFP detection can search without a tag");
    // Repeated entry/dismissal follows the caller's state check, init, disabled branch,
    // and conditional finalization. A missing forced export fails before this sequence.
    for (int pass = 0; pass < 3; pass++) {
        CHECK(state() == 0, "NFC entry starts finalized");
        if (state() != 1) init();
        CHECK(state() == 1, "NFC initialization is observable");
        CHECK(enabled() == 0, "initialization cannot fabricate physical NFC support");
        CHECK(device_state(&device.handle) == NFP_STATE_SEARCH, "NFC initialization preserves NFP detection");
        unsigned char output[0x80]; memset(output, 0x5a, sizeof output);
        CHECK(tag_info(output + 16, &device.handle) == (115u | 97u << 9), "tag access still returns tag-not-found");
        int unchanged = 1; for (unsigned i = 0; i < sizeof output; i++) if (output[i] != 0x5a) unchanged = 0;
        CHECK(unchanged, "no-tag result neither fabricates tag data nor overwrites guards");
        if (state()) fini();
        CHECK(state() == 0 && enabled() == 0, "dismissal finalizes NFC and keeps it disabled");
        CHECK(device_state(&device.handle) == NFP_STATE_SEARCH, "NFC finalization preserves NFP detection");
    }
    CHECK(stop(&device.handle) == RES_OK && device_state(&device.handle) == NFP_STATE_INIT, "NFP detection stops normally");
    nfp_fini();
    CHECK(nfp_lib() == 0, "NFP finalization is observable");
    CHECK(state() == 0, "NFP finalization leaves NFC finalized");
    nfp_init();
    CHECK(nfp_lib() == 1, "NFP initializes again after finalization");
    nfp_fini();
    // A virtual amiibo (DM_AMIIBO, amiibo.h): found at detection with the activate event signalled, mounted, its area created, and found again at the next scan
    {
        char adir[] = "/tmp/t_nfc_amiibo.XXXXXX"; CHECK(mkdtemp(adir) != NULL, "a folder for the amiibo's area");
        setenv("DM_AMIIBO", "0100000000040002", 1); setenv("DM_AMIIBO_DIR", adir, 1);
        Result (*attach)(void *, const NfpDeviceHandle *) = forced(&hle_force_h_nfp_AttachAct, "_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE");
        Result (*mount)(const NfpDeviceHandle *, int, int) = forced(&hle_force_h_nfp_Mount, "_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE");
        Result (*unmount)(const NfpDeviceHandle *) = forced(&hle_force_h_nfp_Unmount, "_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE");
        Result (*model)(void *, const NfpDeviceHandle *) = forced(&hle_force_h_nfp_ModelInfo, "_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE");
        Result (*reg)(void *, const NfpDeviceHandle *) = forced(&hle_force_h_nfp_RegInfo, "_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE");
        Result (*create)(const NfpDeviceHandle *, const NfpAreaCreateInfo *) = forced(&hle_force_h_nfp_CreateArea, "_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE");
        Result (*open_area)(const NfpDeviceHandle *, uint32_t) = forced(&hle_force_h_nfp_OpenArea, "_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj");
        Result (*get_area)(void *, uint64_t *, const NfpDeviceHandle *, uint64_t) = forced(&hle_force_h_nfp_GetArea, "_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm");
        uint8_t ev[0x30];
        nfp_init(); CHECK(enabled() == 1, "NFC is on with a virtual amiibo");
        CHECK(attach(ev, &device.handle) == RES_OK, "activate event attached");
        signals = 0;
        CHECK(start(&device.handle) == RES_OK && device_state(&device.handle) == DM_NFP_DEV_FOUND && signals == 1 && signalled == ev, "detection finds the amiibo and signals the activate event");
        uint8_t tag[0x58], mi[0x40], ri[0x100];
        CHECK(tag_info(tag, &device.handle) == RES_OK && tag[10] == 7, "the tag's info: a 7-byte UID");
        CHECK(model(mi, &device.handle) == DM_NFP_WRONG_STATE, "model info needs the tag mounted");
        CHECK(mount(&device.handle, 0, 1) == RES_OK && device_state(&device.handle) == DM_NFP_DEV_MOUNTED, "the amiibo mounts");
        CHECK(model(mi, &device.handle) == RES_OK && mi[0] == 0x01 && mi[5] == 0x04, "model info is the amiibo ID");
        CHECK(reg(ri, &device.handle) == RES_OK && !strcmp((const char *)ri + 0x5c, "amiibo"), "register info: an owner and a nickname");
        uint8_t init[8] = {1, 2, 3, 4, 5, 6, 7, 8}; NfpAreaCreateInfo ci = {0x1019c800, 0, init, sizeof init};
        CHECK(open_area(&device.handle, 0x1019c800) == DM_NFP_AREA_NOT_CREATED, "no area before it is created");
        CHECK(create(&device.handle, &ci) == RES_OK && open_area(&device.handle, 0x1019c800) == RES_OK, "the area is created and opens");
        CHECK(unmount(&device.handle) == RES_OK && stop(&device.handle) == RES_OK && device_state(&device.handle) == NFP_STATE_INIT, "unmount and stop");
        uint8_t got[0xd8]; uint64_t n = 0;
        CHECK(start(&device.handle) == RES_OK && mount(&device.handle, 0, 1) == RES_OK && open_area(&device.handle, 0x1019c800) == RES_OK
              && get_area(got, &n, &device.handle, sizeof got) == RES_OK && n == 0xd8 && !memcmp(got, init, sizeof init), "the area is there at the next scan");
        CHECK(stop(&device.handle) == RES_OK, "stop with the amiibo mounted");
        char f[256]; snprintf(f, sizeof f, "%s/0100000000040002.area", adir); unlink(f); rmdir(adir);
        unsetenv("DM_AMIIBO"); unsetenv("DM_AMIIBO_DIR");
        CHECK(start(&device.handle) == RES_OK && device_state(&device.handle) == NFP_STATE_SEARCH && tag_info(tag, &device.handle) == (115u | 97u << 9), "without DM_AMIIBO detection finds nothing again");
        stop(&device.handle); nfp_fini();
    }
    CHECK(lookups == 0, "NFC and NFP hooks do not call an original SDK service path");
    printf("NFC availability: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
