// Switches of every shape the compilers lower to jump tables (and some they do not): dense, offset, with case ranges, with fallthrough, in a loop
// (a state machine), nested, sparse (a compare tree), two clusters, and cases with long bodies (halfword entries); switch_word.c has the word
// tables. Each switch has at least 16 case values: GCC builds a table from 11 on (aarch64_case_values_threshold at -O2), clang from 13
// (aarch64-min-jump-table-entries). Every case does a different operation, so that no compiler turns a switch into a table of values; each
// switch is checked against a reference that does the same per case without a switch (a table of function pointers, arrays, if chains, which a
// compiler may turn into switches of its own), over inputs below, inside and above its cases.
#include "rt.h"
#include "switch.h"

volatile uint64_t sw_a = 0x123456789abcdef1ull, sw_b = 0x0fedcba987654321ull;

typedef uint64_t (*op_fn)(uint64_t, uint64_t);
static uint64_t f_add(uint64_t a, uint64_t b) { return a + b; }
static uint64_t f_sub(uint64_t a, uint64_t b) { return a - b; }
static uint64_t f_xor(uint64_t a, uint64_t b) { return a ^ b; }
static uint64_t f_and(uint64_t a, uint64_t b) { return a & b; }
static uint64_t f_or(uint64_t a, uint64_t b) { return a | b; }
static uint64_t f_mul(uint64_t a, uint64_t b) { return a * b; }
static uint64_t f_shl(uint64_t a, uint64_t b) { return a << (b & 31); }
static uint64_t f_shr(uint64_t a, uint64_t b) { return a >> (b & 31); }
static uint64_t f_rot(uint64_t a, uint64_t b) { return (a >> 7) | ((a << 57) ^ b); }
static uint64_t f_neg(uint64_t a, uint64_t b) { return -a + (b >> 3); }
static uint64_t f_not(uint64_t a, uint64_t b) { return ~a ^ (b << 2); }
static uint64_t f_bic(uint64_t a, uint64_t b) { return a & ~b; }
static uint64_t f_orn(uint64_t a, uint64_t b) { return a | ~b; }
static uint64_t f_div(uint64_t a, uint64_t b) { return a / ((b & 0xff) | 1); }
static uint64_t f_rem(uint64_t a, uint64_t b) { return a % ((b & 0xfff) | 1); }
static uint64_t f_max(uint64_t a, uint64_t b) { return a > b ? a : b; }
static uint64_t f_min(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint64_t f_add3(uint64_t a, uint64_t b) { return a + 3 * b; }
static uint64_t f_sub7(uint64_t a, uint64_t b) { return 7 * a - b; }
static uint64_t f_mix(uint64_t a, uint64_t b) { return (a ^ (a >> 13)) * (b | 1); }
static uint64_t f_swap(uint64_t a, uint64_t b) { return __builtin_bswap64(a) + b; }
static uint64_t f_pop(uint64_t a, uint64_t b) { return (uint64_t)__builtin_popcountll(a) + b; }
static uint64_t f_clz(uint64_t a, uint64_t b) { return (uint64_t)__builtin_clzll(a | 1) * b; }
static uint64_t f_none(uint64_t a, uint64_t b) { return a * 31 + b * 17 + 5; }
static const op_fn ops[] = {f_add, f_sub, f_xor, f_and, f_or, f_mul, f_shl, f_shr, f_rot, f_neg, f_not, f_bic,
                            f_orn, f_div, f_rem, f_max, f_min, f_add3, f_sub7, f_mix, f_swap, f_pop, f_clz};

// 1. Dense, small cases: byte entries
RT_NOINLINE uint64_t sw_dense(int x, uint64_t a, uint64_t b) {
    switch (x) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a ^ b;
    case 3: return a & b;
    case 4: return a | b;
    case 5: return a * b;
    case 6: return a << (b & 31);
    case 7: return a >> (b & 31);
    case 8: return (a >> 7) | ((a << 57) ^ b);
    case 9: return -a + (b >> 3);
    case 10: return ~a ^ (b << 2);
    case 11: return a & ~b;
    case 12: return a | ~b;
    case 13: return a / ((b & 0xff) | 1);
    case 14: return a % ((b & 0xfff) | 1);
    case 15: return a > b ? a : b;
    case 16: return a < b ? a : b;
    case 17: return a + 3 * b;
    case 18: return 7 * a - b;
    case 19: return (a ^ (a >> 13)) * (b | 1);
    case 20: return __builtin_bswap64(a) + b;
    case 21: return (uint64_t)__builtin_popcountll(a) + b;
    case 22: return (uint64_t)__builtin_clzll(a | 1) * b;
    default: return a * 31 + b * 17 + 5;
    }
}
static uint64_t ref_dense(int x, uint64_t a, uint64_t b) {
    return x >= 0 && x < 23 ? ops[x](a, b) : f_none(a, b);
}

