// Mii service layer (nn::mii::Database, the Mii editor applet). On hardware Database talks to the mii:u service (an IDatabaseService the SDK keeps at Database +0) and AppendMii /
// ShowMiiEdit start the MiiEdit library applet; the host has neither. It answers with the six default Miis, the player's console database when they supply it (mii_db_load: the
// mii service's system save, read-only), and Miis made in its stand-in for the editor (AppendMii: the default Mii, kept in a file of the host's own).
// Mario Kart 8 Deluxe (SDK 4.4.0) initializes a Database at boot (mii:e and mii:u were the first services it opened itself) and builds 12 random Miis for its title screen's demo race; their
// heads are drawn by the SDK's Mii rendering code linked into main, from the MiiModel system data (the host's stand-in below, mounted by hle_fs.c MountSystemData).
// The CharInfo the host builds (0x58 bytes) is the system's default Mii as the mii service's StoreData::BuildDefault makes it: the 3DS-era default parameters, with the colours converted to
// the Switch's common colour indices (hair 1, eyes 8, mouth 19, glasses 8), a create ID of UUID version 4 (fixed for each default Mii, fresh for a random one) and the nickname "no name";
// every field is within CharInfoRaw::Verify's ranges (SDK 4.4.0 sdk+0x1b68c8), so CharInfoAccessor::IsValid accepts it.
#include "hle.h"
#include <sys/stat.h>
#include <unistd.h>

uint64_t dm_entropy64(void);

#define MII_RES_NOT_FOUND 0x87Eu        // nn::mii module 126, description 4
#define MII_RES_CANCELED 0xE7Eu         // description 7: the player closed the applet (the SDK's ShowMiiEdit treats it as success)
#define MII_RES_TEST_MODE 0x1987Eu      // description 204: importing a database needs the system's test mode

typedef struct __attribute__((packed)) {
    uint8_t create_id[16]; uint16_t nickname[11];
    uint8_t font_region, favorite_color, gender, height, build, type, region_move;
    uint8_t faceline_type, faceline_color, faceline_wrinkle, faceline_make;
    uint8_t hair_type, hair_color, hair_flip;
    uint8_t eye_type, eye_color, eye_scale, eye_aspect, eye_rotate, eye_x, eye_y;
    uint8_t eyebrow_type, eyebrow_color, eyebrow_scale, eyebrow_aspect, eyebrow_rotate, eyebrow_x, eyebrow_y;
    uint8_t nose_type, nose_scale, nose_y;
    uint8_t mouth_type, mouth_color, mouth_scale, mouth_aspect, mouth_y;
    uint8_t beard_color, beard_type, mustache_type, mustache_scale, mustache_y;
    uint8_t glass_type, glass_color, glass_scale, glass_y;
    uint8_t mole_type, mole_scale, mole_x, mole_y, reserved;
} mii_charinfo;
_Static_assert(sizeof(mii_charinfo) == 0x58, "nn::mii::CharInfo is 0x58 bytes");

// index ≥ 0: default Mii <index> (a fixed create ID, so a game that saved one finds the same Mii again); < 0: a random Mii (a fresh one)
static void mii_default(mii_charinfo *c, int gender, int index) {
    memset(c, 0, sizeof *c);
    uint64_t a = index >= 0 ? 0x4d49494e44454641ull + (uint64_t)index : dm_entropy64(), b = index >= 0 ? 0x554c54444d484f53ull : dm_entropy64();
    memcpy(c->create_id, &a, 8); memcpy(c->create_id + 8, &b, 8);
    c->create_id[6] = (uint8_t)((c->create_id[6] & 0x0f) | 0x40); c->create_id[8] = (uint8_t)((c->create_id[8] & 0x3f) | 0x80);
    if (index >= 0) c->favorite_color = (uint8_t)(index * 2);   // red, yellow, green, sky blue, purple, white
    static const char name[] = "no name";
    for (size_t i = 0; i < sizeof name - 1; i++) c->nickname[i] = (uint16_t)name[i];
    c->gender = gender == 1; c->height = 64; c->build = 64;
    c->hair_type = 33; c->hair_color = 1;
    c->eye_type = 2; c->eye_color = 8; c->eye_scale = 4; c->eye_aspect = 3; c->eye_rotate = 4; c->eye_x = 2; c->eye_y = 12;
    c->eyebrow_type = 6; c->eyebrow_color = 1; c->eyebrow_scale = 4; c->eyebrow_aspect = 3; c->eyebrow_rotate = 6; c->eyebrow_x = 2; c->eyebrow_y = 10;
    c->nose_type = 1; c->nose_scale = 4; c->nose_y = 9;
    c->mouth_type = 23; c->mouth_color = 19; c->mouth_scale = 4; c->mouth_aspect = 3; c->mouth_y = 13;
    c->beard_color = 8; c->mustache_scale = 4; c->mustache_y = 10;
    c->glass_color = 8; c->glass_scale = 4; c->glass_y = 10;
    c->mole_scale = 4; c->mole_x = 2; c->mole_y = 20;
}

