// The RomFS image's first-use read telemetry unit test (takes milliseconds): includes hle_fs.c directly and calls the real ReadFile take-over. pread is replaced by a fake that
// does the real read and then moves a fake clock (now_ns, which the read path times with) by a scripted time, so a slow read is injected with no sleep. The log (stderr) is read back.
// The image is a hand-built 6-entry RomFS in romfs_image.py's layout, /data/a = 100 KiB of 'A', /data/b an empty directory, /data/c = "xy", /data/d = 20 KiB of 'D';
// every expected value below is written by hand from that layout and the rules (first use = any byte not read before this session, 8 ms default threshold, first 16 stalls then powers of two).
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
static ssize_t fake_pread(int fd, void *buf, size_t n, off_t off);
#define pread fake_pread
#include "../hle_fs.c"
#undef pread
ssize_t pread(int, void *, size_t, off_t);

struct dm_flags g_flag;
static uint64_t g_clock, g_delay, g_clock_calls;   // the fake clock, what the next read takes, and how often the read path asked it
uint64_t g_t0_ns;
uint64_t now_ns(void) { g_clock_calls++; return g_clock; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
static ssize_t fake_pread(int fd, void *buf, size_t n, off_t off) { ssize_t r = pread(fd, buf, n, off); g_clock += g_delay; return r; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)

#define BIG (100 << 10)                    // bytes of file a
#define DIG (20 << 10)                     // bytes of file d
#define A_OFF 32u                          // where a's bytes begin in the image (the header is 32 bytes)
#define D_OFF (32u + BIG + 2u)             // where d's bytes begin
#define MS 1000000ull

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
    FILE *f = fopen(ip, "wb"); if (!f) { free(ip); return NULL; }
    fwrite(img, 1, sizeof img, f); fclose(f);
    return ip;
}
static int remove_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw) { (void)sb; (void)type; (void)ftw; return remove(path); }

static int g_logfd = -1;   // stderr, redirected to a temp file the test reads back and truncates
static char *log_text(void) {
    off_t n = lseek(g_logfd, 0, SEEK_END); char *b = calloc(1, (size_t)n + 1);
    if (n > 0) { ssize_t k = pread(g_logfd, b, (size_t)n, 0); (void)k; }
    return b;
}
static void log_clear(void) { if (ftruncate(g_logfd, 0)) {} lseek(g_logfd, 0, SEEK_SET); }
static int count_in(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }

static void stall_reset(const char *ms) {   // a new session: nothing read yet, the threshold read again from the environment
    pthread_mutex_lock(&g_stall_lock); free(g_stall.r); memset(&g_stall, 0, sizeof g_stall); pthread_mutex_unlock(&g_stall_lock);
    atomic_store(&g_stall_ms, -1); atomic_store(&g_stall_mi, -1);
    if (ms) setenv("DM_FS_STALL_MS", ms, 1); else unsetenv("DM_FS_STALL_MS");
    g_clock = 1000 * MS; g_delay = 0; g_clock_calls = 0; log_clear();
}
// One nn::fs ReadFile of a file through its guest path: the read takes ms (nanoseconds, the fake clock's step) and the bytes must be the file's
static int rd(const char *path, uint64_t off, size_t n, uint64_t took_ns) {
    static uint8_t buf[BIG];
    uint64_t h; if (h_fs_OpenFile(&h, path, MODE_READ) != RES_OK) return 0;
    g_delay = took_ns; size_t got = 0; Result r = h_fs_ReadFile(&got, h, (int64_t)off, buf, n); g_delay = 0;
    h_fs_CloseFile(h);
    return !r && got == n;
}

