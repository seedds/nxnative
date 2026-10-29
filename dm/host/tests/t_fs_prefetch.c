// The fs prefetch list's record and walk unit test (takes seconds): includes hle_fs.c directly and calls its static implementation; pread is replaced by an intercepting fake, so the
// walk's reads are told apart from the game's (recording is on while fake_track is on) and real reads reach the real pread. The image is a hand-built 6-entry RomFS in
// romfs_image.py's layout, /data/a = 100 KiB of 'A', /data/b an empty directory, /data/c = "xy", /data/d = 20 KiB of 'D' (entry offsets are all hand-written from that layout).
// One binary, four phases keyed by DM_PHASE (the Makefile recipe fs-prefetch runs them in order as four processes; phases 1 and 2 share one list file through DM_FS_USER):
//   1 record: with DM_FS_RECORD=1 the reads of a and d append one range row each, a full re-read and a 2-byte read append nothing, and the identity row is the image's size;
//   2 prefetch: two game reads (a's first 40 KiB, all of d) before a walk called directly (no thread) — it reads only a's uncovered 60 KiB tail once, then skips the rest, and appends nothing;
//   3 another image: a list whose identity row names another image size is refused with no read at all;
//   4 malformed: a stamp-less list with a garbage row is refused (no read); a stamped list tolerates a garbage row and reads exactly its valid range row.
// All expected values are hand-written from the image layout and Horizon semantics, not computed by calling the implementation under test.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
static ssize_t fake_pread(int fd, void *buf, size_t n, off_t off);
#define pread fake_pread
#include "../hle_fs.c"
#undef pread
ssize_t pread(int, void *, size_t, off_t);

struct dm_flags g_flag; uint64_t g_t0_ns; uint64_t now_ns(void) { return 0; }   // stubs for the log timestamps and the read path
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }

// The walk's reads, logged while fake_track is on: one (offset, length) per pread, bounded like the recording arrays
static int g_track;
static struct { off_t a; size_t n; } g_track_log[4096];
static int g_track_n;
static ssize_t fake_pread(int fd, void *buf, size_t n, off_t off) {
    if (g_track) { if (g_track_n < 4096) { g_track_log[g_track_n].a = off; g_track_log[g_track_n].n = n; } g_track_n++; }
    return pread(fd, buf, n, off);
}

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)

// The image's layout, hand-written once (the entries' .a offsets are image-file relative): the header is 32 bytes, then the data area, then the index (6 entries of 24
// bytes plus the name pool "dataabcd"), so the image is 123066 bytes in all and that is what the identity row must name
#define BIG (100 << 10)                    // bytes of file a
#define DIG (20 << 10)                     // bytes of file d
#define A_OFF 32u                          // where a's bytes begin (the data area follows the header at once)
#define C_OFF (32u + BIG)                  // "xy" of file c
#define D_OFF (32u + BIG + 2u)             // where d's bytes begin
#define ROW_A "32\t102400\n"               // (a's offset, 100 KiB)
#define ROW_D "102434\t20480\n"            // (d's offset, 20 KiB)
#define LIST_1 "image\t123066\n" ROW_A
#define LIST_2 "image\t123066\n" ROW_A ROW_D