// The console's Mii database, from the player's own console: <DM_SYSTEM_DIR>/save/8000000000000030/MiiDatabase.dat, the mii service's system save as the Switch and the Switch 2 keep it
// (Switch 2 program 0400000000000039 mounts that save as "mii" and reads mii:/MiiDatabase.dat). The file is 0x1a98 bytes: "NFDB", 100 StoreData of 0x44 bytes, the version (1) at +0x1a94,
// the count at +0x1a95 and a CRC-16/CCITT (polynomial 0x1021, initial 0) of the bytes before it, big-endian, at +0x1a96. A StoreData is the CoreData (seven 32-bit words of bit fields and a
// 10-character nickname), the create ID and two CRCs; mii_from_store unpacks it into the CharInfo the SDK's Get returns, as the mii service does (its stored eyebrow y is 3 below the
// CharInfo's). Read once, never written: Miis a game adds go to the host's own file (mii_host_save)
enum { MII_DB_SIZE = 0x1a98, MII_DB_MAX = 100, MII_STORE = 0x44 };
static uint16_t mii_crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= (uint16_t)(p[i] << 8); for (int b = 0; b < 8; b++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1); }
    return c;
}
static unsigned mii_bits(const uint8_t *w, int word, int at, int len) { uint32_t x; memcpy(&x, w + 4 * word, 4); return (x >> at) & ((1u << len) - 1); }
static void mii_from_store(mii_charinfo *c, const uint8_t *sd) {
    memset(c, 0, sizeof *c);
    memcpy(c->create_id, sd + 0x30, 16); memcpy(c->nickname, sd + 0x1c, 20);
    c->hair_type = mii_bits(sd, 0, 0, 8); c->height = mii_bits(sd, 0, 8, 7); c->mole_type = mii_bits(sd, 0, 15, 1); c->build = mii_bits(sd, 0, 16, 7);
    c->hair_flip = mii_bits(sd, 0, 23, 1); c->hair_color = mii_bits(sd, 0, 24, 7); c->type = mii_bits(sd, 0, 31, 1);
    c->eye_color = mii_bits(sd, 1, 0, 7); c->gender = mii_bits(sd, 1, 7, 1); c->eyebrow_color = mii_bits(sd, 1, 8, 7); c->mouth_color = mii_bits(sd, 1, 16, 7); c->beard_color = mii_bits(sd, 1, 24, 7);
    c->glass_color = mii_bits(sd, 2, 0, 7); c->eye_type = mii_bits(sd, 2, 8, 6); c->region_move = mii_bits(sd, 2, 14, 2); c->mouth_type = mii_bits(sd, 2, 16, 6);
    c->font_region = mii_bits(sd, 2, 22, 2); c->eye_y = mii_bits(sd, 2, 24, 5); c->glass_scale = mii_bits(sd, 2, 29, 3);
    c->eyebrow_type = mii_bits(sd, 3, 0, 5); c->mustache_type = mii_bits(sd, 3, 5, 3); c->nose_type = mii_bits(sd, 3, 8, 5); c->beard_type = mii_bits(sd, 3, 13, 3);
    c->nose_y = mii_bits(sd, 3, 16, 5); c->mouth_aspect = mii_bits(sd, 3, 21, 3); c->mouth_y = mii_bits(sd, 3, 24, 5); c->eyebrow_aspect = mii_bits(sd, 3, 29, 3);
    c->mustache_y = mii_bits(sd, 4, 0, 5); c->eye_rotate = mii_bits(sd, 4, 5, 3); c->glass_y = mii_bits(sd, 4, 8, 5); c->eye_aspect = mii_bits(sd, 4, 13, 3);
    c->mole_x = mii_bits(sd, 4, 16, 5); c->eye_scale = mii_bits(sd, 4, 21, 3); c->mole_y = mii_bits(sd, 4, 24, 5);
    c->glass_type = mii_bits(sd, 5, 0, 5); c->favorite_color = mii_bits(sd, 5, 8, 4); c->faceline_type = mii_bits(sd, 5, 12, 4); c->faceline_color = mii_bits(sd, 5, 16, 4);
    c->faceline_wrinkle = mii_bits(sd, 5, 20, 4); c->faceline_make = mii_bits(sd, 5, 24, 4); c->eye_x = mii_bits(sd, 5, 28, 4);
    c->eyebrow_scale = mii_bits(sd, 6, 0, 4); c->eyebrow_rotate = mii_bits(sd, 6, 4, 4); c->eyebrow_x = mii_bits(sd, 6, 8, 4); c->eyebrow_y = (uint8_t)(mii_bits(sd, 6, 12, 4) + 3);
    c->nose_scale = mii_bits(sd, 6, 16, 4); c->mouth_scale = mii_bits(sd, 6, 20, 4); c->mustache_scale = mii_bits(sd, 6, 24, 4); c->mole_scale = mii_bits(sd, 6, 28, 4);
}
// The database the service answers from: the console's Miis (g_mii_own of them), then the Miis made in the host's stand-in for the editor (AppendMii), up to MII_DB_MAX in all
static mii_charinfo g_mii_db[MII_DB_MAX];
static int g_mii_n, g_mii_own, g_mii_changed;   // g_mii_changed: a Mii was added since the game last asked IsUpdated about the database
static pthread_mutex_t g_mii_mu = PTHREAD_MUTEX_INITIALIZER;
// <0: the file is not a database (logged); else the Miis read
static int mii_db_parse(const uint8_t *b, size_t size, mii_charinfo *out) {
    if (size != MII_DB_SIZE || memcmp(b, "NFDB", 4)) return -1;
    if (b[0x1a94] != 1 || b[0x1a95] > MII_DB_MAX) return -2;
    if (mii_crc16(b, 0x1a96) != (uint16_t)(b[0x1a96] << 8 | b[0x1a97])) return -3;
    for (int i = 0; i < b[0x1a95]; i++) mii_from_store(&out[i], b + 4 + (size_t)i * MII_STORE);
    return b[0x1a95];
}
// The Miis made in the host's stand-in for the editor (AppendMii), kept for every game in <system folder>/host/MiiDatabase.dat so that a Mii made once is there at the next start, as on a
// console; the player's console file is never written. The host's own format: "DMMI", the version (1), the count, 2 bytes of zeros, then the CharInfos
enum { MII_HOST_HEAD = 8 };
const char *dm_system_dir(char *buf, size_t n);   // hle_fs.c: $DM_SYSTEM_DIR, default $DM_ROOT/run/platform/system
static int mii_host_path(char *p, size_t n) { char t[1024]; return snprintf(p, n, "%s/host/MiiDatabase.dat", dm_system_dir(t, sizeof t)) < (int)n; }
// <0: not the host's database; else the Miis read into out (room for max)
static int mii_host_parse(const uint8_t *b, size_t size, mii_charinfo *out, int max) {
    if (size < MII_HOST_HEAD || memcmp(b, "DMMI", 4) || b[4] != 1) return -1;
    int n = b[5]; if (n > max || size != MII_HOST_HEAD + (size_t)n * sizeof(mii_charinfo)) return -1;
    memcpy(out, b + MII_HOST_HEAD, (size_t)n * sizeof(mii_charinfo));
    return n;
}
static int mii_host_save(void) {   // under g_mii_mu: the host's Miis, written to a new file that then replaces the old one
    char p[1024], t[1100]; if (!mii_host_path(p, sizeof p)) return 0;
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    int n = g_mii_n - g_mii_own; uint8_t head[MII_HOST_HEAD] = {'D', 'M', 'M', 'I', 1, (uint8_t)n};
    snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "wb");
    int ok = f && fwrite(head, 1, sizeof head, f) == sizeof head && fwrite(g_mii_db + g_mii_own, sizeof(mii_charinfo), (size_t)n, f) == (size_t)n;
    if (f && fclose(f)) ok = 0;
    if (ok) ok = !rename(t, p);
    if (!ok) { unlink(t); LOG("[RED] mii: cannot write %s: the Mii is kept for this run only", p); }
    return ok;
}
static void mii_db_load(void) {
    char t[1024], p[1100]; const char *sys = dm_system_dir(t, sizeof t); g_mii_n = g_mii_own = 0;
    snprintf(p, sizeof p, "%s/save/8000000000000030/MiiDatabase.dat", sys);
    FILE *f = fopen(p, "rb");
    if (f) {
        uint8_t *b = malloc(MII_DB_SIZE + 1); size_t n = b ? fread(b, 1, MII_DB_SIZE + 1, f) : 0; fclose(f);
        int r = b ? mii_db_parse(b, n, g_mii_db) : -1; free(b);
        static const char *why[] = {"", "not a Mii database (size or NFDB)", "an unknown version or count", "its CRC does not match"};
        if (r < 0) LOG("[RED] mii: %s is %s: the database stays empty", p, why[-r]);
        else { g_mii_n = g_mii_own = r; LOG("mii: %d Miis of the console's database from %s", r, p); }
    }
    if (!mii_host_path(p, sizeof p) || !(f = fopen(p, "rb"))) return;
    size_t cap = MII_HOST_HEAD + MII_DB_MAX * sizeof(mii_charinfo) + 1;
    uint8_t *b = malloc(cap); size_t n = b ? fread(b, 1, cap, f) : 0; fclose(f);
    int r = b ? mii_host_parse(b, n, g_mii_db + g_mii_n, MII_DB_MAX - g_mii_n) : -1; free(b);
    if (r < 0) LOG("[RED] mii: %s is not the host's Mii database, or does not fit beside the console's: its Miis are not listed", p);
    else { g_mii_n += r; LOG("mii: %d Miis made in the host's editor, from %s", r, p); }
}
static void mii_db_once(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, mii_db_load); }

