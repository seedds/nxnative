// The personality-free unwinder of rt/unwind.h. It reads this module's own unwind tables: .eh_frame_hdr's binary search table (the linker
// script's __eh_frame_hdr_start) to find a return address's FDE, the CIE and FDE (augmentations z, P, L, R), the call frame program up to the
// return address (the DW_CFA operations the compilers emit on AArch64: CFA definitions, register saves at CFA offsets, advances, restores,
// remember / restore state), and the LSDA's call-site table to find a landing pad. Registers: 0..30 x0..x30, 31 sp, 64..95 v0..v31 (d8..d15
// are the callee-saved ones). Anything it does not know ends the program with a [RED] line.
#include "unwind.h"

typedef struct {
    uint64_t x[31], sp, pc, d[8];  // d8..d15
} rt_uw_context;

__attribute__((noreturn)) void rt_uw_install(rt_uw_context *c, uint64_t pad, void *exception);

extern const uint8_t __eh_frame_hdr_start[];

volatile uint64_t rt_unwind_frames, rt_unwind_pads;
static rt_catcher *rt_catch_top;
static struct {
    uint64_t value;
} rt_exception;

__attribute__((noreturn)) static void rt_uw_fail(const char *why) {
    rt_begin("[RED] rt_unwind: ");
    rt_str(why);
    rt_end();
    rt_svc_exit_process();
}

// ---- DWARF encodings ----

static uint64_t uleb(const uint8_t **p) {
    uint64_t v = 0;
    unsigned shift = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    return v;
}

static int64_t sleb(const uint8_t **p) {
    int64_t v = 0;
    unsigned shift = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) {
        v |= -((int64_t)1 << shift);
    }
    return v;
}

// A pointer in DW_EH_PE encoding enc at *p (pcrel against its own address, datarel against base); 0xff (omit) reads nothing
static uint64_t encoded(const uint8_t **p, uint8_t enc, uint64_t datarel) {
    if (enc == 0xff) {
        return 0;
    }
    const uint8_t *at = *p;
    uint64_t v;
    switch (enc & 0x0f) {
    case 0x00: memcpy(&v, at, 8); *p += 8; break;
    case 0x01: v = uleb(p); break;
    case 0x02: { uint16_t t; memcpy(&t, at, 2); v = t; *p += 2; break; }
    case 0x03: { uint32_t t; memcpy(&t, at, 4); v = t; *p += 4; break; }
    case 0x04: memcpy(&v, at, 8); *p += 8; break;
    case 0x09: v = (uint64_t)sleb(p); break;
    case 0x0a: { int16_t t; memcpy(&t, at, 2); v = (uint64_t)(int64_t)t; *p += 2; break; }
    case 0x0b: { int32_t t; memcpy(&t, at, 4); v = (uint64_t)(int64_t)t; *p += 4; break; }
    case 0x0c: memcpy(&v, at, 8); *p += 8; break;
    default: rt_uw_fail("a pointer encoding it does not know");
    }
    switch (enc & 0x70) {
    case 0x00: break;
    case 0x10: v += (uint64_t)(uintptr_t)at; break;
    case 0x30: v += datarel; break;
    default: rt_uw_fail("a pointer application it does not know");
    }
    if (enc & 0x80) {
        memcpy(&v, (const void *)(uintptr_t)v, 8);
    }
    return v;
}

// ---- FDEs ----

typedef struct {
    uint64_t start, end, lsda;
    const uint8_t *cie_program, *cie_end, *fde_program, *fde_end;
    uint64_t code_align;
    int64_t data_align;
    unsigned ra;
} rt_fde;

