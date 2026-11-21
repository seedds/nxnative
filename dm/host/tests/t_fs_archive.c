// Archive members of mod folders (hle_fs.c vf_open, arh1.h): a synthetic ARH1 index, encrypted as the games' are, with four members (/ab, /cd, /d/ef and /d/g, whose path the trie spells wholly), its archive, and a
// mount whose two overlay folders hold a replacement for /ab (in both, the first winning), one for /d/ef with its name in upper case, a file the archive does not have, an empty
// file (left out), and a loose file the RomFS has. The index read through the host points the replaced members past the archive's end, stored as is; the archive reads its own bytes, zeros up to the
// next 16-byte boundary and then the mod files; /cd is untouched. A mount without overlay folders, and one whose overlay holds no member, open both files unchanged; a mount
// with no overlay of its own takes the game's other mod folders (DM_ROMFS_OVERLAY), as add-on content does.
// Expectations are written by hand.
#include "../hle_fs.c"

struct dm_flags g_flag;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void put(const char *dir, const char *name, const void *d, size_t n) {
    char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f);
}
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// The index: root node 0 (next 0), '/' → node 47 (next 128), 'a' → 225 ("b", file 0), 'c' → 227 ("d", file 1), 'd' → 228 (next 256), '/' → 303 (next 384), 'e' → 485
// ("f", file 2), 'g' → 487 (no characters left: the trie spells the whole path, file 3)
static size_t make_arh(uint8_t *d, uint32_t key) {
    enum { ND = 512, NS = 24 };
    int32_t dict[ND * 2]; for (int i = 0; i < ND; i++) { dict[2 * i] = -1; dict[2 * i + 1] = -1; }   // free nodes
    uint8_t strings[NS + 1] = { 0 };   // 25 bytes: 6 whole words encrypted, the last byte a plain partial word
    strings[1] = 'b'; put32(strings + 3, 0);     // "b" + ID 0 at 1
    strings[7] = 'd'; put32(strings + 9, 1);     // "d" + ID 1 at 7
    strings[13] = 'f'; put32(strings + 15, 2);   // "f" + ID 2 at 13
    put32(strings + 20, 3);                      // "" + ID 3 at 19
    dict[0] = 0; dict[1] = -1;
    dict[2 * 47] = 128; dict[2 * 47 + 1] = 0;
    dict[2 * (128 ^ 'a')] = -1; dict[2 * (128 ^ 'a') + 1] = 47;
    dict[2 * (128 ^ 'c')] = -7; dict[2 * (128 ^ 'c') + 1] = 47;
    dict[2 * (128 ^ 'd')] = 256; dict[2 * (128 ^ 'd') + 1] = 47;
    dict[2 * (256 ^ '/')] = 384; dict[2 * (256 ^ '/') + 1] = 128 ^ 'd';
    dict[2 * (384 ^ 'e')] = -13; dict[2 * (384 ^ 'e') + 1] = 256 ^ '/';
    dict[2 * (384 ^ 'g')] = -19; dict[2 * (384 ^ 'g') + 1] = 256 ^ '/';
    size_t so = ARH1_HEADER, sn = sizeof strings, no = (so + sn + 3) & ~3u, nn = sizeof dict, fo = no + nn;
    memset(d, 0, fo + 4 * ARH1_ENTRY);
    memcpy(d, "arh1", 4); put32(d + 4, (uint32_t)sn); put32(d + 8, ND); put32(d + 12, (uint32_t)so); put32(d + 16, (uint32_t)sn);
    put32(d + 20, (uint32_t)no); put32(d + 24, (uint32_t)nn); put32(d + 28, (uint32_t)fo); put32(d + 32, 4); put32(d + 36, key);
    uint32_t k = key ^ 0xF3F35353u;
    memcpy(d + so, strings, sn); for (size_t i = 0; i + 4 <= sn; i += 4) put32(d + so + i, arh1_u32(d + so + i) ^ k);
    memcpy(d + no, dict, nn); for (size_t i = 0; i < nn; i += 4) put32(d + no + i, arh1_u32(d + no + i) ^ k);
    // members: /ab at 0 (4 bytes, XBC1-wrapped: flags 2, expanded 9), /cd at 16 (4 bytes, as is), /d/ef at 20 (3 bytes, hidden flag kept), /d/g at 16 (2 bytes)
    uint8_t *e = d + fo;
    put64(e, 0); put32(e + 8, 4); put32(e + 12, 9); put32(e + 16, 2); put32(e + 20, 0);
    put64(e + 24, 16); put32(e + 32, 4); put32(e + 36, 0); put32(e + 40, 0); put32(e + 44, 1);
    put64(e + 48, 20); put32(e + 56, 3); put32(e + 60, 0); put32(e + 64, 1); put32(e + 68, 2);
    put64(e + 72, 16); put32(e + 80, 2); put32(e + 84, 0); put32(e + 88, 0); put32(e + 92, 3);
    return fo + 4 * ARH1_ENTRY;
}

