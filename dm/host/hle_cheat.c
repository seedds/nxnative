// Cheat VM: runs Atmosphère-format cheats (dmnt cheat VM opcodes, the format the switch-cheats-db collections use) against guest memory.
// DM_CHEATS=<file> names the cheat file for the running build ([Name] cheats, {Name} master code that always runs); DM_CHEATS_ON=<n,n,...> picks cheats by their
// 1-based position among the listed cheats in the file, [Name] entries with code (a [Name] with no code is a comment; 0 or unset = none). Guest pointers are host pointers here, so the VM reads and writes them directly, through vm_read_overwrite /
// vm_write so that a stale pointer in a chain makes that cheat skip the tick instead of faulting; writes to code pages are made writable for the store and the
// instruction cache flushed. The enabled cheats run about 60 times a second on a host thread, as dmnt runs them at the display rate on the console.
// Region bases: dmnt's MemoryAccessType 0 main, 1 heap, 2 alias, 3 ASLR (hle_svc.c dm_guest_region_base), and the host's own 4 (region_base); key conditions read the held buttons (nn::hid::NpadButton bits = dmnt's).
#include "dm.h"
#include <mach/mach.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Region 4 is the host's own: the newest NRO the game loaded through nn::ro (loader.c), 0 while there is none (the cheat's reads fail and it skips the tick).
// Super Mario Sunshine's code lives in its recompiled-game NRO, which its cheats find on Horizon through the SDK's ro module list, which the host does not have
static uint64_t region_base(int region) {
    if (region != 4) return dm_guest_region_base(region);
    module_t *m = nro_newest_loaded(); return m ? (uint64_t)(uintptr_t)m->base : 0;
}
uint64_t dm_input_buttons(void);

#define MAX_CHEATS 256
#define MAX_WORDS 4096
typedef struct { char name[96]; uint32_t *op; int n; int on; int master; } cheat_t;
static cheat_t g_ch[MAX_CHEATS]; static int g_nch;
static uint64_t g_reg[16], g_saved[16];
static int g_fault;   // a read or write failed during this cheat's run: the rest of it is skipped
static int g_dbg; static uint64_t g_dbg_writes, g_dbg_last, g_dbg_skips;   // DM_CHEATS_DEBUG=1: once a second, what each cheat did

