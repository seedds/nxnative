// ctl_scan helper (dm/tools/ctl_scan.py compiles it with cc on first use): prints every aligned value of one type in a memory dump that lies in [lo, hi].
// Usage: ctl_scan <file> <base address, hex> <type: i32 | i64 | f32 | f64> <lo> <hi> [max results]
// Each result is "<address hex> <value>"; the address is the base plus the value's offset in the file. Integer bounds are exact, float bounds are doubles.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BUF = 1 << 22 };

static int is_int(const char *t) { return t[0] == 'i'; }

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: ctl_scan <file> <base hex> <i32|i64|f32|f64> <lo> <hi> [max]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 3; }
    uint64_t base = strtoull(argv[2], NULL, 16);
    const char *type = argv[3];
    int wide = strcmp(type, "i64") == 0 || strcmp(type, "f64") == 0;
    if (!wide && strcmp(type, "i32") != 0 && strcmp(type, "f32") != 0) { fprintf(stderr, "unknown type %s\n", type); return 2; }
    int64_t ilo = 0, ihi = 0;
    double flo = 0, fhi = 0;
    if (is_int(type)) { ilo = strtoll(argv[4], NULL, 10); ihi = strtoll(argv[5], NULL, 10); }
    else { flo = strtod(argv[4], NULL); fhi = strtod(argv[5], NULL); }
    long max = argc > 6 ? atol(argv[6]) : 1000000, found = 0;
    size_t step = wide ? 8 : 4;
    static unsigned char buf[BUF];
    uint64_t off = 0;
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        for (size_t i = 0; i + step <= got; i += step) {
            if (is_int(type)) {
                int64_t v;
                if (wide) { memcpy(&v, buf + i, 8); } else { int32_t x; memcpy(&x, buf + i, 4); v = x; }
                if (v >= ilo && v <= ihi) { printf("%llx %lld\n", (unsigned long long)(base + off + i), (long long)v); if (++found >= max) goto done; }
            } else {
                double v;
                if (wide) { memcpy(&v, buf + i, 8); } else { float x; memcpy(&x, buf + i, 4); v = x; }
                if (v >= flo && v <= fhi) { printf("%llx %.9g\n", (unsigned long long)(base + off + i), v); if (++found >= max) goto done; }
            }
        }
        off += got;
    }
done:
    fclose(f);
    return 0;
}
