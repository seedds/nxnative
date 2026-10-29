// The corpus's run time (rt.h): output lines, checks, self-relocation, the TLS block, DT_INIT / DT_FINI and the init and fini arrays, exit.
#include "rt.h"

// ---- Output ----

static char rt_line[512];
static unsigned rt_len;
static unsigned rt_checks, rt_failed;

static void rt_put(char c) {
    if (rt_len < sizeof rt_line) {
        rt_line[rt_len++] = c;
    }
}

void rt_str(const char *s) {
    while (*s) {
        rt_put(*s++);
    }
}

void rt_begin(const char *name) {
    rt_len = 0;
    rt_str(name);
}

void rt_hex(uint64_t v, int digits) {
    static const char hex[] = "0123456789abcdef";
    rt_put(' ');
    for (int i = digits - 1; i >= 0; --i) {
        rt_put(hex[(v >> (4 * i)) & 15]);
    }
}

void rt_dec(int64_t v) {
    char buf[24];
    int n = 0;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do {
        buf[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    rt_put(' ');
    if (v < 0) {
        rt_put('-');
    }
    while (n) {
        rt_put(buf[--n]);
    }
}

void rt_end(void) {
    rt_svc_output_debug_string(rt_line, rt_len);
    rt_len = 0;
}

void rt_say(const char *line) {
    rt_begin(line);
    rt_end();
}

void rt_check(const char *name, uint64_t got, uint64_t want, int digits) {
    ++rt_checks;
    rt_begin(name);
    rt_hex(got, digits);
    if (got == want) {
        rt_str(" ok");
    } else {
        ++rt_failed;
        rt_str(" want");
        rt_hex(want, digits);
        rt_str(" FAIL");
    }
    rt_end();
}

void rt_check128(const char *name, uint64_t got_lo, uint64_t got_hi, uint64_t want_lo, uint64_t want_hi) {
    ++rt_checks;
    rt_begin(name);
    rt_hex(got_hi, 16);
    rt_hex(got_lo, 16);
    if (got_lo == want_lo && got_hi == want_hi) {
        rt_str(" ok");
    } else {
        ++rt_failed;
        rt_str(" want");
        rt_hex(want_hi, 16);
        rt_hex(want_lo, 16);
        rt_str(" FAIL");
    }
    rt_end();
}

void rt_note(const char *name, uint64_t value, int digits) {
    rt_begin(name);
    rt_hex(value, digits);
    rt_end();
}

int rt_finish(void) {
    if (rt_failed) {
        rt_begin("FAIL");
        rt_dec(rt_failed);
        rt_str(" of");
        rt_dec(rt_checks);
    } else {
        rt_begin("PASS");
        rt_dec(rt_checks);
    }
    rt_end();
    return rt_failed ? 1 : 0;
}

// ---- Self-relocation ----

typedef struct {
    int64_t tag;
    uint64_t val;
} rt_dyn;
typedef struct {
    uint64_t offset, info;
    int64_t addend;
} rt_rela;
typedef struct {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} rt_sym;

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23, DT_RELRSZ = 35, DT_RELR = 36 };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };

extern char _start[];
// A pointer the linker relocates: it holds _start's address once the module is relocated, 0 before (the module is linked at 0). A loader that
// relocated the module already (the host's loader.c relocates every module it maps) leaves nothing for rt_relocate to do
static char *volatile rt_self = _start;

static void rt_apply_rela(uintptr_t base, const rt_rela *r, uint64_t bytes, const rt_sym *symtab) {
    for (uint64_t i = 0; i < bytes / sizeof *r; ++i) {
        uint64_t *where = (uint64_t *)(base + r[i].offset);
        const uint32_t type = (uint32_t)r[i].info, sym = (uint32_t)(r[i].info >> 32);
        if (type == R_AARCH64_RELATIVE) {
            *where = base + (uint64_t)r[i].addend;
        } else if (type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT) {
            const rt_sym *s = &symtab[sym];
            const uint64_t value = s->shndx ? base + s->value : 0;  // the corpus imports nothing: an undefined (weak) symbol is 0
            *where = value + (type == R_AARCH64_JUMP_SLOT ? 0 : (uint64_t)r[i].addend);
        } else {
            rt_say("[RED] rt_relocate: unknown relocation type");
            rt_svc_exit_process();
        }
    }
}