// 2. Cases from 100, one missing: the index is x - 100
RT_NOINLINE uint64_t sw_offset(int x, uint64_t a, uint64_t b) {
    switch (x) {
    case 100: return a + b;
    case 101: return a - b;
    case 102: return a ^ b;
    case 103: return a & b;
    case 104: return a | b;
    case 105: return a * b;
    case 106: return a << (b & 31);
    case 107: return a >> (b & 31);
    case 108: return (a >> 7) | ((a << 57) ^ b);
    case 109: return -a + (b >> 3);
    case 110: return ~a ^ (b << 2);
    case 111: return a & ~b;
    case 113: return a / ((b & 0xff) | 1);
    case 114: return a % ((b & 0xfff) | 1);
    case 115: return a > b ? a : b;
    case 116: return a < b ? a : b;
    case 117: return a + 3 * b;
    default: return a * 31 + b * 17 + 5;
    }
}
static uint64_t ref_offset(int x, uint64_t a, uint64_t b) {
    return x >= 100 && x <= 117 && x != 112 ? ops[x - 100](a, b) : f_none(a, b);
}

// 3. Case ranges of characters (a GNU extension both compilers take): many values, few targets
RT_NOINLINE uint64_t sw_chars(unsigned char c, uint64_t a, uint64_t b) {
    switch (c) {
    case 'a' ... 'f': return a + c;
    case 'g' ... 'm': return a ^ (b + c);
    case 'n': return a * c;
    case 'o' ... 'r': return b - c;
    case 's': case 'u': case 'w': return a << (c & 15);
    case 't': case 'v': return a | b | c;
    case 'x' ... 'z': return a & (b ^ c);
    case '0' ... '9': return (a >> (c - '0')) + b;
    case '_': return ~b;
    default: return 3 * a + c;
    }
}
static uint64_t ref_chars(unsigned char c, uint64_t a, uint64_t b) {
    if (c >= 'a' && c <= 'f') return a + c;
    if (c >= 'g' && c <= 'm') return a ^ (b + c);
    if (c == 'n') return a * c;
    if (c >= 'o' && c <= 'r') return b - c;
    if (c == 's' || c == 'u' || c == 'w') return a << (c & 15);
    if (c == 't' || c == 'v') return a | b | c;
    if (c >= 'x' && c <= 'z') return a & (b ^ c);
    if (c >= '0' && c <= '9') return (a >> (c - '0')) + b;
    if (c == '_') return ~b;
    return 3 * a + c;
}

