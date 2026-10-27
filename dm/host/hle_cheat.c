// Atmosphère-format cheat engine: the same cheat files a modded Switch runs (atmosphere/contents/<title ID>/cheats/<main
// build ID>.txt, plus toggles.txt) work unmodified here. The virtual machine, file parser and execution model are ports of
// dmnt (Atmosphère's cheat manager): every opcode, width, region encoding and limit matches, so a distributed cheat table
// for the exact game build behaves the same as on hardware. Differences, all deliberate:
//   · memory access goes through a guard (access_kind below): the game runs in this process, so a bad address would kill
//     the whole app, not just the game; a rejected operation is skipped and logged, where dmnt's debug handle just fails
//   · writes into a module's sealed code area (dmnt's debug handle bypasses page permissions; we cannot on iOS): on the Mac
//     the pages are anonymous and get unsealed, written, flushed and resealed; on iOS they are shared signed pages and the
//     write is rejected (the rewriter, patch_nso.py, is the iOS way to patch code)
//   · PauseProcess / ResumeProcess reuse the host's SetThreadActivity stop-the-world (the IL2CPP GC's pause machinery)
// The engine runs on its own thread at 12 Hz (dmnt's rate), like dmnt independent of the frame rate, and reloads the cheat
// file / toggles.txt when their mtime changes, so toggling a cheat from the Files app needs no in-game UI.
// Keypress conditions read the held-button mask from hle_hid: its bit layout (buttons 0-15, stick directions 16-23) is
// NpadButton's, the same bits dmnt's key masks use.
// Limits and semantics keyed to dmnt: 0x80 cheats, 0x100 opcode words each, 0x400 words in the assembled program,
// 0x40-byte names, 16 VM registers, 256 static registers cleared on reload, registers/saved slots/loops reset every pass.
#include "dm.h"
#include "hle.h"
#include "hle_cheat.h"
#include <TargetConditionals.h>
#include <ctype.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if !TARGET_OS_IPHONE
#include <libkern/OSCacheControl.h>
#endif

// Guest memory layout and input, implemented by hle_svc.c / hle_hid.c / hle_thread.c (a unit test supplies its own)
void svc_guest_range(uint8_t **base, size_t *size);            // whole guest address space (hle_svc.c): the Aslr region
void svc_heap_range(uint8_t **base, size_t *size);             // heap as opened by SetHeapSize (size 0 = not yet opened)
void svc_alias_range(uint8_t **base, size_t *size);            // alias region
int svc_guest_mapped(uint64_t a, size_t n);                    // inside one registered mapping (stacks, alias pages)
uint64_t dm_input_held(void);                                  // buttons held right now (hle_hid.c)
void svc_thread_pause_all(int on);                             // stop-the-world for every guest thread (hle_thread.c)

enum { CHEAT_MAX = 0x80, CHEAT_OPS = 0x100, CHEAT_NAME = 0x40, CHEAT_FILE_MAX = 64 * 1024,
       VM_PROG = 0x400, VM_REGS = 0x10, VM_STATIC = 0x100 };
enum { MEM_MAIN = 0, MEM_HEAP = 1, MEM_ALIAS = 2, MEM_ASLR = 3, MEM_ABS = 4 };
enum { OP_STORE_STATIC = 0, OP_BEGIN_COND = 1, OP_END_COND = 2, OP_LOOP = 3, OP_LDR_STATIC = 4, OP_LDR_MEM = 5,
       OP_STR_STATIC = 6, OP_MATH_STATIC = 7, OP_KEYPRESS = 8, OP_MATH_REG = 9, OP_STR_REG = 10, OP_EXTW = 12,
       OP_REG_COND = 0xC0, OP_SAVE_RESTORE = 0xC1, OP_SAVE_RESTORE_MASK = 0xC2, OP_RW_STATIC = 0xC3,
       OP_EXT_KEYPRESS = 0xC4, OP_DEXTW = 0xF0, OP_PAUSE = 0xFF0, OP_RESUME = 0xFF1, OP_LOG = 0xFFF };

typedef struct { char name[CHEAT_NAME]; uint32_t n; uint32_t op[CHEAT_OPS]; } cheat_def;
typedef struct { int enabled; uint32_t rejects; cheat_def def; } cheat_ent;

// Parsed cheat table. Entry 0 is the master cheat (always enabled, runs first); all state lives on the engine thread
// (the parser and executor below run nowhere else), so it needs no lock.
static cheat_ent g_ent[CHEAT_MAX];
static int g_nent;

// The assembled program: the master cheat plus every enabled cheat, in table order (dmnt LoadProgram). prog_owner maps
// each word back to its cheat, so a rejected memory operation can be reported against the cheat that asked for it.
static uint32_t v_prog[VM_PROG], v_n;
static uint8_t v_owner[VM_PROG];
static int v_dirty = 1;

// VM state. v_static survives passes (dmnt ResetStaticRegisters only on a cheat-file reload); the rest is reset per pass.
static uint64_t v_r[VM_REGS], v_saved[VM_REGS], v_static[VM_STATIC];
static size_t v_loop_top[VM_REGS], v_ip;
static int v_depth, v_ok;
static int cur_owner;                       // cheat whose instruction is executing (rejection reporting)
static uint32_t g_reject;                   // rejected memory operations (guard hit), across the engine's life
static uint32_t g_code_reject;              // code-page writes rejected on iOS
static int g_trace;                         // DM_CHEAT_LOG: one line per executed instruction
static useconds_t g_sleep_us = 83333;       // 12 Hz, dmnt's rate

// ---------------------------------------------------------------- file parsing (dmnt ParseCheats / ParseCheatToggles)

static cheat_ent *free_ent(void) {
    for (int i = 1; i < CHEAT_MAX; i++) if (!g_ent[i].def.n) return &g_ent[i];
    return NULL;
}

