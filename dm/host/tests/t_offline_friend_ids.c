// SDK overloads from Super Kirby Clash (#987) must use the same offline behavior
// as the Friend-object list and ErrorCode forms, without entering an SDK service.
#define DM_LOG_IMPL
#include "../hle_offline.c"
#include <mach-o/getsect.h>
#include <stdarg.h>

static int unexpected, checks, failed, enters, leaves;
static char message[256];
void dm_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(message, sizeof message, fmt, ap); va_end(ap);
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
int main(void) {
    Result (*ids)(int *, void *, int, int, const void *) = hook("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdEiiRKNS0_12FriendFilterE");
    Result (*ids_uid)(int *, void *, const void *, int, int, const void *) = hook("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEiiRKNS0_12FriendFilterE");
    void (*show)(Result) = hook("_ZN2nn3err23ShowNetworkServiceErrorENS_6ResultE");
    uint64_t output[2] = {0x123456789abcdef0, 0xfedcba9876543210}, before[2];
    uint8_t uid[16] = {1}, filter[32] = {0};
    memcpy(before, output, sizeof before);
    if (ids) {
        int count = 7;
        CHECK(ids(&count, output, 2, 2, filter) == RES_FAIL, "friend IDs without Uid remain unavailable offline");
        CHECK(count == 0, "unavailable ID list reports zero entries");
        CHECK(!memcmp(output, before, sizeof output), "unavailable ID list leaves client storage untouched");
        count = 7;
        CHECK(ids(&count, NULL, 0, 0, filter) == RES_FAIL && count == 0, "zero-capacity ID query remains empty");
    }
    if (ids_uid) {
        int count = 7;
        CHECK(ids_uid(&count, output, uid, 2, 2, filter) == RES_FAIL, "friend IDs with Uid remain unavailable offline");
        CHECK(count == 0, "unavailable per-user ID list reports zero entries");
        CHECK(!memcmp(output, before, sizeof output), "unavailable per-user ID list leaves client storage untouched");
        count = 7;
        CHECK(ids_uid(&count, NULL, uid, 0, 0, filter) == RES_FAIL && count == 0, "zero-capacity per-user query remains empty");
    }
    if (show) {
        show(0x8ad23u); // module 291, description 1110; Result is not an ErrorCode pair
        CHECK(strstr(message, "module 291") && strstr(message, "description 1110"), "Result error logs its module and description");
        CHECK(enters == 1 && leaves == 1, "error applet notification enters and leaves once");
    }
    CHECK(unexpected == 0, "offline overloads use no network service or event binding");
    printf("offline friend IDs: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
