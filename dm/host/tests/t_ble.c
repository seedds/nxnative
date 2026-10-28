// Offline BLE hooks: resolve the actual SDK registrations, keep discovery empty,
// and bind idle events without opening an IPC session or touching output buffers.
#include "../hle_offline.c"
#include <mach-o/getsect.h>
#include <assert.h>

static int bound_events;
void hle_bind_system_event(void *ev, int clear) {
    assert(ev != NULL && clear == 0);
    bound_events++;
}
int dm_sk_poll(void *fds, uint64_t n, int ms) { (void)fds; (void)n; (void)ms; abort(); }
int dm_sk_select(int n, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *t) {
    (void)n; (void)r; (void)w; (void)e; (void)t; abort();
}
extern const struct mach_header_64 _mh_execute_header;
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *entries = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL;
    unsigned count = 0;
    for (size_t i = 0; entries && i < bytes / sizeof *entries; i++) {
        if (!strcmp(entries[i].name, name)) { fn = (void *)entries[i].fn; count++; }
    }
    if (count != 1 || !fn) { fprintf(stderr, "Missing or duplicate BLE hook: %s\n", name); exit(1); }
    return fn;
}
int main(void) {
    Result (*init)(void) = hook("_ZN2nn9bluetooth13InitializeBleEv");
    assert(init() == RES_OK);
    const char *events[] = {
        "_ZN2nn9bluetooth19AcquireBleScanEventEPNS_2os15SystemEventTypeE",
        "_ZN2nn9bluetooth37AcquireBleConnectionStateChangedEventEPNS_2os15SystemEventTypeE",
        "_ZN2nn9bluetooth31AcquireBleServiceDiscoveryEventEPNS_2os15SystemEventTypeE",
        "_ZN2nn9bluetooth24AcquireBleMtuConfigEventEPNS_2os15SystemEventTypeE",
        "_ZN2nn9bluetooth28AcquireBleGattOperationEventEPNS_2os15SystemEventTypeE",
    };
    uint8_t ev[0x100] = {0};
    for (size_t i = 0; i < sizeof events / sizeof *events; i++) {
        Result (*bind)(void *) = hook(events[i]);
        assert(bind(ev) == RES_OK);
        assert(bound_events == (int)i + 1);
    }
    Result (*start)(const void *) = hook("_ZN2nn9bluetooth23StartBleScanSmartDeviceERKNS0_17GattAttributeUuidE");
    Result (*stop)(void) = hook("_ZN2nn9bluetooth22StopBleScanSmartDeviceEv");
    assert(start(ev) == RES_OK && stop() == RES_OK);
    const char *lists[] = {
        "_ZN2nn9bluetooth16GetBleScanResultEPNS0_13BleScanResultEi",
        "_ZN2nn9bluetooth24GetBleConnectionInfoListEPNS0_17BleConnectionInfoEi",
    };
    for (size_t i = 0; i < sizeof lists / sizeof *lists; i++) {
        int (*list)(void *, int) = hook(lists[i]);
        uint8_t output[128], before[128];
        memset(output, 0xa5, sizeof output); memcpy(before, output, sizeof before);
        assert(list(output, 1) == 0 && !memcmp(output, before, sizeof output));
        assert(list(NULL, 0) == 0);
    }
    puts("BLE: 10 unique SDK hooks, 5 idle event bindings, empty discovery and untouched output: OK");
    return 0;
}