static int parse_cheats(const char *s, size_t len) {
    memset(g_ent, 0, sizeof g_ent);              // a reload starts from an empty table (dmnt ResetAllCheatEntries)
    g_nent = 0;
    cheat_ent *cur = NULL;
    size_t i = 0;
    while (i < len) {
        if (isspace((unsigned char)s[i])) i++;
        else if (s[i] == '[') {                    // [name]: a new cheat
            cur = free_ent();
            if (!cur) return 0;
            size_t j = i + 1;
            while (s[j] != ']') { if (++j >= len) return 0; }
            size_t n = j - i - 1; if (n > CHEAT_NAME - 1) n = CHEAT_NAME - 1;
            memcpy(cur->def.name, s + i + 1, n); cur->def.name[n] = 0;
            i = j + 1;
        } else if (s[i] == '{') {                  // {name}: the master cheat (entry 0, only one, forced on)
            if (g_ent[0].def.n) return 0;
            cur = &g_ent[0];
            size_t j = i + 1;
            while (s[j] != '}') { if (++j >= len) return 0; }
            size_t n = j - i - 1; if (n > CHEAT_NAME - 1) n = CHEAT_NAME - 1;
            memcpy(cur->def.name, s + i + 1, n); cur->def.name[n] = 0;
            i = j + 1;
        } else if (isxdigit((unsigned char)s[i])) {
            if (!cur || cur->def.n >= CHEAT_OPS) return 0;
            char w[9];
            for (size_t j = 0; j < 8; j++) if (i + j >= len || !isxdigit((unsigned char)s[i + j])) return 0;
            memcpy(w, s + i, 8); w[8] = 0;
            cur->def.op[cur->def.n++] = (uint32_t)strtoul(w, NULL, 16);
            i += 8;
        } else {
            // A junk line -- a header or footer a cheat tool left in (this game's table ships its title line bare,
            // without the braces that would make it the master). dmnt rejects such a file outright; we skip lines
            // that carry no syntax at all, but garbage inside a code line still rejects the whole file, as in dmnt
            size_t ls = i;
            while (ls > 0 && s[ls - 1] != '\n') ls--;
            for (size_t j = ls; j < i; j++)
                if (isxdigit((unsigned char)s[j]) || s[j] == '[' || s[j] == '{') return 0;
            while (i < len && s[i] != '\n') i++;
        }
    }
    if (g_ent[0].def.n) g_ent[0].enabled = 1;                 // the master cheat cannot be disabled (dmnt parity)
    g_nent = 1;
    while (g_nent < CHEAT_MAX && g_ent[g_nent].def.n) g_nent++;
    for (int k = 1; k < g_nent; k++) g_ent[k].enabled = 0;   // everything starts off; toggles.txt (and the menu) turn rows on
    return 1;
}

// Baked cheat patches (the full story is at the baked section below): the tables are declared early because
// parse_toggles drives baked-only rows too -- with everything off by default, toggles.txt is the only thing that
// turns a row on, and a baked patch without a file entry has no other place to read its switch from
enum { BAKED_MAX = 64 };
static struct { char name[CHEAT_NAME]; uint32_t mask; uint8_t *flag; int on; int forced; } g_baked[BAKED_MAX];
static int g_nbaked = -1;                     // -1 = not discovered yet (the module has to be mapped first)

// [name] 1/true/on or 0/false/off (case-insensitive values); names with no parsed cheat are ignored (dmnt allows)
static void parse_toggles(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (isspace((unsigned char)s[i])) { i++; continue; }
        if (s[i] == '#') { while (i < len && s[i] != '\n') i++; continue; }   // comment line (toggles_save writes one)
        if (s[i] != '[') return;                  // malformed line: keep what was applied so far and stop
        size_t j = i + 1;
        while (s[j] != ']') { if (++j >= len) return; }
        char name[CHEAT_NAME];
        size_t n = j - i - 1; if (n > CHEAT_NAME - 1) n = CHEAT_NAME - 1;
        memcpy(name, s + i + 1, n); name[n] = 0;
        i = j + 1;
        while (i < len && isspace((unsigned char)s[i])) i++;
        size_t e = i;
        while (e < len && !isspace((unsigned char)s[e])) e++;
        char val[8]; size_t vn = e - i; if (vn > sizeof val - 1) vn = sizeof val - 1;
        memcpy(val, s + i, vn); val[vn] = 0;
        int on = !strcasecmp(val, "1") || !strcasecmp(val, "true") || !strcasecmp(val, "on");
        int off = !strcasecmp(val, "0") || !strcasecmp(val, "false") || !strcasecmp(val, "off");
        for (int k = 1; k < g_nent; k++)
            if (!strncmp(g_ent[k].def.name, name, CHEAT_NAME)) {
                if (on) g_ent[k].enabled = 1;
                else if (off) g_ent[k].enabled = 0;
            }
        if (g_nbaked > 0)
            for (int k = 0; k < g_nbaked; k++)
                if (!strncmp(g_baked[k].name, name, CHEAT_NAME)) {
                    if (on) g_baked[k].forced = 1;
                    else if (off) g_baked[k].forced = 0;
                }
        i = e;
    }
}

// ---------------------------------------------------------------- guest memory guard

// How [a, a+n) is reachable. The module classification mirrors the loader's page sealing (loader.c map_image):
// RX = [base, base+rx) plus the patch area; the key page is read-only; everything else inside a module image is data.
enum { ACC_NONE = 0, ACC_RW = 1, ACC_CODE = 2, ACC_RO = 3 };
static int access_kind(uint64_t a, size_t n) {
    if (n > 8 || a + n < a) return ACC_NONE;      // the VM only ever moves 1/2/4/8 bytes; a wrapped range is a bad code
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->base) continue;
        uint64_t b = (uint64_t)m->base, rx = b + ((m->text_end + 0x3FFFull) & ~0x3FFFull);
        uint64_t pb = b + (int64_t)m->patch_off, plen = m->patch_len;                   // patch_off may be negative
        uint64_t end = b + m->size, pend = pb + plen;
        uint64_t lo = pb < b ? pb : b, hi = pend > end ? pend : end;                    // image ∪ patch area
        if (a < lo || n > hi - lo || a - lo > hi - lo - n) continue;
        if (a < rx && rx - a >= n) return ACC_CODE;
        if (plen && a >= pb && a - pb <= plen - n) return ACC_CODE;
        uint64_t kb = b + m->key_page_off;
        if (a >= kb && a - kb <= m->key_page_size - n) return ACC_RO;
        return ACC_RW;
    }
    uint8_t *hb; size_t hn;
    svc_heap_range(&hb, &hn);
    if (hb && hn && a >= (uint64_t)hb && a - (uint64_t)hb <= hn - n) return ACC_RW;
    if (svc_guest_mapped(a, n)) return ACC_RW;    // MapMemory stacks and MapPhysicalMemory alias pages
    return ACC_NONE;
}

