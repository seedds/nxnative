// Word jump tables, the third entry size: GCC picks word entries when a case lies more than about 128 KB from the table's base label, so one case
// here runs a long straight line of volatile updates (GCC build only); clang's table-relative word tables come from building this file without its
// compressed tables (build_corpus.sh), so its build keeps the case short.
#include "rt.h"
#include "switch.h"

static volatile uint64_t sw_word_sink;

#define WR10(s) s s s s s s s s s s
#define WORD_STEP sw_word_sink = sw_word_sink * 3 + a;
#ifdef __clang__
#define WORD_BODY WR10(WR10(WORD_STEP))
#define WORD_STEPS 100
#else
#define WORD_BODY WR10(WR10(WR10(WR10(WORD_STEP)))) WR10(WR10(WR10(WORD_STEP)))
#define WORD_STEPS 11000
#endif

RT_NOINLINE uint64_t sw_word(unsigned x, uint64_t a, uint64_t b) {
    sw_word_sink = b;
    switch (x) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: WORD_BODY return sw_word_sink;
    case 3: return a ^ b;
    case 4: return a * b;
    case 5: return a | (b << 3);
    case 6: return a & (b >> 3);
    case 7: return ~a + b;
    case 8: return a + 2 * b;
    case 9: return 3 * a - b;
    case 10: return a % ((b & 0xff) | 1);
    case 11: return b / ((a & 0xff) | 1);
    case 12: return (a << 12) + b;
    case 13: return (b >> 13) ^ a;
    case 14: return a * 14 + b * 41;
    case 15: return (a | 0xf) & (b | 0xf0);
    default: return a * 31 + b * 17 + 5;
    }
}

static uint64_t ref_word(unsigned x, uint64_t a, uint64_t b) {
    if (x == 2) {
        uint64_t s = b;
        for (int i = 0; i < WORD_STEPS; ++i) {
            s = s * 3 + a;
        }
        return s;
    }
    if (x >= 16) {
        return a * 31 + b * 17 + 5;
    }
    const uint64_t r[16] = {a + b, a - b, 0, a ^ b, a * b, a | (b << 3), a & (b >> 3), ~a + b, a + 2 * b, 3 * a - b,
                            a % ((b & 0xff) | 1), b / ((a & 0xff) | 1), (a << 12) + b, (b >> 13) ^ a, a * 14 + b * 41, (a | 0xf) & (b | 0xf0)};
    return r[x];
}

void switch_word_checks(uint64_t a, uint64_t b) {
    unsigned bad = 0;
    uint64_t h = RT_MIX_INIT;
    for (unsigned x = 0; x < 20; ++x) {
        const uint64_t r = sw_word(x, a, b);
        bad += r != ref_word(x, a, b);
        h = rt_mix(h, x == 2 ? 0 : r);  // case 2 runs WORD_STEPS updates, which differ between the builds
    }
    RT_CHECK32("switch.word", bad, 0);
    rt_note("switch.word.digest", h, 16);
}