// 4. Fallthrough: case k runs the bodies of k, k+1, ... up to a break
RT_NOINLINE uint64_t sw_fall(unsigned x, uint64_t a, uint64_t b) {
    uint64_t r = b;
    switch (x) {
    case 0: r += a;
    /* fallthrough */
    case 1: r ^= a << 1;
    /* fallthrough */
    case 2: r *= 3;
    break;
    case 3: r -= a;
    /* fallthrough */
    case 4: r = (r >> 5) | (r << 59);
    /* fallthrough */
    case 5: r += 0x55;
    /* fallthrough */
    case 6: r ^= a;
    break;
    case 7: r = ~r;
    /* fallthrough */
    case 8: r += a * 7;
    /* fallthrough */
    case 9: r -= 1;
    /* fallthrough */
    case 10: r *= a | 1;
    break;
    case 11: r += 11;
    /* fallthrough */
    case 12: r ^= 0x1200;
    /* fallthrough */
    case 13: r = r * 13 + a;
    break;
    case 14: r >>= 14;
    /* fallthrough */
    case 15: r |= a & 0xff00;
    break;
    default: r = a + b + x;
    }
    return r;
}
static uint64_t ref_fall(unsigned x, uint64_t a, uint64_t b) {
    uint64_t r = b;
    if (x > 15) return a + b + x;
    if (x <= 2) {
        if (x == 0) r += a;
        if (x <= 1) r ^= a << 1;
        r *= 3;
        return r;
    }
    if (x <= 6) {
        if (x == 3) r -= a;
        if (x <= 4) r = (r >> 5) | (r << 59);
        if (x <= 5) r += 0x55;
        r ^= a;
        return r;
    }
    if (x <= 10) {
        if (x == 7) r = ~r;
        if (x <= 8) r += a * 7;
        if (x <= 9) r -= 1;
        r *= a | 1;
        return r;
    }
    if (x <= 13) {
        if (x == 11) r += 11;
        if (x <= 12) r ^= 0x1200;
        r = r * 13 + a;
        return r;
    }
    if (x == 14) r >>= 14;
    r |= a & 0xff00;
    return r;
}

// 5. A state machine: a switch in a loop, the table's targets branch back to its head
RT_NOINLINE uint64_t sw_machine(const unsigned char *prog, unsigned n, uint64_t a) {
    uint64_t acc = a, stack = 0;
    unsigned pc = 0, steps = 0;
    while (pc < n && steps++ < 1000) {
        switch (prog[pc]) {
        case 0: acc += 1; pc += 1; break;
        case 1: acc *= 3; pc += 1; break;
        case 2: acc ^= acc >> 7; pc += 1; break;
        case 3: stack = acc; pc += 1; break;
        case 4: acc += stack; pc += 1; break;
        case 5: pc = (acc & 1) ? pc + 2 : pc + 1; break;
        case 6: if (acc % 5) { pc = prog[pc + 1]; } else { pc += 2; } break;
        case 7: acc = (acc << 3) | (acc >> 61); pc += 1; break;
        case 8: acc -= 0x1234; pc += 1; break;
        case 9: return acc ^ stack;
        case 10: acc = acc * acc; pc += 1; break;
        case 11: stack ^= acc; pc += 1; break;
        case 12: acc |= 0x8000; pc += 1; break;
        case 13: acc &= ~0xffull; pc += 1; break;
        case 14: acc += pc; pc += 1; break;
        case 15: stack = stack * 7 + 1; pc += 1; break;
        default: acc = ~acc; pc += 1; break;
        }
    }
    return acc + steps;
}
static uint64_t ref_machine(const unsigned char *prog, unsigned n, uint64_t a) {
    uint64_t acc = a, stack = 0;
    unsigned pc = 0, steps = 0;
    while (pc < n && steps++ < 1000) {
        const unsigned op = prog[pc];
        if (op == 0) { acc += 1; pc += 1; }
        else if (op == 1) { acc *= 3; pc += 1; }
        else if (op == 2) { acc ^= acc >> 7; pc += 1; }
        else if (op == 3) { stack = acc; pc += 1; }
        else if (op == 4) { acc += stack; pc += 1; }
        else if (op == 5) { pc = (acc & 1) ? pc + 2 : pc + 1; }
        else if (op == 6) { if (acc % 5) pc = prog[pc + 1]; else pc += 2; }
        else if (op == 7) { acc = (acc << 3) | (acc >> 61); pc += 1; }
        else if (op == 8) { acc -= 0x1234; pc += 1; }
        else if (op == 9) return acc ^ stack;
        else if (op == 10) { acc = acc * acc; pc += 1; }
        else if (op == 11) { stack ^= acc; pc += 1; }
        else if (op == 12) { acc |= 0x8000; pc += 1; }
        else if (op == 13) { acc &= ~0xffull; pc += 1; }
        else if (op == 14) { acc += pc; pc += 1; }
        else if (op == 15) { stack = stack * 7 + 1; pc += 1; }
        else { acc = ~acc; pc += 1; }
    }
    return acc + steps;
}