// A rejected operation: skip it and report against the running cheat (first hits and then every 1000th; dmnt fails
// silently through the debug handle, but a wrong table here would otherwise be undebuggable). Addresses in the not-yet-
// opened heap are silent: dmnt attaches to a process whose heap exists, and cheats written for boot would otherwise
// report rejects every tick until SetHeapSize.
static void reject(uint64_t a, const char *what) {
    uint8_t *hb; size_t hn; svc_heap_range(&hb, &hn);
    uint8_t *as; size_t asn; svc_guest_range(&as, &asn);
    if (!hn && hb && as && (uint64_t)hb <= a && a < (uint64_t)as + asn) return;
    g_reject++;
    cheat_ent *e = cur_owner < CHEAT_MAX ? &g_ent[cur_owner] : NULL;
    if (!e) { LOG("[RED] cheat: %s at %#llx outside guest memory, skipped", what, (unsigned long long)a); return; }
    e->rejects++;
    if (e->rejects <= 2 || e->rejects % 1000 == 0)
        LOG("[RED] cheat '%s': %s at %#llx outside guest memory, skipped (%u so far)", e->def.name, what,
            (unsigned long long)a, e->rejects);
}

// dmnt reads through the debug handle: a failed read leaves the value 0. Same here, minus the crash.
static uint64_t vm_read(uint64_t a, int width) {
    uint64_t v = 0;
    if (access_kind(a, width) == ACC_NONE) { reject(a, "read"); return 0; }
    memcpy(&v, (void *)a, width);
    return v;
}

static int vm_write(uint64_t a, uint64_t v, int width) {
    int kind = access_kind(a, width);
    if (kind == ACC_NONE || kind == ACC_RO) { reject(a, "write"); return 0; }
    if (kind == ACC_CODE) {
#if TARGET_OS_IPHONE
        // The code pages are shared signed pages remapped from __TEXT (loader.c): no MAP_JIT, no write. The rewriter
        // (patch_nso.py) is the way to patch code in an iOS build.
        if (!g_code_reject++) LOG("[RED] cheat: write into sealed code at %#llx rejected (iOS code pages are not writable)", (unsigned long long)a);
        return 0;
#else
        uint64_t pg = a & ~0x3FFFull, plen = ((a + width - pg) + 0x3FFFull) & ~0x3FFFull;
        if (mprotect((void *)pg, plen, PROT_READ | PROT_WRITE)) { reject(a, "write (code mprotect)"); return 0; }
        dm_guest_write_prepare((void *)a, width);
        memcpy((void *)a, &v, width);
        mprotect((void *)pg, plen, PROT_READ | PROT_EXEC);
        sys_icache_invalidate((void *)a, width);
        return 1;
#endif
    }
    dm_guest_write_prepare((void *)a, width);     // dirty-page tracking (crash.c) must see host writes into guest memory
    memcpy((void *)a, &v, width);
    return 1;
}

static uint64_t region_base(int mem_type) {
    switch (mem_type) {
    case MEM_HEAP: { uint8_t *b; size_t n; svc_heap_range(&b, &n); return (uint64_t)b; }
    case MEM_ALIAS: { uint8_t *b; size_t n; svc_alias_range(&b, &n); return (uint64_t)b; }
    case MEM_ASLR: { uint8_t *b; size_t n; svc_guest_range(&b, &n); return (uint64_t)b; }
    case MEM_ABS: return 0;
    case MEM_MAIN:
    default:
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return (uint64_t)g_mod[i].base;
        return 0;
    }
}
static uint64_t region_addr(int mem_type, uint64_t rel) { return region_base(mem_type) + rel; }

// ---------------------------------------------------------------- the virtual machine (dmnt CheatVirtualMachine)

typedef struct {
    int op, begin_block, width;
    int mem_type, cond_type, math_type, comp_type, ofs_type, op_type;
    int reg, reg2, reg3, ofs_reg, val_reg, other_reg;
    int include_ofs, incr_reg, add_ofs_reg, has_imm, load_from_reg, is_else, start_loop, auto_repeat;
    uint32_t num_iters, key_mask, log_id, static_idx;
    uint64_t rel, value, key_mask64;
    int should[VM_REGS];
} vmop;

static uint32_t next_dword(void) {
    if (v_ip >= v_n) { v_ok = 0; return 0; }
    return v_prog[v_ip++];
}
static uint64_t next_vmint(int width) {
    uint32_t d = next_dword();
    if (width == 8) return ((uint64_t)d << 32) | next_dword();
    if (width == 4) return d;
    if (width == 2) return d & 0xffff;
    if (width == 1) return d & 0xff;
    return 0;
}
static int width_ok(int width) { return width == 1 || width == 2 || width == 4 || width == 8; }
static uint64_t width_mask(int width) { return width == 8 ? ~0ull : (1ull << (width * 8)) - 1; }

