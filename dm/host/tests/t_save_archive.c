// Save archive unit test (no game, no device, takes a second): packs a save tree and restores it, and checks what a
// damaged, cut-short or forged archive does. Every expected value is written by hand from the format in
// save_archive.c; nothing is compared against the implementation's own idea of the file.
//
//   (1) a tree with files, nested and empty folders, a zero-byte file, a name outside ASCII and a case-collision
//       alias (.dm-case-N + xattr) exports to an archive that starts with the magic, and exports again byte for byte;
//   (2) importing it into an empty folder and exporting that gives the same bytes: names, kinds and contents survive;
//   (3) an import replaces the committed save, keeps the replaced tree as <save>.pre-import and clears .work / .new;
//   (4) a damaged checksum, a cut-short file, trailing bytes and an unknown format version are refused, and the
//       save that was there stays as it was;
//   (5) ".." and unsorted entries are refused before anything is written;
//   (6) another game's archive is refused when both title IDs are known, and accepted when one is not;
//   (7) a game that also has device save data gets both trees back;
//   (8) nothing to export is its own answer, and a mount (the game running) refuses both directions;
//   (9) an empty save (the game mounted but never saved) still round-trips.
#include <dirent.h>
#include <ftw.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "../save_archive.h"

static int g_in_use;
int dm_fs_save_in_use(void) { return g_in_use; }   // the strong implementation; save_archive.c's is weak

#define UNICODE_NAME "\xC3\x9C" "n" "\xC3\xAF" "cod" "\xC3\xA9" " name.txt"   // "Ünïcodé name.txt"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  \xE2\x9C\x97 "); } else printf("  \xE2\x9C\x93 "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int mk(const char *p) {   // mkdir -p
    char b[1024];
    if (strlen(p) >= sizeof b) return 0;
    strcpy(b, p);
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; if (mkdir(b, 0755) && errno != EEXIST) return 0; *s = '/'; }
    return !mkdir(b, 0755) || errno == EEXIST;
}
static int put(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static int exists(const char *p) { struct stat st; return !lstat(p, &st); }
static long size_of(const char *p) { struct stat st; return !lstat(p, &st) ? (long)st.st_size : -1; }
static int is_dir(const char *p) { struct stat st; return !lstat(p, &st) && S_ISDIR(st.st_mode); }
static int same_bytes(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    int same = 1;
    for (;;) {
        unsigned char x[65536], y[65536];
        size_t nx = fread(x, 1, sizeof x, fa), ny = fread(y, 1, sizeof y, fb);
        if (nx != ny || memcmp(x, y, nx)) { same = 0; break; }
        if (nx < sizeof x) break;
    }
    fclose(fa); fclose(fb);
    return same;
}
static int has_magic(const char *p) {
    unsigned char b[8] = {0};
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b, f); fclose(f);
    return n == 8 && !memcmp(b, "NXSAVE", 6) && !b[6] && !b[7];
}
// <dir>/<name> in a buffer
static char *at(char *buf, size_t n, const char *dir, const char *name) { snprintf(buf, n, "%s/%s", dir, name); return buf; }
// A forged archive of directory entries only: no data, so the checks that matter here (names, order, roots) run before
// any checksum is read.
static int forge(const char *path, const char *const *names, int count, uint64_t title) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    unsigned char h[32] = {0};
    memcpy(h, "NXSAVE", 6);
    h[8] = 1;   // version 1
    for (int i = 0; i < 8; i++) h[16 + i] = (unsigned char)(title >> (8 * i));
    for (int i = 0; i < 4; i++) h[24 + i] = (unsigned char)((unsigned)count >> (8 * i));
    int ok = fwrite(h, 1, sizeof h, f) == sizeof h;
    for (int i = 0; ok && i < count; i++) {
        size_t len = strlen(names[i]);
        unsigned char e[12] = {0};
        e[0] = 2;   // directory
        e[2] = (unsigned char)len; e[3] = (unsigned char)(len >> 8);
        ok = fwrite(e, 1, sizeof e, f) == sizeof e && fwrite(names[i], 1, len, f) == len;
    }
    fclose(f);
    return ok;
}
static int flip_byte(const char *path, long from_end) {
    FILE *f = fopen(path, "r+b"); if (!f) return 0;
    if (fseek(f, -from_end, SEEK_END)) { fclose(f); return 0; }
    int c = fgetc(f); if (c == EOF) { fclose(f); return 0; }
    fseek(f, -from_end, SEEK_END);
    int ok = fputc(c ^ 0x5a, f) != EOF;
    fclose(f);
    return ok;
}
static int poke32(const char *path, long off, unsigned v) {
    FILE *f = fopen(path, "r+b"); if (!f) return 0;
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    int ok = !fseek(f, off, SEEK_SET) && fwrite(b, 1, 4, f) == 4;
    fclose(f);
    return ok;
}

