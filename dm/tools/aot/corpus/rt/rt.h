// The corpus's run time (dm/tools/aot/corpus): no libc, two supervisor calls (svcOutputDebugString, svcExitProcess), a line formatter and the
// checks every program reports through. Each check prints one deterministic line, `<name> <value> ok` or `<name> <value> want <expected> FAIL`;
// rt_finish prints the last line, `PASS <checks>` or `FAIL <failed> of <checks>`. Executors compare whole outputs: a native run on ARM64 hardware,
// the dynarmic reference (nxaot_run) and, later, the recompiled code must print the same lines.
#ifndef NXAOT_CORPUS_RT_H
#define NXAOT_CORPUS_RT_H

#include <stddef.h>
#include <stdint.h>

#define RT_NOINLINE __attribute__((noinline))
#define RT_USED __attribute__((used))

// Inputs the compiler cannot see through: every check reads its operands from volatile storage, so constant folding keeps nothing from the
// instructions under test
#define RT_OPAQUE(x) ({ __typeof__(x) rt_v_ = (x); __asm__ volatile("" : "+r"(rt_v_)); rt_v_; })

void rt_svc_output_debug_string(const char *s, size_t n);
__attribute__((noreturn)) void rt_svc_exit_process(void);

// One output line: rt_begin, pieces, rt_end (one svcOutputDebugString per line)
void rt_begin(const char *name);
void rt_str(const char *s);
void rt_hex(uint64_t v, int digits);
void rt_dec(int64_t v);
void rt_end(void);
void rt_say(const char *line);

// Checks: name, value, expected. The value is printed in hex with the digits of its width
void rt_check(const char *name, uint64_t got, uint64_t want, int digits);
#define RT_CHECK64(name, got, want) rt_check((name), (uint64_t)(got), (uint64_t)(want), 16)
#define RT_CHECK32(name, got, want) rt_check((name), (uint64_t)(uint32_t)(got), (uint64_t)(uint32_t)(want), 8)
void rt_check128(const char *name, uint64_t got_lo, uint64_t got_hi, uint64_t want_lo, uint64_t want_hi);
// A check whose value is only printed and compared by the executors (no expected constant in the source)
void rt_note(const char *name, uint64_t value, int digits);
int rt_finish(void);  // the PASS / FAIL line; the exit code main returns (0 when every check passed)

// FNV-1a over 64-bit words, for checks that summarise many values in one line
static inline uint64_t rt_mix(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h = (h ^ ((v >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    }
    return h;
}
#define RT_MIX_INIT 0xcbf29ce484222325ull

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

// setjmp / longjmp of rt/setjmp.S: x19..x30, sp and d8..d15
typedef uint64_t jmp_buf[22];
__attribute__((returns_twice)) int setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int value);

#endif