static int vm_decode(vmop *o) {
    v_ok = 1;
    memset(o, 0, sizeof *o);
    uint32_t f = next_dword();
    if (!v_ok) return 0;
    o->op = (f >> 28) & 0xF;
    if (o->op >= OP_EXTW) o->op = o->op << 4 | ((f >> 24) & 0xF);
    if (o->op >= OP_DEXTW) o->op = o->op << 4 | ((f >> 20) & 0xF);
    switch (o->op) {
    case OP_BEGIN_COND: case OP_KEYPRESS: case OP_EXT_KEYPRESS: case OP_REG_COND: o->begin_block = 1; break;
    default: break;
    }
    switch (o->op) {
    case OP_STORE_STATIC: {                       // 0TMR00AA AAAAAAAA YYYYYYYY (YYYYYYYY)
        o->width = (f >> 24) & 0xF; o->mem_type = (f >> 20) & 0xF; o->ofs_reg = (f >> 16) & 0xF;
        o->rel = (uint64_t)(f & 0xFF) << 32 | next_dword();
        o->value = next_vmint(o->width);
        break;
    }
    case OP_BEGIN_COND: {                         // 1TMC00AA AAAAAAAA YYYYYYYY (YYYYYYYY)
        o->width = (f >> 24) & 0xF; o->mem_type = (f >> 20) & 0xF; o->cond_type = (f >> 16) & 0xF;
        o->include_ofs = ((f >> 12) & 0xF) != 0; o->ofs_reg = (f >> 8) & 0xF;
        o->rel = (uint64_t)(f & 0xFF) << 32 | next_dword();
        o->value = next_vmint(o->width);
        break;
    }
    case OP_END_COND: o->is_else = ((f >> 24) & 0xF) == 1; break;   // 2X000000
    case OP_LOOP: {                               // 300R0000 VVVVVVVV / 310R0000
        o->start_loop = ((f >> 24) & 0xF) == 0; o->reg = (f >> 16) & 0xF;
        if (o->start_loop) o->num_iters = next_dword();
        break;
    }
    case OP_LDR_STATIC:                           // 400R0000 VVVVVVVV VVVVVVVV
        o->reg = (f >> 16) & 0xF;
        o->value = (uint64_t)next_dword() << 32 | next_dword();
        break;
    case OP_LDR_MEM: {                            // 5TMRI0AA AAAAAAAA
        o->width = (f >> 24) & 0xF; o->mem_type = (f >> 20) & 0xF; o->reg = (f >> 16) & 0xF;
        o->load_from_reg = (f >> 12) & 0xF; o->ofs_reg = (f >> 8) & 0xF;
        o->rel = (uint64_t)(f & 0xFF) << 32 | next_dword();
        break;
    }
    case OP_STR_STATIC:                           // 6T0RIor0 VVVVVVVV VVVVVVVV
        o->width = (f >> 24) & 0xF; o->reg = (f >> 16) & 0xF;
        o->incr_reg = ((f >> 12) & 0xF) != 0; o->add_ofs_reg = ((f >> 8) & 0xF) != 0;
        o->ofs_reg = (f >> 4) & 0xF;
        o->value = (uint64_t)next_dword() << 32 | next_dword();
        break;
    case OP_MATH_STATIC: {                        // 7T0RC000 VVVVVVVV
        o->width = (f >> 24) & 0xF; o->reg = (f >> 16) & 0xF; o->math_type = (f >> 12) & 0xF;
        o->value = next_dword();
        break;
    }
    case OP_KEYPRESS:                             // 8kkkkkkk
        o->key_mask = f & 0x0FFFFFFF;
        break;
    case OP_EXT_KEYPRESS:                         // C4r00000 kkkkkkkk kkkkkkkk
        o->key_mask64 = (uint64_t)next_dword() << 32 | next_dword();
        o->auto_repeat = ((f >> 20) & 0xF) != 0;
        break;
    case OP_MATH_REG: {                           // 9TCRSIs0 (VVVVVVVV (VVVVVVVV))
        o->width = (f >> 24) & 0xF; o->math_type = (f >> 20) & 0xF; o->reg = (f >> 16) & 0xF; o->reg2 = (f >> 12) & 0xF;
        o->has_imm = ((f >> 8) & 0xF) != 0;
        if (o->has_imm) { o->reg3 = 0; o->value = next_vmint(o->width); }
        else o->reg3 = (f >> 4) & 0xF;
        break;
    }
    case OP_STR_REG: {                            // ATSRIOxa (aaaaaaaa)
        o->width = (f >> 24) & 0xF; o->reg = (f >> 20) & 0xF; o->reg2 = (f >> 16) & 0xF;
        o->incr_reg = ((f >> 12) & 0xF) != 0; o->ofs_type = (f >> 8) & 0xF; o->ofs_reg = (f >> 4) & 0xF;
        switch (o->ofs_type) {
        case 0: case 1: break;
        case 2: o->rel = (uint64_t)(f & 0xF) << 32 | next_dword(); break;
        case 3: o->mem_type = (f >> 4) & 0xF; break;
        case 4: case 5: o->mem_type = (f >> 4) & 0xF; o->rel = (uint64_t)(f & 0xF) << 32 | next_dword(); break;
        default: o->ofs_type = 0; break;
        }
        break;
    }
    case OP_REG_COND: {                           // C0TcSX## (see dmnt for the operand shapes)
        o->width = (f >> 20) & 0xF; o->cond_type = (f >> 16) & 0xF; o->val_reg = (f >> 12) & 0xF; o->comp_type = (f >> 8) & 0xF;
        switch (o->comp_type) {
        case 4: o->value = next_vmint(o->width); break;
        case 5: o->other_reg = (f >> 4) & 0xF; break;
        case 0: o->mem_type = (f >> 4) & 0xF; o->rel = (uint64_t)(f & 0xF) << 32 | next_dword(); break;
        case 1: o->mem_type = (f >> 4) & 0xF; o->ofs_reg = f & 0xF; break;
        case 2: o->reg2 = (f >> 4) & 0xF; o->rel = (uint64_t)(f & 0xF) << 32 | next_dword(); break;
        case 3: o->reg2 = (f >> 4) & 0xF; o->ofs_reg = f & 0xF; break;
        }
        break;
    }
    case OP_SAVE_RESTORE:                         // C10D0Sx0
        o->reg = (f >> 16) & 0xF; o->reg2 = (f >> 8) & 0xF; o->op_type = (f >> 4) & 0xF;
        break;
    case OP_SAVE_RESTORE_MASK:                    // C2x0XXXX
        o->op_type = (f >> 20) & 0xF;
        for (int i = 0; i < VM_REGS; i++) o->should[i] = (f & (1u << i)) != 0;
        break;
    case OP_RW_STATIC:                            // C3000XXx
        o->static_idx = (f >> 4) & 0xFF; o->reg = f & 0xF;
        break;
    case OP_PAUSE: case OP_RESUME: break;         // FF0 / FF1: no operands
    case OP_LOG: {                                // FFFTIX## (comp_type reuses dmnt's val_type; see dmnt for the shapes)
        o->width = (f >> 16) & 0xF; o->log_id = (f >> 12) & 0xF; o->comp_type = (f >> 8) & 0xF;
        switch (o->comp_type) {
        case 4: o->val_reg = (f >> 4) & 0xF; break;
        case 0: o->mem_type = (f >> 4) & 0xF; o->rel = (uint64_t)(f & 0xF) << 32 | next_dword(); break;
        case 1: o->mem_type = (f >> 4) & 0xF; o->ofs_reg = f & 0xF; break;
        case 2: o->reg2 = (f >> 4) & 0xF; o->rel = (uint64_t)(f & 0xF) << 32 | next_dword(); break;
        case 3: o->reg2 = (f >> 4) & 0xF; o->ofs_reg = f & 0xF; break;
        }
        break;
    }
    default:
        v_ok = 0;                                 // unrecognized opcode: decoding fails, as in dmnt
        break;
    }
    return v_ok;
}

// dmnt SkipConditionalBlock: decode (not scan) forward to the end of the current block; a keypress conditional that is
// not satisfied skips to its end. A zero condition depth is a broken table: dmnt aborts; we log once and stop the pass.
static void skip_block(int is_if) {
    if (v_depth <= 0) {
        static int warned;
        if (!warned++) LOG("[RED] cheat: mismatched conditional structure, execution stopped");
        v_ok = 0;
        return;
    }
    size_t want = v_depth - 1;
    vmop o;
    while (v_depth > want && vm_decode(&o)) {
        if (o.begin_block) v_depth++;
        else if (o.op == OP_END_COND) {
            if (!o.is_else) v_depth--;
            else if (is_if && v_depth - 1 == want) break;   // an if continues at its else
        }
    }
}