int main(void) {
    char root[1024], save[1100], *ip = NULL;
    const char *sp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/t_fs_stall.XXXXXX", sp ? sp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    snprintf(save, sizeof save, "%s/save", root); mkdir(save, 0755);
    char lp[1100]; snprintf(lp, sizeof lp, "%s/log.txt", root);
    g_logfd = open(lp, O_RDWR | O_CREAT | O_TRUNC, 0644); if (g_logfd < 0 || dup2(g_logfd, 2) < 0) { perror("log"); return 1; }
    ip = rom_build(root); if (!ip) { CHECK(0, "the fixture could not write the image into the temp tree"); goto done; }
    setenv("DM_ROMFS", ip, 1); setenv("DM_SAVE", save, 1);
    CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image");
    CHECK(mount("folder", save, 0) == RES_OK, "mount a folder beside the image");
    char folder_file[1200]; snprintf(folder_file, sizeof folder_file, "%s/plain", save);
    FILE *plain = fopen(folder_file, "wb"); if (!plain) { CHECK(0, "the fixture could not write a folder file"); goto done; }
    { static char fb[BIG]; memset(fb, 'F', sizeof fb); fwrite(fb, 1, sizeof fb, plain); fclose(plain); }
    char *t;

    // ---------- classes, the default threshold and the summary line ----------
    stall_reset(NULL);
    CHECK(rd("rom:/data/a", 0, 40 << 10, 12 * MS), "read a's first 40 KiB, taking 12 ms");
    CHECK(g_stall.reads[0] == 1 && g_stall.bytes[0] == 40960 && g_stall.reads[1] == 0, "a first read counts as one first-use read of its 40960 bytes (got %llu reads, %llu bytes, %llu repeat)",
          (unsigned long long)g_stall.reads[0], (unsigned long long)g_stall.bytes[0], (unsigned long long)g_stall.reads[1]);
    t = log_text();
    CHECK(count_in(t, "[fs] first-use read waited 12.0 ms for 40.0 KiB at image offset 0x20\n") == 1 && count_in(t, "first-use read") == 1, "one stall line at the default 8 ms: 12.0 ms, 40.0 KiB, image offset 0x20 (a's bytes begin at 32)");
    free(t); log_clear();
    CHECK(rd("rom:/data/a", 0, 40 << 10, 1 * MS), "read the same 40 KiB again, 1 ms");
    CHECK(g_stall.reads[0] == 1 && g_stall.reads[1] == 1 && g_stall.bytes[1] == 40960 && g_stall.bytes[0] == 40960, "a re-read counts as repeat: first use still 1 read, repeat 1 read of 40960 bytes");
    CHECK(rd("rom:/data/a", 0, 40 << 10, 30 * MS), "read it a third time, slowly (30 ms)");
    t = log_text(); CHECK(t[0] == 0, "a slow repeat read never logs a stall line"); free(t);
    CHECK(rd("rom:/data/a", 20 << 10, 40 << 10, 3 * MS), "read a at 20 KiB for 40 KiB: half old, half new, 3 ms");
    CHECK(g_stall.reads[0] == 2 && g_stall.bytes[0] == 40960 + 20480 && g_stall.reads[1] == 2, "the overlapping read is first use, and only its 20480 new bytes count (first use %llu reads, %llu bytes; repeat %llu)",
          (unsigned long long)g_stall.reads[0], (unsigned long long)g_stall.bytes[0], (unsigned long long)g_stall.reads[1]);
    CHECK(rd("rom:/data/c", 0, 2, 7999999), "read c's two bytes in 7.999999 ms: first use, below the threshold");
    t = log_text(); CHECK(t[0] == 0, "no stall line one nanosecond below 8 ms"); free(t);
    CHECK(rd("rom:/data/d", 0, DIG, 8 * MS), "read d in full in exactly 8 ms");
    t = log_text(); CHECK(count_in(t, "first-use read waited 8.0 ms for 20.0 KiB at image offset 0x19022\n") == 1 && count_in(t, "first-use read") == 1, "one stall line at exactly the threshold, at image offset 0x19022 (d begins at 102434)"); free(t);
    log_clear();
    CHECK(rd("folder:/plain", 0, BIG, 50 * MS), "read a folder-backed file in 50 ms");
    CHECK(g_stall.reads[0] == 4 && g_stall.reads[1] == 2 && g_stall.bytes[0] == 40960 + 20480 + 2 + 20480 && g_stall.bytes[1] == 40960 * 2, "a folder-backed read enters neither class (first use %llu, repeat %llu reads)", (unsigned long long)g_stall.reads[0], (unsigned long long)g_stall.reads[1]);
    t = log_text(); CHECK(t[0] == 0, "and writes no line"); free(t);
    dm_fs_stall_summary();
    t = log_text();
    CHECK(count_in(t, "[fs] image reads: first use 4 reads, 0.1 MiB, 31 ms (p50 8.2, p99 12.0, worst 12.0); repeat 2 reads, 0.1 MiB, 31 ms (worst 30.0)\n") == 1,
          "the exit summary: 4 first-use reads in 31 ms (p50 is the 7.999999 ms read's bucket edge, p99 the 12 ms read capped at its time), 2 repeat reads in 31 ms%s%s", count_in(t, "image reads") ? "" : " (got: ", count_in(t, "image reads") ? "" : t);
    free(t); log_clear();
    dm_fs_stall_summary();
    t = log_text(); CHECK(t[0] == 0, "a second exit with no first-use growth writes no summary"); free(t);
    CHECK(rd("rom:/data/a", 60 << 10, 4 << 10, 1 * MS), "read 4 KiB of a never read before");
    dm_fs_stall_summary();
    t = log_text(); CHECK(count_in(t, "image reads: first use 5 reads") == 1, "growth brings the summary again"); free(t);

    // ---------- the periodic summary: at most every 10 s of wall time, only while first-use bytes grew ----------
    stall_reset(NULL);
    CHECK(rd("rom:/data/a", 0, 1024, 1 * MS), "a first read, which starts the 10 s window");
    g_clock += 5000 * MS; CHECK(rd("rom:/data/a", 1024, 1024, 1 * MS), "a second first-use read 5 s later");
    t = log_text(); CHECK(t[0] == 0, "no summary inside the 10 s window"); free(t);
    g_clock += 6000 * MS; CHECK(rd("rom:/data/a", 2048, 1024, 1 * MS), "a third one, 11 s after the first");
    t = log_text(); CHECK(count_in(t, "image reads: first use 3 reads, 0.0 MiB, 3 ms (p50 1.0, p99 1.0, worst 1.0); repeat 0 reads, 0.0 MiB, 0 ms (worst 0.0)\n") == 1, "one summary then (p50 and p99 are the 1 ms reads' bucket edge, 1.024 ms, capped at the worst 1.0)%s%s", count_in(t, "image reads") ? "" : " (got: ", count_in(t, "image reads") ? "" : t); free(t);
    log_clear();
    CHECK(rd("rom:/data/a", 3072, 1024, 1 * MS), "a fourth read right after");
    t = log_text(); CHECK(t[0] == 0, "no second summary inside the new window"); free(t);
    g_clock += 11000 * MS; CHECK(rd("rom:/data/a", 0, 1024, 1 * MS), "a repeat read 11 s later");
    t = log_text(); CHECK(count_in(t, "image reads: first use 4 reads") == 1 && count_in(t, "repeat 1 reads") == 1, "the window's end summarizes the fourth read's growth, and counts the repeat read"); free(t);
    log_clear();
    g_clock += 11000 * MS; CHECK(rd("rom:/data/a", 0, 1024, 1 * MS), "another repeat read 11 s later");
    t = log_text(); CHECK(t[0] == 0, "a window that ends with no first-use growth writes nothing"); free(t);

    // ---------- DM_FS_STALL_MS=0: off ----------
    stall_reset("0");
    CHECK(rd("rom:/data/a", 0, 40 << 10, 50 * MS), "with DM_FS_STALL_MS=0 read 40 KiB in 50 ms");
    CHECK(g_stall.reads[0] == 0 && g_stall.reads[1] == 0 && g_stall.n == 0 && g_clock_calls == 0, "nothing counted, no range kept and the clock never asked (%llu calls)", (unsigned long long)g_clock_calls);
    dm_fs_stall_summary();
    t = log_text(); CHECK(t[0] == 0, "no stall line and no summary"); free(t);

    // ---------- DM_FS_STALL_MS=100: the threshold is the variable's ----------
    stall_reset("100");
    CHECK(rd("rom:/data/a", 0, 1024, 99 * MS), "with DM_FS_STALL_MS=100 read 1 KiB in 99 ms");
    t = log_text(); CHECK(t[0] == 0, "no line below 100 ms"); free(t);
    CHECK(rd("rom:/data/a", 4096, 1024, 100 * MS), "read another 1 KiB in 100 ms");
    t = log_text(); CHECK(count_in(t, "first-use read waited 100.0 ms for 1.0 KiB at image offset 0x1020\n") == 1, "one line at 100 ms, at image offset 0x1020"); free(t);

    // ---------- the flood limit ----------
    stall_reset(NULL);
    int ok = 1; for (int i = 0; i < 40; i++) ok &= rd("rom:/data/a", (uint64_t)i * 1024, 512, 10 * MS);
    CHECK(ok && g_stall.reads[0] == 40 && g_stall.stalls == 40, "40 first-use reads of 512 bytes, each 10 ms: 40 stalls");
    t = log_text();
    CHECK(count_in(t, "first-use read waited") == 17, "17 lines: the first 16 and then the power of two 32 (got %d)", count_in(t, "first-use read waited"));
    CHECK(count_in(t, "(stall 32)\n") == 1 && count_in(t, "(stall ") == 1, "only the line past 16 carries its count: (stall 32)");
    CHECK(strstr(t, "image offset 0x20\n") != NULL && strstr(t, "at image offset 0x7c20 (stall 32)\n") != NULL, "the first line is read 1 at 0x20 and the counted line read 32 at 0x7c20 (a + 31 KiB)");
    free(t);

done:
    free(ip);
    nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS);   // clean temp tree, no shell
    dprintf(1, "== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