// The object keeps its service pointer at +0, which the constructor zeroes and the destructor releases through its vtable when set; the host never sets it
HLE_FORCE("_ZN2nn3mii8Database10InitializeEv", "mii:u service; the host's database: the six default Miis, the console's own when supplied, those made in AppendMii", Result, h_mii_Init, void *self) {
    (void)self; mii_db_once();
    LOG("mii: Database::Initialize (the six default Miis, %d of the console's own, %d made in the host's editor)", g_mii_own, g_mii_n - g_mii_own); return RES_OK;
}
HLE_FORCE("_ZN2nn3mii8Database8FinalizeEv", "mii:u service; nothing to release", void, h_mii_Fin, void *self) { (void)self; }
HLE_FORCE("_ZNK2nn3mii8Database11IsAvailableEv", "mii:u service; always available", int, h_mii_Avail, const void *self) { (void)self; return 1; }
// True once after a Mii was added (AppendMii), for a question that includes the database; the default Miis never change
HLE_FORCE("_ZN2nn3mii8Database9IsUpdatedEi", "mii:u service; true once after AppendMii added a Mii", int, h_mii_Updated, void *self, int src) {
    (void)self; if (!(src & 1)) return 0;
    pthread_mutex_lock(&g_mii_mu); int r = g_mii_changed; g_mii_changed = 0; pthread_mutex_unlock(&g_mii_mu); return r;
}
HLE_FORCE("_ZNK2nn3mii8Database14IsFullDatabaseEv", "mii:u service; full at 100 Miis", int, h_mii_Full, const void *self) { (void)self; return g_mii_n >= MII_DB_MAX; }
// Source flags: 1 the console's database, 2 the six default Miis every console has; the database's come first, as the mii service lists them. Mario Kart 8 Deluxe asks for the defaults with room for 6 and never reads the count
// (main+0x81e954): its boot-time list of Mii icons (main+0x419dc8) builds a head for each of the six, and zeroed entries failed the model's CharInfo check
enum { MII_SRC_DATABASE = 1, MII_SRC_DEFAULT = 2, MII_DEFAULTS = 6 };
static int mii_get(void *buf, int n, int src, size_t stride) {
    int k = 0; pthread_mutex_lock(&g_mii_mu);
    for (int i = 0; (src & MII_SRC_DATABASE) && buf && k < n && i < g_mii_n; i++, k++) {
        uint8_t *e = (uint8_t *)buf + (size_t)k * stride;
        memcpy(e, &g_mii_db[i], sizeof(mii_charinfo));
        if (stride > sizeof(mii_charinfo)) { uint32_t source = 0; memcpy(e + sizeof(mii_charinfo), &source, 4); }   // CharInfoElement: the CharInfo and its source (0 = database)
    }
    for (int d = 0; (src & MII_SRC_DEFAULT) && buf && k < n && d < MII_DEFAULTS; d++, k++) {
        uint8_t *e = (uint8_t *)buf + (size_t)k * stride;
        mii_default((mii_charinfo *)e, d >= 3, d);
        if (stride > sizeof(mii_charinfo)) { uint32_t source = 1; memcpy(e + sizeof(mii_charinfo), &source, 4); }   // 1 = default
    }
    pthread_mutex_unlock(&g_mii_mu); return k;
}
HLE_FORCE("_ZNK2nn3mii8Database8GetCountEi", "mii:u service; the database's Miis and the six default ones", int, h_mii_Count, const void *self, int src) {
    (void)self; return (src & MII_SRC_DATABASE ? g_mii_n : 0) + (src & MII_SRC_DEFAULT ? MII_DEFAULTS : 0);
}
HLE_FORCE("_ZN2nn3mii8Database3GetEPiPNS0_15CharInfoElementEii", "mii:u service; the database's Miis and the six default ones", Result, h_mii_GetE, void *self, int *out, void *buf, int n, int src) {
    (void)self; *out = mii_get(buf, n, src, sizeof(mii_charinfo) + 4); return RES_OK;
}
HLE_FORCE("_ZN2nn3mii8Database3GetEPiPNS0_8CharInfoEii", "mii:u service; the database's Miis and the six default ones", Result, h_mii_Get, void *self, int *out, void *buf, int n, int src) {
    (void)self; *out = mii_get(buf, n, src, sizeof(mii_charinfo)); return RES_OK;
}
// A Mii is found by its create ID (CharInfo +0)
static int mii_find(const void *info) { int r = -1; pthread_mutex_lock(&g_mii_mu); for (int i = 0; info && r < 0 && i < g_mii_n; i++) if (!memcmp(g_mii_db[i].create_id, info, 16)) r = i; pthread_mutex_unlock(&g_mii_mu); return r; }
HLE_FORCE("_ZNK2nn3mii8Database8GetIndexEPiRKNS0_8CharInfoE", "mii:u service; the Mii's place in the console's database", Result, h_mii_Index, const void *self, int *out, const void *info) {
    (void)self; *out = mii_find(info); return *out < 0 ? MII_RES_NOT_FOUND : RES_OK;
}
HLE_FORCE("_ZNK2nn3mii8Database12UpdateLatestEPNS0_8CharInfoEi", "mii:u service; the database's copy of the Mii", Result, h_mii_Latest, const void *self, void *info, int src) {
    (void)self; int i = (src & MII_SRC_DATABASE) ? mii_find(info) : -1;
    if (i < 0) return MII_RES_NOT_FOUND;
    memcpy(info, &g_mii_db[i], sizeof(mii_charinfo)); return RES_OK;
}
// RandomDesc {age, gender, race}: gender 0 male, 1 female, 2 either. The host's "random" Mii is the default one of that gender
HLE_FORCE("_ZN2nn3mii8Database11BuildRandomEPNS0_8CharInfoERKNS0_10RandomDescE", "mii:u service; the default Mii", Result, h_mii_Random, void *self, mii_charinfo *out, const int *desc) {
    (void)self; mii_default(out, desc ? desc[1] : 0, -1); return RES_OK;
}
HLE_FORCE("_ZNK2nn3mii8Database12BuildDefaultEPNS0_8CharInfoEi", "mii:u service; default Mii <index>", Result, h_mii_Default, const void *self, mii_charinfo *out, int index) {
    (void)self; mii_default(out, index >= 3, index < 0 || index >= MII_DEFAULTS ? 0 : index); return RES_OK;
}
HLE_FORCE("_ZN2nn3mii8Database6FormatEv", "mii:u service; the database is already empty", Result, h_mii_Format, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn3mii8Database6ImportEPKvm", "mii:u service; importing needs the system's test mode", Result, h_mii_Import, void *self, const void *p, size_t n) {
    (void)self; (void)p; LOG("[RED] mii: Database::Import of %zu bytes refused (the host has no test mode)", n); return MII_RES_TEST_MODE;
}
HLE_FORCE("_ZNK2nn3mii8Database6ExportEPvm", "mii:u service; nothing to export", Result, h_mii_Export, const void *self, void *p, size_t n) {
    (void)self; (void)p; (void)n; LOG("[RED] mii: Database::Export refused"); return MII_RES_TEST_MODE;
}
// MiiModel system data (0100000000000802) stand-in, also the Switch 2's MiiResource (0400000000000849, below). The Mii rendering code an application links (Mario Kart 8 Deluxe 4.0.0: main+0x8f1400..+0x8fc000) reads one texture file
// (NXTexture{Low,Mid}{Linear,SRGB}.dat) and one shape file (Shape{Mid,High}.dat) whole and builds each part of a head from them; the firmware's files are not the host's to ship, so it writes
// files of the same layout in which every part is blank:
//   texture file: +0x0c u32 largest decompressed size of each of the 11 part kinds, +0x38 their alignments, then from +0x64 one 0x1c-byte entry per part (u32 offset, u32 size, u32 stored size,
//     4 zlib parameters, then the texture: u16 width / height at +0x14, format index, mip count and tile mode index at +0x18), hats 132, eyes 62, eyebrows 24, beards 2, wrinkles 12, make-up 12,
//     glasses 20, moles 2, mouths 37, mustaches 6, noselines 18 (main+0x8fb0dc);
//   shape file: +0x0c the 12 largest sizes, +0x3c their alignments, from +0x6c one 0x58-byte entry per part (offset, size, stored size, zlib parameters, five vertex attribute offsets
//     and sizes, index offset and size, bounding box), beards 4, facelines 12, masks 12, hats / foreheads / hair in normal and cap form 132 each, glasses 1, noses 18, noselines 18
//     (main+0x8faee8); then the hair transforms (132 × 0x48, +0x12704) and faceline transforms (12 × 0x24, +0x14c24).
// Parts cannot be absent: the firmware's files have every one, and the Mii code relies on it. A texture whose size is 0 is skipped when the model is built (main+0x8faaf4; Nintendo
// Switch Sports Resort main+0x1cdead0), but the model's texture getter answers for a part by the create flags alone (Switch Sports Resort main+0x1cd5490: the hat texture whenever
// the normal or hat flag was given), so a game that asks for a skipped part's texture gets an empty holder: with every part of size 0, Switch Sports Resort's two-player skate
// registered the hat texture of a Mii model leaving its practice zone (main+0x3fbc9ac) and called nvnTextureGetFlags with a null texture (main+0x49c54). Every texture entry
// points at one shared blank texture instead: 8x4 texels of format index 0 (nn::gfx R8_Unorm, main+0x42d2688), one level, optimal tiling, the same 0x20 zero bytes as the mesh below,
// which the reader inflates into the texture's storage (512 bytes on NVN, one GOB). Shapes cannot be absent either: the faceline is drawn without a check (its position buffer,
// main+0x8286c4: a null buffer read at main+0xa690d0), so every shape entry points at one shared mesh, one vertex at the origin (attribute 0, the position: 4 halves) and the triangle
// (0, 0, 0) (u16 indices at +0x10), 0x20 bytes of zeros inflated with nn::util::DecompressZlib, stored here as one raw deflate block. It builds, and draws nothing. The largest sizes
// size the model's memory pool (main+0x8f2e6c) and the first alignment is that pool's alignment, so they are one page each instead of 0. The first 12 bytes of each header are not read
enum { MII_TEX_PARTS = 327, MII_TEX_SIZE = 0x64 + 0x1c * MII_TEX_PARTS, MII_SHAPE_TABLES = 0x6c + 0x58 * 857 + 0x48 * 132 + 0x24 * 12, MII_MESH_AT = 0x14de0, MII_MESH = 0x20,
       MII_TEXELS_AT = 0x2430, MII_TEX_W = 8, MII_TEX_H = 4 };
