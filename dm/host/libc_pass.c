// The C library passthrough of the x86-64 execution engine (Linux and Windows; dm/host/CMakeLists.txt builds this file in place of hle_table.c, which stays Apple's).
// hle_table.c connects C library functions consistent with AAPCS64 and with no struct layout dependency straight to Apple's libc: a guest call there is a native call of
// the host's function. Recompiled code reaches a host function only through its host-call thunk (docs/design/aot-recompiler.md 2.4; dm/host/tools/gen_hle_thunks.py), so
// here each passthrough function is a PASS entry: the same name in the import table (the swhle section, as hle_table.c's L entries), declared with the guest's own types
// (the generator writes the thunk from them), and a C function libc_<name> that calls the host's C library. Why entries with C bodies, and not the generator reading the C
// library's declarations from the system headers (-I): the guest's types are not the host's everywhere (long is 32 bits on Windows: strtol, strtoul and lroundf answer
// with the 64-bit functions here, as the Switch's long is 64 bits; time_t is passed as the guest's 64-bit value), some of the functions are static inline functions of a
// header on Windows (mingw-w64's gmtime_r and localtime_r), whose address differs in every translation unit, so the import table's entry and the thunk's call would name
// two functions, and a header's prototype carries qualifiers and attributes the generator's C reader would have to learn per C library. A body of one call costs a direct
// call more than the thunk's own; the table is the same 63 functions as hle_table.c, none variadic and none taking a structure by value (the generator refuses both).
// struct tm is the guest's: glibc's has Apple's layout (nine ints, tm_gmtoff, tm_zone), so Linux reads and writes what Apple does; Windows' has the nine ints only, so
// mktime, timegm, gmtime_r, localtime_r and strftime there neither read nor write tm_gmtoff and tm_zone (the guest's two fields keep what they held).
// mktime and localtime_r are also hle_time.c's forced take-overs, which win at binding (loader.c bind_sym), as on Apple
#include "hle.h"
#include <math.h>
#include <time.h>
#if !defined(_WIN32)
#include <strings.h>   // strcasecmp, strncasecmp (Windows: string.h)
#endif

// PASS(name, ret, params...): the C library function name the guest imports, its guest prototype; the body follows. The generator reads PASS( as an entry named name,
// C function libc_<name> (external: HLE_LINKAGE in hle.h)
#define PASS(name, ret, ...)                                                                          \
    HLE_LINKAGE ret libc_##name(__VA_ARGS__);                                                         \
    PLAT_TABLE_ENTRY(swhle) static const hle_ent hle_pass_##name = {#name, (void *)libc_##name};      \
    HLE_LINKAGE ret libc_##name(__VA_ARGS__)

