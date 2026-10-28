// libc passthrough table: C library functions consistent with AAPCS64 and with no struct layout dependency, connected directly to Apple libc. Goes into the swhle section.
// Elsewhere the entries name the host C library's functions too, but guest code reaches them only through the execution engine (Phase 2), which owns the calling convention
// (Windows' long is 32 bits: strtol / strtoul return 32-bit values there, and its struct tm has no tm_gmtoff / tm_zone)
#include "dm.h"
#include <math.h>
#include <ctype.h>
#include <time.h>
typedef struct { const char *name; void *fn; } hle_ent;
#define L(n) PLAT_TABLE_ENTRY(swhle) static const hle_ent hle_lib_##n = { #n, (void *)n };
L(memcpy) L(memset) L(memmove) L(memcmp) L(memchr) L(strlen) L(strnlen) L(strcmp) L(strncmp)
L(strcpy) L(strncpy) L(strcat) L(strncat) L(strchr) L(strrchr) L(strstr) L(strspn) L(strcspn)
L(strpbrk) L(strcasecmp) L(strncasecmp) L(atoi) L(atof) L(strtol) L(strtoul) L(strtoll) L(strtoull)
L(strtod) L(strtof) L(sqrtf) L(cbrtf) L(expf) L(acosf) L(cosf) L(sinf) L(atan2f) L(fmodf) L(cos)
L(sin) L(powf) L(atanf) L(tanf) L(asinf) L(log10) L(pow) L(logf) L(ldexpf) L(log10f) L(log)
L(ldexp) L(frexp) L(atan2) L(modff) L(exp2f) L(exp2) L(log2f) L(log2) L(lroundf)
L(mktime) L(timegm) L(gmtime_r) L(localtime_r) L(strftime)