static int cmp_cond(int cond_type, uint64_t a, uint64_t b) {
    switch (cond_type) {
    case 1: return a > b; case 2: return a >= b; case 3: return a < b; case 4: return a <= b;
    case 5: return a == b; case 6: return a != b; default: return 0;
    }
}

static uint64_t s_keyold;

static void vm_execute(void) {
    uint64_t held = dm_input_held();
    for (int i = 0; i < VM_REGS; i++) { v_r[i] = 0; v_saved[i] = 0; v_loop_top[i] = 0; }
    v_ip = 0; v_depth = 0; v_ok = 1;
    vmop o;
    while (vm_decode(&o)) {
        if (g_trace) LOG("cheat ip %zu: op %#x", v_ip, o.op);
        cur_owner = v_ip ? v_owner[v_ip - 1] : 0;
        if (o.begin_block) v_depth++;
        switch (o.op) {
        case OP_STORE_STATIC:
            if (width_ok(o.width)) vm_write(region_addr(o.mem_type, o.rel + v_r[o.ofs_reg]), o.value, o.width);
            break;
        case OP_BEGIN_COND: {                      // an invalid width reads as 0, as in dmnt, and the compare still runs
            uint64_t src = width_ok(o.width) ? vm_read(region_addr(o.mem_type, o.include_ofs ? v_r[o.ofs_reg] + o.rel : o.rel), o.width) : 0;
            if (!cmp_cond(o.cond_type, src, o.value)) skip_block(1);
            break;
        }
        case OP_END_COND:
            if (o.is_else) skip_block(0);
            else if (v_depth > 0) v_depth--;
            break;
        case OP_LOOP:
            if (o.start_loop) { v_r[o.reg] = o.num_iters; v_loop_top[o.reg] = v_ip; }
            else {
                v_r[o.reg]--;
                if (v_r[o.reg] != 0) v_ip = v_loop_top[o.reg];
            }
            break;
        case OP_LDR_STATIC: v_r[o.reg] = o.value; break;
        case OP_LDR_MEM: {
            uint64_t src;
            if (o.load_from_reg == 1) src = v_r[o.reg] + o.rel;
            else if (o.load_from_reg == 2) src = v_r[o.ofs_reg] + o.rel;
            else if (o.load_from_reg == 3) src = region_addr(o.mem_type, v_r[o.ofs_reg] + o.rel);
            else src = region_addr(o.mem_type, o.rel);
            if (width_ok(o.width)) v_r[o.reg] = vm_read(src, o.width);   // an invalid width leaves the register alone
            break;
        }
        case OP_STR_STATIC: {
            uint64_t dst = v_r[o.reg];
            if (o.add_ofs_reg) dst += v_r[o.ofs_reg];
            if (width_ok(o.width)) vm_write(dst, o.value, o.width);
            if (o.incr_reg) v_r[o.reg] += o.width;                        // the increment runs even on an invalid width
            break;
        }
        case OP_MATH_STATIC: {
            switch (o.math_type) {
            case 0: v_r[o.reg] += o.value; break;
            case 1: v_r[o.reg] -= o.value; break;
            case 2: v_r[o.reg] *= o.value; break;
            case 3: v_r[o.reg] <<= o.value; break;
            case 4: v_r[o.reg] >>= o.value; break;
            default: break;                       // the register-register extensions are opcode 9
            }
            if (width_ok(o.width)) v_r[o.reg] &= width_mask(o.width);
            break;
        }
        case OP_KEYPRESS:
            if ((o.key_mask & held) != o.key_mask) skip_block(1);
            break;
        case OP_EXT_KEYPRESS: {
            uint64_t m = o.key_mask64;
            int held_all = (m & held) == m;
            if (!o.auto_repeat) {
                if (!held_all || (m & s_keyold) == m) skip_block(1);   // fires once per press unless auto-repeat
            } else if (!held_all) skip_block(1);
            break;
        }
        case OP_MATH_REG: {
            uint64_t a = v_r[o.reg2];
            uint64_t b = o.has_imm ? o.value : v_r[o.reg3];
            uint64_t r = 0;
            switch (o.math_type) {
            case 0: r = a + b; break;
            case 1: r = a - b; break;
            case 2: r = a * b; break;
            case 3: r = a << b; break;
            case 4: r = a >> b; break;
            case 5: r = a & b; break;
            case 6: r = a | b; break;
            case 7: r = ~a; break;
            case 8: r = a ^ b; break;
            case 9: r = a; break;
            case 10: case 11: case 12: case 13: {  // float add / sub / mul / div on the width's bit pattern
                if (o.width == 4) {
                    float fa, fb = 0; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
                    float fr = o.math_type == 10 ? fa + fb : o.math_type == 11 ? fa - fb : o.math_type == 12 ? fa * fb : fa / fb;
                    uint32_t u; memcpy(&u, &fr, 4); r = u;
                } else if (o.width == 8) {
                    double fa, fb = 0; memcpy(&fa, &a, 8); memcpy(&fb, &b, 8);
                    double fr = o.math_type == 10 ? fa + fb : o.math_type == 11 ? fa - fb : o.math_type == 12 ? fa * fb : fa / fb;
                    memcpy(&r, &fr, 8);
                }
                break;
            }
            }
            if (width_ok(o.width)) r &= width_mask(o.width);
            v_r[o.reg] = r;
            break;
        }
        case OP_STR_REG: {
            uint64_t dst = v_r[o.reg2];
            switch (o.ofs_type) {
            case 0: break;
            case 1: dst += v_r[o.ofs_reg]; break;
            case 2: dst += o.rel; break;
            case 3: dst = region_addr(o.mem_type, v_r[o.reg2]); break;
            case 4: dst = region_addr(o.mem_type, o.rel); break;
            case 5: dst = region_addr(o.mem_type, v_r[o.reg2] + o.rel); break;
            }
            if (width_ok(o.width)) vm_write(dst, v_r[o.reg], o.width);
            if (o.incr_reg) v_r[o.reg2] += o.width;
            break;
        }
        case OP_REG_COND: {
            uint64_t src = width_ok(o.width) ? (v_r[o.val_reg] & width_mask(o.width)) : 0;
            uint64_t cv;
            if (o.comp_type == 4) cv = width_ok(o.width) ? (o.value & width_mask(o.width)) : 0;
            else if (o.comp_type == 5) cv = width_ok(o.width) ? (v_r[o.other_reg] & width_mask(o.width)) : 0;
            else {
                uint64_t addr;
                if (o.comp_type == 0) addr = region_addr(o.mem_type, o.rel);
                else if (o.comp_type == 1) addr = region_addr(o.mem_type, v_r[o.ofs_reg]);
                else if (o.comp_type == 2) addr = v_r[o.reg2] + o.rel;
                else addr = v_r[o.reg2] + v_r[o.ofs_reg];
                cv = width_ok(o.width) ? vm_read(addr, o.width) : 0;
            }
            if (!cmp_cond(o.cond_type, src, cv)) skip_block(1);
            break;
        }
        case OP_SAVE_RESTORE:
            switch (o.op_type) {
            case 3: v_r[o.reg] = 0; break;
            case 2: v_saved[o.reg] = 0; break;
            case 1: v_saved[o.reg] = v_r[o.reg2]; break;
            default: v_r[o.reg] = v_saved[o.reg2]; break;   // 0 = restore
            }
            break;
        case OP_SAVE_RESTORE_MASK: {
            int to_saved = o.op_type == 1 || o.op_type == 2;
            int clear = o.op_type == 2 || o.op_type == 3;
            for (int i = 0; i < VM_REGS; i++) if (o.should[i]) {
                if (clear) { if (to_saved) v_saved[i] = 0; else v_r[i] = 0; }
                else { if (to_saved) v_saved[i] = v_r[i]; else v_r[i] = v_saved[i]; }
            }
            break;
        }
        case OP_RW_STATIC:
            if (o.static_idx < 0x80) v_r[o.reg] = v_static[o.static_idx];
            else if (o.static_idx < VM_STATIC) v_static[o.static_idx] = v_r[o.reg];
            break;
        case OP_PAUSE:
        case OP_RESUME: {
            static int paused;
            int want = o.op == OP_PAUSE;
            if (want != paused) { paused = want; LOG("cheat: game %s by cheat code", want ? "paused" : "resumed"); }
            svc_thread_pause_all(want);
            break;
        }
        case OP_LOG: {
            uint64_t v;
            if (o.comp_type == 4) v = width_ok(o.width) ? (v_r[o.val_reg] & width_mask(o.width)) : 0;
            else {
                uint64_t addr;
                if (o.comp_type == 0) addr = region_addr(o.mem_type, o.rel);
                else if (o.comp_type == 1) addr = region_addr(o.mem_type, v_r[o.ofs_reg]);
                else if (o.comp_type == 2) addr = v_r[o.reg2] + o.rel;
                else addr = v_r[o.reg2] + v_r[o.ofs_reg];
                v = width_ok(o.width) ? vm_read(addr, o.width) : 0;
            }
            static uint64_t last_log_ns[16];
            uint64_t now = now_ns();
            if (now - last_log_ns[o.log_id & 15] > 1000000000ull) {   // rate-limited: 12 passes a second would flood the log
                last_log_ns[o.log_id & 15] = now;
                LOG("cheat log id %u: %#llx", o.log_id, (unsigned long long)v);
            }
            break;
        }
        default: break;                            // unknown opcodes are no-ops, as in dmnt
        }
    }
    s_keyold = held;
}