_Static_assert(MII_TEX_SIZE == 0x2428 && MII_SHAPE_TABLES == 0x14dd4 && MII_TEXELS_AT >= MII_TEX_SIZE && MII_TEX_W * MII_TEX_H == MII_MESH, "MiiModel stand-in layout");
static const uint8_t mii_mesh_z[] = {0x78, 0x01, 0x01, MII_MESH, 0, (uint8_t)~MII_MESH, 0xff, [7 ... 7 + MII_MESH - 1] = 0, 0x00, MII_MESH, 0x00, 0x01};   // zlib: stored block, Adler-32 of 32 zeros
enum { MII_SHAPE_SIZE = MII_MESH_AT + sizeof mii_mesh_z, MII_TEXFILE_SIZE = MII_TEXELS_AT + sizeof mii_mesh_z };
static void mii_textures(uint8_t *b) {   // every part the one blank texture (see above): its 0x20 zero bytes are the mesh's
    for (int i = 0; i < MII_TEX_PARTS; i++) {
        uint8_t *e = b + 0x64 + 0x1c * i; uint32_t at = MII_TEXELS_AT, size = MII_MESH, stored = sizeof mii_mesh_z; uint16_t w = MII_TEX_W, h = MII_TEX_H;
        memcpy(e, &at, 4); memcpy(e + 4, &size, 4); memcpy(e + 8, &stored, 4);   // data offset, inflated size, stored size
        memcpy(e + 0x14, &w, 2); memcpy(e + 0x16, &h, 2); e[0x18] = 0; e[0x19] = 1; e[0x1a] = 0;   // R8_Unorm, one level, optimal tiling
    }
    memcpy(b + MII_TEXELS_AT, mii_mesh_z, sizeof mii_mesh_z);
}
static void mii_shapes(uint8_t *b) {
    for (int i = 0; i < 857; i++) {
        uint32_t *e = (uint32_t *)(b + 0x6c + 0x58 * i);
        e[0] = MII_MESH_AT; e[1] = MII_MESH; e[2] = sizeof mii_mesh_z;   // data offset, inflated size, stored size
        e[4 + 5] = 8;                                                    // attribute 0 (offset 0) is one 8-byte vertex; attributes 1-4 absent
        e[14] = 0x10; e[15] = 6;                                         // index offset and size: three u16 indices
    }
    memcpy(b + MII_MESH_AT, mii_mesh_z, sizeof mii_mesh_z);
}
static int mii_write(const char *dir, const char *name, int parts, size_t size) {
    char p[1280]; snprintf(p, sizeof p, "%s/%s", dir, name);
    uint8_t *b = calloc(1, size); FILE *f = fopen(p, "wb");
    for (int i = 0; b && i < parts; i++) { uint32_t page = 0x1000; memcpy(b + 0xc + 4 * i, &page, 4); memcpy(b + 0xc + 4 * parts + 4 * i, &page, 4); }
    if (b && parts == 12) mii_shapes(b);
    if (b && parts == 11) mii_textures(b);
    int ok = b && f && fwrite(b, 1, size, f) == size;
    if (f && fclose(f)) ok = 0;
    free(b);
    if (!ok) LOG("[RED] mii: cannot write the Mii model stand-in %s", p);
    return ok;
}
// One stand-in serves both archives, the Switch's MiiModel (0100000000000802) and the Switch 2's (0400000000000849), whose files have the same layout (Nintendo Switch Sports Resort's
// accessors, main+0x1cdcd80..+0x1cdd0c4: 11 texture part kinds from +0x64 in 0x1c-byte entries, 12 shape kinds from +0x6c in 0x58-byte entries, hair transforms at +0x12704, faceline
// at +0x14c24); the Switch 2's adds its own texture sets, OunceTexture{Low,Mid,High}{Linear,SRGB}.dat, and a game reads only the files it names. Written once per process (the query
// and the mount both ask, for either archive), so a host with another layout never leaves old files behind; again only if the directory changes (a test's DM_TEMP)
int dm_mii_standin(const char *dir) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static char done_dir[1024]; static int done;
    pthread_mutex_lock(&mu);
    if (done && !strcmp(done_dir, dir)) { pthread_mutex_unlock(&mu); return done > 0; }
    char p[1024]; strlcpy(p, dir, sizeof p);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
    static const char *tex[] = {"NXTextureLowLinear.dat", "NXTextureLowSRGB.dat", "NXTextureMidLinear.dat", "NXTextureMidSRGB.dat",
                                "OunceTextureLowLinear.dat", "OunceTextureLowSRGB.dat", "OunceTextureMidLinear.dat", "OunceTextureMidSRGB.dat", "OunceTextureHighLinear.dat", "OunceTextureHighSRGB.dat"},
                      *shape[] = {"ShapeMid.dat", "ShapeHigh.dat"};
    int ok = 1;
    for (size_t i = 0; i < sizeof tex / sizeof *tex; i++) ok &= mii_write(dir, tex[i], 11, MII_TEXFILE_SIZE);
    for (size_t i = 0; i < sizeof shape / sizeof *shape; i++) ok &= mii_write(dir, shape[i], 12, MII_SHAPE_SIZE);
    LOG("mii: Mii model system data is the host's stand-in (every texture part one blank 8x4 texture, every shape one degenerate triangle: Mii heads draw nothing) in %s", dir);
    strlcpy(done_dir, dir, sizeof done_dir); done = ok ? 1 : -1;
    pthread_mutex_unlock(&mu);
    return ok;
}

