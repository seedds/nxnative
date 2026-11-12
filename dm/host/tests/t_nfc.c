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
void hle_signal_system_event(void *ev) { (void)ev; }
// The included SDK source also registers vsync hooks; NFC must never enter them.
uint64_t hle_system_event_binding(void *ev) { (void)ev; abort(); }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; abort(); }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_mii_default_charinfo(void *out) { memset(out, 0, 0x58); }
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
    {   // A scanned amiibo file: found by a running or a later detection, read from the dump's plain blocks, its application area kept in a file under DM_ROOT
        Result (*model_info)(uint8_t *, const NfpDeviceHandle *) = forced(&hle_force_h_nfp_ModelInfo, "_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE");
        Result (*mount)(const NfpDeviceHandle *, int, int) = forced(&hle_force_h_nfp_Mount, "_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE");
        Result (*unmount)(const NfpDeviceHandle *) = forced(&hle_force_h_nfp_Unmount, "_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE");
        Result (*common)(uint8_t *, const NfpDeviceHandle *) = forced(&hle_force_h_nfp_CommonInfo, "_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE");
        Result (*reg)(uint8_t *, const NfpDeviceHandle *) = forced(&hle_force_h_nfp_RegInfo, "_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE");
        Result (*open_area)(const NfpDeviceHandle *, uint32_t) = forced(&hle_force_h_nfp_OpenArea, "_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj");
        Result (*create_area)(const NfpDeviceHandle *, const uint8_t *) = forced(&hle_force_h_nfp_CreateArea, "_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE");
        Result (*get_area)(void *, uint64_t *, const NfpDeviceHandle *, uint64_t) = forced(&hle_force_h_nfp_GetArea, "_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm");
        Result (*set_area)(const NfpDeviceHandle *, const void *, uint64_t) = forced(&hle_force_h_nfp_SetArea, "_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm");
        char root[512], path[600], cmd[700]; const char *tmp = getenv("TMPDIR");
        snprintf(root, sizeof root, "%s/t_nfc.%d", tmp && *tmp ? tmp : "/tmp", (int)getpid()); mkdir(root, 0755); setenv("DM_ROOT", root, 1);
        uint8_t dump[540]; memset(dump, 0, sizeof dump);
        static const uint8_t head[] = {0x04, 0x11, 0x22, 0xBF, 0x33, 0x44, 0x55, 0x66, 0x44, 0x48, 0x0F, 0xE0, 0xF1, 0x10, 0xFF, 0xEE, 0xA5, 0x01, 0x02};
        static const uint8_t model[] = {0x01, 0x00, 0x00, 0x00, 0x03, 0x4B, 0x09, 0x02};
        memcpy(dump, head, sizeof head); memcpy(dump + 0x54, model, sizeof model);
        snprintf(path, sizeof path, "%s/Link.bin", root);
        FILE *f = fopen(path, "wb"); fwrite(dump, 1, sizeof dump, f); fclose(f);
        snprintf(cmd, sizeof cmd, "%s/short.bin", root); f = fopen(cmd, "wb"); fwrite(dump, 1, 100, f); fclose(f);
        CHECK(!dm_amiibo_scan(cmd) && !dm_amiibo_name(), "a file that is no amiibo dump is refused");
        nfp_init();
        NfpDeviceHandle h = {NFP_DEVICE_ID, {0}}; uint8_t out[0x100];
        CHECK(dm_amiibo_scan(path) && !strcmp(dm_amiibo_name(), "Link"), "an amiibo dump goes on the reader");
        CHECK(device_state(&h) == NFP_STATE_INIT, "no detection runs: the game sees nothing yet");
        start(&h);
        CHECK(device_state(&h) == NFP_STATE_FOUND, "a detection finds the amiibo scanned before it");
        CHECK(model_info(out, &h) == NFP_RES_WRONG_STATE, "model info needs a mounted tag");
        CHECK(tag_info(out, &h) == RES_OK && !memcmp(out, "\x04\x11\x22\x33\x44\x55\x66", 7) && out[0x0A] == 7 && out[0x20] == 1 && out[0x24] == 2, "tag info: the 7-byte UID without its check byte, NFC-A type 2");
        CHECK(mount(&h, 0, 3) == RES_OK && device_state(&h) == NFP_STATE_MOUNTED, "mount");
        CHECK(model_info(out, &h) == RES_OK && !memcmp(out, "\x01\x00\x00\x00\x4B\x03\x09", 7), "model info: the model number in host byte order");
        CHECK(common(out, &h) == RES_OK && out[4] == 0x02 && out[5] == 0x01 && out[8] == 0xD8, "common info: the write counter and the application area size");
        CHECK(reg(out, &h) == RES_OK && !strcmp((char *)out + 0x5C, "nxnative"), "register info: the host's owner");
        CHECK(open_area(&h, 0x1019C800) == NFP_RES_AREA_NOT_CREATED, "a fresh amiibo has no application area");
        uint8_t info[0x48] = {0}, data[4] = {1, 2, 3, 4}; uint32_t id = 0x1019C800, size = 4; const void *dp = data;
        memcpy(info, &id, 4); memcpy(info + 8, &dp, 8); memcpy(info + 0x10, &size, 4);
        CHECK(create_area(&h, info) == RES_OK && create_area(&h, info) == NFP_RES_AREA_EXISTS, "the application area is created once");
        uint8_t five = 5; CHECK(set_area(&h, &five, 1) == RES_OK, "a write to the application area");
        CHECK(unmount(&h) == RES_OK && device_state(&h) == NFP_STATE_FOUND, "unmount leaves the amiibo on the reader");
        stop(&h);
        CHECK(!dm_amiibo_name(), "the detection that found it ends the scan");
        start(&h);
        CHECK(device_state(&h) == NFP_STATE_SEARCH, "the next detection needs a new scan");
        CHECK(dm_amiibo_scan(path) && device_state(&h) == NFP_STATE_FOUND, "a scan during a detection is found at once");
        uint64_t got = 0; uint8_t area[0xD8];
        CHECK(mount(&h, 0, 3) == RES_OK && open_area(&h, 1) == NFP_RES_WRONG_AREA_ID && open_area(&h, 0x1019C800) == RES_OK, "the saved application area opens by its access ID only");
        CHECK(get_area(area, &got, &h, sizeof area) == RES_OK && got == 0xD8 && area[0] == 5 && area[1] == 2 && area[3] == 4 && area[4] == 0, "the application area comes back from its file");
        dm_amiibo_remove();
        CHECK(device_state(&h) == NFP_STATE_REMOVED && tag_info(out, &h) == NFP_RES_TAG_NOT_FOUND, "taking the amiibo away is seen by the game");
        stop(&h); nfp_fini();
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); if (system(cmd)) fails++;
    }
    CHECK(lookups == 0, "NFC and NFP hooks do not call an original SDK service path");
    printf("NFC availability: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