// The FDE whose range holds pc, through .eh_frame_hdr's sorted table of (initial location, FDE) pairs
static int find_fde(uint64_t pc, rt_fde *out) {
    const uint8_t *h = __eh_frame_hdr_start;
    const uint64_t hdr = (uint64_t)(uintptr_t)h;
    if (h[0] != 1) {
        rt_uw_fail(".eh_frame_hdr of an unknown version");
    }
    const uint8_t frame_enc = h[1], count_enc = h[2], table_enc = h[3];
    const uint8_t *p = h + 4;
    (void)encoded(&p, frame_enc, hdr);
    const uint64_t count = encoded(&p, count_enc, hdr);
    if (table_enc != 0x3b) {
        rt_uw_fail("an .eh_frame_hdr table that is not datarel sdata4");
    }
    const int32_t *table = (const int32_t *)p;
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
        const uint64_t mid = (lo + hi) / 2;
        if (hdr + (uint64_t)(int64_t)table[2 * mid] <= pc) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return 0;
    }
    const uint8_t *fde = (const uint8_t *)(uintptr_t)(hdr + (uint64_t)(int64_t)table[2 * (lo - 1) + 1]);
    uint32_t len;
    memcpy(&len, fde, 4);
    if (len == 0xffffffffu) {
        rt_uw_fail("a 64-bit FDE");
    }
    const uint8_t *fde_end = fde + 4 + len;
    int32_t cie_off;
    memcpy(&cie_off, fde + 4, 4);
    const uint8_t *cie = fde + 4 - cie_off;
    // The CIE: version, augmentation, alignments, return address register, augmentation data (P, L, R), initial instructions
    uint32_t cie_len;
    memcpy(&cie_len, cie, 4);
    const uint8_t *cie_end = cie + 4 + cie_len;
    const uint8_t *q = cie + 8;
    const uint8_t version = *q++;
    const char *aug = (const char *)q;
    while (*q) {
        ++q;
    }
    ++q;
    out->code_align = uleb(&q);
    out->data_align = sleb(&q);
    out->ra = version == 1 ? *q++ : (unsigned)uleb(&q);
    uint8_t fde_enc = 0, lsda_enc = 0xff;
    if (aug[0] == 'z') {
        const uint64_t n = uleb(&q);
        const uint8_t *data_end = q + n;
        for (const char *a = aug + 1; *a; ++a) {
            if (*a == 'R') {
                fde_enc = *q++;
            } else if (*a == 'L') {
                lsda_enc = *q++;
            } else if (*a == 'P') {
                const uint8_t penc = *q++;
                (void)encoded(&q, penc, 0);
            } else if (*a != 'S' && *a != 'B') {
                rt_uw_fail("a CIE augmentation it does not know");
            }
        }
        q = data_end;
    }
    out->cie_program = q;
    out->cie_end = cie_end;
    // The FDE: its range, its augmentation data (the LSDA), its instructions
    const uint8_t *r = fde + 8;
    out->start = encoded(&r, fde_enc, 0);
    out->end = out->start + encoded(&r, fde_enc & 0x0f, 0);
    out->lsda = 0;
    if (aug[0] == 'z') {
        const uint64_t n = uleb(&r);
        const uint8_t *data_end = r + n;
        if (lsda_enc != 0xff && n) {
            out->lsda = encoded(&r, lsda_enc, 0);
        }
        r = data_end;
    }
    out->fde_program = r;
    out->fde_end = fde_end;
    return pc >= out->start && pc < out->end;
}

// ---- The call frame program ----

typedef struct {
    unsigned cfa_reg;
    int64_t cfa_off;
    uint8_t saved[96];      // 1: saved at CFA + off
    int64_t off[96];
} rt_rules;

static void run_program(const uint8_t *p, const uint8_t *end, const rt_fde *f, uint64_t pc, uint64_t *loc, rt_rules *r,
                        const rt_rules *initial) {
    rt_rules stack[4];
    unsigned depth = 0;
    while (p < end && *loc <= pc) {
        const uint8_t op = *p++;
        const uint8_t hi = op & 0xc0, low = op & 0x3f;
        if (hi == 0x40) {  // DW_CFA_advance_loc
            *loc += low * f->code_align;
        } else if (hi == 0x80) {  // DW_CFA_offset
            r->saved[low] = 1;
            r->off[low] = (int64_t)uleb(&p) * f->data_align;
        } else if (hi == 0xc0) {  // DW_CFA_restore
            r->saved[low] = initial ? initial->saved[low] : 0;
            r->off[low] = initial ? initial->off[low] : 0;
        } else {
            switch (op) {
            case 0x00: break;  // nop
            case 0x02: *loc += *p++ * f->code_align; break;
            case 0x03: { uint16_t d; memcpy(&d, p, 2); p += 2; *loc += d * f->code_align; break; }
            case 0x04: { uint32_t d; memcpy(&d, p, 4); p += 4; *loc += d * f->code_align; break; }
            case 0x05: { const uint64_t reg = uleb(&p); const int64_t o = (int64_t)uleb(&p) * f->data_align; if (reg < 96) { r->saved[reg] = 1; r->off[reg] = o; } break; }
            case 0x06: { const uint64_t reg = uleb(&p); if (reg < 96) { r->saved[reg] = initial ? initial->saved[reg] : 0; r->off[reg] = initial ? initial->off[reg] : 0; } break; }
            case 0x07:
            case 0x08: { const uint64_t reg = uleb(&p); if (reg < 96) { r->saved[reg] = 0; } break; }  // undefined, same value
            case 0x0a:
                if (depth == 4) {
                    rt_uw_fail("DW_CFA_remember_state nested too deep");
                }
                stack[depth++] = *r;
                break;
            case 0x0b:
                if (!depth) {
                    rt_uw_fail("DW_CFA_restore_state without a remembered state");
                }
                *r = stack[--depth];
                break;
            case 0x0c: r->cfa_reg = (unsigned)uleb(&p); r->cfa_off = (int64_t)uleb(&p); break;
            case 0x0d: r->cfa_reg = (unsigned)uleb(&p); break;
            case 0x0e: r->cfa_off = (int64_t)uleb(&p); break;
            case 0x11: { const uint64_t reg = uleb(&p); const int64_t o = sleb(&p) * f->data_align; if (reg < 96) { r->saved[reg] = 1; r->off[reg] = o; } break; }
            case 0x12: r->cfa_reg = (unsigned)uleb(&p); r->cfa_off = sleb(&p) * f->data_align; break;
            case 0x13: r->cfa_off = sleb(&p) * f->data_align; break;
            case 0x2d: break;  // DW_CFA_AARCH64_negate_ra_state: no pointer authentication on the Cortex-A57
            case 0x2e: (void)uleb(&p); break;  // DW_CFA_GNU_args_size
            default: rt_uw_fail("a call frame instruction it does not know");
            }
        }
    }
}