static char *rom_build(const char *root) {   // writes the image and returns its path (the caller frees it)
    static uint8_t img[32 + (100 << 10) + 2 + (20 << 10) + 152];
    memset(img, 0, sizeof img);
    uint64_t hd[3] = {32 + BIG + 2 + DIG, 6 * 24 + 8, 6};   // ioff, index bytes, entries
    memcpy(img, "DMROMFS1", 8); memcpy(img + 8, hd, sizeof hd);
    memset(img + 32, 'A', BIG);
    memcpy(img + 32 + BIG, "xy", 2);
    memset(img + 32 + BIG + 2, 'D', DIG);
    img_ent e[6] = {{1, 1, 0, 0, 0, 0}, {2, 4, 0, 4, 0, 0}, {32, BIG, 4, 1, 1, 0}, {6, 0, 5, 1, 0, 0}, {32 + BIG, 2, 6, 1, 1, 0}, {32 + BIG + 2, DIG, 7, 1, 1, 0}};
    memcpy(img + 32 + BIG + 2 + DIG, e, sizeof e);
    memcpy(img + 32 + BIG + 2 + DIG + sizeof e, "dataabcd", 8);
    char *ip = malloc(1100); snprintf(ip, 1100, "%s/rom.img", root);
    FILE *f = fopen(ip, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
    return ip;
}

static char *slurp(const char *p, size_t *out) {
    FILE *f = fopen(p, "rb"); if (!f) { *out = 0; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); size_t k = fread(b, 1, (size_t)n, f); fclose(f);
    b[k] = 0; *out = k; return b;
}
// The list is read back whole and compared with the expectation; on a miss its exact contents are printed
static int list_eq(const char *p, const char *want) {
    size_t k = 0; char *b = slurp(p, &k);
    int r = b && !strcmp(b, want);
    if (!r) printf("    the list file holds:\n%s\n    (expected)\n%s", b ? b : "(nothing)", want);
    free(b); return r;
}
static int read_file(const char *path, uint64_t off, void *buf, size_t n) {   // one nn::fs ReadFile of the image's file, the game's own path
    uint64_t h; if (h_fs_OpenFile(&h, path, MODE_READ) != RES_OK) return 0;
    size_t got = 0; Result r = h_fs_ReadFile(&got, h, (int64_t)off, buf, n);
    h_fs_CloseFile(h);
    return !r && got == n;
}

int main(void) {
    const char *ph = getenv("DM_PHASE"); int phase = ph ? atoi(ph) : 0;
    if (phase < 1 || phase > 4) { fprintf(stderr, "DM_PHASE=1..4 must name the phase (make fs-prefetch runs them in order)\n"); return 2; }
    unsetenv("DM_FS_RECORD"); unsetenv("DM_FS_PREFETCH");   // the mount hook must stay out: the drives here are set by the test, not by the environment
    char root[1024], save[1100], list[1100], *ip;
    const char *sp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/t_fs_prefetch.XXXXXX", sp ? sp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    snprintf(save, sizeof save, "%s/save", root); mkdir(save, 0755);
    ip = rom_build(root);
    setenv("DM_ROMFS", ip, 1); setenv("DM_SAVE", save, 1); setenv("DM_FS_PREFETCH_RATE", "0", 1);   // the walk uncapped: no sleep, and the read lengths stay exact
    const char *shared = getenv("DM_FS_USER");
    if (shared && *shared) strlcpy(list, shared, sizeof list); else snprintf(list, sizeof list, "%s/fs_user.tsv", root);   // direct runs default beside the scratch root; the recipe's phases 1 and 2 pass one shared file
    if (phase <= 2) setenv("DM_FS_USER", list, 1);   // phases 1 and 2 share that list; 3 and 4 write lists of their own below

    if (phase == 1) {                                            // ---------- phase 1: record ----------
        unlink(list);                                            // a fresh list: the recipe shares the file with phase 2, so start clean whatever ran before
        setenv("DM_FS_RECORD", "1", 1);
        CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image with the recorder on");
        char *buf = malloc(BIG);
        // A folder-backed read has an ordinary fd, whose high bits also decode to mount zero.
        CHECK(mount("folder", save, 0) == RES_OK, "mount a folder beside the tracked image");
        char folder_file[1200]; snprintf(folder_file, sizeof folder_file, "%s/plain", save);
        FILE *plain = fopen(folder_file, "wb"); memset(buf, 'F', BIG); fwrite(buf, 1, BIG, plain); fclose(plain);
        CHECK(read_file("folder:/plain", 0, buf, BIG), "read a folder-backed file while image recording is on");
        CHECK(list_eq(list, "image\t123066\n"), "folder reads never enter the image's range list");
        CHECK(read_file("rom:/data/a", 0, buf, BIG) && buf[0] == 'A' && buf[BIG / 2] == 'A' && buf[BIG - 1] == 'A', "the layout: a reads 100 KiB of 'A' in full");
        CHECK(list_eq(list, LIST_1), "the list: the identity row with the image's size and a's row only");
        CHECK(read_file("rom:/data/a", 0, buf, BIG), "a re-read of a in full");
        CHECK(list_eq(list, LIST_1), "a covered read appends nothing");
        char *d = malloc(DIG);
        CHECK(read_file("rom:/data/d", 0, d, DIG) && d[0] == 'D' && d[DIG - 1] == 'D', "the layout: d reads 20 KiB of 'D' in full");
        CHECK(list_eq(list, LIST_2), "d's row appends after a's");
        CHECK(read_file("rom:/data/c", 0, buf, 2) && buf[0] == 'x' && buf[1] == 'y', "c reads xy");
        CHECK(list_eq(list, LIST_2), "a 2-byte read never appends");
        free(buf); free(d);
    } else if (phase == 2) {                                     // ---------- phase 2: the walk skips what the game read ----------
        CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image (recording and prefetch off: the list is only read)");
        atomic_store(&g_fsimgrack, 0); atomic_store(&g_fsrng_on, 1);   // the mount's slot in g_mnt is 0 (fresh table), as fs_prefetch_start would set it
        char *buf = malloc(BIG);
        CHECK(read_file("rom:/data/a", 0, buf, 40 << 10), "the game read: a's first 40 KiB");
        CHECK(read_file("rom:/data/d", 0, buf, DIG), "the game read: all of d");
        size_t lb = 0; char *before = slurp(list, &lb);
        g_track = 1; g_track_n = 0;
        fs_prefetch_main((void *)(intptr_t)g_mnt[0].img->fd);   // called directly: no thread, no rate cap to sleep
        g_track = 0;
        int in_covered = 0; for (int k = 0; k < g_track_n; k++) if (g_track_log[k].a < (off_t)(A_OFF + (40 << 10)) || g_track_log[k].a + g_track_log[k].n > (off_t)D_OFF) in_covered++;
        CHECK(g_track_n == 1 && g_track_log[0].a == (off_t)(A_OFF + (40 << 10)) && g_track_log[0].n == BIG - (40 << 10),
              "the walk reads a's uncovered 60 KiB tail once at %llu+%llu (got %d reads, first at %llu+%llu)", (unsigned long long)(A_OFF + (40 << 10)),
              (unsigned long long)(BIG - (40 << 10)), g_track_n, g_track_n ? (unsigned long long)g_track_log[0].a : 0, g_track_n ? (unsigned long long)g_track_log[0].n : 0);
        CHECK(!in_covered, "nothing of a's covered head or of d (the game read both; %d reads touch them)", in_covered);
        size_t la = 0; char *after = slurp(list, &la);
        CHECK(lb == la && (lb == 0 || !memcmp(before, after, lb)), "the walk records nothing (the list's bytes are unchanged)");
        free(before); free(after); free(buf);
    } else if (phase == 3) {                                     // ---------- phase 3: the list of another image ----------
        char p[1300]; snprintf(p, sizeof p, "%s/other.tsv", root);
        FILE *f = fopen(p, "w"); fputs("image\t123999\n" ROW_A, f); fclose(f);   // a plausible list of another version's image
        setenv("DM_FS_USER", p, 1);
        CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount this run's image");
        atomic_store(&g_fsimgrack, 0); atomic_store(&g_fsrng_on, 1);
        g_track = 1; g_track_n = 0;
        fs_prefetch_main((void *)(intptr_t)g_mnt[0].img->fd);
        g_track = 0;
        CHECK(g_track_n == 0, "a list of another image size is refused without one read (got %d)", g_track_n);
    } else {                                                     // ---------- phase 4: malformed lists ----------
        CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image");
        atomic_store(&g_fsimgrack, 0); atomic_store(&g_fsrng_on, 1);
        char p[1300]; snprintf(p, sizeof p, "%s/nostamp.tsv", root);
        FILE *f = fopen(p, "w"); fputs("this line is not a row\n" ROW_A, f); fclose(f);   // a range row with no identity row, plus a garbage row
        setenv("DM_FS_USER", p, 1);
        g_track = 1; g_track_n = 0;
        fs_prefetch_main((void *)(intptr_t)g_mnt[0].img->fd);
        g_track = 0;
        CHECK(g_track_n == 0, "a stamp-less list with a garbage row is refused without one read (got %d)", g_track_n);
        char q[1300]; snprintf(q, sizeof q, "%s/stamped.tsv", root);
        f = fopen(q, "w"); fputs("image\t123066\nthis line is not a row\n" ROW_D, f); fclose(f);   // the stamp decides: one garbage row in the middle
        setenv("DM_FS_USER", q, 1);
        g_track = 1; g_track_n = 0;
        fs_prefetch_main((void *)(intptr_t)g_mnt[0].img->fd);
        g_track = 0;
        CHECK(g_track_n == 1 && g_track_log[0].a == (off_t)D_OFF && g_track_log[0].n == DIG,
              "a stamped list tolerates its garbage row and reads exactly the valid row's bytes (got %d reads)", g_track_n);
    }

    free(ip);
    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); system(cmd);
    printf("phase %d: == %d checks, %d failed ==\n", phase, checks, fails);
    return fails != 0;
}