// ---------------------------------------------------------------- in-game menu (hidden hotkey, front_cheat_menu)
// A merged toggleable view of baked patches and file cheats for the front end's menu (iOS panel). The rows and the
// engine tick run on different threads: g_mu guards the tables. A file cheat that also exists as a baked patch shows
// as one row (its enabled bit drives both); a baked patch without a file entry has its own bit. The hidden hotkey is
// a button combo held for 0.8 s (default L+R+ZL+ZR, DM_CHEAT_MENU_KEYS="L+R+ZL+ZR" overrides, dm_keys names); the
// same combo closes the menu again.
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // the engine tick vs the front end's menu calls
static uint64_t g_menu_mask = (1ull << 6) | (1ull << 7) | (1ull << 8) | (1ull << 9);   // L | R | ZL | ZR

void front_cheat_menu(int open);   // the iOS front end shows its panel; the weak no-op below covers builds without one
__attribute__((weak)) void front_cheat_menu(int open) { (void)open; }

static void menu_hotkey_tick(uint64_t held, uint64_t now) {
    static uint64_t since;
    static int open;
    if ((held & g_menu_mask) != g_menu_mask) { since = 0; return; }
    if (since == UINT64_MAX) return;                       // already fired for this press: wait for a release
    if (!since) { since = now; return; }
    if (now - since >= 800000000ull) {
        since = UINT64_MAX;
        open = !open;
        LOG("cheat: menu %s by hotkey (hold the combo again to close)", open ? "open" : "closed");
        front_cheat_menu(open);
    }
}

static int menu_hotkey_parse(void) {
    const char *e = getenv("DM_CHEAT_MENU_KEYS");
    if (!e || !*e) return 1;
    static const char *const names[16] = {"A","B","X","Y","LS","RS","L","R","ZL","ZR","Plus","Minus","Left","Up","Right","Down"};
    uint64_t mask = 0;
    char buf[256]; snprintf(buf, sizeof buf, "%s", e);
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        int hit = 0;
        for (int b = 0; b < 16; b++) if (!strcasecmp(tok, names[b])) { mask |= 1ull << b; hit = 1; }
        if (!hit) { LOG("[RED] cheat: DM_CHEAT_MENU_KEYS unknown key %s (L R ZL ZR Plus Minus Left Up Right Down)", tok); return 0; }
    }
    if (mask) g_menu_mask = mask;
    return 1;
}

// ---------------------------------------------------------------- baked cheat patches (patch_nso DM_CHEAT_FILE)
// Code-write cheats cannot run from the cheat file on iOS (sealed code pages), so the rewrite bakes them: each site
// branches to a stub that tests a flag byte and runs the original or the cheat instruction. The flags page (appended
// after the key page, header 'DMCF') lists the baked cheats by name with their keypress masks; the engine maintains the
// bytes from the cheat file's toggle state and the held buttons, so a baked patch is switched at run time exactly like
// a file cheat: edit toggles.txt, switch back to the game. With the engine off (DM_CHEATS=off) the page stays zero:
// every baked patch off. Cheats whose names match a baked patch are skipped in the assembled program (assemble below).
static int baked_named(const char *name) {
    if (g_nbaked <= 0) return 0;
    for (int i = 0; i < g_nbaked; i++) if (!strcmp(g_baked[i].name, name)) return 1;
    return 0;
}

