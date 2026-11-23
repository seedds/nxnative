// Shared runtime code-write support for Atmosphère cheats (hle_cheats.c) and exlaunch mod plugins (hle_exl.c).
// Both paths write into executable module pages through a separate mach_vm_remap writable alias (keeping the game's
// own mapping read+execute so running cores never fault on an mprotect toggle) and refuse writes to any text word the
// rewriter changed or moved (<module>.changed.tsv, verified against the module's build ID).
#ifndef DM_CODE_WRITE_H
#define DM_CODE_WRITE_H

#include "dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#if !TARGET_OS_IPHONE
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_prot.h>
#endif

// Map a second, writable alias of module m's image (same physical pages, copy=FALSE). Returns NULL on iOS (signed
// code pages cannot be made writable) or if the remap fails.
static uint8_t *dm_code_alias_map(const module_t *m) {
#if TARGET_OS_IPHONE
    (void)m;
    return NULL;
#else
    if (!m || !m->base || !m->size) return NULL;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 0x4000;   // 16 KB on Apple Silicon, a safe floor elsewhere
    uint64_t size = ((uint64_t)m->size + (uint64_t)pg - 1) & ~((uint64_t)pg - 1);
    mach_vm_address_t dst = 0;
    vm_prot_t cur = 0, mx = 0;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, size, 0, VM_FLAGS_ANYWHERE,
                                     mach_task_self(), (mach_vm_address_t)(uintptr_t)m->base, FALSE,
                                     &cur, &mx, VM_INHERIT_SHARE);
    if (kr) return NULL;
    kr = mach_vm_protect(mach_task_self(), dst, size, 0, VM_PROT_READ | VM_PROT_WRITE);
    if (kr) { mach_vm_deallocate(mach_task_self(), dst, size); return NULL; }
    return (uint8_t *)(uintptr_t)dst;
#endif
}

// Sorted non-overlapping {offset, length} runs of text words the rewriter changed or moved (<module>.changed.tsv).
// state: 0 unread, 1 valid, -1 missing / OOM / build ID mismatch (code writes refused).
typedef struct {
    uint32_t (*run)[2];
    int n;
    int state;
} dm_changed_t;

static void dm_changed_load(dm_changed_t *c, const module_t *m, const char *dir, const char *who) {
    if (!c || c->state || !m || !dir) return;
    c->state = -1;
    char p[1100];
    snprintf(p, sizeof p, "%s/%s.changed.tsv", dir, m->name);
    FILE *f = fopen(p, "r");
    if (!f) {
        LOG("[RED] %s: no %s (the modules come from an older rewriter): code writes to %s are refused; rewrite them with dm/tools/patch_nso.py", who, p, m->name);
        return;
    }
    char line[256], bid[40] = "";
    int cap = 0;
    unsigned long off, len;
    while (fgets(line, sizeof line, f)) {   // NOLINT(clang-analyzer-unix.BlockInCriticalSection): callers load outside hot frame paths (cheats under cold load lock, exl before taking its view lock)
        if (sscanf(line, "# BUILD_ID %39s", bid) == 1) continue;
        if (sscanf(line, "%lx %lx", &off, &len) != 2) continue;
        if (c->n == cap) {
            cap = cap ? cap * 2 : 1024;
            uint32_t (*grown)[2] = realloc(c->run, (size_t)cap * sizeof *c->run);
            if (!grown) { fclose(f); free(c->run); c->run = NULL; c->n = 0; return; }
            c->run = grown;
        }
        c->run[c->n][0] = (uint32_t)off;
        c->run[c->n][1] = (uint32_t)len;
        c->n++;
    }
    fclose(f);
    if (strcasecmp(bid, m->build_id)) {
        LOG("[RED] %s: %s is for build %s, not %s's %s: code writes are refused", who, p, bid, m->name, m->build_id);
        free(c->run); c->run = NULL; c->n = 0;
        return;
    }
    c->state = 1;
    LOG("%s: %d runs of text words the rewriter changed or moved in %s (code writes into them are refused)", who, c->n, m->name);
}

// First changed run overlapping [off, off + len): >= 0 hit index, -1 clean, -2 list missing/invalid.
static int dm_changed_hit(const dm_changed_t *c, uint64_t off, uint64_t len) {
    if (c->state != 1) return -2;
    int lo = 0, hi = c->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if ((uint64_t)c->run[mid][0] + c->run[mid][1] <= off) lo = mid + 1;
        else hi = mid;
    }
    return lo < c->n && c->run[lo][0] < off + len ? lo : -1;
}

#endif
