// Unit tests for DM_MEMSTAT / DM_MEMSTAT_WALK: interval parsing, walk vs cached class rows, and comma-containing paths.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../dm.h"

int dm_memstat_walk_interval(const char *s);
void dm_memstat_sample(FILE *f, double t, id<MTLDevice> dev, int walk);
void dm_diag_samplers_start(void);

module_t *g_mod;
int g_nmod;
void svc_guest_range(uint8_t **base, size_t *size) { *base = NULL; *size = 0; }
int thr_name_of(pthread_t pt, char *b, size_t n) { (void)pt; (void)b; (void)n; return 0; }
uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }

static int checks, failed;
static void expect_int(const char *name, int want, int got) {
    checks++;
    if (want != got) {
        failed++;
        fprintf(stderr, "%s: expected %d, got %d\n", name, want, got);
    }
}
static void expect_true(const char *name, int cond) {
    checks++;
    if (!cond) {
        failed++;
        fprintf(stderr, "%s: condition failed\n", name);
    }
}
static void expect_str(const char *name, const char *want, const char *got) {
    checks++;
    if (strcmp(want, got) != 0) {
        failed++;
        fprintf(stderr, "%s: expected '%s', got '%s'\n", name, want, got);
    }
}

static int split_tabs(char *line, char **cols, int max_cols) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    int n = 0;
    char *p = line;
    while (n < max_cols) {
        cols[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    return n;
}

static void read_line_at(FILE *f, long off, char *buf, size_t cap) {
    fflush(f);
    fseek(f, off, SEEK_SET);
    buf[0] = '\0';
    (void)fgets(buf, (int)cap, f);
    fseek(f, 0, SEEK_END);
}

int main(void) { @autoreleasepool {
    // 1. DM_MEMSTAT_WALK parsing
    expect_int("walk null", 1, dm_memstat_walk_interval(NULL));
    expect_int("walk empty", 1, dm_memstat_walk_interval(""));
    expect_int("walk 1", 1, dm_memstat_walk_interval("1"));
    expect_int("walk 30", 30, dm_memstat_walk_interval("30"));
    expect_int("walk 0", 1, dm_memstat_walk_interval("0"));
    expect_int("walk negative", 1, dm_memstat_walk_interval("-5"));
    expect_int("walk alpha", 1, dm_memstat_walk_interval("abc"));
    expect_int("walk trailing", 1, dm_memstat_walk_interval("30s"));
    expect_int("walk leading space", 1, dm_memstat_walk_interval(" 30"));
    expect_int("walk overflow", 1, dm_memstat_walk_interval("99999999999999999999999"));

    // 2. sample() walk=1 vs walk=0 caches class columns and preserves TSV format
    FILE *tf = tmpfile();
    if (!tf) return 2;

    long off0 = ftell(tf);
    dm_memstat_sample(tf, 0.0, nil, 1);
    char l0[1024];
    read_line_at(tf, off0, l0, sizeof l0);

    // Dirty a 16 MiB anonymous mapping (class "other") so a real walk would observe +16 MiB.
    const size_t map_sz = 16u << 20;
    uint8_t *buf = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    expect_true("mmap 16 MiB", buf != MAP_FAILED);
    if (buf != MAP_FAILED) {
        size_t pg = (size_t)getpagesize();
        for (size_t i = 0; i < map_sz; i += pg) buf[i] = (uint8_t)(i >> 12);
    }

    long off1 = ftell(tf);
    dm_memstat_sample(tf, 1.0, nil, 0);
    char l1[1024];
    read_line_at(tf, off1, l1, sizeof l1);

    long off2 = ftell(tf);
    dm_memstat_sample(tf, 2.0, nil, 1);
    char l2[1024];
    read_line_at(tf, off2, l2, sizeof l2);

    if (buf != MAP_FAILED) munmap(buf, map_sz);
    fclose(tf);

    char *c0[20], *c1[20], *c2[20];
    int n0 = split_tabs(l0, c0, 20);
    int n1 = split_tabs(l1, c1, 20);
    int n2 = split_tabs(l2, c2, 20);
    expect_int("row0 col count", 17, n0);
    expect_int("row1 col count", 17, n1);
    expect_int("row2 col count", 17, n2);
    if (n0 == 17 && n1 == 17 && n2 == 17) {
        expect_str("row0 seconds", "0", c0[0]);
        expect_str("row1 seconds", "1", c1[0]);
        expect_str("row2 seconds", "2", c2[0]);
        // Class columns (8..14) and charged total (15) must match verbatim when walk=0.
        for (int i = 8; i <= 15; i++) {
            char name[32];
            snprintf(name, sizeof name, "walk=0 repeats col %d", i);
            expect_str(name, c0[i], c1[i]);
        }
        // After walk=1, "other" (col 14) must reflect the +16 MiB dirty mapping.
        unsigned long long v0 = 0, r0 = 0, b0 = 0, v2 = 0, r2 = 0, b2 = 0;
        expect_int("parse row0 other", 3, sscanf(c0[14], "%llu/%llu/%llu", &v0, &r0, &b0));
        expect_int("parse row2 other", 3, sscanf(c2[14], "%llu/%llu/%llu", &v2, &r2, &b2));
        expect_true("walk=1 sees other virt growth", v2 >= v0 + 16);
        expect_true("walk=1 sees other res growth", r2 >= r0 + 16);
        expect_true("walk=1 sees other bill growth", b2 >= b0 + 16);
    }

    // 3. End-to-end: paths containing commas (even trailing ,<digits>) are preserved verbatim, and header/row format is intact.
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char dir[512];
    snprintf(dir, sizeof dir, "%s/t_memstat_%d,1", tmp, (int)getpid());
    expect_int("mkdir comma dir", 0, mkdir(dir, 0700));
    char path[640];
    snprintf(path, sizeof path, "%s/out,30", dir);

    setenv("DM_MEMSTAT", path, 1);
    setenv("DM_MEMSTAT_WALK", "30", 1);
    unsetenv("DM_THRCPU");
    dm_diag_samplers_start();

    char hdr0[1024] = {0}, hdr1[1024] = {0}, row0[1024] = {0};
    int got_lines = 0;
    for (int i = 0; i < 200; i++) {
        FILE *rf = fopen(path, "r");
        if (rf) {
            if (fgets(hdr0, sizeof hdr0, rf) && fgets(hdr1, sizeof hdr1, rf) && fgets(row0, sizeof row0, rf)) {
                got_lines = 1;
                fclose(rf);
                break;
            }
            fclose(rf);
        }
        usleep(10000);
    }
    expect_true("comma path written", got_lines);
    if (got_lines) {
        expect_true("header comment", strncmp(hdr0, "# unit MiB;", 11) == 0);
        expect_str("column header",
                   "seconds\tphys_footprint\tavailable\tmetal_alloc\tinternal\tcompressed\tgraphics\theadroom\tguest\tcode\tmalloc\tgpu\tstack\tfile\tother\tcharged total\tunattributed\n",
                   hdr1);
        char *rc[20];
        expect_int("e2e row col count", 17, split_tabs(row0, rc, 20));
    }

    unlink(path);
    rmdir(dir);

    printf("t_memstat: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