static void baked_discover(void) {
    if (g_nbaked >= 0) return;
    g_nbaked = 0;
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->base || strcmp(m->name, "main") || !m->flags_page_off) continue;
        uint8_t *p = m->base + m->flags_page_off;
        if (memcmp(p, "DMCF", 4)) { LOG("[RED] cheat: flags page at main+%#llx carries no DMCF header", (unsigned long long)m->flags_page_off); return; }
        uint32_t n = *(uint32_t *)(p + 4);
        if (n > BAKED_MAX) n = BAKED_MAX;
        for (uint32_t k = 0; k < n; k++) {
            const uint8_t *r = p + 0x20 + (size_t)k * 0x48;
            g_baked[g_nbaked].mask = *(const uint32_t *)r;
            snprintf(g_baked[g_nbaked].name, CHEAT_NAME, "%s", (const char *)r + 8);
            g_baked[g_nbaked].flag = p + 0x1400 + (size_t)k * 4;   // 4-aligned: the stub reads it with ldr w9
            g_baked[g_nbaked].on = -1;        // unknown; the first tick sets and logs it
            g_baked[g_nbaked].forced = 0;
            g_nbaked++;
        }
        LOG("cheat: %d baked patches on the flags page at main+%#llx (toggled like file cheats, through toggles.txt)",
            g_nbaked, (unsigned long long)m->flags_page_off);
        return;
    }
}

// One maintenance step: the file table's toggle state (default on, dmnt's default), then the keypress gate.
void cheat_baked_pump(void) {
    baked_discover();
    if (g_nbaked <= 0) return;
    uint64_t held = dm_input_held();
    for (int i = 0; i < g_nbaked; i++) {
        int on = g_baked[i].forced;                        // its own bit when no file row carries the name
        for (int k = 1; k < g_nent; k++)
            if (!strcmp(g_ent[k].def.name, g_baked[i].name)) { on = g_ent[k].enabled; break; }
        if (g_baked[i].mask && (held & g_baked[i].mask) != g_baked[i].mask) on = 0;
        if (on != g_baked[i].on) {
            g_baked[i].on = on;
            *g_baked[i].flag = (uint8_t)on;
            LOG("cheat: baked patch '%s' %s", g_baked[i].name, on ? "on" : "off");
        }
    }
}

int cheat_menu_count(void) {
    baked_discover();                            // the menu can open before the engine's first pump
    pthread_mutex_lock(&g_mu);
    int n = g_nbaked > 0 ? g_nbaked : 0;
    for (int k = 1; k < g_nent; k++) if (g_ent[k].def.n && !baked_named(g_ent[k].def.name)) n++;
    pthread_mutex_unlock(&g_mu);
    return n;
}

// Where toggles.txt lives (the directory of the last loaded cheat table), so menu switches persist across launches:
// every row's state is written back on a menu toggle, and the next boot starts from exactly that file
static char g_tdir[1100];

static void toggles_save_locked(void) {       // caller holds g_mu
    if (!g_tdir[0]) return;
    char path[1100];
    snprintf(path, sizeof path, "%s/toggles.txt", g_tdir);
    FILE *f = fopen(path, "w");
    if (!f) { LOG("[RED] cheat: cannot write %s (menu switches will not survive a restart)", path); return; }
    fputs("# cheat switches, written by the game when you flip one (edit while playing: picked up within a second)\n", f);
    for (int i = 0; i < (g_nbaked > 0 ? g_nbaked : 0); i++) {
        int on = g_baked[i].forced;
        for (int k = 1; k < g_nent; k++)
            if (!strcmp(g_ent[k].def.name, g_baked[i].name)) { on = g_ent[k].enabled; break; }
        fprintf(f, "[%s] %s\n", g_baked[i].name, on ? "on" : "off");
    }
    for (int k = 1; k < g_nent; k++) {
        if (!g_ent[k].def.n || baked_named(g_ent[k].def.name)) continue;
        fprintf(f, "[%s] %s\n", g_ent[k].def.name, g_ent[k].enabled ? "on" : "off");
    }
    fclose(f);
}

// One row: the name and the effective state (a keypress-gated patch reads off until its buttons are held)
void cheat_menu_get(int i, char *name, int cap, int *on) {
    baked_discover();
    pthread_mutex_lock(&g_mu);
    int n = 0;
    const char *src = NULL;
    int eff = 0;
    if (g_nbaked > 0 && i < g_nbaked) {
        src = g_baked[i].name;
        eff = g_baked[i].forced;
        for (int k = 1; k < g_nent; k++)
            if (!strcmp(g_ent[k].def.name, src)) { eff = g_ent[k].enabled; break; }
        if (g_baked[i].mask && (dm_input_held() & g_baked[i].mask) != g_baked[i].mask) eff = 0;
    } else {
        n = g_nbaked > 0 ? g_nbaked : 0;
        for (int k = 1; k < g_nent; k++) {
            if (!g_ent[k].def.n || baked_named(g_ent[k].def.name)) continue;
            if (n++ == i) { src = g_ent[k].def.name; eff = g_ent[k].enabled; break; }
        }
    }
    if (src) snprintf(name, cap, "%s", src);
    else { snprintf(name, cap, "?"); eff = 0; }
    if (on) *on = eff;
    pthread_mutex_unlock(&g_mu);
}

void cheat_menu_toggle(int i) {
    baked_discover();
    pthread_mutex_lock(&g_mu);
    int n = 0;
    if (g_nbaked > 0 && i < g_nbaked) {
        int *p = &g_baked[i].forced;
        for (int k = 1; k < g_nent; k++)
            if (!strcmp(g_ent[k].def.name, g_baked[i].name)) { p = &g_ent[k].enabled; break; }   // a baked name with a file row: one switch drives both
        *p = !*p;
        v_dirty = 1;
        toggles_save_locked();
    } else {
        n = g_nbaked > 0 ? g_nbaked : 0;
        for (int k = 1; k < g_nent; k++) {
            if (!g_ent[k].def.n || baked_named(g_ent[k].def.name)) continue;
            if (n++ == i) { g_ent[k].enabled = !g_ent[k].enabled; v_dirty = 1; toggles_save_locked(); break; }
        }
    }
    pthread_mutex_unlock(&g_mu);
}

// ---------------------------------------------------------------- assembly and the public entry points