// The Mii editor (MiiEdit library applet). AppendMii opens it to make a new Mii: the host answers as a player who saves the editor's starting Mii, the system's default one (male,
// "no name") with a create ID of its own, added to the database and kept in the host's file (mii_host_save); *index is its place in the database. Mario Golf: Super Rush's Golf
// Adventure asks for one when the database holds none, and went back to its save slots at a cancel (issue #2361). A full database cancels, as the editor cannot add one then
HLE_FORCE("_ZN2nn3mii9AppendMiiEPi", "MiiEdit applet; the default Mii is made and added", Result, h_mii_Append, int *index) {
    DM_APPLET("Mii editor"); mii_db_once();
    pthread_mutex_lock(&g_mii_mu);
    if (g_mii_n >= MII_DB_MAX) { pthread_mutex_unlock(&g_mii_mu); LOG("mii: AppendMii: the database is full (cancelled)"); return MII_RES_CANCELED; }
    int i = g_mii_n++; mii_default(&g_mii_db[i], 0, -1); g_mii_changed = 1;
    int kept = mii_host_save();
    pthread_mutex_unlock(&g_mii_mu);
    if (index) *index = i;
    LOG("mii: AppendMii: the default Mii is added as database entry %d%s", i, kept ? "" : " (this run only)");
    return RES_OK;
}
HLE_FORCE("_ZN2nn3mii11ShowMiiEditEv", "MiiEdit applet; closed at once", Result, h_mii_Edit, void) { DM_APPLET("Mii editor"); LOG("mii: ShowMiiEdit (the host has no Mii editor)"); return RES_OK; }
