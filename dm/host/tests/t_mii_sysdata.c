// The Mii model system data stand-in (hle_mii.c dm_mii_standin, mounted by hle_fs.c MountSystemData): the Switch's MiiModel (0100000000000802) and the Switch 2's MiiResource
// (0400000000000849) both mount, with every texture and shape file the games name, in the layout their Mii code reads. Nintendo Switch Sports Resort mounts the Switch 2 archive as
// MiiResource: and, refused, crashed later reading a texture file's part sizes through a null pointer. Expectations are written from the games' readers, not from this code
#include "../hle_fs.c"
#include "../hle_mii.c"
#include "bcat_absent.h"   // hle_fs.c weak-imports the BCAT provider (nextendo_bcat.h), which this test does not link
#include <sys/stat.h>
#include <zlib.h>
struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
uint64_t dm_entropy64(void) { return 0x1234; }
module_t *g_mod; int g_nmod;
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
void *guest_sym(const char *name) { (void)name; return NULL; }
int dm_game_imports(const char *prefix) { (void)prefix; return 0; }
void dm_thread_service_request(void) {}
void dm_applet_enter(const char *name) { (void)name; }   // the Mii editor (hle_mii.c), not used here
void dm_applet_leave(void) {}
static int checks, fails;
#define CHECK(c, text) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", text); } while (0)
// Reads a whole file of a mount through the host's file calls, as the game's loader does
static uint8_t *read_all(const char *path, int64_t *size) {
    uint64_t h; if (h_fs_OpenFile(&h, path, MODE_READ)) return NULL;
    int64_t n = 0; if (h_fs_GetFileSize(&n, h) || n <= 0) { h_fs_CloseFile(h); return NULL; }
    uint8_t *b = malloc((size_t)n); size_t got = 0;
    Result r = h_fs_ReadFile(&got, h, 0, b, (size_t)n); h_fs_CloseFile(h);
    if (r || got != (size_t)n) { free(b); return NULL; }
    *size = n; return b;
}
static uint32_t u32(const uint8_t *b, size_t at) { uint32_t v; memcpy(&v, b + at, 4); return v; }
// A texture file: 11 part kinds with sizes and alignments that are not 0 (they size the model's memory pool), and every part a texture, as in the firmware's files: the Mii code
// skips a part of size 0 (Switch Sports Resort main+0x1cdead0: entry +4 != 0) but answers for it by the model's create flags (CharModel's texture getter, main+0x1cd5490), so a
// game that asks a skipped part's texture gets an empty holder and calls nvnTextureGetFlags with a null texture (main+0x49c54). An entry: data offset, inflated size, stored size;
// the texture from +0x10: u16 width and height at +0x14, the format index (0-6, main+0x42d2688) at +0x18, the mip count at +0x19, the tile mode index (0-1) at +0x1a (main+0x1cdcf00).
// Each part's data inflates (nn::util::DecompressZlib, main+0x1cdcbe8) to its size, which is no more than its texture's bytes: the reader inflates into the texture's storage
// without a bound of its own. The entries end at 0x2428 (noselines, the last kind, at +0x2230, 18 of 0x1c bytes)
static int texture_ok(const char *path) {
    int64_t n = 0; uint8_t *b = read_all(path, &n); if (!b) return 0;
    int ok = n >= 0x2428;
    for (int i = 0; ok && i < 11; i++) ok = u32(b, 0xc + 4 * i) && u32(b, 0x38 + 4 * i);
    for (int i = 0; ok && i < 327; i++) {
        const uint8_t *e = b + 0x64 + 0x1c * i; uint32_t off = u32(e, 0), size = u32(e, 4), stored = u32(e, 8); uint16_t w, h; memcpy(&w, e + 0x14, 2); memcpy(&h, e + 0x16, 2);
        ok = size && stored && (uint64_t)off + stored <= (uint64_t)n && w && h && e[0x18] == 0 && e[0x19] == 1 && e[0x1a] <= 1 && size <= (uint32_t)w * h;   // format 0: R8, a byte a texel
        uint8_t out[4096]; uLongf got = sizeof out;
        ok = ok && size <= sizeof out && uncompress(out, &got, b + off, stored) == Z_OK && got == size;
    }
    free(b); return ok;
}
// A shape file: 12 kinds from +0x6c in 0x58-byte entries (857 of them), each pointing at one mesh inside the file, then the hair and faceline transforms (+0x12704, +0x14c24)
static int shape_ok(const char *path) {
    int64_t n = 0; uint8_t *b = read_all(path, &n); if (!b) return 0;
    int ok = n > 0x14c24 + 0x24 * 12;
    for (int i = 0; ok && i < 12; i++) ok = u32(b, 0xc + 4 * i) && u32(b, 0x3c + 4 * i);
    for (int i = 0; ok && i < 857; i++) { size_t e = 0x6c + 0x58 * (size_t)i; ok = u32(b, e) + u32(b, e + 8) <= (uint64_t)n && u32(b, e + 4) > 0; }
    free(b); return ok;
}
int main(void) {
    char root[1024], p[1200];
    snprintf(root, sizeof root, "%s/mii.XXXXXX", getenv("TMPDIR") ?: "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    snprintf(p, sizeof p, "%s/temp", root); setenv("DM_TEMP", p, 1);
    size_t cache = 0;
    CHECK(h_fs_QuerySysDataCache(&cache, 0x0400000000000849ull) == RES_OK && cache > 0, "the Switch 2's MiiResource has a cache size");
    CHECK(h_fs_MountSysData("MiiResource", 0x0400000000000849ull, NULL, cache) == RES_OK, "and mounts (it was \"target not found\")");
    static const char *tex[] = {"NXTextureLowLinear", "NXTextureLowSRGB", "NXTextureMidLinear", "NXTextureMidSRGB",
                                "OunceTextureLowLinear", "OunceTextureLowSRGB", "OunceTextureMidLinear", "OunceTextureMidSRGB", "OunceTextureHighLinear", "OunceTextureHighSRGB"};
    int all = 1; for (size_t i = 0; i < sizeof tex / sizeof *tex; i++) { snprintf(p, sizeof p, "MiiResource:/%s.dat", tex[i]); all &= texture_ok(p); }
    CHECK(all, "every texture file Switch Sports Resort names (NX and Ounce sets) reads in the texture layout, every part a texture that inflates");
    CHECK(shape_ok("MiiResource:/ShapeMid.dat") && shape_ok("MiiResource:/ShapeHigh.dat"), "both shape files read in the shape layout, every part one mesh in the file");
    g_nmnt = 0;
    CHECK(h_fs_QuerySysDataCache(&cache, 0x0100000000000802ull) == RES_OK && h_fs_MountSysData("MiiResource", 0x0100000000000802ull, NULL, cache) == RES_OK,
          "the Switch's MiiModel still mounts");
    CHECK(texture_ok("MiiResource:/NXTextureMidSRGB.dat") && shape_ok("MiiResource:/ShapeHigh.dat"), "with its files");
    snprintf(p, sizeof p, "%s/temp_sysdata/mii", root); struct stat a;
    char q[1200]; snprintf(q, sizeof q, "%s/temp_sysdata/0100000000000802", root); struct stat b2;
    CHECK(!stat(p, &a) && S_ISDIR(a.st_mode) && stat(q, &b2), "both archives are the one stand-in directory");
    snprintf(q, sizeof q, "%s/ShapeMid.dat", p); struct stat s1, s2; stat(q, &s1);
    CHECK(dm_mii_standin(p) == 1 && !stat(q, &s2) && s1.st_ino == s2.st_ino && s1.st_mtime == s2.st_mtime, "asked again, the stand-in is not written again");
    CHECK(h_fs_MountSysData("Other", 0x0100000000000803ull, NULL, 0) == RES_TARGET_NOT_FOUND, "another system data archive: target not found");
    rm_tree(root);
    printf("== mii system data: %d checks, %d failed ==\n", checks, fails); return fails != 0;
}