static int assemble_failed;
static void assemble(void) {
    v_n = 0;
    for (int i = 0; i < g_nent; i++) {
        if (!g_ent[i].def.n || (i && !g_ent[i].enabled)) continue;
        if (i && baked_named(g_ent[i].def.name)) continue;   // runs as a baked patch; its flag follows this table's toggle state
        if (v_n + g_ent[i].def.n > VM_PROG) {
            if (!assemble_failed) LOG("[RED] cheat: the enabled cheats exceed the %u-word program limit, all idle until reload", VM_PROG);
            assemble_failed = 1;
            v_n = 0;
            return;
        }
        for (uint32_t k = 0; k < g_ent[i].def.n; k++) { v_prog[v_n] = g_ent[i].def.op[k]; v_owner[v_n] = (uint8_t)i; v_n++; }
    }
    assemble_failed = 0;
}

void cheat_execute(void) {
    if (v_dirty) { assemble(); v_dirty = 0; }
    if (!v_n) { s_keyold = dm_input_held(); return; }
    vm_execute();
}

int cheat_reject_count(void) { return (int)g_reject; }
int cheat_enabled_count(void) {
    int n = 0;
    for (int i = 0; i < g_nent; i++) if (g_ent[i].def.n && (i == 0 || g_ent[i].enabled)) n++;
    return n;
}

// ---------------------------------------------------------------- loading (discovery, mtime hot reload, engine thread)

static char *read_file(const char *path, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[CHEAT_FILE_MAX];
    size_t n = fread(buf, 1, sizeof buf, f);
    int too_big = fgetc(f) != EOF;
    fclose(f);
    if (too_big) { LOG("[RED] cheat: %s is larger than %u bytes, ignored", path, CHEAT_FILE_MAX); return NULL; }
    *out_n = n;
    return buf;
}

// dmnt names the cheat file after main's first 8 build-ID bytes (16 hex chars); some tools use all 16 bytes. Both work.
static int cheat_file_path(const char *dir, const char *bid, char *out, size_t cap) {
    if (strlen(bid) >= 16) {
        snprintf(out, cap, "%s/%.16s.txt", dir, bid);
        if (!access(out, R_OK)) return 1;
    }
    if (strlen(bid) > 16) {
        snprintf(out, cap, "%s/%s.txt", dir, bid);
        if (!access(out, R_OK)) return 1;
    }
    snprintf(out, cap, "%s/%.16s.txt", dir, bid);
    return 0;
}

int cheat_reload(const char *dir) {
    strlcpy(g_tdir, dir, sizeof g_tdir);
    const char *bid = dm_main_build();
    if (!*bid) { LOG("[RED] cheat: this build output carries no build ID, a cheat file cannot be matched"); return 0; }
    char path[1100];
    if (!cheat_file_path(dir, bid, path, sizeof path)) return 0;
    size_t n;
    char *s = read_file(path, &n);
    if (!s) return 0;
    if (!parse_cheats(s, n)) {
        LOG("[RED] cheat: %s rejected (a line is malformed or a limit was exceeded)", path);
        g_nent = 0; v_dirty = 1;
        return 0;
    }
    int master = g_ent[0].def.n != 0;
    char tpath[1100];
    snprintf(tpath, sizeof tpath, "%s/toggles.txt", dir);
    size_t tn;
    if ((s = read_file(tpath, &tn))) parse_toggles(s, tn);
    for (int i = 0; i < VM_STATIC; i++) v_static[i] = 0;          // dmnt ResetStaticRegisters on reload
    v_dirty = 1;
    if (g_code_reject) LOG("cheat: %u code-page writes were rejected since the last load", g_code_reject);
    g_code_reject = 0;
    LOG("cheat: %s: %d cheats%s, %d enabled (build %s)", path, g_nent - (master ? 0 : 1), master ? " (1 master)" : "",
        cheat_enabled_count(), bid);
    return 1;
}

static time_t mtime_of(const char *path) {
    struct stat st;
    return stat(path, &st) ? (time_t)-1 : st.st_mtime;
}

static void *cheat_thread(void *arg) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // the game's threads (one of them real-time) would starve a default-class thread
    const char *dir = arg;
    const char *bid = dm_main_build();
    char cpath[1100], tpath[1100];
    cheat_file_path(dir, bid, cpath, sizeof cpath);           // the watcher tracks whichever name the file has
    snprintf(tpath, sizeof tpath, "%s/toggles.txt", dir);
    time_t cm = -2, tm = -2;
    int ever_loaded = 0;
    for (;;) {
        menu_hotkey_tick(dm_input_held(), now_ns());
        pthread_mutex_lock(&g_mu);                          // the menu (front-end main thread) reads and toggles rows under the same lock
        time_t c = mtime_of(cpath), t = mtime_of(tpath);
        if (c != cm) {
            cm = c; tm = t;
            if (cheat_reload(dir)) ever_loaded = 1;
            else if (!ever_loaded) LOG("cheat: no cheat file for build %s (looked for %s), cheats off", bid, cpath);
            else LOG("cheat: cheat file gone or rejected, cheats off until it returns");
        } else {
            if (t != tm) {
                tm = t;
                size_t tn;
                char *s = read_file(tpath, &tn);
                if (s) { parse_toggles(s, tn); v_dirty = 1; LOG("cheat: toggles reloaded, %d enabled", cheat_enabled_count()); }
            }
            cheat_baked_pump();                             // baked patch flags: they act even with no cheat file at all
            cheat_execute();
        }
        pthread_mutex_unlock(&g_mu);
        usleep(g_sleep_us);
    }
    return NULL;
}

void cheat_start(void) {
    const char *off = getenv("DM_CHEATS");
    if (off && !strcmp(off, "off")) return;
    if (!*dm_main_build()) { LOG("[RED] cheat: this build output carries no build ID, a cheat file cannot be matched"); return; }
    const char *dir = getenv("DM_CHEATS_DIR");
    char buf[1100];
    if (!dir || !*dir) {
        const char *save = getenv("DM_SAVE");                   // Mac default: next to the save directory, like pso_user.tsv
        if (!save || !*save) return;                            // no directory configured: cheats off, nothing logged
        snprintf(buf, sizeof buf, "%s/../cheats", save);
        dir = buf;
    }
    const char *rate = getenv("DM_CHEAT_RATE");
    long hz = rate ? strtol(rate, NULL, 10) : 12;
    if (hz < 1) hz = 1;
    if (hz > 1000) hz = 1000;
    g_sleep_us = (useconds_t)(1000000 / hz);
    g_trace = getenv("DM_CHEAT_LOG") != NULL;
    menu_hotkey_parse();
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 << 10);
    if (pthread_create(&th, &a, cheat_thread, strdup(dir))) LOG("[RED] cheat: cannot start the engine thread");
    else pthread_detach(th);
}
