// hle_mii.c unit test: the console's Mii database (<DM_SYSTEM_DIR>/save/8000000000000030/MiiDatabase.dat). A StoreData's bit fields unpack into the CharInfo fields they hold
// (eyebrow y 3 higher), a file with a wrong CRC, size or version leaves the database empty, and Get lists the database's Miis (source 0) before the six default ones (source 1).
// AppendMii adds the default Mii after them, keeps it in <DM_SYSTEM_DIR>/host/MiiDatabase.dat (the console's file untouched), and a later start lists it again
#include "../hle_mii.c"
#include <unistd.h>
static uint64_t g_entropy = 0x0123456789abcdefull;
uint64_t dm_entropy64(void) { return g_entropy += 0x1111; }
const char *dm_system_dir(char *buf, size_t n) { (void)buf; (void)n; const char *v = getenv("DM_SYSTEM_DIR"); return v ? v : "build/t_mii.none"; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); puts(""); fails++; } } while (0)
static void put_bits(uint8_t *sd, int word, int at, int len, unsigned v) {
    uint32_t x; memcpy(&x, sd + 4 * word, 4); x = (x & ~(((1u << len) - 1) << at)) | ((v & ((1u << len) - 1)) << at); memcpy(sd + 4 * word, &x, 4);
}
static void db_crc(uint8_t *b) { uint16_t c = mii_crc16(b, 0x1a96); b[0x1a96] = (uint8_t)(c >> 8); b[0x1a97] = (uint8_t)c; }