// One frame up, by the FDE that holds pc - 1 (pc is a return address): the rules there, the CFA, the saved registers
static void step(rt_uw_context *c, const rt_fde *f) {
    rt_rules initial = {.cfa_reg = 31, .cfa_off = 0};
    for (unsigned i = 0; i < 96; ++i) {
        initial.saved[i] = 0;
        initial.off[i] = 0;
    }
    uint64_t loc = f->start;
    run_program(f->cie_program, f->cie_end, f, ~0ull, &loc, &initial, 0);
    rt_rules r = initial;
    loc = f->start;
    run_program(f->fde_program, f->fde_end, f, c->pc - 1, &loc, &r, &initial);
    const uint64_t base = r.cfa_reg == 31 ? c->sp : r.cfa_reg < 31 ? c->x[r.cfa_reg] : 0;
    if (r.cfa_reg > 31) {
        rt_uw_fail("a CFA on a register it does not know");
    }
    const uint64_t cfa = base + (uint64_t)r.cfa_off;
    uint64_t ra = c->x[30];
    for (unsigned i = 0; i < 31; ++i) {
        if (r.saved[i]) {
            uint64_t v;
            memcpy(&v, (const void *)(uintptr_t)(cfa + (uint64_t)r.off[i]), 8);
            c->x[i] = v;
        }
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (r.saved[72 + i]) {
            memcpy(&c->d[i], (const void *)(uintptr_t)(cfa + (uint64_t)r.off[72 + i]), 8);
        }
    }
    if (r.saved[f->ra]) {
        ra = c->x[f->ra];
    }
    c->sp = cfa;
    c->pc = ra;
    ++rt_unwind_frames;
}

// The landing pad of the call site that holds pc - 1, from the function's LSDA (0: none, or a call site without one)
static uint64_t landing_pad(const rt_fde *f, uint64_t pc) {
    if (!f->lsda) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)f->lsda;
    const uint8_t lp_enc = *p++;
    uint64_t lp_start = f->start;
    if (lp_enc != 0xff) {
        lp_start = encoded(&p, lp_enc, 0);
    }
    const uint8_t tt_enc = *p++;
    if (tt_enc != 0xff) {
        (void)uleb(&p);
    }
    const uint8_t cs_enc = *p++;
    const uint64_t cs_len = uleb(&p);
    const uint8_t *end = p + cs_len;
    while (p < end) {
        const uint64_t start = encoded(&p, cs_enc, 0), len = encoded(&p, cs_enc, 0), lp = encoded(&p, cs_enc, 0);
        (void)uleb(&p);  // the action: no personality runs here, every pad is entered
        if (pc - 1 >= lp_start + start && pc - 1 < lp_start + start + len) {
            return lp ? lp_start + lp : 0;
        }
    }
    return 0;
}

// The walk: from the context of the function that called rt_throw or _Unwind_Resume (rt/unwind.S), at that call, frame by frame up to the
// next landing pad or the catcher
__attribute__((noreturn)) static void search(rt_uw_context *c) {
    for (;;) {
        rt_catcher *k = rt_catch_top;
        if (k && c->sp >= k->env[12]) {
            // The frame of the function that registered the catcher (setjmp's sp is its sp at the call): the catch
            rt_catch_top = k->prev;
            k->value = rt_exception.value;
            longjmp(k->env, 1);
        }
        rt_fde f;
        if (!find_fde(c->pc - 1, &f)) {
            break;  // no unwind entry: the outermost frame
        }
        const uint64_t pad = landing_pad(&f, c->pc);
        if (pad) {
            ++rt_unwind_pads;
            rt_uw_install(c, pad, &rt_exception);
        }
        step(c, &f);
    }
    rt_uw_fail("an exception reached the outermost frame with no catcher");
}

void rt_try_push(rt_catcher *c) {
    c->prev = rt_catch_top;
    rt_catch_top = c;
}

void rt_try_pop(rt_catcher *c) {
    if (rt_catch_top == c) {
        rt_catch_top = c->prev;
    }
}

// rt/unwind.S's rt_throw and _Unwind_Resume (what a cleanup landing pad calls when it is done) save their caller's context and come here
__attribute__((noreturn)) void rt_uw_raise(rt_uw_context *c, uint64_t value) {
    rt_exception.value = value;
    search(c);
}

__attribute__((noreturn)) void rt_uw_resume(rt_uw_context *c) {
    search(c);
}