// 6. Nested: a switch inside a case of another
RT_NOINLINE uint64_t sw_nested(unsigned x, unsigned y, uint64_t a, uint64_t b) {
    switch (x) {
    case 0: return a + y;
    case 1:
        switch (y) {
        case 0: return a * 5;
        case 1: return b * 7;
        case 2: return a ^ b;
        case 3: return a - b;
        case 4: return a >> 3;
        case 5: return b << 4;
        case 6: return a | 0xf0;
        case 7: return b & 0xffff;
        case 8: return a + 0x88;
        case 9: return b - 0x99;
        case 10: return a * b + 10;
        case 11: return (a ^ 0xbb) << 1;
        case 12: return b % 12;
        case 13: return a / 13;
        case 14: return ~b;
        case 15: return a & ~0xfull;
        default: return a + b + y;
        }
    case 2: return b - y;
    case 3: return a * b * y;
    case 4: return (a | b) + y;
    case 5: return (a & b) ^ y;
    case 6: return ~(a + y);
    case 7: return a % (y + 1);
    case 8: return b / (y + 1);
    case 9: return (a << (y & 7)) + b;
    case 10: return (b >> (y & 7)) ^ a;
    case 11: return a - b * y;
    case 12: return (a + b) * 12;
    case 13: return a ^ (b << 13);
    case 14: return (a | y) & b;
    case 15: return a + b + 15;
    default: return x + y + a;
    }
}
static uint64_t ref_nested(unsigned x, unsigned y, uint64_t a, uint64_t b) {
    if (x == 1) {
        const uint64_t inner[16] = {a * 5, b * 7, a ^ b, a - b, a >> 3, b << 4, a | 0xf0, b & 0xffff,
                                    a + 0x88, b - 0x99, a * b + 10, (a ^ 0xbb) << 1, b % 12, a / 13, ~b, a & ~0xfull};
        return y < 16 ? inner[y] : a + b + y;
    }
    if (x < 16) {
        const uint64_t outer[16] = {a + y, 0, b - y, a * b * y, (a | b) + y, (a & b) ^ y, ~(a + y), a % (y + 1),
                                    b / (y + 1), (a << (y & 7)) + b, (b >> (y & 7)) ^ a, a - b * y, (a + b) * 12, a ^ (b << 13),
                                    (a | y) & b, a + b + 15};
        return outer[x];
    }
    return x + y + a;
}

// 7. Sparse cases: a compare tree, no table
RT_NOINLINE uint64_t sw_sparse(int x, uint64_t a, uint64_t b) {
    switch (x) {
    case -3: return a + b;
    case 1: return a - b;
    case 7: return a ^ b;
    case 30: return a * b;
    case 200: return a | b;
    case 1000: return a & b;
    case 50000: return a << 3;
    case 1 << 20: return b >> 3;
    default: return a * 31 + b * 17 + 5;
    }
}
static uint64_t ref_sparse(int x, uint64_t a, uint64_t b) {
    if (x == -3) return a + b;
    if (x == 1) return a - b;
    if (x == 7) return a ^ b;
    if (x == 30) return a * b;
    if (x == 200) return a | b;
    if (x == 1000) return a & b;
    if (x == 50000) return a << 3;
    if (x == 1 << 20) return b >> 3;
    return a * 31 + b * 17 + 5;
}

