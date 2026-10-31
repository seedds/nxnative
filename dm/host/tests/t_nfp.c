// Virtual amiibo (hle_nfp.c): the file store (new file, Ryujinx's VirtualAmiiboFile form read and written back, unparseable file moved aside) and the nfp state machine
// (scan, events, results, application area, tag UUID); the host services it calls are stubs that record the signalled events
#include "../hle_nfp.c"
#include <pthread.h>
#include <time.h>

static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static void *g_ev[16]; static int g_nev;
static void ev_put(void *g) { pthread_mutex_lock(&g_ev_mu); if (g_nev < 16) g_ev[g_nev] = g; g_nev++; pthread_mutex_unlock(&g_ev_mu); }
void hle_signal_system_event(void *g) { ev_put(g); }
int hle_signal_bound_system_event(void *g) { ev_put(g); return 1; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
uint32_t hle_hid_connected_npad(void) { return 0; }
static uint64_t g_rng;
uint64_t dm_entropy64(void) { return ++g_rng * 0x9E3779B97F4A7C15ull; }
void dm_mii_default_charinfo(void *out) { for (int i = 0; i < 0x58; i++) ((uint8_t *)out)[i] = (uint8_t)(0xa0 + i); }
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

#define ID "0100000000040002"
static char g_dir[1024];

static void new_dir(void) {
    const char *t = getenv("TMPDIR"); snprintf(g_dir, sizeof g_dir, "%s/t_nfp.XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); exit(2); }
    setenv("DM_AMIIBO_DIR", g_dir, 1);
}
static void put_file(const char *name, const char *text) {
    char p[1200]; snprintf(p, sizeof p, "%s/%s", g_dir, name);
    FILE *f = fopen(p, "wb"); if (!f) { perror("fopen"); exit(2); }
    fputs(text, f); fclose(f);
}
static char *get_file(const char *name) {
    static char buf[4096]; char p[1200]; snprintf(p, sizeof p, "%s/%s", g_dir, name);
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); return buf;
}

static void test_new_file(void) {
    new_dir(); amiibo_file f;
    CHECK(amiibo_load(ID, "Wolf Link", &f) == 0, "load of a missing file succeeds");
    CHECK(get_file(ID ".json") != NULL, "the new file is saved");
    CHECK(f.uuid_len == 0 && f.writes == 0 && f.n_areas == 0, "new file is empty");
    CHECK(strcmp(f.nick, "Wolf Link") == 0, "nickname is the default");
    amiibo_free(&f);
    CHECK(amiibo_load(ID, "x", &f) == 0 && strcmp(f.nick, "Wolf Link") == 0, "the saved file loads again");
    amiibo_free(&f);
    CHECK(amiibo_load(ID, "x", &f) == 0, "reload");
    amiibo_free(&f);
    CHECK(amiibo_load("0100000000040003", "Ä\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4", &f) == 0 && strlen(f.nick) == 20, "default nickname is cut to 10 characters");
    amiibo_free(&f);
}

static void test_ryujinx_file(void) {
    new_dir();
    const char *src = "{\"FileVersion\":1,\"TagUuid\":\"AQIDBAUGBwgJ\",\"AmiiboId\":\"" ID "\",\"FirstWriteDate\":\"2024-05-01T12:34:56.789+02:00\",\"LastWriteDate\":\"2024-05-02T08:00:00+02:00\",\"WriteCounter\":3,\"ApplicationAreas\":[{\"ApplicationAreaId\":1234,\"ApplicationArea\":\"3q2+7w==\"}],\"NickName\":null,\"Extra\":{\"a\":[1,2]}}";
    put_file(ID ".json", src);
    amiibo_file f, g;
    CHECK(amiibo_load(ID, "Wolf Link", &f) == 0, "load");
    static const uint8_t u[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(f.uuid_len == 9 && !memcmp(f.uuid, u, 9), "uuid");
    CHECK(f.writes == 3, "write counter");
    CHECK(f.n_areas == 1 && f.areas[0].id == 1234 && f.areas[0].size == 4 && !memcmp(f.areas[0].data, "\xde\xad\xbe\xef", 4), "application area");
    CHECK(f.nick[0] == 0, "null nickname is empty");
    CHECK(amiibo_save(&f) == 0, "save");
    CHECK(amiibo_load(ID, "", &g) == 0, "load again");
    CHECK(!strcmp(g.first, f.first) && !strcmp(g.first, "2024-05-01T12:34:56.789+02:00") && !strcmp(g.last, "2024-05-02T08:00:00+02:00"), "dates");
    CHECK(g.uuid_len == 9 && !memcmp(g.uuid, u, 9) && g.writes == 3 && g.nick[0] == 0, "fields");
    CHECK(g.n_areas == 1 && g.areas[0].id == 1234 && g.areas[0].size == 4 && !memcmp(g.areas[0].data, "\xde\xad\xbe\xef", 4), "area after save");
    char *t = get_file(ID ".json");
    CHECK(t && strstr(t, "\"Extra\":{\"a\":[1,2]}"), "extra member kept");
    CHECK(g.version == 1 && t && strstr(t, "\"FileVersion\":1,"), "FileVersion kept");
    amiibo_free(&f); amiibo_free(&g);
}

static void test_bad_file(void) {
    new_dir();
    const char *bad = "{\"FileVersion\":0,\"TagUu";
    put_file(ID ".json", bad);
    amiibo_file f;
    CHECK(amiibo_load(ID, "Wolf Link", &f) == 0, "load of a bad file succeeds");
    CHECK(f.n_areas == 0 && f.writes == 0 && !strcmp(f.nick, "Wolf Link"), "new file");
    char *t = get_file(ID ".json.bad");
    CHECK(t && !strcmp(t, bad), "old text in .bad");
    amiibo_free(&f);
}

#define RES(d) ((uint32_t)(d) << 9 | 115u)
static uint64_t g_act, g_deact;   // stand-ins for the guest's SystemEventType objects
static NfpDeviceHandle g_h;

static int ev_count(void) { pthread_mutex_lock(&g_ev_mu); int n = g_nev; pthread_mutex_unlock(&g_ev_mu); return n; }
static int wait_events(int n) {   // waits up to 1 s for n recorded events
    for (int i = 0; i < 100 && ev_count() < n; i++) nanosleep(&(struct timespec){0, 10000000}, NULL);
    return ev_count() >= n;
}
static void nfp_start(void) {   // a fresh amiibo folder, Initialize, ListDevices, the two events, StartDetection
    new_dir(); h_nfp_Fini(); h_nfp_Init();
    pthread_mutex_lock(&g_ev_mu); g_nev = 0; pthread_mutex_unlock(&g_ev_mu);
    int n = 0; NfpDeviceHandle hs[4];
    CHECK(h_nfp_List(hs, &n, 4) == RES_OK && n == 1, "one device");
    g_h = hs[0];
    CHECK(h_nfp_AttachAct(&g_act, &g_h) == RES_OK && h_nfp_AttachDeact(&g_deact, &g_h) == RES_OK, "events attached");
}

static void test_scan_flow(void) {
    nfp_start();
    CHECK(h_nfp_Start(&g_h) == RES_OK && h_nfp_State(&g_h) == 1, "searching after StartDetection");
    CHECK(dm_amiibo_searching() == 1, "dm_amiibo_searching while searching");
    CHECK(dm_amiibo_scan(ID, "Wolf Link", 0) == 1, "scan placed");
    CHECK(h_nfp_State(&g_h) == 2 && dm_amiibo_searching() == 0, "tag found");
    CHECK(wait_events(2) && g_ev[0] == &g_act && g_ev[1] == &g_deact, "activate, then deactivate");
    CHECK(h_nfp_Mount(&g_h, 0, 3) == RES_OK && h_nfp_State(&g_h) == 4, "mounted");
    uint8_t m[0x40]; memset(m, 0xee, sizeof m);
    CHECK(h_nfp_ModelInfo(m, &g_h) == RES_OK, "GetModelInfo");
    CHECK(m[0] == 0x01 && m[1] == 0x00 && m[2] == 0 && m[3] == 0 && m[4] == 0x04 && m[5] == 0 && m[6] == 0 && m[7] == 0 && m[0x3f] == 0, "model info of Wolf Link");
    CHECK(h_nfp_Unmount(&g_h) == RES_OK && h_nfp_State(&g_h) == 2, "unmounted");
    CHECK(h_nfp_Stop(&g_h) == RES_OK && h_nfp_State(&g_h) == 0, "stopped");
    nanosleep(&(struct timespec){0, 200000000}, NULL);
    CHECK(ev_count() == 2, "no further event");
}

static void test_not_searching(void) {
    nfp_start();
    CHECK(dm_amiibo_scan(ID, "Wolf Link", 0) == 0 && h_nfp_State(&g_h) == 0, "scan ignored without StartDetection");
}

static void test_wrong_state(void) {
    nfp_start();
    uint8_t t[0x58];
    CHECK(h_nfp_Mount(&g_h, 0, 3) == RES(73), "Mount without a tag");
    CHECK(h_nfp_TagInfo(t, &g_h) == RES(73), "GetTagInfo without a tag");
    NfpDeviceHandle other = { NFP_DEVICE_ID + 1, {0} };
    CHECK(h_nfp_Mount(&other, 0, 3) == RES(64), "another device handle");
    h_nfp_Start(&g_h); dm_amiibo_scan(ID, "Wolf Link", 0);
    CHECK(h_nfp_Mount(&g_h, 1, 3) == RES(65), "Mount with model type 1");
    wait_events(2);
}

static void test_area(void) {
    nfp_start(); h_nfp_Start(&g_h); dm_amiibo_scan(ID, "Wolf Link", 0); h_nfp_Mount(&g_h, 0, 3);
    CHECK(h_nfp_OpenArea(&g_h, 1234) == RES(128), "open a missing area");
    uint32_t big[4] = {1234, 0xd9, 0, 0}; uint64_t pbig = (uint64_t)(uintptr_t)g_dir; memcpy(&big[2], &pbig, 8);
    CHECK(h_nfp_CreateArea(&g_h, big) == RES(65) && h_nfp_OpenArea(&g_h, 1234) == RES(128), "create with 0xd9 bytes: wrong argument, no area");
    uint32_t init[4] = {0}; uint64_t pinit = (uint64_t)(uintptr_t)"\x11\x22\x33\x44";
    init[0] = 1234; init[1] = 4; memcpy(&init[2], &pinit, 8);
    CHECK(h_nfp_CreateArea(&g_h, init) == RES_OK, "create");
    CHECK(h_nfp_CreateArea(&g_h, init) == RES(168), "create again");
    CHECK(h_nfp_OpenArea(&g_h, 1234) == RES_OK, "open");
    uint8_t b[16]; uint64_t n = 0; memset(b, 0, sizeof b);
    CHECK(h_nfp_GetArea(b, &n, &g_h, 2) == RES_OK && n == 2 && b[0] == 0x11 && b[1] == 0x22 && b[2] == 0, "read into a 2-byte buffer");
    CHECK(h_nfp_SetArea(&g_h, "ABCDEFGH", 8) == RES_OK, "write 8 bytes");
    wait_events(2);
    h_nfp_Fini(); h_nfp_Init();
    CHECK(h_nfp_Start(&g_h) == RES_OK && dm_amiibo_scan(ID, "Wolf Link", 0) == 1 && h_nfp_Mount(&g_h, 0, 3) == RES_OK && h_nfp_OpenArea(&g_h, 1234) == RES_OK, "scan and open again");
    memset(b, 0, sizeof b); n = 0;
    CHECK(h_nfp_GetArea(b, &n, &g_h, sizeof b) == RES_OK && n == 8 && !memcmp(b, "ABCDEFGH", 8), "the data is read back from the file");
    wait_events(4);
}

static void tag_uuid(uint8_t *u) {
    uint8_t t[0x58]; memset(t, 0xee, sizeof t);
    CHECK(h_nfp_TagInfo(t, &g_h) == RES_OK && t[0x0a] == 9 && t[9] == 0, "GetTagInfo");
    uint32_t proto, type; memcpy(&proto, t + 0x20, 4); memcpy(&type, t + 0x24, 4);
    CHECK(proto == 0xffffffffu && type == 0xffffffffu, "protocol and tag type");
    memcpy(u, t, 9);
}
static void test_uuid(void) {
    uint8_t a[9], b[9]; amiibo_file f;
    nfp_start(); h_nfp_Start(&g_h); dm_amiibo_scan(ID, "Wolf Link", 0); tag_uuid(a);
    h_nfp_Start(&g_h); dm_amiibo_scan(ID, "Wolf Link", 0); tag_uuid(b);
    CHECK(!memcmp(a, b, 9), "random off: the same UUID");
    CHECK(a[3] == (0x88 ^ a[0] ^ a[1] ^ a[2]) && a[8] == (a[3] ^ a[4] ^ a[5] ^ a[6]), "UUID check bytes");
    CHECK(amiibo_load(ID, "", &f) == 0 && f.uuid_len == 9 && !memcmp(f.uuid, a, 9), "the UUID is stored in the file");
    amiibo_free(&f);
    h_nfp_Start(&g_h); dm_amiibo_scan(ID, "Wolf Link", 1); tag_uuid(a);
    h_nfp_Start(&g_h); dm_amiibo_scan(ID, "Wolf Link", 1); tag_uuid(b);
    CHECK(memcmp(a, b, 9) != 0, "random on: different UUIDs");
    CHECK(a[3] == (0x88 ^ a[0] ^ a[1] ^ a[2]) && b[8] == (b[3] ^ b[4] ^ b[5] ^ b[6]), "random UUID check bytes");
    CHECK(amiibo_load(ID, "", &f) == 0 && f.uuid_len == 9 && memcmp(f.uuid, a, 9) && memcmp(f.uuid, b, 9), "the file's TagUuid does not change");
    amiibo_free(&f);
    wait_events(8);
}

static void test_rescan(void) {
    nfp_start(); h_nfp_Start(&g_h);
    CHECK(dm_amiibo_scan(ID, "Wolf Link", 0) == 1, "first scan");
    CHECK(wait_events(2), "first event pair");
    h_nfp_Start(&g_h);
    CHECK(dm_amiibo_scan("01000000034b0902", "Link (Archer)", 0) == 1, "second scan without StopDetection");
    CHECK(wait_events(4) && g_ev[2] == &g_act && g_ev[3] == &g_deact, "the event pair again");
    uint8_t m[0x40];
    CHECK(h_nfp_Mount(&g_h, 0, 3) == RES_OK && h_nfp_ModelInfo(m, &g_h) == RES_OK && m[0] == 0x01 && m[1] == 0x00 && m[4] == 0x4b && m[5] == 0x03 && m[6] == 0x09, "the second amiibo's model info");
}

int main(void) {
    test_new_file(); test_ryujinx_file(); test_bad_file();
    test_scan_flow(); test_not_searching(); test_wrong_state(); test_area(); test_uuid(); test_rescan();
    if (fails) { fprintf(stderr, "t_nfp: %d of %d checks failed\n", fails, checks); return 1; }
    printf("t_nfp: %d checks ok\n", checks);
    return 0;
}