static size_t read_all(const char *path, uint8_t *buf, size_t cap, int64_t *size) {
    uint64_t h = 0; size_t got = 0;
    if (h_fs_OpenFile(&h, path, MODE_READ) != RES_OK) return (size_t)-1;
    if (h_fs_GetFileSize(size, h) != RES_OK || h_fs_ReadFile(&got, h, 0, buf, cap) != RES_OK) got = (size_t)-1;
    h_fs_CloseFile(h); return got;
}

int main(void) {
    char root[] = "/tmp/t_fs_archive.XXXXXX"; if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char base[1100], ov1[1100], ov2[1100], ov3[1100], sub[1200];
    snprintf(base, sizeof base, "%s/base", root); snprintf(ov1, sizeof ov1, "%s/ov1", root); snprintf(ov2, sizeof ov2, "%s/ov2", root); snprintf(ov3, sizeof ov3, "%s/ov3", root);
    mkdir(base, 0755); mkdir(ov1, 0755); mkdir(ov2, 0755); mkdir(ov3, 0755);
    snprintf(sub, sizeof sub, "%s/D", ov1); mkdir(sub, 0755);
    static uint8_t arh[16384]; size_t an = make_arh(arh, 0x12345678);
    const uint8_t ard[23] = { 'x','b','c','1', 0,0,0,0,0,0,0,0,0,0,0,0, 'C','D','C','D', 'E','F','G' };
    put(base, "bf.arh", arh, an); put(base, "bf.ard", ard, sizeof ard); put(base, "loose", "base", 4);
    put(ov1, "ab", "MOD-AB-1", 8); put(sub, "EF", "mod-ef", 6); put(ov1, "nothere", "?", 1); put(ov1, "loose", "over", 4);
    put(ov2, "ab", "MOD-AB-2!", 9);
    put(ov3, "zz", "?", 1);
    put(ov2, "cd", "", 0);   // an empty file (an entry an archive tool could not unpack): left out, /cd stays the game's

    {   // the parser alone
        arh1_t a; CHECK(arh1_open(arh, an, &a) == 0, "ARH1 opens");
        CHECK(arh1_find(&a, "/ab") == 0 && arh1_find(&a, "/cd") == 1 && arh1_find(&a, "/d/ef") == 2 && arh1_find(&a, "/d/g") == 3, "members found: %lld %lld %lld %lld", (long long)arh1_find(&a, "/ab"), (long long)arh1_find(&a, "/cd"), (long long)arh1_find(&a, "/d/ef"), (long long)arh1_find(&a, "/d/g"));
        CHECK(arh1_find(&a, "/a") < 0 && arh1_find(&a, "/abc") < 0 && arh1_find(&a, "/xb") < 0 && arh1_find(&a, "/d") < 0 && arh1_find(&a, "/d/e") < 0 && arh1_find(&a, "/d/") < 0 && arh1_find(&a, "/d/gg") < 0 && arh1_find(&a, "") < 0, "other paths are not members");
        arh1_free(&a);
        uint8_t bad[64] = "arh2"; CHECK(arh1_open(bad, sizeof bad, &a) == -1, "not ARH1");
        memcpy(bad, arh, 48); put32(bad + 28, 60); CHECK(arh1_open(bad, sizeof bad, &a) == -1, "a file table past the end is refused");
    }

    CHECK(mount("plain", base, 1) == RES_OK && mount("mod", base, 1) == RES_OK && mount("none", base, 1) == RES_OK, "mount the base three times");
    snprintf(g_mnt[g_nmnt - 2].overlay, sizeof g_mnt[0].overlay, "%s:%s", ov1, ov2);
    strlcpy(g_mnt[g_nmnt - 1].overlay, ov3, sizeof g_mnt[0].overlay);

    static uint8_t buf[32768]; int64_t size = 0;
    CHECK(read_all("plain:/bf.arh", buf, sizeof buf, &size) == an && !memcmp(buf, arh, an), "without mod folders the index is the game's");
    CHECK(read_all("none:/bf.arh", buf, sizeof buf, &size) == an && !memcmp(buf, arh, an), "mod folders with no member leave the index as it was");
    CHECK(read_all("none:/bf.ard", buf, sizeof buf, &size) == sizeof ard && size == sizeof ard, "and the archive");

    CHECK(read_all("mod:/bf.arh", buf, sizeof buf, &size) == an && size == (int64_t)an, "the index with mod files has the game's size");
    arh1_t a; CHECK(arh1_open(buf, an, &a) == 0, "and is still ARH1");
    uint8_t *e0 = arh1_entry(&a, buf, 0), *e1 = arh1_entry(&a, buf, 1), *e2 = arh1_entry(&a, buf, 2); uint64_t o0, o2; memcpy(&o0, e0, 8); memcpy(&o2, e2, 8);
    CHECK(o0 == 32 && arh1_u32(e0 + 8) == 8 && arh1_u32(e0 + 12) == 0 && arh1_u32(e0 + 16) == 0, "/ab: the first folder's 8 bytes at 32, stored as is (offset %llu size %u)", (unsigned long long)o0, arh1_u32(e0 + 8));
    CHECK(o2 == 48 && arh1_u32(e2 + 8) == 6 && arh1_u32(e2 + 16) == 1, "/d/ef from D/EF at 48, hidden flag kept (offset %llu)", (unsigned long long)o2);
    CHECK(!memcmp(e1, arh + a.table + ARH1_ENTRY, ARH1_ENTRY), "/cd unchanged");
    arh1_free(&a);

    CHECK(read_all("mod:/bf.ard", buf, sizeof buf, &size) == 54 && size == 54, "the archive grows to 54 bytes (%lld)", (long long)size);
    static const uint8_t pad[9] = { 0 };
    CHECK(!memcmp(buf, ard, sizeof ard) && !memcmp(buf + 23, pad, 9) && !memcmp(buf + 32, "MOD-AB-1", 8) && !memcmp(buf + 40, pad, 8) && !memcmp(buf + 48, "mod-ef", 6), "game bytes, zeros, then the mod files");
    {   // a read across the archive's end and into the first mod file
        uint64_t h = 0; size_t got = 0;
        CHECK(h_fs_OpenFile(&h, "mod:/bf.ard", MODE_READ) == RES_OK && h_fs_ReadFile(&got, h, 20, buf, 16) == RES_OK && got == 16 && !memcmp(buf, "EFG", 3) && !memcmp(buf + 12, "MOD-", 4), "a read across segments");
        CHECK(h_fs_ReadFile(&got, h, 50, buf, 100) == RES_OK && got == 4 && !memcmp(buf, "d-ef", 4), "a read past the end stops there");
        CHECK(h_fs_GetFileOpenMode(h) == MODE_READ, "read-only");
        h_fs_CloseFile(h);
        CHECK(h_fs_OpenFile(&h, "mod:/bf.ard", MODE_WRITE) != RES_OK, "a write open is refused");
    }
    CHECK(read_all("mod:/loose", buf, sizeof buf, &size) == 4 && !memcmp(buf, "over", 4), "a loose RomFS file still comes from the overlay");
    {   // another mount's archive (add-on content) takes the game's other mod folders too, after its own (none here)
        CHECK(mount("dlc", base, 1) == RES_OK, "mount the base as add-on content");
        setenv("DM_ROMFS_OVERLAY", ov2, 1);
        CHECK(read_all("dlc:/bf.ard", buf, sizeof buf, &size) == 41 && !memcmp(buf + 32, "MOD-AB-2!", 9), "the RomFS's mod folder replaces a member of the add-on's archive (%lld bytes)", (long long)size);
        unsetenv("DM_ROMFS_OVERLAY");
    }

    char rm[1200]; snprintf(rm, sizeof rm, "rm -rf '%s'", root); if (system(rm)) fprintf(stderr, "t_fs_archive: %s left behind\n", root);
    if (fails) { fprintf(stderr, "t_fs_archive: %d failed\n", fails); return 1; }
    printf("t_fs_archive: ok\n");
    return 0;
}