static int mem_read(uint64_t a, void *out, int w) {
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)a, (vm_size_t)w, (vm_address_t)out, &got) != KERN_SUCCESS || got != (vm_size_t)w) { g_fault = 1; memset(out, 0, (size_t)w); return 0; }
    return 1;
}
static uint64_t rd(uint64_t a, int w) { uint64_t v = 0; mem_read(a, &v, w); return v; }
static void wr(uint64_t a, uint64_t v, int w) {
    g_dbg_writes++; g_dbg_last = a;
    if (vm_write(mach_task_self(), (vm_address_t)a, (vm_offset_t)&v, (mach_msg_type_number_t)w) == KERN_SUCCESS) return;
    // a code page (read + execute): make it writable for the store, put the protection back and flush the instruction cache
    vm_address_t pg = (vm_address_t)(a & ~(uint64_t)(vm_page_size - 1)); vm_size_t len = (vm_size_t)(((a + (uint64_t)w + vm_page_size - 1) & ~(uint64_t)(vm_page_size - 1)) - pg);
    if (vm_protect(mach_task_self(), pg, len, 0, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) { g_fault = 1; return; }
    memcpy((void *)(uintptr_t)a, &v, (size_t)w);
    vm_protect(mach_task_self(), pg, len, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    sys_icache_invalidate((void *)(uintptr_t)a, (size_t)w);
}
static uint64_t mask(uint64_t v, int w) { return w >= 8 ? v : v & ((1ull << (w * 8)) - 1); }
static int cond(uint64_t a, uint64_t b, int c) {
    switch (c) { case 1: return a > b; case 2: return a >= b; case 3: return a < b; case 4: return a <= b; case 5: return a == b; case 6: return a != b; }
    return 0;
}
static uint64_t arith(uint64_t a, uint64_t b, int c) {
    switch (c) { case 0: return a + b; case 1: return a - b; case 2: return a * b; case 3: return a << b; case 4: return a >> b;
                 case 5: return a & b; case 6: return a | b; case 7: return ~a; case 8: return a ^ b; case 9: return a; }
    return a;
}
static int oplen(const uint32_t *op) {   // words in the instruction starting at op
    uint32_t w = op[0]; int T = (w >> 24) & 0xf;
    switch (w >> 28) {
    case 0x0: case 0x1: return (T == 8) ? 4 : 3;
    case 0x2: return 1;
    case 0x3: return ((w >> 24) & 0xf) == 0 ? 2 : 1;
    case 0x4: return 3;
    case 0x5: return 2;
    case 0x6: return 3;
    case 0x7: return 2;
    case 0x8: return 1;
    case 0x9: return ((w >> 8) & 0xf) ? (T == 8 ? 3 : 2) : 1;
    case 0xA: { int o = (w >> 8) & 0xf; return (o == 2 || o == 4 || o == 5) ? 2 : 1; }
    case 0xC: {
        int sub = (w >> 24) & 0xf;
        if (sub == 0) { int x = (w >> 8) & 0xf, T0 = (w >> 20) & 0xf; return (x == 0 || x == 2) ? 2 : x == 4 ? (T0 == 8 ? 3 : 2) : 1; }
        return 1;
    }
    case 0xF: return 1;
    }
    return 1;
}
// skip forward from index i (just after a failed condition) to the matching else (21) or end (20); returns the index after it
static int skip_cond(const uint32_t *op, int n, int i, int stop_at_else) {
    int depth = 0;
    while (i < n) {
        uint32_t w = op[i]; int k = w >> 28;
        if (k == 0x1 || k == 0x8 || (k == 0xC && ((w >> 24) & 0xf) == 0)) depth++;
        else if (k == 0x2) {
            int is_else = (w >> 24) & 0xf;
            if (!depth) { if (!is_else || stop_at_else) return i + 1; }
            else if (!is_else) depth--;
        }
        i += oplen(op + i);
    }
    return n;
}
static void run(const uint32_t *op, int n) {
    int loop_start[16]; for (int k = 0; k < 16; k++) loop_start[k] = -1;
    int i = 0;
    while (i < n && !g_fault) {
        uint32_t w = op[i]; int len = oplen(op + i), T = (w >> 24) & 0xf;
        switch (w >> 28) {
        case 0x0: {   // 0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV): [M + R + A] = V
            int M = (w >> 20) & 0xf, R = (w >> 16) & 0xf; uint64_t A = ((uint64_t)(w & 0xff) << 32) | op[i + 1];
            uint64_t v = T == 8 ? ((uint64_t)op[i + 2] << 32) | op[i + 3] : op[i + 2];
            wr(region_base(M) + g_reg[R] + A, v, T); break;
        }
        case 0x1: {   // 1TMC00AA AAAAAAAA VVVVVVVV (VVVVVVVV): if [M + A] C V
            int M = (w >> 20) & 0xf, C = (w >> 16) & 0xf; uint64_t A = ((uint64_t)(w & 0xff) << 32) | op[i + 1];
            uint64_t v = T == 8 ? ((uint64_t)op[i + 2] << 32) | op[i + 3] : op[i + 2];
            if (!cond(rd(region_base(M) + A, T), v, C)) { g_dbg_skips++; if (g_dbg) LOG("cheats: condition at %#llx (%#llx) %d %#llx false", (unsigned long long)(region_base(M) + A), (unsigned long long)rd(region_base(M) + A, T), C, (unsigned long long)v); i = skip_cond(op, n, i + len, 1); continue; }
            break;
        }
        case 0x2:   // 20000000 end / 21000000 else: reaching an else while executing means the true branch ran, skip to the end
            if (T == 1) { i = skip_cond(op, n, i + 1, 0); continue; }
            break;
        case 0x3: {   // 300R0000 VVVVVVVV loop start / 310R0000 loop end
            int R = (w >> 16) & 0xf;
            if (T == 0) { g_reg[R] = op[i + 1]; loop_start[R] = i + len; }
            else if (g_reg[R] > 1 && loop_start[R] >= 0) { g_reg[R]--; i = loop_start[R]; continue; }
            break;
        }
        case 0x4: g_reg[(w >> 16) & 0xf] = ((uint64_t)op[i + 1] << 32) | op[i + 2]; break;   // 400R0000 VVVVVVVV VVVVVVVV
        case 0x5: {   // 5TMRI0AA AAAAAAAA: R = [M + A] (I 0) or [R + A] (I 1)
            int M = (w >> 20) & 0xf, R = (w >> 16) & 0xf, I = (w >> 12) & 0xf; uint64_t A = ((uint64_t)(w & 0xff) << 32) | op[i + 1];
            g_reg[R] = rd((I ? g_reg[R] : region_base(M)) + A, T); break;
        }
        case 0x6: {   // 6T0RIor0 VVVVVVVV VVVVVVVV: [R (+ r)] = V; I: R += T
            int R = (w >> 16) & 0xf, I = (w >> 12) & 0xf, o = (w >> 8) & 0xf, r = (w >> 4) & 0xf;
            wr(g_reg[R] + (o ? g_reg[r] : 0), ((uint64_t)op[i + 1] << 32) | op[i + 2], T); if (I) g_reg[R] += (uint64_t)T; break;
        }
        case 0x7: { int R = (w >> 16) & 0xf, C = (w >> 12) & 0xf; g_reg[R] = mask(arith(g_reg[R], op[i + 1], C), T); break; }   // 7T0RC000 VVVVVVVV
        case 0x8: {   // 8kkkkkkk: run until the matching end while all those keys are held
            uint32_t keys = w & 0x0fffffff;
            if ((dm_input_buttons() & keys) != keys) { i = skip_cond(op, n, i + 1, 1); continue; }
            break;
        }
        case 0x9: {   // 9TCRS0s0: R = S op s / 9TCRS100 V: R = S op V
            int C = (w >> 20) & 0xf, R = (w >> 16) & 0xf, S = (w >> 12) & 0xf, imm = (w >> 8) & 0xf, s = (w >> 4) & 0xf;
            uint64_t b = imm ? (T == 8 ? ((uint64_t)op[i + 1] << 32) | op[i + 2] : op[i + 1]) : g_reg[s];
            g_reg[R] = mask(arith(g_reg[S], b, C), T); break;
        }
        case 0xA: {   // ATSRIOxa (aaaaaaaa): store S at an address built from R, a register, a memory region and an immediate
            int S = (w >> 20) & 0xf, R = (w >> 16) & 0xf, I = (w >> 12) & 0xf, O = (w >> 8) & 0xf, x = (w >> 4) & 0xf;
            uint64_t a = len == 2 ? ((uint64_t)(w & 0xf) << 32) | op[i + 1] : 0, addr = g_reg[R];
            switch (O) { case 1: addr += g_reg[x]; break; case 2: addr += a; break; case 3: addr = region_base(x) + g_reg[R]; break;
                         case 4: addr = region_base(x) + a; break; case 5: addr = region_base(x) + g_reg[R] + a; break; }
            wr(addr, g_reg[S], T); if (I) g_reg[R] += (uint64_t)T; break;
        }
        case 0xC:
            if (((w >> 24) & 0xf) == 0) {   // C0TcSX..: if register S c (memory / register / static value)
                int T0 = (w >> 20) & 0xf, c = (w >> 16) & 0xf, S = (w >> 12) & 0xf, X = (w >> 8) & 0xf, M = (w >> 4) & 0xf, r = w & 0xf;
                uint64_t a = len >= 2 ? ((uint64_t)(w & 0xf) << 32) | op[i + 1] : 0, other = 0;
                switch (X) { case 0: other = rd(region_base(M) + a, T0); break; case 1: other = rd(region_base(M) + g_reg[r], T0); break;
                             case 2: other = rd(g_reg[M] + a, T0); break; case 3: other = rd(g_reg[M] + g_reg[r], T0); break;
                             case 4: other = T0 == 8 ? ((uint64_t)op[i + 1] << 32) | op[i + 2] : op[i + 1]; break; case 5: other = g_reg[M]; break; }
                if (!cond(mask(g_reg[S], T0), mask(other, T0), c)) { i = skip_cond(op, n, i + len, 1); continue; }
            } else if (((w >> 24) & 0xf) == 1) {   // C10D0Sx0: save (x 0) / restore (x 1) register D <-> slot S
                int D = (w >> 16) & 0xf, S = (w >> 8) & 0xf, x = (w >> 4) & 0xf;
                if (x == 0) g_saved[S] = g_reg[D]; else g_reg[D] = g_saved[S];
            }
            break;
        default: break;   // FF0 pause / FF1 resume / FFF debug log: nothing to do on the host
        }
        i += len;
    }
}

static int parse(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { LOG("[RED] cheats: cannot open %s", path); return 0; }
    char line[512]; cheat_t *cur = NULL; static uint32_t words[MAX_CHEATS][256]; int bad = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '[' || *p == '{') {
            char close = *p == '[' ? ']' : '}'; char *e = strchr(p + 1, close); if (!e || g_nch >= MAX_CHEATS) { cur = NULL; continue; }
            cur = &g_ch[g_nch]; memset(cur, 0, sizeof *cur); cur->op = words[g_nch]; cur->master = close == '}';
            snprintf(cur->name, sizeof cur->name, "%.*s", (int)(e - p - 1), p + 1); g_nch++; continue;
        }
        if (!cur) continue;
        for (char *t = strtok(p, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
            if (strlen(t) != 8 || strspn(t, "0123456789abcdefABCDEF") != 8) { bad++; continue; }
            if (cur->n < 256) cur->op[cur->n++] = (uint32_t)strtoul(t, NULL, 16);
        }
    }
    fclose(f);
    int k = 0; for (int c = 0; c < g_nch; c++) if (g_ch[c].n) { if (k != c) g_ch[k] = g_ch[c]; k++; }   // titles without code ([game, version] lines) are not cheats: positions count real ones only
    g_nch = k;
    if (bad) LOG("[RED] cheats: %d words in %s are not 8-digit hex and were ignored", bad, path);
    return g_nch;
}

static void *cheat_thread(void *arg) {
    (void)arg; sleep(3);   // past module loading and the SDK's start-up
    uint64_t tick = 0; g_dbg = getenv("DM_CHEATS_DEBUG") != NULL;
    for (;;) {
        for (int c = 0; c < g_nch; c++) {
            if (!(g_ch[c].on || g_ch[c].master) || !g_ch[c].n) continue;
            g_fault = 0; g_dbg_writes = g_dbg_skips = 0; run(g_ch[c].op, g_ch[c].n);
            if (g_dbg && tick % 60 == 0) LOG("cheats: [%s] writes %llu (last %#llx) conditions false %llu%s", g_ch[c].name, (unsigned long long)g_dbg_writes, (unsigned long long)g_dbg_last, (unsigned long long)g_dbg_skips, g_fault ? " memory fault" : "");
        }
        tick++;
        usleep(16667);
    }
    return NULL;
}

// Called by the front end once the start-up settings are applied (DM_CHEATS_ON may come from its cheats list)
void dm_cheats_start(void) {
    static int once; if (once++) return;
    const char *path = getenv("DM_CHEATS"); if (!path || !*path || !parse(path)) return;
    const char *on = getenv("DM_CHEATS_ON"); int enabled = 0;
    // Positions count the listed cheats only, as the front end's menu lists them: master codes always run and take no number (Super Mario Sunshine's file starts with one)
    if (on) for (const char *p = on; *p; ) {
        int k = atoi(p);
        for (int c = 0, n = 0; c < g_nch; c++) if (!g_ch[c].master && ++n == k) { g_ch[c].on = 1; break; }
        const char *q = strchr(p, ','); if (!q) break; p = q + 1;
    }
    char list[1024] = ""; for (int c = 0; c < g_nch; c++) if (g_ch[c].on || g_ch[c].master) { enabled++; strlcat(list, " [", sizeof list); strlcat(list, g_ch[c].name, sizeof list); strlcat(list, "]", sizeof list); }
    LOG("cheats: %d in %s, %d running:%s", g_nch, path, enabled, list);
    if (!enabled) return;
    pthread_t th; if (!pthread_create(&th, NULL, cheat_thread, NULL)) pthread_detach(th);
}
