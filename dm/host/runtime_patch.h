// Mac launch-time code choices, applied before sealing text. Validate the entire file before any write.
// DMRT1<TAB>build ID (16 bytes, hex)<TAB>text end (hex)<TAB>DM_ variable[<TAB>DM_ variant variable]
// With a variant variable set to a value other than default, the choice <value>_<variant value> is taken when the manifest declares it (Metroid Prime 4:
// DM_INTERNAL_RESOLUTION=2160p_full with DM_SCREEN_RATIO=1610 applies 2160p_full_1610); otherwise <value>
// choice<TAB>value; write<TAB>value<TAB>offset (hex)<TAB>expected bytes<TAB>replacement bytes
#ifndef DM_RUNTIME_PATCH_H
#define DM_RUNTIME_PATCH_H
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RP_CHOICES = 16, RP_WRITES = 1024, RP_BYTES = 64 };
typedef struct { size_t off, len; unsigned choice; uint8_t before[RP_BYTES], after[RP_BYTES]; } rp_write;
typedef struct { char var[64], variant[64], choices[RP_CHOICES][32]; unsigned nc, nw; rp_write writes[RP_WRITES]; } rp_plan;
static int rp_error(char *err, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap); return -1;
}
static int rp_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static int rp_hex(const char *s, uint8_t *out, size_t *len) {
    size_t n = strlen(s); if (!n || n % 2 || n / 2 > RP_BYTES) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = rp_digit((unsigned char)s[2 * i]), b = rp_digit((unsigned char)s[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)((a << 4) | b);
    }
    *len = n / 2; return 0;
}
static int rp_offset(const char *s, size_t *out) {
    if (!*s) return -1;
    for (const char *p = s; *p; p++) if (rp_digit((unsigned char)*p) < 0) return -1;
    char *end; errno = 0; unsigned long long v = strtoull(s, &end, 16);
    if (errno || *end || v > SIZE_MAX) return -1;
    *out = (size_t)v; return 0;
}
static int rp_token(const char *s, size_t cap, int variable) {
    size_t n = strlen(s); if (!n || n >= cap || (variable && strncmp(s, "DM_", 3))) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || (!variable && *p >= 'a' && *p <= 'z'))) return 0;
    return 1;
}
static int rp_choice(const rp_plan *p, const char *s) {
    for (unsigned i = 0; i < p->nc; i++) if (!strcmp(s, p->choices[i])) return (int)i;
    return -1;
}
static int rp_read(FILE *f, rp_plan *p, const char *build, size_t text_end, const uint8_t *base, size_t size, char *err, size_t en) {
    char *line = NULL; size_t cap = 0; ssize_t len; unsigned row = 0; int header = 0, rc = -1;
#define RP_FAIL(...) do { rp_error(err, en, __VA_ARGS__); goto done; } while (0)
    while ((len = getline(&line, &cap, f)) >= 0) {
        row++;
        if (len > 1024 || memchr(line, 0, (size_t)len)) RP_FAIL("line %u: too long or embedded NUL", row);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len || line[0] == '#') continue;
        char *c[6], *s = line; unsigned nc = 0;
        do { if (nc == 6) RP_FAIL("line %u: too many fields", row); c[nc++] = s; s = strchr(s, '\t'); if (s) *s++ = 0; } while (s);
        if (!header) {
            size_t te; uint8_t bid[RP_BYTES]; size_t bn;
            if ((nc != 4 && nc != 5) || strcmp(c[0], "DMRT1") || strlen(c[1]) != 32 || rp_hex(c[1], bid, &bn) || strcmp(c[1], build) ||
                rp_offset(c[2], &te) || te != text_end || te > size || !rp_token(c[3], sizeof p->var, 1) || (nc == 5 && !rp_token(c[4], sizeof p->variant, 1)))
                RP_FAIL("line %u: incompatible runtime patch header/build/layout", row);
            strcpy(p->var, c[3]); if (nc == 5) strcpy(p->variant, c[4]); header = 1; continue;
        }
        if (nc == 2 && !strcmp(c[0], "choice")) {
            if (p->nw || p->nc == RP_CHOICES || !rp_token(c[1], sizeof p->choices[0], 0) || rp_choice(p, c[1]) >= 0) RP_FAIL("line %u: invalid/duplicate/late choice", row);
            strcpy(p->choices[p->nc++], c[1]); continue;
        }
        if (nc != 5 || strcmp(c[0], "write") || p->nw == RP_WRITES) RP_FAIL("line %u: invalid write", row);
        int choice = rp_choice(p, c[1]);
        if (choice < 0 || !strcmp(c[1], "default")) RP_FAIL("line %u: write needs a declared non-default choice", row);
        rp_write *w = &p->writes[p->nw]; size_t n;
        if (rp_offset(c[2], &w->off) || rp_hex(c[3], w->before, &w->len) || rp_hex(c[4], w->after, &n) || n != w->len || w->off % 4 || w->len % 4 ||
            w->off >= text_end || w->len > text_end - w->off) RP_FAIL("line %u: invalid/alignment/out-of-text write", row);
        w->choice = (unsigned)choice;
        if (memcmp(base + w->off, w->before, w->len)) RP_FAIL("line %u: expected instruction bytes differ at %zx", row, w->off);
        for (unsigned i = 0; i < p->nw; i++) {
            const rp_write *a = &p->writes[i];
            if (a->choice == w->choice && a->off < w->off + w->len && w->off < a->off + a->len) RP_FAIL("line %u: overlapping writes", row);
        }
        p->nw++;
    }
    if (ferror(f) || !header || !p->nw || rp_choice(p, "default") < 0) RP_FAIL("incomplete/unreadable runtime patch file");
    for (unsigned c = 0; c < p->nc; c++) {
        unsigned count = 0; for (unsigned i = 0; i < p->nw; i++) count += p->writes[i].choice == c;
        if (!count && strcmp(p->choices[c], "default")) RP_FAIL("choice has no writes: %s", p->choices[c]);
    }
    rc = 0;
done:
    free(line); return rc;
#undef RP_FAIL
}
static int runtime_patch_apply(FILE *f, const char *build, size_t text_end, uint8_t *base, size_t size, char *err, size_t en, unsigned *applied) {
    *applied = 0;
    rp_plan *p = calloc(1, sizeof *p); if (!p) return rp_error(err, en, "runtime patch allocation");
    int rc = rp_read(f, p, build, text_end, base, size, err, en);
    if (!rc) {
        const char *v = getenv(p->var); if (!v) v = "default";
        int choice = rp_choice(p, v);
        const char *w = p->variant[0] ? getenv(p->variant) : NULL;
        if (choice >= 0 && w && *w && strcmp(w, "default") && strcmp(v, "default")) {
            char name[96]; snprintf(name, sizeof name, "%s_%s", v, w);
            int alt = rp_choice(p, name); if (alt >= 0) choice = alt;
        }
        if (choice < 0) rc = rp_error(err, en, "unsupported %s=%s", p->var, v);
        else for (unsigned i = 0; i < p->nw; i++) if (p->writes[i].choice == (unsigned)choice) {
            memcpy(base + p->writes[i].off, p->writes[i].after, p->writes[i].len); (*applied)++;
        }
    }
    free(p); return rc;
}
#endif