// ---------- Memory and strings ----------
PASS(memcpy, void *, void *d, const void *s, size_t n) { return memcpy(d, s, n); }
PASS(memset, void *, void *d, int c, size_t n) { return memset(d, c, n); }
PASS(memmove, void *, void *d, const void *s, size_t n) { return memmove(d, s, n); }
PASS(memcmp, int, const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
PASS(memchr, void *, const void *s, int c, size_t n) { return memchr(s, c, n); }
PASS(strlen, size_t, const char *s) { return strlen(s); }
PASS(strnlen, size_t, const char *s, size_t n) { return strnlen(s, n); }
PASS(strcmp, int, const char *a, const char *b) { return strcmp(a, b); }
PASS(strncmp, int, const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
PASS(strcpy, char *, char *d, const char *s) { return strcpy(d, s); }
PASS(strncpy, char *, char *d, const char *s, size_t n) { return strncpy(d, s, n); }
PASS(strcat, char *, char *d, const char *s) { return strcat(d, s); }
PASS(strncat, char *, char *d, const char *s, size_t n) { return strncat(d, s, n); }
PASS(strchr, char *, const char *s, int c) { return strchr(s, c); }
PASS(strrchr, char *, const char *s, int c) { return strrchr(s, c); }
PASS(strstr, char *, const char *h, const char *n) { return strstr(h, n); }
PASS(strspn, size_t, const char *s, const char *accept) { return strspn(s, accept); }
PASS(strcspn, size_t, const char *s, const char *reject) { return strcspn(s, reject); }
PASS(strpbrk, char *, const char *s, const char *accept) { return strpbrk(s, accept); }
PASS(strcasecmp, int, const char *a, const char *b) { return strcasecmp(a, b); }
PASS(strncasecmp, int, const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }

// ---------- Numbers from text: long is 64 bits on the Switch, so strtol and strtoul are the long long functions here (the same on LP64 Linux) ----------
PASS(atoi, int, const char *s) { return atoi(s); }
PASS(atof, double, const char *s) { return atof(s); }
PASS(strtol, int64_t, const char *s, char **end, int base) { return strtoll(s, end, base); }
PASS(strtoul, uint64_t, const char *s, char **end, int base) { return strtoull(s, end, base); }
PASS(strtoll, int64_t, const char *s, char **end, int base) { return strtoll(s, end, base); }
PASS(strtoull, uint64_t, const char *s, char **end, int base) { return strtoull(s, end, base); }
PASS(strtod, double, const char *s, char **end) { return strtod(s, end); }
PASS(strtof, float, const char *s, char **end) { return strtof(s, end); }

// ---------- Mathematics (lroundf's long: llroundf) ----------
PASS(sqrtf, float, float x) { return sqrtf(x); }
PASS(cbrtf, float, float x) { return cbrtf(x); }
PASS(expf, float, float x) { return expf(x); }
PASS(acosf, float, float x) { return acosf(x); }
PASS(cosf, float, float x) { return cosf(x); }
PASS(sinf, float, float x) { return sinf(x); }
PASS(atan2f, float, float y, float x) { return atan2f(y, x); }
PASS(fmodf, float, float x, float y) { return fmodf(x, y); }
PASS(cos, double, double x) { return cos(x); }
PASS(sin, double, double x) { return sin(x); }
PASS(powf, float, float x, float y) { return powf(x, y); }
PASS(atanf, float, float x) { return atanf(x); }
PASS(tanf, float, float x) { return tanf(x); }
PASS(asinf, float, float x) { return asinf(x); }
PASS(log10, double, double x) { return log10(x); }
PASS(pow, double, double x, double y) { return pow(x, y); }
PASS(logf, float, float x) { return logf(x); }
PASS(ldexpf, float, float x, int e) { return ldexpf(x, e); }
PASS(log10f, float, float x) { return log10f(x); }
PASS(log, double, double x) { return log(x); }
PASS(ldexp, double, double x, int e) { return ldexp(x, e); }
PASS(frexp, double, double x, int *e) { return frexp(x, e); }
PASS(atan2, double, double y, double x) { return atan2(y, x); }
PASS(modff, float, float x, float *ip) { return modff(x, ip); }
PASS(exp2f, float, float x) { return exp2f(x); }
PASS(exp2, double, double x) { return exp2(x); }
PASS(log2f, float, float x) { return log2f(x); }
PASS(log2, double, double x) { return log2(x); }
PASS(lroundf, int64_t, float x) { return (int64_t)llroundf(x); }

// ---------- Calendar time: the guest's time_t is a 64-bit integer (time_t is 64 bits on Linux and with mingw-w64 too; copied, not reinterpreted) ----------
// The zone name glibc writes into the guest's struct tm (tm_zone: a pointer to the C library's own static strings, "GMT" or the local zone's) is read by the guest: on the
// x86-64 engine it becomes an arena copy (dm_garena_literal; aot.h, "Host memory the guest sees"), as hle_time.c's take-overs do. Only a pointer the call wrote is replaced:
// mktime and timegm leave the struct as it was when they fail, and its tm_zone is then the guest's own. Windows' struct tm has no tm_zone; Apple's text (hle_table.c, and the
// native form under DM_NATIVE_EXEC or DM_TEST_GUEST_C) hands over the C library's pointer as it is
#if DM_NATIVE_EXEC || defined(DM_TEST_GUEST_C) || defined(_WIN32)
#define GUEST_TM_ZONE(r) r
#define GUEST_TM_CALL(f, tm) f(tm)
#else
#include "aot.h"
static struct tm *guest_tm_zone(struct tm *r) {   // the result of gmtime_r / localtime_r: on success the call wrote every field, tm_zone too
    if (r && r->tm_zone) r->tm_zone = dm_garena_literal(r->tm_zone);
    return r;
}
static time_t guest_tm_call(time_t (*f)(struct tm *), struct tm *tm) {   // mktime / timegm: tm_zone replaced only when the call wrote it
    const char *was = tm->tm_zone;
    const time_t r = f(tm);
    if (tm->tm_zone && tm->tm_zone != was) tm->tm_zone = dm_garena_literal(tm->tm_zone);
    return r;
}
#define GUEST_TM_ZONE(r) guest_tm_zone(r)
#define GUEST_TM_CALL(f, tm) guest_tm_call(f, tm)
#endif
PASS(mktime, int64_t, struct tm *tm) { return (int64_t)GUEST_TM_CALL(mktime, tm); }
PASS(timegm, int64_t, struct tm *tm) { return (int64_t)GUEST_TM_CALL(timegm, tm); }
PASS(gmtime_r, struct tm *, const int64_t *t, struct tm *out) { time_t v = (time_t)*t; return GUEST_TM_ZONE(gmtime_r(&v, out)); }
PASS(localtime_r, struct tm *, const int64_t *t, struct tm *out) { time_t v = (time_t)*t; return GUEST_TM_ZONE(localtime_r(&v, out)); }
PASS(strftime, size_t, char *s, size_t max, const char *fmt, const struct tm *tm) { return strftime(s, max, fmt, tm); }