// 8. Two clusters, far apart: two tables, or a table and a compare
RT_NOINLINE uint64_t sw_clusters(int x, uint64_t a, uint64_t b) {
    switch (x) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a ^ b;
    case 3: return a & b;
    case 4: return a | b;
    case 5: return a * b;
    case 6: return a << 2;
    case 7: return a >> 2;
    case 8: return a + 8;
    case 9: return b + 9;
    case 10: return a * 10 + b;
    case 11: return b * 11 - a;
    case 12: return (a ^ b) + 12;
    case 13: return (a | b) - 13;
    case 14: return (a & b) * 14;
    case 15: return a + b * 15;
    case 1000: return a * 3;
    case 1001: return b * 5;
    case 1002: return a - 1;
    case 1003: return b - 2;
    case 1004: return a ^ 0xff;
    case 1005: return b ^ 0xf0f0;
    case 1006: return a + b + 1;
    case 1007: return a - b - 1;
    case 1008: return (a >> 1) + b;
    case 1009: return (b >> 1) + a;
    case 1010: return a * b * 3;
    case 1011: return (a << 11) ^ b;
    case 1012: return (b << 12) ^ a;
    case 1013: return a % 1013;
    case 1014: return b / 1014;
    case 1015: return ~(a ^ b);
    default: return a * 31 + b * 17 + 5;
    }
}
static uint64_t ref_clusters(int x, uint64_t a, uint64_t b) {
    if (x >= 0 && x <= 15) {
        const uint64_t r[16] = {a + b, a - b, a ^ b, a & b, a | b, a * b, a << 2, a >> 2, a + 8, b + 9,
                                a * 10 + b, b * 11 - a, (a ^ b) + 12, (a | b) - 13, (a & b) * 14, a + b * 15};
        return r[x];
    }
    if (x >= 1000 && x <= 1015) {
        const uint64_t r[16] = {a * 3, b * 5, a - 1, b - 2, a ^ 0xff, b ^ 0xf0f0, a + b + 1, a - b - 1, (a >> 1) + b, (b >> 1) + a,
                                a * b * 3, (a << 11) ^ b, (b << 12) ^ a, a % 1013, b / 1014, ~(a ^ b)};
        return r[x - 1000];
    }
    return a * 31 + b * 17 + 5;
}

// 9. Long case bodies: the distances need halfword entries (GCC past 0x1f0 bytes from its base label, clang past 255 words). Each case repeats
// its own operation 64 times on a volatile, so that no compiler merges the cases into one body with a table of constants
static volatile uint64_t sw_sink;
#define SW_R4(s) s s s s
#define SW_LONG(s) SW_R4(SW_R4(SW_R4(s)))
#define L0 sw_sink = sw_sink * 3 + a;
#define L1 sw_sink = (sw_sink ^ a) + 5;
#define L2 sw_sink = (sw_sink << 1) ^ a;
#define L3 sw_sink = sw_sink - (a >> 3);
#define L4 sw_sink = (sw_sink >> 1) | (sw_sink << 63);
#define L5 sw_sink = sw_sink + (sw_sink >> 7) + 1;
#define L6 sw_sink = ~sw_sink + a;
#define L7 sw_sink = sw_sink * a + 7;
#define L8 sw_sink = (sw_sink & a) + 0x88;
#define L9 sw_sink = (sw_sink | 9) - a;
#define L10 sw_sink = sw_sink ^ (sw_sink << 10);
#define L11 sw_sink = sw_sink + a * 11;
#define L12 sw_sink = (sw_sink >> 12) + a;
#define L13 sw_sink = sw_sink * 13 - a;
#define L14 sw_sink = (sw_sink ^ 0xe) * 3;
#define L15 sw_sink = sw_sink - (sw_sink >> 15) + a;
static uint64_t step(unsigned k, uint64_t s, uint64_t a) {
    if (k == 0) return s * 3 + a;
    if (k == 1) return (s ^ a) + 5;
    if (k == 2) return (s << 1) ^ a;
    if (k == 3) return s - (a >> 3);
    if (k == 4) return (s >> 1) | (s << 63);
    if (k == 5) return s + (s >> 7) + 1;
    if (k == 6) return ~s + a;
    if (k == 7) return s * a + 7;
    if (k == 8) return (s & a) + 0x88;
    if (k == 9) return (s | 9) - a;
    if (k == 10) return s ^ (s << 10);
    if (k == 11) return s + a * 11;
    if (k == 12) return (s >> 12) + a;
    if (k == 13) return s * 13 - a;
    if (k == 14) return (s ^ 0xe) * 3;
    return s - (s >> 15) + a;
}
RT_NOINLINE uint64_t sw_long(unsigned x, uint64_t a) {
    sw_sink = 1;
    switch (x) {
    case 0: SW_LONG(L0) break;
    case 1: SW_LONG(L1) break;
    case 2: SW_LONG(L2) break;
    case 3: SW_LONG(L3) break;
    case 4: SW_LONG(L4) break;
    case 5: SW_LONG(L5) break;
    case 6: SW_LONG(L6) break;
    case 7: SW_LONG(L7) break;
    case 8: SW_LONG(L8) break;
    case 9: SW_LONG(L9) break;
    case 10: SW_LONG(L10) break;
    case 11: SW_LONG(L11) break;
    case 12: SW_LONG(L12) break;
    case 13: SW_LONG(L13) break;
    case 14: SW_LONG(L14) break;
    case 15: SW_LONG(L15) break;
    default: sw_sink = a; break;
    }
    return sw_sink;
}
static uint64_t ref_long(unsigned x, uint64_t a) {
    if (x >= 16) {
        return a;
    }
    uint64_t s = 1;
    for (int i = 0; i < 64; ++i) {
        s = step(x, s, a);
    }
    return s;
}

