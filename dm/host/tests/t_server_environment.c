// Offline service takeovers are looked up by their SDK names in the real swforce registrations. A missing or misspelled name would let the SDK call its own service client.
#define DM_LOG_IMPL
#include "../hle_offline.c"
#include <mach-o/getsect.h>
#include <stdarg.h>

static int unexpected, checks, failed, enters, leaves;
static char message[256];
void dm_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
}
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; unexpected++; }
void dm_applet_enter(const char *name) { if (strcmp(name, "error")) unexpected++; enters++; }
void dm_applet_leave(void) { leaves++; }
// hle_offline.c's socket overloads go to the host's sockets (hle_net.c): none may run here
int dm_sk_socket(int f, int t, int p) { (void)f; (void)t; (void)p; unexpected++; return -1; }
int dm_sk_bind(int fd, const void *a, unsigned n) { (void)fd; (void)a; (void)n; unexpected++; return -1; }
int dm_sk_connect(int fd, const void *a, unsigned n) { (void)fd; (void)a; (void)n; unexpected++; return -1; }
int dm_sk_shutdown(int fd, int how) { (void)fd; (void)how; unexpected++; return -1; }
int dm_sk_name(int fd, void *a, unsigned *n, int peer) { (void)fd; (void)a; (void)n; (void)peer; unexpected++; return -1; }
int dm_sk_getopt(int fd, int l, int o, void *v, unsigned *n) { (void)fd; (void)l; (void)o; (void)v; (void)n; unexpected++; return -1; }
int dm_sk_setopt(int fd, int l, int o, const void *v, unsigned n) { (void)fd; (void)l; (void)o; (void)v; (void)n; unexpected++; return -1; }
int dm_sk_fcntl(int fd, int c, int v) { (void)fd; (void)c; (void)v; unexpected++; return -1; }
long dm_sk_recvfrom(int fd, void *d, unsigned long n, int f, void *a, unsigned *an) { (void)fd; (void)d; (void)n; (void)f; (void)a; (void)an; unexpected++; return -1; }
long dm_sk_sendto(int fd, const void *d, unsigned long n, int f, const void *a, unsigned an) { (void)fd; (void)d; (void)n; (void)f; (void)a; (void)an; unexpected++; return -1; }
int dm_sk_poll(void *fds, uint64_t n, int ms) { (void)fds; (void)n; (void)ms; unexpected++; return -1; }
int dm_sk_select(int n, uint64_t *r, uint64_t *w, uint64_t *e, const int64_t *tv) { (void)n; (void)r; (void)w; (void)e; (void)tv; unexpected++; return -1; }
int dm_sk_errno(void) { unexpected++; return 0; }
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *entries = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL;
    unsigned count = 0;
    for (size_t i = 0; entries && i < bytes / sizeof *entries; i++) {
        if (strcmp(entries[i].name, name)) continue;
        count++; fn = entries[i].fn;
        CHECK(entries[i].why && entries[i].why[0], "takeover has a reason");
    }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
static void check_server_environment(void) {
    int (*type)(void) = hook("_ZN2nn3nsd35GetApplicationServerEnvironmentTypeEv");
    void (*identifier)(char *) = hook("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE");
    if (type && identifier) {
        struct { char value[8]; uint8_t guard[8]; } env;
        memset(&env, 0xa5, sizeof env);
        identifier(env.value);
        CHECK(!memcmp(env.value, "lp1\0\0\0\0\0", 8), "the environment identifier is retail lp1");
        for (size_t i = 0; i < sizeof env.guard; i++) CHECK(env.guard[i] == 0xa5, "identifier writes only its eight-byte output");
        // oe::ServerEnvironmentType 1 is lp1 (the game's enum-to-name table): the type agrees with the identifier. oe::GetServerEnvironmentType reaches it through the sdk's PLT
        CHECK(type() == 1, "the server environment type is retail lp1 (1)");
    }
    Result (*status)(uint32_t *) = hook("_ZN2nn5prepo21GetTransmissionStatusEPNS0_18TransmissionStatusE");
    if (status) {
        uint32_t out = UINT32_MAX;
        CHECK(status(&out) == RES_OK, "play report status succeeds offline");
        CHECK(out == 0, "play report transmission is idle");
    }
}