void rt_relocate(uintptr_t base, const rt_dyn *dyn) {
    if ((uintptr_t)rt_self == base) {
        return;
    }
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0, symtab = 0;
    for (; dyn->tag != DT_NULL; ++dyn) {
        switch (dyn->tag) {
        case DT_SYMTAB: symtab = dyn->val; break;
        case DT_RELA: rela = dyn->val; break;
        case DT_RELASZ: relasz = dyn->val; break;
        case DT_JMPREL: jmprel = dyn->val; break;
        case DT_PLTRELSZ: pltsz = dyn->val; break;
        case DT_RELR: relr = dyn->val; break;
        case DT_RELRSZ: relrsz = dyn->val; break;
        default: break;
        }
    }
    const rt_sym *syms = (const rt_sym *)(base + symtab);
    if (relasz) {
        rt_apply_rela(base, (const rt_rela *)(base + rela), relasz, syms);
    }
    if (pltsz) {
        rt_apply_rela(base, (const rt_rela *)(base + jmprel), pltsz, syms);
    }
    // DT_RELR, as dm/host/loader.c relr_apply reads it: an even entry is an offset to relocate, an odd one a bitmap of the 63 slots that follow
    const uint64_t *e = (const uint64_t *)(base + relr);
    uint64_t *where = 0;
    for (uint64_t i = 0; i < relrsz / 8; ++i) {
        uint64_t v = e[i];
        if (!(v & 1)) {
            where = (uint64_t *)(base + v);
            *where++ += base;
            continue;
        }
        for (unsigned b = 0; (v >>= 1); ++b) {
            if (v & 1) {
                where[b] += base;
            }
        }
        where += 63;
    }
}

// ---- TLS: the main thread's block for the local-exec model (tp, a 16-byte TCB, then the TLS segment). The corpus has zero-initialised TLS only
// (nro.ld asserts no .tdata and at most 240 bytes of .tbss), so the block in bss is ready as it is

static uint8_t rt_tls_block[256] __attribute__((aligned(16)));

uintptr_t rt_tls_init(void) {
    return (uintptr_t)rt_tls_block;
}

// ---- DT_INIT, DT_FINI, the init and fini arrays ----

typedef void (*rt_fn)(void);
extern const rt_fn __init_array_start[], __init_array_end[], __fini_array_start[], __fini_array_end[];
volatile unsigned rt_init_ran, rt_fini_ran;

// DT_INIT and DT_FINI: the linkers point them at _init and _fini (GNU ld and ld.lld default to these names)
void _init(void) {
    rt_init_ran |= 1;
}

void _fini(void) {
    rt_fini_ran |= 1;
}

void rt_init(void) {
    _init();
    for (const rt_fn *f = __init_array_start; f < __init_array_end; ++f) {
        (*f)();
    }
}

__attribute__((noreturn)) void rt_exit(void) {
    for (const rt_fn *f = __fini_array_end; f > __fini_array_start;) {
        (*--f)();
    }
    _fini();
    rt_svc_exit_process();
}

// ---- Unwinding: the corpus's cleanup landing pads are never entered (nothing throws), but -fexceptions code names these ----

struct _Unwind_Exception;
__attribute__((noreturn)) void _Unwind_Resume(struct _Unwind_Exception *e) {
    (void)e;
    rt_say("[RED] _Unwind_Resume: the corpus never unwinds");
    rt_svc_exit_process();
}

int __gcc_personality_v0(int version, int actions, uint64_t exception_class, void *exception, void *context) {
    (void)version;
    (void)actions;
    (void)exception_class;
    (void)exception;
    (void)context;
    rt_say("[RED] __gcc_personality_v0: the corpus never unwinds");
    rt_svc_exit_process();
}