int main(void) {
    static uint8_t db[MII_DB_SIZE];
    memcpy(db, "NFDB", 4); db[0x1a94] = 1; db[0x1a95] = 2;
    uint8_t *a = db + 4, *b = db + 4 + MII_STORE;
    put_bits(a, 0, 0, 8, 131); put_bits(a, 0, 8, 7, 100); put_bits(a, 0, 24, 7, 99); put_bits(a, 0, 31, 1, 1);   // hair 131, height 100, hair colour 99, special
    put_bits(a, 1, 7, 1, 1); put_bits(a, 2, 8, 6, 59); put_bits(a, 2, 24, 5, 18); put_bits(a, 3, 0, 5, 23);      // female, eye type 59, eye y 18, eyebrow type 23
    put_bits(a, 5, 8, 4, 11); put_bits(a, 5, 28, 4, 12); put_bits(a, 6, 12, 4, 15); put_bits(a, 6, 28, 4, 8);    // favourite colour 11, eye x 12, eyebrow y 15 (stored), mole scale 8
    const char nick[] = "Jo"; for (int i = 0; nick[i]; i++) { a[0x1c + 2 * i] = (uint8_t)nick[i]; }
    for (int i = 0; i < 16; i++) { a[0x30 + i] = (uint8_t)(0xa0 + i); b[0x30 + i] = (uint8_t)(0xb0 + i); }
    db_crc(db);

    mii_charinfo c[MII_DB_MAX];
    CHECK(mii_db_parse(db, sizeof db, c) == 2, "two Miis parsed");
    CHECK(c[0].hair_type == 131 && c[0].height == 100 && c[0].hair_color == 99 && c[0].type == 1, "word 0: hair %d height %d colour %d type %d", c[0].hair_type, c[0].height, c[0].hair_color, c[0].type);
    CHECK(c[0].gender == 1 && c[0].eye_type == 59 && c[0].eye_y == 18 && c[0].eyebrow_type == 23, "words 1-3");
    CHECK(c[0].favorite_color == 11 && c[0].eye_x == 12 && c[0].mole_scale == 8, "words 5-6");
    CHECK(c[0].eyebrow_y == 18, "eyebrow y is the stored one + 3: %d", c[0].eyebrow_y);
    CHECK(c[0].nickname[0] == 'J' && c[0].nickname[1] == 'o' && c[0].nickname[2] == 0 && c[0].nickname[10] == 0, "nickname");
    CHECK(c[0].create_id[0] == 0xa0 && c[0].create_id[15] == 0xaf && c[1].create_id[0] == 0xb0, "create IDs");
    CHECK(c[0].height == 100 && c[1].height == 0, "the second Mii is its own record");

    static uint8_t bad[MII_DB_SIZE]; memcpy(bad, db, sizeof db); bad[0x100] ^= 1;
    CHECK(mii_db_parse(bad, sizeof bad, c) == -3, "a CRC mismatch is refused");
    CHECK(mii_db_parse(db, sizeof db - 1, c) == -1, "a short file is refused");
    memcpy(bad, db, sizeof db); bad[0x1a94] = 2; db_crc(bad);
    CHECK(mii_db_parse(bad, sizeof bad, c) == -2, "version 2 is refused");

    // Through the service calls, from a DM_SYSTEM_DIR in the build folder
    char root[] = "build/t_mii.XXXXXX", p[256];
    CHECK(mkdtemp(root) != NULL, "temporary folder");
    snprintf(p, sizeof p, "%s/save", root); mkdir(p, 0755); snprintf(p, sizeof p, "%s/save/8000000000000030", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/save/8000000000000030/MiiDatabase.dat", root);
    FILE *f = fopen(p, "wb"); CHECK(f && fwrite(db, 1, sizeof db, f) == sizeof db, "write the database"); if (f) fclose(f);
    setenv("DM_SYSTEM_DIR", root, 1);
    CHECK(h_mii_Init(NULL) == RES_OK && g_mii_n == 2, "Initialize reads the database: %d", g_mii_n);
    CHECK(h_mii_Count(NULL, MII_SRC_DATABASE) == 2 && h_mii_Count(NULL, MII_SRC_DEFAULT) == 6 && h_mii_Count(NULL, 3) == 8, "counts");
    struct __attribute__((packed)) { mii_charinfo c; uint32_t source; } el[10]; int n = -1;
    CHECK(h_mii_GetE(NULL, &n, el, 10, 3) == RES_OK && n == 8, "Get of both: %d", n);
    CHECK(el[0].source == 0 && el[1].source == 0 && el[2].source == 1 && !memcmp(el[0].c.create_id, a + 0x30, 16), "the database's Miis first, then the defaults");
    CHECK(h_mii_GetE(NULL, &n, el, 1, 3) == RES_OK && n == 1, "Get stops at the room given");
    int idx = -1;
    CHECK(h_mii_Index(NULL, &idx, &el[0].c) == RES_OK && idx == 0, "GetIndex by create ID");
    CHECK(h_mii_Index(NULL, &idx, &el[2].c) == MII_RES_NOT_FOUND, "a default Mii is not in the database");
    mii_charinfo latest = el[1].c; latest.height = 7;
    CHECK(h_mii_Latest(NULL, &latest, MII_SRC_DATABASE) == RES_OK && latest.height == c[1].height, "UpdateLatest gives the database's copy");

    // AppendMii: the default Mii after the console's two, kept in the host's file; the console's file is not written
    struct stat before; stat(p, &before);
    int ai = -1;
    CHECK(h_mii_Append(&ai) == RES_OK && ai == 2 && g_mii_n == 3 && g_mii_own == 2, "AppendMii adds entry 2: %d, %d Miis", ai, g_mii_n);
    CHECK(h_mii_Updated(NULL, MII_SRC_DEFAULT) == 0 && h_mii_Updated(NULL, MII_SRC_DATABASE) == 1 && h_mii_Updated(NULL, MII_SRC_DATABASE) == 0, "IsUpdated once, for the database");
    CHECK(h_mii_Count(NULL, MII_SRC_DATABASE) == 3 && h_mii_GetE(NULL, &n, el, 10, 3) == RES_OK && n == 9 && el[2].source == 0 && el[3].source == 1, "Get lists the new Mii in the database");
    mii_charinfo made = el[2].c;
    CHECK(made.nickname[0] == 'n' && made.height == 64 && memcmp(made.create_id, el[0].c.create_id, 16), "the default Mii, with a create ID of its own");
    CHECK(h_mii_Index(NULL, &idx, &made) == RES_OK && idx == 2, "GetIndex finds the new Mii");
    struct stat after; stat(p, &after);
    CHECK(after.st_size == before.st_size && after.st_mtimespec.tv_nsec == before.st_mtimespec.tv_nsec && after.st_mtimespec.tv_sec == before.st_mtimespec.tv_sec, "the console's file is not written");
    char hp[256]; snprintf(hp, sizeof hp, "%s/host/MiiDatabase.dat", root);
    CHECK(stat(hp, &after) == 0 && after.st_size == MII_HOST_HEAD + (off_t)sizeof(mii_charinfo), "the host's file holds one Mii: %lld bytes", (long long)after.st_size);
    // the next start lists it again, with the same create ID
    memset(g_mii_db, 0, sizeof g_mii_db); mii_db_load();
    CHECK(g_mii_n == 3 && g_mii_own == 2 && !memcmp(g_mii_db[2].create_id, made.create_id, 16), "a later start reads the host's Mii back: %d", g_mii_n);
    // a host file that is not the host's database is refused and lists nothing
    FILE *hf = fopen(hp, "r+b"); if (hf) { fputc('X', hf); fclose(hf); }
    mii_db_load(); CHECK(g_mii_n == 2, "a damaged host file is refused: %d", g_mii_n);
    // a full database cancels
    g_mii_n = MII_DB_MAX; CHECK(h_mii_Append(&ai) == MII_RES_CANCELED, "a full database cancels"); g_mii_n = 2;
    unlink(hp); snprintf(hp, sizeof hp, "%s/host", root); rmdir(hp);
    unlink(p); snprintf(p, sizeof p, "%s/save/8000000000000030", root); rmdir(p); snprintf(p, sizeof p, "%s/save", root); rmdir(p); rmdir(root);

    printf("t_mii: %s\n", fails ? "FAILED" : "all checks pass");
    return fails != 0;
}