static int remove_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw) { (void)sb; (void)type; (void)ftw; return remove(path); }
int main(void) {
    char root[1024], a[1200], b[1200], c[1200], dev[1200], p[1400], x[1200], y[1200];
    const char *tp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/t_save_archive.XXXXXX", tp ? tp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    at(a, sizeof a, root, "a"); at(b, sizeof b, root, "b"); at(c, sizeof c, root, "c");
    at(x, sizeof x, root, "a.dmsave"); at(y, sizeof y, root, "b.dmsave");
    setenv("DM_SAVE", a, 1);

    printf("packing\n");
    CHECK(mk(a), "make the save folder");
    CHECK(mk(at(p, sizeof p, a, "sub/empty-dir")) && mk(at(p, sizeof p, a, "sub/deeper")), "make nested and empty folders");
    char one[1200], nested[1200];
    at(one, sizeof one, a, "slot1.dat"); at(nested, sizeof nested, a, "sub/deeper/nested.bin");
    CHECK(put(one, "hello save", 10), "write a small file");
    unsigned char *big = malloc(300000);
    for (int i = 0; i < 300000; i++) big[i] = (unsigned char)(i * 7 + 3);
    CHECK(put(nested, big, 300000), "write a 300 KB file across several read chunks");
    free(big);
    CHECK(put(at(p, sizeof p, a, "sub/zero"), "", 0), "write a zero-byte file");
    CHECK(put(at(p, sizeof p, a, UNICODE_NAME), "utf8", 4), "write a name outside ASCII");
    // A case collision as hle_fs.c makes one: two guest names that differ only in case, the second a .dm-case-N alias
    CHECK(put(at(p, sizeof p, a, "Alias.bin"), "upper", 5), "write Alias.bin");
    char alias[1400];
    at(alias, sizeof alias, a, ".dm-case-1");
    CHECK(put(alias, "lower", 5) && !setxattr(alias, "org.d2rmobile.guest-name", "alias.bin", 9, 0, XATTR_NOFOLLOW), "write alias.bin as the case alias .dm-case-1");

    int rc = dm_save_export(x, 0x0100000000000001ull, NULL, 0);
    CHECK(rc == DM_SAVE_OK && has_magic(x), "export A (rc %d)", rc);
    long n1 = size_of(x);
    rc = dm_save_export(y, 0x0100000000000001ull, NULL, 0);
    CHECK(rc == DM_SAVE_OK && same_bytes(x, y), "exporting the same save again is byte for byte the same file (%ld bytes)", n1);

    printf("restoring into an empty folder\n");
    unlink(y);
    setenv("DM_SAVE", b, 1);
    rc = dm_save_import(x, 0x0100000000000001ull, NULL, 0);
    CHECK(rc == DM_SAVE_OK, "import into B (rc %d)", rc);
    CHECK(exists(at(p, sizeof p, b, "slot1.dat")), "B has the small file");
    CHECK(is_dir(at(p, sizeof p, b, "sub/empty-dir")) && is_dir(at(p, sizeof p, b, "sub/deeper")), "B has the empty and nested folders");
    CHECK(size_of(at(p, sizeof p, b, "sub/deeper/nested.bin")) == 300000, "B's 300 KB file is 300000 bytes");
    CHECK(size_of(at(p, sizeof p, b, "sub/zero")) == 0, "B's zero-byte file is there");
    CHECK(exists(at(p, sizeof p, b, UNICODE_NAME)), "B kept the non-ASCII name");
    CHECK(size_of(at(p, sizeof p, b, ".dm-case-1")) == 5 && exists(at(p, sizeof p, b, "Alias.bin")), "B rebuilt both case-colliding names, the alias as .dm-case-1");
    char guest[64] = {0};
    CHECK(getxattr(at(p, sizeof p, b, ".dm-case-1"), "org.d2rmobile.guest-name", guest, sizeof guest, 0, XATTR_NOFOLLOW) == 9 && !strcmp(guest, "alias.bin"), "the alias carries its guest name back (%s)", guest);
    setenv("DM_SAVE", b, 1);
    rc = dm_save_export(y, 0x0100000000000001ull, NULL, 0);
    CHECK(rc == DM_SAVE_OK && same_bytes(x, y), "B exports to the same bytes as A: names, kinds and contents survived");

    printf("replacing a save\n");
    CHECK(put(at(p, sizeof p, b, "marker.txt"), "old save", 8), "write a marker into the save about to be replaced");
    CHECK(mk(at(p, sizeof p, root, "b.work/x")) && mk(at(p, sizeof p, root, "b.new")), "leave a .work and a .new from an interrupted run");
    rc = dm_save_import(x, 0x0100000000000001ull, NULL, 0);
    CHECK(rc == DM_SAVE_OK, "import over the existing save (rc %d)", rc);
    CHECK(!exists(at(p, sizeof p, b, "marker.txt")), "the replaced save is no longer in place");
    CHECK(size_of(at(p, sizeof p, root, "b.pre-import/marker.txt")) == 8, "it is kept as b.pre-import, marker and all");
    CHECK(!exists(at(p, sizeof p, root, "b.work")) && !exists(at(p, sizeof p, root, "b.new")), "the leftover .work and .new are cleared");
    CHECK(!exists(at(p, sizeof p, root, "b.import")), "the staging folder is gone");

    printf("refusing damage\n");
    const char *before = "still here";
    CHECK(put(at(p, sizeof p, b, "marker.txt"), before, strlen(before)), "put a marker in the save");
    char bad[1200]; at(bad, sizeof bad, root, "bad.dmsave");
    unlink(bad);
    CHECK(put(bad, "", 0), "make room for a damaged copy");
    {   // copy x to bad, then corrupt its last byte (the last file's checksum)
        FILE *in = fopen(x, "rb"), *out = fopen(bad, "wb"); char buf[65536]; size_t n;
        while (in && out && (n = fread(buf, 1, sizeof buf, in))) fwrite(buf, 1, n, out);
        if (in) fclose(in); if (out) fclose(out);
    }
    char msg[512] = "";
    CHECK(flip_byte(bad, 2), "flip a byte in the last checksum");
    rc = dm_save_import(bad, 0x0100000000000001ull, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT && exists(at(p, sizeof p, b, "marker.txt")), "a damaged archive is refused (rc %d: %s) and the save is untouched", rc, msg);
    CHECK(!exists(at(p, sizeof p, root, "b.import")), "and its staging folder is removed");

    {   // cut the file in half
        FILE *in = fopen(x, "rb"); long sz = size_of(x);
        unsigned char *all = malloc((size_t)sz); size_t got = in ? fread(all, 1, (size_t)sz, in) : 0; if (in) fclose(in);
        CHECK(got == (size_t)sz && put(bad, all, got / 2), "copy the first half of the archive");
        free(all);
    }
    rc = dm_save_import(bad, 0x0100000000000001ull, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT && exists(at(p, sizeof p, b, "marker.txt")), "a cut-short archive is refused (rc %d: %s)", rc, msg);

    {   // the whole archive plus one byte
        FILE *in = fopen(x, "rb"), *out = fopen(bad, "wb"); char buf[65536]; size_t n;
        while (in && out && (n = fread(buf, 1, sizeof buf, in))) fwrite(buf, 1, n, out);
        if (in) fclose(in); if (out) fputc('!', out), fclose(out);
    }
    rc = dm_save_import(bad, 0x0100000000000001ull, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT && exists(at(p, sizeof p, b, "marker.txt")), "bytes after the last entry are refused (rc %d: %s)", rc, msg);

    {   // unknown format version
        FILE *in = fopen(x, "rb"), *out = fopen(bad, "wb"); char buf[65536]; size_t n;
        while (in && out && (n = fread(buf, 1, sizeof buf, in))) fwrite(buf, 1, n, out);
        if (in) fclose(in); if (out) fclose(out);
    }
    CHECK(poke32(bad, 8, 2), "write format version 2");
    rc = dm_save_import(bad, 0x0100000000000001ull, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT && exists(at(p, sizeof p, b, "marker.txt")), "an unknown format version is refused (rc %d: %s)", rc, msg);

    printf("refusing other archives\n");
    const char *traversal[] = {"save", "save/../evil"};
    CHECK(forge(bad, traversal, 2, 0), "forge an archive whose entry climbs out of the save");
    rc = dm_save_import(bad, 0, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT, "a \"..\" entry is refused (rc %d: %s)", rc, msg);
    CHECK(!exists(at(p, sizeof p, root, "evil")), "nothing was written outside the save");

    const char *trailing[] = {"save", "save/"};
    CHECK(forge(bad, trailing, 2, 0), "forge an archive whose root has a trailing slash");
    rc = dm_save_import(bad, 0, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT, "a root with a trailing slash is refused (rc %d: %s)", rc, msg);

    const char *orphan[] = {"save", "save/a/b"};
    CHECK(forge(bad, orphan, 2, 0), "forge an archive that lists a folder before its parent");
    rc = dm_save_import(bad, 0, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT, "an entry whose folder is missing is refused (rc %d: %s)", rc, msg);

    const char *unsorted[] = {"save", "save/b", "save/a"};
    CHECK(forge(bad, unsorted, 3, 0), "forge an archive out of order");
    rc = dm_save_import(bad, 0, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT, "unsorted entries are refused (rc %d: %s)", rc, msg);

    const char *duplicate[] = {"save", "save/a", "save/a"};
    CHECK(forge(bad, duplicate, 3, 0), "forge an archive with a duplicated entry");
    rc = dm_save_import(bad, 0, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT, "duplicate entries are refused (rc %d: %s)", rc, msg);

    const char *no_save[] = {"save_device", "save_device/island.dat"};
    CHECK(forge(bad, no_save, 2, 0), "forge an archive without the account save");
    rc = dm_save_import(bad, 0, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_FORMAT, "an archive with no save folder is refused (rc %d: %s)", rc, msg);

    printf("other game, unknown game\n");
    rc = dm_save_import(x, 0x0100000000000002ull, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_GAME && exists(at(p, sizeof p, b, "marker.txt")), "another game's archive is refused (rc %d: %s)", rc, msg);
    rc = dm_save_import(x, 0, NULL, 0);
    CHECK(rc == DM_SAVE_OK, "a launcher that does not know its own title ID may still import (rc %d)", rc);
    setenv("DM_SAVE", a, 1);
    rc = dm_save_export(y, 0, NULL, 0);
    CHECK(rc == DM_SAVE_OK, "an export that records no title ID (rc %d)", rc);
    setenv("DM_SAVE", b, 1);
    rc = dm_save_import(y, 0x0100000000000002ull, NULL, 0);
    CHECK(rc == DM_SAVE_OK, "an archive with no title ID is accepted for any game (rc %d)", rc);

    printf("device save data\n");
    at(dev, sizeof dev, root, "a_device");
    mk(dev);
    CHECK(put(at(p, sizeof p, dev, "island.dat"), "island", 6), "add a device save beside the account save");
    setenv("DM_SAVE", a, 1);
    unlink(y);
    msg[0] = 0;
    rc = dm_save_export(y, 0x0100000000000001ull, msg, sizeof msg);
    CHECK(rc == DM_SAVE_OK, "export both saves (rc %d: %s)", rc, msg);
    setenv("DM_SAVE", c, 1);
    rc = dm_save_import(y, 0x0100000000000001ull, msg, sizeof msg);
    CHECK(rc == DM_SAVE_OK, "import both saves into an empty folder (rc %d: %s)", rc, msg);
    CHECK(exists(at(p, sizeof p, c, "slot1.dat")) && exists(at(p, sizeof p, root, "c_device/island.dat")), "both trees are in place");
    unlink(x);
    setenv("DM_SAVE", c, 1);
    rc = dm_save_export(x, 0x0100000000000001ull, NULL, 0);
    CHECK(rc == DM_SAVE_OK && same_bytes(x, y), "and export again to the same bytes");

    printf("nothing, and a running game\n");
    {   char none[1200]; at(none, sizeof none, root, "no-save"); setenv("DM_SAVE", none, 1);
        rc = dm_save_export(x, 0, msg, sizeof msg);
        CHECK(rc == DM_SAVE_ERR_EMPTY, "a game that never saved has nothing to export (rc %d: %s)", rc, msg); }
    g_in_use = 1;
    rc = dm_save_export(x, 0, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_USAGE, "a mounted save is not exported (rc %d: %s)", rc, msg);
    rc = dm_save_import(x, 0, msg, sizeof msg);
    CHECK(rc == DM_SAVE_ERR_USAGE, "nor replaced (rc %d: %s)", rc, msg);
    g_in_use = 0;

    printf("a mounted but empty save\n");
    {   char empty[1200]; at(empty, sizeof empty, root, "empty-save"); mk(empty); setenv("DM_SAVE", empty, 1);
        unlink(y);
        rc = dm_save_export(y, 0, msg, sizeof msg);
        CHECK(rc == DM_SAVE_ERR_EMPTY && !exists(y), "a save folder with no files in it is not an export (rc %d: %s)", rc, msg); }

    printf("\n%d checks, %d failed\n", checks, fails);
    if (!fails) {   // leave a clean temp tree behind
        nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    }
    return fails ? 1 : 0;
}