static const unsigned char machine_prog[] = {0, 1, 2, 3, 7, 4, 5, 8, 1, 6, 2, 0, 1, 7, 6, 13, 2, 10, 4, 3, 11, 12, 14, 15, 13, 1, 9, 0};

int main(void) {
    const uint64_t a = sw_a, b = sw_b;
    unsigned bad[9] = {0};
    uint64_t h = RT_MIX_INIT;
    for (int x = -8; x < 140; ++x) {
        const int xc = x + (x >= 70 ? 925 : 0);
        const uint64_t r1 = sw_dense(x, a, b), r2 = sw_offset(x, a, b), r7 = sw_sparse(x, a, b), r8 = sw_clusters(xc, a, b);
        bad[0] += r1 != ref_dense(x, a, b);
        bad[1] += r2 != ref_offset(x, a, b);
        bad[6] += r7 != ref_sparse(x, a, b);
        bad[7] += r8 != ref_clusters(xc, a, b);
        h = rt_mix(rt_mix(rt_mix(rt_mix(h, r1), r2), r7), r8);
        for (unsigned y = 0; y < 20; ++y) {
            bad[5] += sw_nested((unsigned)x & 31, y, a, b) != ref_nested((unsigned)x & 31, y, a, b);
        }
    }
    static const int probes[] = {-3, 1, 7, 30, 200, 1000, 50000, 1 << 20, 1001, 1015, 999, 1016};
    for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
        const int x = probes[i];
        bad[6] += sw_sparse(x, a, b) != ref_sparse(x, a, b);
        bad[7] += sw_clusters(x, a, b) != ref_clusters(x, a, b);
        h = rt_mix(h, sw_sparse(x, a, b) ^ sw_clusters(x, a, b));
    }
    for (unsigned c = 0; c < 256; ++c) {
        bad[2] += sw_chars((unsigned char)c, a, b) != ref_chars((unsigned char)c, a, b);
        h = rt_mix(h, sw_chars((unsigned char)c, a, b));
    }
    for (unsigned x = 0; x < 20; ++x) {
        bad[3] += sw_fall(x, a, b) != ref_fall(x, a, b);
        bad[8] += sw_long(x, a) != ref_long(x, a);
        h = rt_mix(rt_mix(h, sw_fall(x, a, b)), sw_long(x, a));
    }
    for (unsigned start = 0; start < 8; ++start) {
        bad[4] += sw_machine(machine_prog, sizeof machine_prog, a + start) != ref_machine(machine_prog, sizeof machine_prog, a + start);
        h = rt_mix(h, sw_machine(machine_prog, sizeof machine_prog, a + start));
    }
    RT_CHECK32("switch.dense", bad[0], 0);
    RT_CHECK32("switch.offset", bad[1], 0);
    RT_CHECK32("switch.chars", bad[2], 0);
    RT_CHECK32("switch.fallthrough", bad[3], 0);
    RT_CHECK32("switch.machine", bad[4], 0);
    RT_CHECK32("switch.nested", bad[5], 0);
    RT_CHECK32("switch.sparse", bad[6], 0);
    RT_CHECK32("switch.clusters", bad[7], 0);
    RT_CHECK32("switch.long", bad[8], 0);
    rt_note("switch.digest", h, 16);
    switch_word_checks(a, b);
    return rt_finish();
}
