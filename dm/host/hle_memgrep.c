// DM_MEMGREP=<hex bytes>[,<hex bytes>...]: DM_MEMGREP_WAIT seconds (default 15) after start, scan every readable VM region of this task for each byte signature and dump up to 4 hits per signature (± DM_MEMGREP_SPAN bytes, default 4 MiB, page-wise: unreadable pages dump as zeros) to memgrep_s<sig>_<n>_<addr>.bin beside the cwd; each hit is logged with its address.
// Offline RE helper: fish a decompressed buffer (a pak index the game parsed, a table the game built) out of the running guest without a debugger.
#include "dm.h"
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char g_sig[8][256];
static int g_sig_len[8], g_sig_n;
static long g_wait = 15, g_span = 4 << 20;

static int hexnyb(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void scan_one(int sig) {
    int hits = 0;
    vm_address_t addr = 0;
    for (;;) {
        vm_size_t sz = 0; vm_region_basic_info_data_64_t vi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &addr, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&vi, &cnt, &obj) != KERN_SUCCESS) break;
        mach_port_deallocate(mach_task_self(), obj);
        if (vi.protection & VM_PROT_READ) {
            vm_size_t off = 0;
            while (off < sz && hits < 4) {
                unsigned char stack[1 << 16];
                vm_size_t chunk = sz - off < (vm_size_t)sizeof stack ? sz - off : (vm_size_t)sizeof stack;
                vm_size_t got = 0;
                if (vm_read_overwrite(mach_task_self(), addr + off, chunk, (vm_address_t)stack, &got) != KERN_SUCCESS) break;
                for (vm_size_t i = 0; i + g_sig_len[sig] <= got && hits < 4; i++) {
                    if (stack[i] != g_sig[sig][0] || memcmp(stack + i, g_sig[sig], g_sig_len[sig])) continue;
                    vm_address_t hit = addr + off + i;
                    vm_address_t lo = hit > (vm_address_t)g_span ? hit - (vm_address_t)g_span : (vm_address_t)0, hi = hit + (vm_address_t)g_span;   // no region clamp: the buffer may span many small regions, unreadable pages dump as zeros
                    vm_size_t dn = hi - lo;
                    unsigned char *win = calloc(1, dn);   // page-wise read: guest heap regions are small and many, the window usually crosses several
                    if (win) {
                        for (vm_address_t p = lo; p < hi; p += 4096) {
                            vm_size_t pg = hi - p < 4096 ? hi - p : 4096, pgr = 0;
                            if (vm_read_overwrite(mach_task_self(), p, pg, (vm_address_t)(win + (p - lo)), &pgr) != KERN_SUCCESS) memset(win + (p - lo), 0, pg);
                        }
                        char path[256];
                        snprintf(path, sizeof path, "memgrep_s%d_%d_%llx.bin", sig, hits, (unsigned long long)hit);
                        FILE *f = fopen(path, "wb"); if (f) { fwrite(win, 1, dn, f); fclose(f); }
                        LOG("[memgrep] sig %d hit %d at %#llx (region %#llx+%#llx) -> %s (%#llx bytes)", sig, hits, (unsigned long long)hit, (unsigned long long)addr, (unsigned long long)sz, path, (unsigned long long)dn);
                    }
                    free(win);
                    hits++; i += g_sig_len[sig];   // past this hit; the scan continues in the same chunk
                }
                off += chunk;
            }
        }
        addr += sz;
        if (hits >= 4) break;
    }
    LOG("[memgrep] sig %d done: %d hit(s) for the %d-byte signature", sig, hits, g_sig_len[sig]);
}

static void *memgrep_thread(void *unused) {
    (void)unused;
    for (long s = 0; s < g_wait; s++) sleep(1);
    for (int sig = 0; sig < g_sig_n; sig++) scan_one(sig);
    return NULL;
}

__attribute__((constructor)) static void memgrep_start(void) {
    const char *e = getenv("DM_MEMGREP");
    if (!e || !*e) return;
    char *sigs = strdup(e);
    for (char *tok = strtok(sigs, ","); tok && g_sig_n < (int)(sizeof g_sig / sizeof g_sig[0]); tok = strtok(NULL, ",")) {
        for (const char *p = tok; p[0] && p[1] && g_sig_len[g_sig_n] < (int)sizeof g_sig[0]; p += 2) {
            int hi = hexnyb(p[0]), lo = hexnyb(p[1]);
            if (hi < 0 || lo < 0) die("DM_MEMGREP: bad hex at %s", p);
            g_sig[g_sig_n][g_sig_len[g_sig_n]++] = (unsigned char)(hi << 4 | lo);
        }
        g_sig_n++;
    }
    free(sigs);
    if (getenv("DM_MEMGREP_WAIT")) g_wait = strtol(getenv("DM_MEMGREP_WAIT"), NULL, 10);
    if (getenv("DM_MEMGREP_SPAN")) g_span = strtol(getenv("DM_MEMGREP_SPAN"), NULL, 10);
    pthread_t th;
    if (pthread_create(&th, NULL, memgrep_thread, NULL)) die("DM_MEMGREP: cannot start its thread");
    pthread_detach(th);
    LOG("[memgrep] will scan %d signature(s) in %lds", g_sig_n, g_wait);
}