static void check_friend_ids(void) {
    Result (*ids)(int *, void *, int, int, const void *) = hook("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdEiiRKNS0_12FriendFilterE");
    Result (*ids_uid)(int *, void *, const void *, int, int, const void *) = hook("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEiiRKNS0_12FriendFilterE");
    const uint64_t before[] = {0x123456789abcdef0, 0xfedcba9876543210};
    uint64_t output[2];
    uint8_t uid[16] = {1}, filter[32] = {0};
    if (ids) {
        int count = 7;
        memcpy(output, before, sizeof output);
        CHECK(ids(&count, output, 2, 2, filter) == RES_FAIL, "friend IDs without Uid remain unavailable offline");
        CHECK(count == 0, "unavailable ID list reports zero entries");
        CHECK(!memcmp(output, before, sizeof output), "unavailable ID list leaves client storage untouched");
        count = 7;
        CHECK(ids(&count, NULL, 0, 0, filter) == RES_FAIL && count == 0, "zero-capacity ID query remains empty");
    }
    if (ids_uid) {
        int count = 7;
        memcpy(output, before, sizeof output);
        CHECK(ids_uid(&count, output, uid, 2, 2, filter) == RES_FAIL, "friend IDs with Uid remain unavailable offline");
        CHECK(count == 0, "unavailable per-user ID list reports zero entries");
        CHECK(!memcmp(output, before, sizeof output), "unavailable per-user ID list leaves client storage untouched");
        count = 7;
        CHECK(ids_uid(&count, NULL, uid, 0, 0, filter) == RES_FAIL && count == 0, "zero-capacity per-user query remains empty");
    }
}

static void check_ble(void) {
    void (*init)(void) = hook("_ZN2nn9bluetooth13InitializeBleEv");
    Result (*scan_start)(const void *) = hook("_ZN2nn9bluetooth23StartBleScanSmartDeviceERKNS0_17GattAttributeUuidE");
    Result (*scan_stop)(void) = hook("_ZN2nn9bluetooth22StopBleScanSmartDeviceEv");
    int (*scan_res)(void *, int) = hook("_ZN2nn9bluetooth16GetBleScanResultEPNS0_13BleScanResultEi");
    int (*conn_list)(void *, int) = hook("_ZN2nn9bluetooth24GetBleConnectionInfoListEPNS0_17BleConnectionInfoEi");
    Result (*connect)(const void *) = hook("_ZN2nn9bluetooth19ConnectToGattServerERKNS0_7AddressE");
    Result (*disconnect)(unsigned) = hook("_ZN2nn9bluetooth24DisconnectFromGattServerEj");
    Result (*reg_notif)(const void *) = hook("_ZN2nn9bluetooth33RegisterGattOperationNotificationERKNS0_17GattAttributeUuidE");
    Result (*unreg_notif)(const void *) = hook("_ZN2nn9bluetooth35UnregisterGattOperationNotificationERKNS0_17GattAttributeUuidE");
    const uint8_t uuid_before[20] = {16, 0, 0, 0, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01};
    uint8_t uuid[20], buf[32], buf_before[32], addr[6] = {1, 2, 3, 4, 5, 6};
    memcpy(uuid, uuid_before, sizeof uuid);
    memset(buf_before, 0x5a, sizeof buf_before);
    memcpy(buf, buf_before, sizeof buf);
    if (init) init();
    if (reg_notif && unreg_notif) {
        CHECK(reg_notif(uuid) == RES_OK, "registering a BLE GATT notification path succeeds offline");
        CHECK(!memcmp(uuid, uuid_before, sizeof uuid), "registering a GATT notification path leaves the UUID untouched");
        CHECK(unreg_notif(uuid) == RES_OK, "unregistering a BLE GATT notification path succeeds offline");
        CHECK(!memcmp(uuid, uuid_before, sizeof uuid), "unregistering a GATT notification path leaves the UUID untouched");
    }
    if (scan_start && scan_stop && scan_res) {
        CHECK(scan_start(uuid) == RES_FAIL, "smart-device BLE scan fails offline");
        CHECK(scan_stop() == RES_OK, "stopping a BLE scan succeeds offline");
        CHECK(scan_res(buf, 4) == 0, "BLE scan reports zero results offline");
        CHECK(!memcmp(buf, buf_before, sizeof buf), "empty BLE scan leaves result buffer untouched");
    }
    if (conn_list && connect && disconnect) {
        CHECK(conn_list(buf, 4) == 0, "BLE connection list is empty offline");
        CHECK(!memcmp(buf, buf_before, sizeof buf), "empty BLE connection list leaves buffer untouched");
        CHECK(connect(addr) == RES_FAIL, "BLE GATT connect fails offline");
        CHECK(disconnect(0) == RES_FAIL, "BLE GATT disconnect fails with no connection");
        CHECK(disconnect(1) == RES_FAIL, "BLE GATT disconnect fails for any handle offline");
    }
}

static void check_network_error(void) {
    void (*show)(Result) = hook("_ZN2nn3err23ShowNetworkServiceErrorENS_6ResultE");
    if (show) {
        show(0x8ad23u); // Result with module 291 and description 1110
        CHECK(strstr(message, "module 291") && strstr(message, "description 1110"), "Result error logs its module and description");
        CHECK(enters == 1 && leaves == 1, "error applet notification enters and leaves once");
    }
}

int main(void) {
    check_server_environment();
    check_friend_ids();
    check_ble();
    CHECK(enters == 0 && leaves == 0, "offline queries open no applet");
    check_network_error();
    CHECK(unexpected == 0, "offline handlers need no event binding or network operation");
    printf("offline services: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
